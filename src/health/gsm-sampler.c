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
 * It reads often and writes rarely. The blame counters are read every tick
 * (they die with their process), but the deltas are held in memory and the
 * database is written once per flush interval and on SIGTERM, so the sampler
 * stays within a few hundred kilobytes of writes an hour instead of a commit
 * of several pages every few seconds. Drive health is fetched at the flush
 * cadence too: udisksd refreshes ATA and NVMe SMART data from its own
 * housekeeping timer, which udiskslinuxprovider.c schedules every 10 minutes
 * (g_timeout_add_seconds (10*60, on_housekeeping_timeout, ...) calling
 * udisks_linux_drive_object_housekeeping), so asking more often only returns
 * the same values.
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

/* Matches udisksd's housekeeping period; see the file header. */
#define DEFAULT_FLUSH_INTERVAL_SECS 600

/* A device the map does not know triggers a refresh, but at most this often:
 * partitions and device-mapper nodes never appear in it. */
#define MAP_MISS_REFRESH_SECS 60

typedef struct {
  GsmHealthDb     *db;
  GDBusConnection *bus;
  GMainLoop       *loop;

  /* Turns cgroup io.stat snapshots into per-interval rows that sum to the
   * device total, and remembers what it last saw. */
  GsmBlameTracker *tracker;

  /* Rows for devices the drive map does not know yet. */
  GsmBlameHold    *hold;

  /* Last seen cumulative /proc/<pid>/io counters, keyed by "<pid>". */
  GHashTable      *prev_proc;

  /* Maps "<major>:<minor>" of a whole disk to its stable drive id. */
  GHashTable      *device_to_drive;

  /* "<major>:<minor>" keys already looked up since the last map refresh and
   * not found, so one unmappable device does not cost a refresh per tick. */
  GHashTable      *unknown_devices;
  int64_t          map_refreshed_us;
  guint            misses;

  guint            interval;
  guint            flush_interval;
  int64_t          last_flush_us;
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
 * A PID that comes back lower than we last saw it is not a drive un-writing
 * data — it is a fresh instance reusing the number, with its counters back at
 * zero. In that case the whole current value is new.
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
  const char *drive_id = g_hash_table_lookup (self->device_to_drive, key);

  if (drive_id != NULL)
    return drive_id;

  if (g_hash_table_add (self->unknown_devices, g_steal_pointer (&key)))
    self->misses++;

  return NULL;
}


/* ------------------------------------------------------------------ */

/*
 * Ask udisks which drives exist and rebuild the major:minor map. With
 * `record`, also hand each drive's identity and health to the database
 * (which skips what has not changed). FALSE when udisks could not say,
 * leaving the map as it was.
 */
static gboolean
refresh_drives (Sampler  *self,
                gboolean  record)
{
  g_autoptr (GPtrArray) infos = NULL;
  g_autoptr (GError) error = NULL;

  self->map_refreshed_us = g_get_monotonic_time ();

  infos = gsm_udisks_collect (self->bus, &error);
  if (infos == NULL)
    {
      g_warning ("sampler: udisks unavailable: %s", error->message);
      return FALSE;
    }

  g_hash_table_remove_all (self->device_to_drive);
  g_hash_table_remove_all (self->unknown_devices);

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

    if (!record)
      continue;

    drive = gsm_drive_new (info->serial);
    gsm_drive_apply_info (drive, info);

    gsm_drive_resolve_endurance (
      drive, gsm_health_db_get_user_tbw (self->db, info->serial));

    gsm_health_db_record_drive (self->db, drive);
  }

  return TRUE;
}


static const char *
lookup_drive (guint    major,
              guint    minor,
              gpointer data)
{
  return drive_for_device (data, major, minor);
}


static void
record_delta (const char          *drive_id,
              const GsmBlameDelta *delta,
              gpointer             data)
{
  Sampler *self = data;

  /* cgroup accounting names the device, so these rows are exact. */
  gsm_health_db_record_blame_row (self->db, drive_id, delta->name, "",
                                  delta->basis, delta->wbytes);
}


/*
 * Record the cgroup rows of one interval, and whatever was waiting for a
 * drive. A row for a device the drive map does not know yet is not lost: it
 * stays held until a refresh of the map resolves it.
 */
static void
record_cgroup_deltas (Sampler   *self,
                      GPtrArray *deltas)
{
  gboolean delegated = gsm_blame_tracker_user_io_is_delegated (self->tracker);

  g_autoptr (GPtrArray) wanted = g_ptr_array_new ();

  for (guint i = 0; i < deltas->len; i++)
    {
      GsmBlameDelta *delta = g_ptr_array_index (deltas, i);

      /*
       * Without IO delegation, "user.slice" is a single lump covering every
       * desktop app — the same writes /proc/<pid>/io reports individually. We
       * take the per-process view instead and drop the lump, because counting
       * both would double every app's contribution.
       */
      if (!delegated && g_strcmp0 (delta->cgroup, "user.slice") == 0)
        continue;

      g_ptr_array_add (wanted, delta);
    }

  gsm_blame_hold_resolve (self->hold, wanted, lookup_drive, record_delta, self, FALSE);
}


/*
 * Read the drive map again and settle what was held for unknown devices:
 * those it now knows are recorded, those it still does not know are not a
 * tracked drive and are dropped. Nothing is dropped when udisks was silent.
 */
static gboolean
refresh_and_resolve (Sampler *self,
                     gboolean record)
{
  if (!refresh_drives (self, record))
    return FALSE;

  gsm_blame_hold_resolve (self->hold, NULL, lookup_drive, record_delta, self, TRUE);

  return TRUE;
}


/* The per-process view, used only while IO is not delegated into the
 * session (see above). */
static void
sample_procfs (Sampler *self)
{
  g_autoptr (GPtrArray) procs = NULL;

  /* With delegation on, cgroups already cover user apps exactly; reading
   * /proc as well would double count. */
  if (gsm_blame_tracker_user_io_is_delegated (self->tracker))
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

      /* procfs basis: /proc/<pid>/io gives us the byte count but not the
       * device, so which drive this landed on is inferred. The UI marks it. */
      gsm_health_db_record_blame_row (self->db, drive_id, entry->name, "",
                                      GSM_HEALTH_BASIS_PROCFS, delta);
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


/*
 * One flush: drives and batteries are read now (they move slowly), the
 * rollups run if they are due, and everything reaches storage in a single
 * short transaction.
 */
static void
flush (Sampler  *self,
       gboolean  read_hardware)
{
  g_autoptr (GError) error = NULL;

  if (read_hardware)
    {
      refresh_and_resolve (self, TRUE);
      sample_batteries (self);
      gsm_health_db_roll_up (self->db);
    }

  if (!gsm_health_db_flush (self->db, &error))
    g_warning ("sampler: could not write the record: %s", error->message);

  self->last_flush_us = g_get_monotonic_time ();
}


/*
 * One read of everything: the cgroup tree once, then /proc when it is needed.
 * A device the map has never heard of means the drive may have just appeared:
 * its rows wait (the tracker has already moved on, so they cannot be re-read)
 * and the map is refreshed, at most once a minute, until it resolves them.
 */
static void
record_interval (Sampler *self)
{
  g_autoptr (GPtrArray) deltas = gsm_blame_tracker_update (self->tracker);
  int64_t now_us = g_get_monotonic_time ();

  record_cgroup_deltas (self, deltas);
  sample_procfs (self);

  if ((self->misses == 0 && gsm_blame_hold_count (self->hold) == 0) ||
      now_us - self->map_refreshed_us < MAP_MISS_REFRESH_SECS * G_USEC_PER_SEC)
    return;

  if (refresh_and_resolve (self, FALSE))
    sample_procfs (self);
}


static gboolean
tick (gpointer data)
{
  Sampler *self = data;
  int64_t now_us = g_get_monotonic_time ();

  self->misses = 0;
  record_interval (self);

  if (now_us - self->last_flush_us >= (int64_t) self->flush_interval * G_USEC_PER_SEC)
    flush (self, TRUE);

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
 * prev_proc gains an entry for every PID we have ever seen, and PIDs churn,
 * so on a desktop left running for weeks — which is exactly this daemon's
 * job — it would grow without bound. (The cgroup tracker forgets a cgroup on
 * the read that no longer finds it.)
 *
 * We only ever look up keys we have just observed, so forgetting a dead one
 * costs nothing.
 */
static gboolean
prune (gpointer data)
{
  Sampler *self = data;
  g_autoptr (GPtrArray) procs = gsm_proc_io_collect ();
  g_autoptr (GHashTable) live_procs =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < procs->len; i++) {
    GsmProcIoEntry *entry = g_ptr_array_index (procs, i);

    g_hash_table_add (live_procs, g_strdup_printf ("%d", entry->pid));
  }

  prune_table (self->prev_proc, live_procs);

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

  self.flush_interval = DEFAULT_FLUSH_INTERVAL_SECS;
  interval_env = g_getenv ("GSM_HEALTH_FLUSH_INTERVAL");
  if (interval_env != NULL) {
    int parsed = atoi (interval_env);

    if (parsed >= 5 && parsed <= 86400)
      self.flush_interval = (guint) parsed;
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

  self.tracker = gsm_blame_tracker_new (NULL);
  self.hold = gsm_blame_hold_new ();
  self.prev_proc = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self.device_to_drive = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);
  self.unknown_devices = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, NULL);

  self.loop = g_main_loop_new (NULL, FALSE);

  g_unix_signal_add (SIGTERM, on_terminate, &self);
  g_unix_signal_add (SIGINT, on_terminate, &self);

  /* Prime the deltas: the first tick would otherwise attribute every byte
   * each cgroup and process has written since it started as if it happened
   * just now, putting a huge false spike at the moment tracking was switched
   * on. The tracker's first update only sets its baseline. */
  refresh_drives (&self, TRUE);
  {
    g_autoptr (GPtrArray) baseline = gsm_blame_tracker_update (self.tracker);
    g_autoptr (GPtrArray) procs = gsm_proc_io_collect ();

    for (guint i = 0; i < procs->len; i++) {
      GsmProcIoEntry *entry = g_ptr_array_index (procs, i);
      g_autofree char *key = g_strdup_printf ("%d", entry->pid);

      store_value (self.prev_proc, key, entry->wbytes);
    }
  }
  flush (&self, FALSE);

  g_timeout_add_seconds (self.interval, tick, &self);
  g_timeout_add_seconds (600, prune, &self);

  g_debug ("sampler: running, every %us", self.interval);
  g_main_loop_run (self.loop);

  /* SIGTERM (logout, shutdown): read the counters one last time and write
   * what has accumulated, so a clean stop loses nothing. */
  self.misses = 0;
  record_interval (&self);
  flush (&self, FALSE);

  g_clear_pointer (&self.loop, g_main_loop_unref);
  g_clear_pointer (&self.tracker, gsm_blame_tracker_free);
  g_clear_pointer (&self.hold, gsm_blame_hold_free);
  g_clear_pointer (&self.prev_proc, g_hash_table_unref);
  g_clear_pointer (&self.device_to_drive, g_hash_table_unref);
  g_clear_pointer (&self.unknown_devices, g_hash_table_unref);
  g_clear_object (&self.bus);
  g_clear_object (&self.db);

  return 0;
}
