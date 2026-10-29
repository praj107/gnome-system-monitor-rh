/*
 * gsm-onbehalf — reassign a proxy writer's bytes to whoever caused them.
 *
 * Definitions (all per drive and per interval between two flushes):
 *
 *   charged   the bytes the kernel charged to the proxy writer's cgroup
 *             (io.stat wbytes, Documentation/admin-guide/cgroup-v2.rst).
 *             Rows built here always sum to exactly this, per drive and
 *             writer, by largest-remainder rounding.
 *
 *   systemd-coredump   the handler writes each core through a pipe, so a
 *             crash's cost is its piped core size. Weights are the
 *             estimated core size (see gsm_on_behalf_estimate_core_bytes).
 *             A handler instance whose unit-resources message carries
 *             IO_METRIC_WRITE_BYTES gives that crash's exact figure
 *             (basis coredump-exact, only with IOAccounting=yes); exact
 *             figures are taken first and the remainder goes to the other
 *             crashes by estimate. Originator: COREDUMP_USER_UNIT, else
 *             COREDUMP_UNIT, else the crash's cgroup. A job scope under an
 *             agents-<project>.slice is grouped as "agent jobs: <project>".
 *
 *   systemd-journald   journald's cost per line varies with what it syncs,
 *             so the split is by entry count per originator (basis
 *             journald-lines). A line PID 1 or a user manager wrote about a
 *             unit (UNIT=, USER_UNIT=) counts for that unit; a line from
 *             or about a crash handler instance counts for the crasher;
 *             the rest count for _SYSTEMD_USER_UNIT or _SYSTEMD_UNIT.
 *
 * Where the interval had no usable record (no crash, no entries, or no
 * journal access) the bytes are one explicit "unknown: ..." row, never
 * dropped and never zero.
 *
 * The journal is read through the sd-journal API (sd_journal_open(3)): no
 * subprocess. Journal files are mapped read-only; the user needs the adm or
 * systemd-journal group.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

#include "gsm-blame.h"
#include "gsm-health-db.h"
#include "gsm-onbehalf.h"

#define COREDUMP_SLICE_CGROUP "system.slice/system-systemd\\x2dcoredump.slice"
#define JOURNALD_CGROUP       "system.slice/systemd-journald.service"

/* A core file or maps text larger than this is not believed. */
#define MAX_MAPS_BYTES        (16 * 1024 * 1024)
#define MAX_MAPS_LINE         8192
#define ELF_HEADER_BYTES      4096

#define AGENT_JOBS_PREFIX     "agent jobs: "


const char *
gsm_proxy_writer_name (GsmProxyWriter writer)
{
  switch (writer)
    {
    case GSM_PROXY_COREDUMP:
      return "systemd-coredump@.service";

    case GSM_PROXY_JOURNALD:
      return "systemd-journald.service";

    default:
      return "";
    }
}


static const char *
default_basis (GsmProxyWriter writer)
{
  return writer == GSM_PROXY_COREDUMP ? GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL
                                      : GSM_HEALTH_BASIS_JOURNALD_LINES;
}


static gboolean
is_subtree (const char *path,
            const char *root)
{
  gsize length = strlen (root);

  return strncmp (path, root, length) == 0 &&
         (path[length] == '\0' || path[length] == '/');
}


gboolean
gsm_on_behalf_proxy_for_cgroup (const char     *cgroup,
                                GsmProxyWriter *writer)
{
  g_return_val_if_fail (writer != NULL, FALSE);

  if (cgroup == NULL)
    return FALSE;

  if (is_subtree (cgroup, COREDUMP_SLICE_CGROUP))
    {
      *writer = GSM_PROXY_COREDUMP;
      return TRUE;
    }

  if (is_subtree (cgroup, JOURNALD_CGROUP))
    {
      *writer = GSM_PROXY_JOURNALD;
      return TRUE;
    }

  return FALSE;
}


void
gsm_journal_record_free (GsmJournalRecord *record)
{
  if (record == NULL)
    return;

  g_free (record->message_id);
  g_free (record->unit);
  g_free (record->user_unit);
  g_free (record->about_unit);
  g_free (record->about_user_unit);
  g_free (record->coredump_unit);
  g_free (record->coredump_user_unit);
  g_free (record->coredump_cgroup);
  g_free (record->coredump_proc_maps);
  g_free (record->io_write_bytes);
  g_free (record);
}


void
gsm_journal_source_free (GsmJournalSource *source)
{
  if (source == NULL)
    return;

  if (source->close != NULL)
    source->close (source->data);
  g_free (source);
}


/* ------------------------------------------------------------------ */
/* Estimating a core's size from /proc/<pid>/maps                      */
/* ------------------------------------------------------------------ */

static gboolean
starts_with_any (const char *text,
                 const char *const *prefixes)
{
  for (guint i = 0; prefixes[i] != NULL; i++)
    {
      if (g_str_has_prefix (text, prefixes[i]))
        return TRUE;
    }

  return FALSE;
}


/*
 * One line of proc_pid_maps(5):
 *   start-end perms offset dev inode [path]
 * Returns the bytes of this mapping a default dump contains.
 */
static uint64_t
dumped_bytes_of_line (const char *line)
{
  static const char *const never_dumped[] = { "[vvar", "[vsyscall]", "[vdso]", NULL };
  static const char *const anon_shared_paths[] = { "/dev/zero", "/SYSV", "/memfd:", NULL };
  const char *p = line;
  char *end = NULL;
  uint64_t start;
  uint64_t stop;
  uint64_t offset;
  uint64_t inode;
  char perms[5];

  start = g_ascii_strtoull (p, &end, 16);
  if (end == p || *end != '-')
    return 0;

  p = end + 1;
  stop = g_ascii_strtoull (p, &end, 16);
  if (end == p || *end != ' ' || stop <= start)
    return 0;

  p = end + 1;
  if (strlen (p) < 6 || p[4] != ' ')
    return 0;
  memcpy (perms, p, 4);
  perms[4] = '\0';

  p += 5;
  offset = g_ascii_strtoull (p, &end, 16);
  if (end == p || *end != ' ')
    return 0;

  /* The device field ("00:00"), then the inode. */
  p = strchr (end + 1, ' ');
  if (p == NULL)
    return 0;
  p++;
  inode = g_ascii_strtoull (p, &end, 10);
  if (end == p)
    return 0;

  p = end;
  while (*p == ' ')
    p++;

  if (starts_with_any (p, never_dumped))
    return 0;

  /* coredump_filter bit 0: anonymous private (heap, stack, bare mmap). */
  if (inode == 0 && perms[3] == 'p')
    return stop - start;

  /* Bit 1: anonymous shared, which show up as /dev/zero, SysV and memfd. */
  if (perms[3] == 's' && starts_with_any (p, anon_shared_paths))
    return stop - start;

  /* Bit 4: the ELF headers of a file-backed private mapping. */
  if (perms[3] == 'p' && perms[0] == 'r' && offset == 0 && inode != 0)
    return MIN (stop - start, (uint64_t) ELF_HEADER_BYTES);

  return 0;
}


uint64_t
gsm_on_behalf_estimate_core_bytes (const char *proc_maps)
{
  const char *line = proc_maps;
  uint64_t total = 0;

  if (proc_maps == NULL || strlen (proc_maps) > MAX_MAPS_BYTES)
    return 0;

  while (*line != '\0')
    {
      const char *newline = strchr (line, '\n');
      gsize length = newline != NULL ? (gsize) (newline - line) : strlen (line);

      if (length > 0 && length < MAX_MAPS_LINE)
        {
          char buffer[MAX_MAPS_LINE];

          memcpy (buffer, line, length);
          buffer[length] = '\0';
          total += dumped_bytes_of_line (buffer);
        }

      if (newline == NULL)
        break;
      line = newline + 1;
    }

  return total;
}


/* ------------------------------------------------------------------ */
/* Naming originators                                                  */
/* ------------------------------------------------------------------ */

static char *
decode_dash_escapes (const char *text)
{
  g_auto (GStrv) parts = g_strsplit (text, "\\x2d", -1);

  return g_strjoinv ("-", parts);
}


/* job-<project>-<pid>.scope, the scope an agent job runs in. */
static char *
agent_project_from_unit (const char *unit)
{
  g_autofree char *stem = NULL;
  const char *dash;

  if (unit == NULL || !g_str_has_prefix (unit, "job-") || !g_str_has_suffix (unit, ".scope"))
    return NULL;

  stem = g_strndup (unit + 4, strlen (unit) - 4 - strlen (".scope"));
  dash = strrchr (stem, '-');
  if (dash == NULL || dash == stem || dash[1] == '\0')
    return NULL;

  for (const char *c = dash + 1; *c != '\0'; c++)
    {
      if (!g_ascii_isdigit (*c))
        return NULL;
    }

  {
    g_autofree char *project = g_strndup (stem, (gsize) (dash - stem));

    return decode_dash_escapes (project);
  }
}


/* A job scope under agents-<project>.slice in a cgroup path. */
static char *
agent_project_from_cgroup (const char *cgroup)
{
  g_auto (GStrv) parts = NULL;
  const char *project_slice = NULL;
  const char *leaf = NULL;

  if (cgroup == NULL)
    return NULL;

  parts = g_strsplit (cgroup, "/", -1);
  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (g_str_has_prefix (parts[i], "agents-") && g_str_has_suffix (parts[i], ".slice"))
        project_slice = parts[i];
      if (parts[i][0] != '\0')
        leaf = parts[i];
    }

  if (project_slice == NULL || leaf == NULL || !g_str_has_prefix (leaf, "job-"))
    return NULL;

  {
    g_autofree char *project = g_strndup (project_slice + strlen ("agents-"),
                                          strlen (project_slice) - strlen ("agents-") -
                                          strlen (".slice"));

    return decode_dash_escapes (project);
  }
}


static char *
agent_jobs_name (char *project)
{
  char *name;

  if (project == NULL)
    return NULL;

  name = g_strconcat (AGENT_JOBS_PREFIX, project, NULL);
  g_free (project);

  return name;
}


static char *
originator_for_unit (const char *unit)
{
  char *agent = agent_jobs_name (agent_project_from_unit (unit));

  return agent != NULL ? agent : gsm_blame_normalize_name (unit);
}


static gboolean
present (const char *text)
{
  return text != NULL && text[0] != '\0';
}


static char *
crash_originator (const GsmJournalRecord *record)
{
  char *agent = agent_jobs_name (agent_project_from_cgroup (record->coredump_cgroup));

  if (agent != NULL)
    return agent;

  if (present (record->coredump_user_unit))
    return originator_for_unit (record->coredump_user_unit);

  if (present (record->coredump_unit))
    return originator_for_unit (record->coredump_unit);

  if (present (record->coredump_cgroup))
    return gsm_blame_node_name (record->coredump_cgroup + (record->coredump_cgroup[0] == '/'));

  return g_strdup ("unknown: crash record names no unit");
}


/* ------------------------------------------------------------------ */
/* Splitting exactly                                                   */
/* ------------------------------------------------------------------ */

/*
 * Split `total` over `weights` in proportion, giving each its floor share
 * and the leftover units to the largest remainders, so the parts add up to
 * `total` exactly. All weights zero gives all zeros.
 */
static void
distribute (uint64_t        total,
            const uint64_t *weights,
            guint           count,
            uint64_t       *shares)
{
  unsigned __int128 sum = 0;
  g_autofree unsigned __int128 *remainder = g_new0 (unsigned __int128, count);
  uint64_t given = 0;

  for (guint i = 0; i < count; i++)
    {
      sum += weights[i];
      shares[i] = 0;
    }

  if (sum == 0)
    return;

  for (guint i = 0; i < count; i++)
    {
      unsigned __int128 product = (unsigned __int128) total * weights[i];

      shares[i] = (uint64_t) (product / sum);
      remainder[i] = product % sum;
      given += shares[i];
    }

  for (uint64_t left = total - given; left > 0; left--)
    {
      guint best = count;

      for (guint i = 0; i < count; i++)
        {
          if (weights[i] != 0 && (best == count || remainder[i] > remainder[best]))
            best = i;
        }

      shares[best]++;
      remainder[best] = 0;
    }
}


typedef struct
{
  char    *originator;
  char    *instance;          /* the handler instance's unit name */
  uint64_t estimate_bytes;
  gboolean exact_known;
  uint64_t exact_bytes;
} Crash;


static void
crash_free (Crash *crash)
{
  g_free (crash->originator);
  g_free (crash->instance);
  g_free (crash);
}


typedef struct
{
  GPtrArray  *crashes;                /* Crash* */
  GHashTable *crash_by_instance;      /* instance -> Crash* (not owned) */
  GHashTable *exact_by_instance;      /* instance -> uint64_t* */
  GHashTable *line_counts;            /* originator -> uint64_t* */
  GPtrArray  *line_units;             /* unit names to resolve once all crashes are known */
  uint64_t    lines_without_unit;
} Interval;


static void
interval_clear (Interval *iv)
{
  g_clear_pointer (&iv->crashes, g_ptr_array_unref);
  g_clear_pointer (&iv->crash_by_instance, g_hash_table_unref);
  g_clear_pointer (&iv->exact_by_instance, g_hash_table_unref);
  g_clear_pointer (&iv->line_counts, g_hash_table_unref);
  g_clear_pointer (&iv->line_units, g_ptr_array_unref);
}


static uint64_t *
box (uint64_t value)
{
  uint64_t *slot = g_new (uint64_t, 1);

  *slot = value;
  return slot;
}


static void
count_line (Interval   *iv,
            const char *originator)
{
  uint64_t *slot = g_hash_table_lookup (iv->line_counts, originator);

  if (slot == NULL)
    g_hash_table_insert (iv->line_counts, g_strdup (originator), box (1));
  else
    (*slot)++;
}


static const char *
unit_a_line_is_about (const GsmJournalRecord *record)
{
  if (present (record->about_user_unit))
    return record->about_user_unit;
  if (present (record->about_unit))
    return record->about_unit;
  if (present (record->user_unit))
    return record->user_unit;
  if (present (record->unit))
    return record->unit;

  return NULL;
}


static void
read_interval (Interval         *iv,
               GsmJournalSource *source)
{
  GsmJournalRecord *record;

  iv->crashes = g_ptr_array_new_with_free_func ((GDestroyNotify) crash_free);
  iv->crash_by_instance = g_hash_table_new (g_str_hash, g_str_equal);
  iv->exact_by_instance = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  iv->line_counts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  iv->line_units = g_ptr_array_new_with_free_func (g_free);

  while ((record = source->next (source->data)) != NULL)
    {
      const char *about = unit_a_line_is_about (record);

      if (g_strcmp0 (record->message_id, GSM_MESSAGE_ID_COREDUMP) == 0)
        {
          Crash *crash = g_new0 (Crash, 1);

          crash->originator = crash_originator (record);
          crash->instance = g_strdup (record->unit);
          crash->estimate_bytes = gsm_on_behalf_estimate_core_bytes (record->coredump_proc_maps);
          g_ptr_array_add (iv->crashes, crash);
          if (crash->instance != NULL)
            g_hash_table_replace (iv->crash_by_instance, crash->instance, crash);
        }
      else if (g_strcmp0 (record->message_id, GSM_MESSAGE_ID_RESOURCES) == 0 &&
               present (record->about_unit) && present (record->io_write_bytes))
        {
          char *tail = NULL;
          uint64_t bytes = g_ascii_strtoull (record->io_write_bytes, &tail, 10);

          if (tail != record->io_write_bytes && *tail == '\0')
            g_hash_table_replace (iv->exact_by_instance, g_strdup (record->about_unit), box (bytes));
        }

      if (about != NULL)
        g_ptr_array_add (iv->line_units, g_strdup (about));
      else
        iv->lines_without_unit++;

      gsm_journal_record_free (record);
    }

  for (guint i = 0; i < iv->crashes->len; i++)
    {
      Crash *crash = g_ptr_array_index (iv->crashes, i);
      uint64_t *exact = crash->instance != NULL
                        ? g_hash_table_lookup (iv->exact_by_instance, crash->instance) : NULL;

      if (exact != NULL)
        {
          crash->exact_known = TRUE;
          crash->exact_bytes = *exact;
        }
    }

  /* A line from or about a handler instance counts for the crasher, which
   * is only known once every crash record has been seen. */
  for (guint i = 0; i < iv->line_units->len; i++)
    {
      const char *unit = g_ptr_array_index (iv->line_units, i);
      Crash *crash = g_hash_table_lookup (iv->crash_by_instance, unit);
      g_autofree char *name = crash != NULL ? g_strdup (crash->originator)
                                            : originator_for_unit (unit);

      count_line (iv, name);
    }

  if (iv->lines_without_unit > 0)
    {
      uint64_t *slot = box (iv->lines_without_unit);

      g_hash_table_insert (iv->line_counts, g_strdup (GSM_ON_BEHALF_NO_UNIT), slot);
    }
}


/* ------------------------------------------------------------------ */
/* The accumulator                                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
  uint64_t bytes[GSM_PROXY_COUNT];
} Charges;

struct _GsmOnBehalf
{
  GHashTable *charges;   /* drive id -> Charges* */
};


GsmOnBehalf *
gsm_on_behalf_new (void)
{
  GsmOnBehalf *self = g_new0 (GsmOnBehalf, 1);

  self->charges = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  return self;
}


void
gsm_on_behalf_free (GsmOnBehalf *self)
{
  if (self == NULL)
    return;

  g_hash_table_unref (self->charges);
  g_free (self);
}


void
gsm_on_behalf_charge (GsmOnBehalf    *self,
                      const char     *drive_id,
                      GsmProxyWriter  writer,
                      uint64_t        wbytes)
{
  Charges *charges;

  g_return_if_fail (self != NULL && drive_id != NULL);
  g_return_if_fail (writer < GSM_PROXY_COUNT);

  if (wbytes == 0)
    return;

  charges = g_hash_table_lookup (self->charges, drive_id);
  if (charges == NULL)
    {
      charges = g_new0 (Charges, 1);
      g_hash_table_insert (self->charges, g_strdup (drive_id), charges);
    }

  charges->bytes[writer] += wbytes;
}


gboolean
gsm_on_behalf_has_charges (GsmOnBehalf *self)
{
  g_return_val_if_fail (self != NULL, FALSE);

  return g_hash_table_size (self->charges) > 0;
}


/* Rows of one (drive, writer): "<name>\n<basis>" -> uint64_t*. */
static void
add_share (GHashTable *rows,
           const char *name,
           const char *basis,
           uint64_t    wbytes)
{
  g_autofree char *key = NULL;
  uint64_t *slot;

  if (wbytes == 0)
    return;

  key = g_strdup_printf ("%s\n%s", name, basis);
  slot = g_hash_table_lookup (rows, key);
  if (slot == NULL)
    g_hash_table_insert (rows, g_steal_pointer (&key), box (wbytes));
  else
    *slot += wbytes;
}


static void
split_coredump (const Interval *iv,
                uint64_t        charged,
                GHashTable     *rows)
{
  guint count = iv->crashes->len;
  g_autofree uint64_t *weights = g_new0 (uint64_t, count + 1);
  g_autofree uint64_t *shares = g_new0 (uint64_t, count + 1);
  uint64_t exact_total = 0;
  uint64_t estimate_total = 0;
  uint64_t remainder;

  for (guint i = 0; i < count; i++)
    {
      const Crash *crash = g_ptr_array_index (iv->crashes, i);

      if (crash->exact_known)
        exact_total += crash->exact_bytes;
    }

  if (exact_total >= charged && exact_total > 0)
    {
      /* More claimed than charged: scale the exact figures down to fit. */
      for (guint i = 0; i < count; i++)
        {
          const Crash *crash = g_ptr_array_index (iv->crashes, i);

          weights[i] = crash->exact_known ? crash->exact_bytes : 0;
        }
      distribute (charged, weights, count, shares);
      for (guint i = 0; i < count; i++)
        add_share (rows, ((Crash *) g_ptr_array_index (iv->crashes, i))->originator,
                   GSM_HEALTH_BASIS_COREDUMP_EXACT, shares[i]);
      return;
    }

  for (guint i = 0; i < count; i++)
    {
      const Crash *crash = g_ptr_array_index (iv->crashes, i);

      if (crash->exact_known)
        add_share (rows, crash->originator, GSM_HEALTH_BASIS_COREDUMP_EXACT, crash->exact_bytes);
      else
        {
          weights[i] = crash->estimate_bytes;
          estimate_total += crash->estimate_bytes;
        }
    }

  remainder = charged - exact_total;
  if (estimate_total == 0)
    {
      add_share (rows, GSM_ON_BEHALF_NO_CRASH_RECORD,
                 GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL, remainder);
      return;
    }

  distribute (remainder, weights, count, shares);
  for (guint i = 0; i < count; i++)
    add_share (rows, ((Crash *) g_ptr_array_index (iv->crashes, i))->originator,
               GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL, shares[i]);
}


static void
split_journald (const Interval *iv,
                uint64_t        charged,
                GHashTable     *rows)
{
  guint count = g_hash_table_size (iv->line_counts);
  g_autofree uint64_t *weights = g_new0 (uint64_t, count + 1);
  g_autofree uint64_t *shares = g_new0 (uint64_t, count + 1);
  g_autofree const char **names = g_new0 (const char *, count + 1);
  GHashTableIter iter;
  gpointer key;
  gpointer value;
  guint n = 0;

  g_hash_table_iter_init (&iter, iv->line_counts);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      names[n] = key;
      weights[n] = *(uint64_t *) value;
      n++;
    }

  if (n == 0)
    {
      add_share (rows, GSM_ON_BEHALF_NO_JOURNAL_ENTRY, GSM_HEALTH_BASIS_JOURNALD_LINES, charged);
      return;
    }

  distribute (charged, weights, n, shares);
  for (guint i = 0; i < n; i++)
    add_share (rows, names[i], GSM_HEALTH_BASIS_JOURNALD_LINES, shares[i]);
}


static int
compare_keys (gconstpointer a,
              gconstpointer b)
{
  return strcmp (*(const char *const *) a, *(const char *const *) b);
}


static void
emit_rows (GHashTable      *rows,
           const char      *drive_id,
           GsmProxyWriter   writer,
           GsmOnBehalfSink  sink,
           gpointer         user_data)
{
  g_autoptr (GPtrArray) keys = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;

  g_hash_table_iter_init (&iter, rows);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (keys, key);
  g_ptr_array_sort (keys, compare_keys);

  for (guint i = 0; i < keys->len; i++)
    {
      const char *text = g_ptr_array_index (keys, i);
      const char *newline = strchr (text, '\n');
      g_autofree char *name = g_strndup (text, (gsize) (newline - text));

      sink (drive_id, name, gsm_proxy_writer_name (writer), newline + 1,
            *(uint64_t *) g_hash_table_lookup (rows, text), user_data);
    }
}


void
gsm_on_behalf_settle (GsmOnBehalf      *self,
                      GsmJournalSource *source,
                      GsmOnBehalfSink   sink,
                      gpointer          user_data)
{
  Interval iv = { 0 };
  GHashTableIter iter;
  gpointer drive_id;
  gpointer value;

  g_return_if_fail (self != NULL && sink != NULL);

  if (!gsm_on_behalf_has_charges (self))
    return;

  if (source != NULL)
    read_interval (&iv, source);

  g_hash_table_iter_init (&iter, self->charges);
  while (g_hash_table_iter_next (&iter, &drive_id, &value))
    {
      const Charges *charges = value;

      for (GsmProxyWriter writer = 0; writer < GSM_PROXY_COUNT; writer++)
        {
          g_autoptr (GHashTable) rows = NULL;

          if (charges->bytes[writer] == 0)
            continue;

          rows = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

          if (source == NULL)
            add_share (rows, GSM_ON_BEHALF_NO_ACCESS, default_basis (writer), charges->bytes[writer]);
          else if (writer == GSM_PROXY_COREDUMP)
            split_coredump (&iv, charges->bytes[writer], rows);
          else
            split_journald (&iv, charges->bytes[writer], rows);

          emit_rows (rows, drive_id, writer, sink, user_data);
        }
    }

  interval_clear (&iv);
  g_hash_table_remove_all (self->charges);
}


/* ------------------------------------------------------------------ */
/* The sd-journal reader                                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_SYSTEMD

/* A crash record carries the whole of /proc/<pid>/maps, which can run to
 * hundreds of kilobytes for a process with many mappings. */
#define DATA_THRESHOLD_BYTES (8 * 1024 * 1024)

typedef struct
{
  sd_journal *journal;
  char       *after_cursor;
  int64_t     since_us;
  gboolean    baseline;       /* nothing to return, only an end cursor */
  gboolean    seeked;
  gboolean    exhausted;
  char       *last_cursor;
} SdState;


static char *
field_of (sd_journal *journal,
          const char *name)
{
  const void *data = NULL;
  size_t length = 0;
  gsize name_length = strlen (name);

  if (sd_journal_get_data (journal, name, &data, &length) < 0 || length <= name_length + 1)
    return NULL;

  return g_strndup ((const char *) data + name_length + 1, length - name_length - 1);
}


static gboolean
step_to_first (SdState *state)
{
  int r;

  state->seeked = TRUE;

  if (state->after_cursor != NULL)
    {
      if (sd_journal_seek_cursor (state->journal, state->after_cursor) < 0)
        return FALSE;
      r = sd_journal_next (state->journal);
      if (r > 0 && sd_journal_test_cursor (state->journal, state->after_cursor) > 0)
        r = sd_journal_next (state->journal);
      return r > 0;
    }

  if (sd_journal_seek_realtime_usec (state->journal, (uint64_t) state->since_us) < 0)
    return FALSE;

  return sd_journal_next (state->journal) > 0;
}


static GsmJournalRecord *
sd_next (gpointer data)
{
  SdState *state = data;
  GsmJournalRecord *record;
  gboolean got;

  if (state->baseline || state->exhausted)
    return NULL;

  got = state->seeked ? sd_journal_next (state->journal) > 0 : step_to_first (state);
  if (!got)
    {
      state->exhausted = TRUE;
      return NULL;
    }

  record = g_new0 (GsmJournalRecord, 1);
  record->message_id = field_of (state->journal, "MESSAGE_ID");
  record->unit = field_of (state->journal, "_SYSTEMD_UNIT");
  record->user_unit = field_of (state->journal, "_SYSTEMD_USER_UNIT");
  record->about_unit = field_of (state->journal, "UNIT");
  record->about_user_unit = field_of (state->journal, "USER_UNIT");

  if (g_strcmp0 (record->message_id, GSM_MESSAGE_ID_COREDUMP) == 0)
    {
      record->coredump_unit = field_of (state->journal, "COREDUMP_UNIT");
      record->coredump_user_unit = field_of (state->journal, "COREDUMP_USER_UNIT");
      record->coredump_cgroup = field_of (state->journal, "COREDUMP_CGROUP");
      record->coredump_proc_maps = field_of (state->journal, "COREDUMP_PROC_MAPS");
    }
  else if (g_strcmp0 (record->message_id, GSM_MESSAGE_ID_RESOURCES) == 0)
    record->io_write_bytes = field_of (state->journal, "IO_METRIC_WRITE_BYTES");

  g_clear_pointer (&state->last_cursor, free);
  sd_journal_get_cursor (state->journal, &state->last_cursor);

  return record;
}


static char *
sd_end_cursor (gpointer data)
{
  SdState *state = data;
  char *cursor = NULL;

  /* After reading to the end, resume right after the last record read: an
   * entry appended since is then counted in the next interval, not lost. */
  if (state->exhausted && state->seeked)
    {
      if (state->last_cursor != NULL)
        return g_strdup (state->last_cursor);
      if (state->after_cursor != NULL)
        return g_strdup (state->after_cursor);
    }

  if (sd_journal_seek_tail (state->journal) < 0 || sd_journal_previous (state->journal) <= 0 ||
      sd_journal_get_cursor (state->journal, &cursor) < 0)
    return g_strdup (state->after_cursor);

  {
    char *copy = g_strdup (cursor);

    free (cursor);
    return copy;
  }
}


static void
sd_close (gpointer data)
{
  SdState *state = data;

  sd_journal_close (state->journal);
  free (state->last_cursor);
  g_free (state->after_cursor);
  g_free (state);
}

#endif /* HAVE_SYSTEMD */


GsmJournalSource *
gsm_journal_open_system (const char *after_cursor,
                         int64_t     since_us,
                         char      **reason)
{
#ifdef HAVE_SYSTEMD
  sd_journal *journal = NULL;
  uint64_t usage = 0;
  SdState *state;
  GsmJournalSource *source;
  int r;

  r = sd_journal_open (&journal, SD_JOURNAL_LOCAL_ONLY);
  if (r < 0)
    {
      if (reason != NULL)
        *reason = g_strdup_printf ("cannot open the journal: %s", g_strerror (-r));
      return NULL;
    }

  /* Opening succeeds even when no file is readable; a journal that holds
   * bytes is the sign that this user can see it. */
  if (sd_journal_get_usage (journal, &usage) < 0 || usage == 0)
    {
      sd_journal_close (journal);
      if (reason != NULL)
        *reason = g_strdup ("no readable journal files (the user needs the adm or "
                            "systemd-journal group)");
      return NULL;
    }

  sd_journal_set_data_threshold (journal, DATA_THRESHOLD_BYTES);

  state = g_new0 (SdState, 1);
  state->journal = journal;
  state->after_cursor = g_strdup (after_cursor);
  state->since_us = since_us;
  state->baseline = after_cursor == NULL && since_us <= 0;

  source = g_new0 (GsmJournalSource, 1);
  source->next = sd_next;
  source->end_cursor = sd_end_cursor;
  source->close = sd_close;
  source->data = state;

  return source;
#else
  (void) after_cursor;
  (void) since_us;

  if (reason != NULL)
    *reason = g_strdup ("built without systemd support");
  return NULL;
#endif
}
