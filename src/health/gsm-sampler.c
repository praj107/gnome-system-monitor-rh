/*
 * gsm-health-sampler — the background half of storage-wear tracking.
 *
 * Runs unprivileged as a systemd user service, only when the user has opted
 * in. It never escalates: everything it reads (udisks2 over the system bus,
 * cgroup io.stat, /proc/<pid>/io, battery sysfs) is available to a normal
 * user. The privileged SMART helper is only ever run on demand from the UI.
 *
 * It exists because the counters that make process blame possible are not
 * durable: a cgroup's io.stat dies with the cgroup, and /proc/<pid>/io dies
 * with the process. Sampling only while a window is open would miss most of
 * what a machine writes, so the honest options were "sample in the background"
 * or "do not make the claim at all".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <signal.h>
#include <stdlib.h>

#include <glib-unix.h>
#include <glib/gi18n.h>

#include "gsm-battery.h"
#include "gsm-blame.h"
#include "gsm-drive-spec.h"
#include "gsm-health-db.h"
#include "gsm-procio.h"
#include "gsm-udisks.h"

#define DEFAULT_INTERVAL_SECS 15

typedef struct {
  GsmHealthDb     *db;
  GDBusConnection *bus;
  GMainLoop       *loop;

  /* Last seen cumulative counters, so we can turn them into deltas.
   * Keyed by "<cgroup>\n<major>:<minor>" and by "<pid>" respectively. */
  GHashTable      *prev_cgroup;
  GHashTable      *prev_proc;

  /* Maps "<major>:<minor>" of a whole disk to its stable drive id. */
  GHashTable      *device_to_drive;

  guint            interval;
} Sampler;


static uint64_t
prev_value (GHashTable *table,
            const char *key)
{
  gpointer found = g_hash_table_lookup (table, key);

  return found ? *(uint64_t *) found : 0;
}


static void
store_value (GHashTable *table,
             const char *key,
             uint64_t    value)
{
  uint64_t *slot = g_new (uint64_t, 1);

  *slot = value;
  g_hash_table_replace (table, g_strdup (key), slot);
}


/*
 * Cumulative counters only ever go up *while the thing they belong to lives*.
 * A cgroup or PID that comes back lower than we last saw it is not a drive
 * un-writing data — it is a fresh instance reusing the name, with its
 * counters back at zero. In that case the whole current value is new.
 */
static uint64_t
delta_since (GHashTable *table,
             const char *key,
             uint64_t    current)
{
  uint64_t previous = prev_value (table, key);

  store_value (table, key, current);

  if (current >= previous)
    return current - previous;

  return current;
}


static const char *
drive_for_device (Sampler *self,
                  guint    major,
                  guint    minor)
{
  g_autofree char *key = g_strdup_printf ("%u:%u", major, minor);

  return g_hash_table_lookup (self->device_to_drive, key);
}


/* ------------------------------------------------------------------ */

static void
sample_drives (Sampler *self)
{
  g_autoptr (GPtrArray) infos = NULL;
  g_autoptr (GError) error = NULL;

  infos = gsm_udisks_collect (self->bus, &error);
  if (infos == NULL) {
    g_warning ("sampler: udisks unavailable: %s", error->message);
    return;
  }

  g_hash_table_remove_all (self->device_to_drive);

  for (guint i = 0; i < infos->len; i++) {
    GsmDriveInfo *info = g_ptr_array_index (infos, i);
    g_autoptr (GsmDrive) drive = NULL;

    if (info->serial == NULL)
      continue;

    if (info->major || info->minor) {
      g_hash_table_replace (self->device_to_drive,
                            g_strdup_printf ("%u:%u", info->major, info->minor),
                            g_strdup (info->serial));
    }

    drive = gsm_drive_new (info->serial);
    gsm_drive_apply_info (drive, info);

    gsm_drive_resolve_endurance (
      drive, gsm_health_db_get_user_tbw (self->db, info->serial));

    gsm_health_db_record_drive (self->db, drive);
  }
}


static void
sample_blame (Sampler *self)
{
  g_autoptr (GPtrArray) cgroups = gsm_blame_collect ();
  g_autoptr (GPtrArray) procs = NULL;
  gboolean delegated = gsm_blame_user_io_is_delegated (cgroups);

  for (guint i = 0; i < cgroups->len; i++) {
    GsmBlameEntry *entry = g_ptr_array_index (cgroups, i);
    const char *drive_id;
    g_autofree char *key = NULL;
    uint64_t delta;

    /*
     * Without IO delegation, "user.slice" is a single lump covering every
     * desktop app — the same writes /proc/<pid>/io reports individually. We
     * take the per-process view instead and drop the lump, because counting
     * both would double every app's contribution.
     */
    if (!delegated && g_strcmp0 (entry->cgroup, "user.slice") == 0)
      continue;

    drive_id = drive_for_device (self, entry->major, entry->minor);
    if (drive_id == NULL)
      continue;

    key = g_strdup_printf ("%s\n%u:%u", entry->cgroup, entry->major, entry->minor);
    delta = delta_since (self->prev_cgroup, key, entry->wbytes);

    /* cgroup accounting names the device, so these rows are exact. */
    gsm_health_db_record_blame (self->db, drive_id, entry->name, delta, TRUE);
  }

  /* With delegation on, cgroups already cover user apps exactly; reading
   * /proc as well would double count. */
  if (delegated)
    return;

  procs = gsm_proc_io_collect ();

  for (guint i = 0; i < procs->len; i++) {
    GsmProcIoEntry *entry = g_ptr_array_index (procs, i);
    const char *drive_id;
    g_autofree char *key = NULL;
    uint64_t delta;

    if (entry->major == 0)
      continue;                   /* no block device behind it — nothing honest to say */

    drive_id = drive_for_device (self, entry->major, entry->minor);
    if (drive_id == NULL)
      continue;

    key = g_strdup_printf ("%d", entry->pid);
    delta = delta_since (self->prev_proc, key, entry->wbytes);

    /* exact = FALSE: /proc/<pid>/io gives us the byte count but not the
     * device, so which drive this landed on is inferred. The UI marks it. */
    gsm_health_db_record_blame (self->db, drive_id, entry->name, delta, FALSE);
  }
}


static void
sample_batteries (Sampler *self)
{
  g_autoptr (GPtrArray) batteries = gsm_battery_collect ();

  for (guint i = 0; i < batteries->len; i++) {
    GsmBatteryInfo *info = g_ptr_array_index (batteries, i);

    gsm_health_db_record_battery (self->db, info->id,
                                  info->health_percent, info->cycle_count);
  }
}


static gboolean
tick (gpointer data)
{
  Sampler *self = data;
  g_autoptr (GError) error = NULL;

  sample_drives (self);
  sample_blame (self);
  sample_batteries (self);

  gsm_health_db_roll_up (self->db);

  if (!gsm_health_db_commit (self->db, &error))
    g_warning ("sampler: could not commit: %s", error->message);

  return G_SOURCE_CONTINUE;
}


static gboolean
on_terminate (gpointer data)
{
  Sampler *self = data;

  g_debug ("sampler: shutting down");
  g_main_loop_quit (self->loop);

  return G_SOURCE_REMOVE;
}


/* Drop remembered counters for things that no longer exist. */
static void
prune_table (GHashTable *previous,
             GHashTable *live)
{
  GHashTableIter iter;
  char *key;

  g_hash_table_iter_init (&iter, previous);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL)) {
    if (!g_hash_table_contains (live, key))
      g_hash_table_iter_remove (&iter);
  }
}


/*
 * The prev_* tables gain an entry for every PID and every cgroup we have ever
 * seen. Both leak without this: PIDs churn, and systemd mints a brand new
 * scope for every single app launch (app-org.chromium.Chromium-<pid>.scope),
 * so on a desktop left running for weeks — which is exactly this daemon's
 * job — they would grow without bound.
 *
 * We only ever look up keys we have just observed, so forgetting a dead one
 * costs nothing.
 */
static gboolean
prune (gpointer data)
{
  Sampler *self = data;
  g_autoptr (GPtrArray) procs = gsm_proc_io_collect ();
  g_autoptr (GPtrArray) cgroups = gsm_blame_collect ();
  g_autoptr (GHashTable) live_procs =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GHashTable) live_cgroups =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < procs->len; i++) {
    GsmProcIoEntry *entry = g_ptr_array_index (procs, i);

    g_hash_table_add (live_procs, g_strdup_printf ("%d", entry->pid));
  }

  for (guint i = 0; i < cgroups->len; i++) {
    GsmBlameEntry *entry = g_ptr_array_index (cgroups, i);

    g_hash_table_add (live_cgroups,
                      g_strdup_printf ("%s\n%u:%u", entry->cgroup,
                                       entry->major, entry->minor));
  }

  prune_table (self->prev_proc, live_procs);
  prune_table (self->prev_cgroup, live_cgroups);

  return G_SOURCE_CONTINUE;
}


int
main (int   argc,
      char *argv[])
{
  Sampler self = { 0 };
  g_autoptr (GError) error = NULL;
  const char *interval_env;

  (void) argc;
  (void) argv;

  self.interval = DEFAULT_INTERVAL_SECS;
  interval_env = g_getenv ("GSM_HEALTH_INTERVAL");
  if (interval_env != NULL) {
    int parsed = atoi (interval_env);

    if (parsed >= 5 && parsed <= 3600)
      self.interval = (guint) parsed;
  }

  self.db = gsm_health_db_new (NULL, &error);
  if (self.db == NULL) {
    g_printerr ("gsm-health-sampler: %s\n", error->message);
    return 1;
  }

  self.bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (self.bus == NULL) {
    g_printerr ("gsm-health-sampler: no system bus: %s\n", error->message);
    return 1;
  }

  self.prev_cgroup = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self.prev_proc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self.device_to_drive = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

  self.loop = g_main_loop_new (NULL, FALSE);

  g_unix_signal_add (SIGTERM, on_terminate, &self);
  g_unix_signal_add (SIGINT, on_terminate, &self);

  /* Prime the deltas: the first tick would otherwise attribute every byte
   * each process has written since it started as if it happened just now,
   * putting a huge false spike at the moment tracking was switched on. */
  sample_drives (&self);
  {
    g_autoptr (GPtrArray) cgroups = gsm_blame_collect ();
    g_autoptr (GPtrArray) procs = gsm_proc_io_collect ();

    for (guint i = 0; i < cgroups->len; i++) {
      GsmBlameEntry *entry = g_ptr_array_index (cgroups, i);
      g_autofree char *key = g_strdup_printf ("%s\n%u:%u", entry->cgroup,
                                              entry->major, entry->minor);

      store_value (self.prev_cgroup, key, entry->wbytes);
    }

    for (guint i = 0; i < procs->len; i++) {
      GsmProcIoEntry *entry = g_ptr_array_index (procs, i);
      g_autofree char *key = g_strdup_printf ("%d", entry->pid);

      store_value (self.prev_proc, key, entry->wbytes);
    }
  }
  gsm_health_db_commit (self.db, NULL);

  g_timeout_add_seconds (self.interval, tick, &self);
  g_timeout_add_seconds (600, prune, &self);

  g_debug ("sampler: running, every %us", self.interval);
  g_main_loop_run (self.loop);

  gsm_health_db_commit (self.db, NULL);

  g_clear_pointer (&self.loop, g_main_loop_unref);
  g_clear_pointer (&self.prev_cgroup, g_hash_table_unref);
  g_clear_pointer (&self.prev_proc, g_hash_table_unref);
  g_clear_pointer (&self.device_to_drive, g_hash_table_unref);
  g_clear_object (&self.bus);
  g_clear_object (&self.db);

  return 0;
}
