/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See procsource.h for the definitions.
 *
 * The selection follows libgtop 2.41.3 (sysdeps/linux/proclist.c): the
 * "active" list keeps the processes whose state letter is R
 * (GLIBTOP_EXCLUDE_IDLE skips every process not GLIBTOP_PROCESS_RUNNING),
 * and the "user" list keeps the processes whose /proc/PID directory belongs
 * to the uid. That directory belongs to the effective uid (root when the
 * process is not dumpable); the collector reads the real uid from
 * /proc/PID/status, which is the same for an ordinary process.
 */

#include "config.h"

#include "procsource.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <glibtop/procstate.h>

namespace
{

/* libgtop's mapping of the state letter of proc_pid_stat(5); a letter it does
 * not name (t, I, K, P ...) is 0, shown as "Sleeping" like the libgtop path. */
guint
status_from_state_letter (uint8_t state)
{
  switch (state)
    {
      case 'R': return GLIBTOP_PROCESS_RUNNING;
      case 'S': return GLIBTOP_PROCESS_INTERRUPTIBLE;
      case 'D': return GLIBTOP_PROCESS_UNINTERRUPTIBLE;
      case 'T': return GLIBTOP_PROCESS_STOPPED;
      case 'Z': return GLIBTOP_PROCESS_ZOMBIE;
      case 'W': return GLIBTOP_PROCESS_SWAPPING;
      case 'X': return GLIBTOP_PROCESS_DEAD;
      default: return 0;
    }
}

bool
has (const gsm_process *record,
     guint64            bits)
{
  return (record->known & bits) == bits;
}

/* write_bytes counts pages the process dirtied; cancelled_write_bytes those
 * it then truncated before they reached storage. The kernel may credit a
 * process with more cancellation than it wrote (it truncated pages another
 * process dirtied), so the difference is floored at 0. */
guint64
written_bytes (const gsm_process *record)
{
  if (record->io_cancelled_write_bytes >= record->io_write_bytes)
    return 0;
  return record->io_write_bytes - record->io_cancelled_write_bytes;
}

void
fill_memory (const gsm_process *record,
             GsmProcFigures    *figures)
{
  const bool anon = has (record, GSM_PROCESS_HAS_RSS_ANON);
  const bool file = has (record, GSM_PROCESS_HAS_RSS_FILE);
  const bool shmem = has (record, GSM_PROCESS_HAS_RSS_SHMEM);

  figures->vmsize_known = has (record, GSM_PROCESS_HAS_VM_SIZE);
  figures->mem_known = anon;
  figures->memshared_known = file && shmem;
  figures->memres_known = anon && file && shmem;

  if (figures->vmsize_known)
    figures->vmsize_bytes = record->vm_size_bytes;
  if (figures->mem_known)
    figures->mem_bytes = record->rss_anon_bytes;
  if (figures->memshared_known)
    figures->memshared_bytes = record->rss_file_bytes + record->rss_shmem_bytes;
  if (figures->memres_known)
    figures->memres_bytes = record->rss_anon_bytes + record->rss_file_bytes + record->rss_shmem_bytes;
}

void
fill_cpu (const GsmProcWindow *window,
          const gsm_process   *record,
          GsmProcFigures      *figures)
{
  const gsm_process *previous = window->previous;
  double ticks_per_s;

  /* utime + stime of /proc/PID/stat count every thread of the group, which
   * the scheduler statistics of /proc/PID/schedstat do not: those are the
   * thread-group leader's alone (sched-stats.rst). The ticks are part of the
   * key file of a record, so they are always present; what can be missing is
   * a baseline or the tick rate. */
  if (previous == NULL || previous->start_ticks != record->start_ticks)
    return;
  if (window->clock_ticks_per_s <= 0)
    return;
  if (!gsm_rate_per_s (previous->utime_ticks + previous->stime_ticks,
                       record->utime_ticks + record->stime_ticks,
                       window->previous_at_ns, window->current_at_ns, &ticks_per_s))
    return;

  /* CPU seconds per s of wall time: 1 is one whole CPU, i.e. 100 %. */
  double percent = ticks_per_s / (double) window->clock_ticks_per_s * 100.0;
  double cap = 100.0 * window->cpu_count;

  if (window->solaris_mode)
    {
      percent /= window->cpu_count;
      cap = 100.0;
    }

  figures->cpu_known = TRUE;
  figures->cpu_percent = std::min (percent, cap);
}

void
fill_io (const GsmProcWindow *window,
         const gsm_process   *record,
         GsmProcFigures      *figures)
{
  const gsm_process *previous = window->previous;
  double read_per_s, write_per_s;

  if (!has (record, GSM_PROCESS_HAS_IO))
    return;

  figures->io_readable = TRUE;
  figures->disk_read_bytes = record->io_read_bytes;
  figures->disk_write_bytes = written_bytes (record);

  if (previous == NULL || previous->start_ticks != record->start_ticks
      || !has (previous, GSM_PROCESS_HAS_IO))
    return;

  if (!gsm_rate_per_s (previous->io_read_bytes, record->io_read_bytes,
                       window->previous_at_ns, window->current_at_ns, &read_per_s)
      || !gsm_rate_per_s (written_bytes (previous), written_bytes (record),
                          window->previous_at_ns, window->current_at_ns, &write_per_s))
    return;

  figures->io_rate_valid = TRUE;
  figures->disk_read_bytes_per_s = (guint64) read_per_s;
  figures->disk_write_bytes_per_s = (guint64) write_per_s;
}

/* A slice of the snapshot's string table, with the `\xNN` escapes of a
 * cgroup directory name decoded. Empty when the slice is empty or outside the
 * table. */
std::string
decoded_slice (const GsmProcWindow *window,
               guint32              offset,
               guint32              length)
{
  if (length == 0 || (gsize) offset + length > window->strings_len)
    return std::string ();

  /* Decoding only shortens, so length + 1 always holds the result. */
  std::string buffer (length + 1, '\0');
  const long decoded = gsm_unescape_name (window->strings + offset, length,
                                          &buffer[0], buffer.size ());

  if (decoded < 0)
    return std::string ();
  buffer.resize ((size_t) decoded);
  return buffer;
}

std::string
class_tooltip_text (const gsm_process *record,
                    const std::string &key)
{
  const char *rule = gsm_class_rule_name (record->class_rule);
  std::string text = std::string ("Rule: ") + rule
                     + " \u00b7 Confidence: " + gsm_confidence_name (record->class_confidence)
                     + " \u00b7 Key: " + (key.empty () ? "none" : key);

  if (has (record, GSM_PROCESS_HAS_AGENT) && strcmp (rule, "agent ancestry") == 0)
    text += " \u00b7 Agent pid: " + std::to_string (record->agent_pid);
  return text;
}

std::string
unit_tooltip_text (const GsmProcLabels &labels)
{
  std::string text = "Unit: " + (labels.shown_unit.empty () ? std::string ("none") : labels.shown_unit);

  if (!labels.user_unit.empty () && !labels.unit.empty ())
    text += "\nOwned by: " + labels.unit;
  if (!labels.session.empty ())
    text += "\nSession: " + labels.session;
  return text;
}

}  /* namespace */


void
gsm_proc_labels_from_record (const GsmProcWindow *window,
                             const gsm_process   *record,
                             GsmProcLabels       *labels)
{
  *labels = GsmProcLabels ();

  if (has (record, GSM_PROCESS_HAS_CLASS))
    {
      labels->class_known = true;
      labels->workload_class = record->workload_class;
      labels->class_name = gsm_class_name (record->workload_class);
      labels->class_rule = gsm_class_rule_name (record->class_rule);
      labels->class_tooltip = class_tooltip_text (record,
                                                  decoded_slice (window, record->class_key_offset,
                                                                 record->class_key_length));
    }

  if (has (record, GSM_PROCESS_HAS_UNIT))
    labels->unit = decoded_slice (window, record->unit_offset, record->unit_length);
  if (has (record, GSM_PROCESS_HAS_USER_UNIT))
    labels->user_unit = decoded_slice (window, record->user_unit_offset, record->user_unit_length);
  if (has (record, GSM_PROCESS_HAS_SESSION))
    labels->session = decoded_slice (window, record->login_session_offset,
                                     record->login_session_length);
  labels->shown_unit = labels->user_unit.empty () ? labels->unit : labels->user_unit;
  labels->unit_tooltip = unit_tooltip_text (*labels);

  labels->owner_known = has (record, GSM_PROCESS_HAS_OWNER_UID);
  labels->owner_uid = labels->owner_known ? record->owner_uid : 0;

  labels->swap_known = has (record, GSM_PROCESS_HAS_VM_SWAP);
  labels->swap_bytes = labels->swap_known ? record->vm_swap_bytes : 0;
}


void
gsm_proc_figures_from_record (const GsmProcWindow *window,
                              const gsm_process   *record,
                              GsmProcFigures      *figures)
{
  *figures = GsmProcFigures ();

  figures->pid = (pid_t) record->pid;
  figures->ppid = (pid_t) record->ppid;
  figures->status = status_from_state_letter (record->state);
  figures->uid_known = has (record, GSM_PROCESS_HAS_UID);
  figures->uid = figures->uid_known ? record->uid_real : 0;
  figures->nice = record->nice;
  figures->num_threads = record->num_threads;
  figures->cpu_time_ticks = record->utime_ticks + record->stime_ticks;

  if (window->boot_time_s >= 0 && window->clock_ticks_per_s > 0)
    {
      figures->start_time_known = TRUE;
      figures->start_time_s = (guint64) window->boot_time_s
                              + record->start_ticks / (guint64) window->clock_ticks_per_s;
    }

  if (has (record, GSM_PROCESS_HAS_CGROUP)
      && (gsize) record->cgroup_offset + record->cgroup_len <= window->strings_len)
    {
      figures->cgroup_path = window->strings + record->cgroup_offset;
      figures->cgroup_path_len = record->cgroup_len;
    }

  figures->record = record;
  figures->strings = window->strings;
  figures->strings_len = window->strings_len;

  figures->swap_known = has (record, GSM_PROCESS_HAS_VM_SWAP);
  if (figures->swap_known)
    figures->swap_bytes = record->vm_swap_bytes;

  fill_memory (record, figures);
  fill_cpu (window, record, figures);
  fill_io (window, record, figures);
}


gboolean
gsm_proc_source_selects (GsmProcWhose       whose,
                         uid_t              uid,
                         const gsm_process *record)
{
  switch (whose)
    {
      case GSM_PROC_WHOSE_ALL:
        return TRUE;

      case GSM_PROC_WHOSE_ACTIVE:
        return record->state == 'R';

      case GSM_PROC_WHOSE_USER:
        return has (record, GSM_PROCESS_HAS_UID) && record->uid_real == uid;
    }
  return FALSE;
}


struct GsmProcSource
{
  gsm_collector *collector = NULL;
  gsm_snapshot *current = NULL;
  gsm_snapshot *previous = NULL;
  /* pid -> its record in `previous`, rebuilt when a sample arrives */
  std::unordered_map<uint32_t, const gsm_process *> previous_by_pid;
  gint64 boot_time_s = -1;
  glong clock_ticks_per_s = 100;
};

static gint64
read_boot_time_s (const char *procfs_root)
{
  g_autofree char *path = g_build_filename (procfs_root, "stat", NULL);
  g_autofree char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return -1;

  /* proc_stat(5): "btime 1700000000" on a line of its own */
  const char *line = g_str_has_prefix (text, "btime ") ? text : strstr (text, "\nbtime ");

  if (line == NULL)
    return -1;
  line += (line == text) ? strlen ("btime ") : strlen ("\nbtime ");

  char *end = NULL;

  errno = 0;
  const gint64 seconds = g_ascii_strtoll (line, &end, 10);
  if (errno != 0 || end == line || seconds < 0)
    return -1;
  return seconds;
}

GsmProcSource *
gsm_proc_source_new (const char *procfs_root)
{
  gsm_collect_config config = {};

  config.struct_size = sizeof (config);
  config.api_version = GSM_COLLECT_API_VERSION;
  config.readers = GSM_READ_PROCESSES;
  config.procfs_root = procfs_root;

  GsmProcSource *source = new GsmProcSource ();

  if (gsm_collect_open (&config, &source->collector) != 0)
    {
      delete source;
      return NULL;
    }

  source->boot_time_s = read_boot_time_s (procfs_root != NULL ? procfs_root : "/proc");
  source->clock_ticks_per_s = sysconf (_SC_CLK_TCK);
  return source;
}

void
gsm_proc_source_free (GsmProcSource *source)
{
  if (source == NULL)
    return;
  gsm_snapshot_free (source->current);
  gsm_snapshot_free (source->previous);
  gsm_collect_free (source->collector);
  delete source;
}

gboolean
gsm_proc_source_sample (GsmProcSource *source)
{
  gsm_snapshot *snapshot = NULL;
  size_t count = 0;

  g_return_val_if_fail (source != NULL, FALSE);

  if (gsm_collect_sample (source->collector, &snapshot) != 0)
    return FALSE;

  gsm_snapshot_free (source->previous);
  source->previous = source->current;
  source->current = snapshot;

  source->previous_by_pid.clear ();
  if (source->previous != NULL)
    {
      const gsm_process *records = gsm_snapshot_processes (source->previous, &count);

      source->previous_by_pid.reserve (count);
      for (size_t i = 0; i < count; i++)
        source->previous_by_pid.emplace (records[i].pid, &records[i]);
    }
  return TRUE;
}

void
gsm_proc_source_reset (GsmProcSource *source)
{
  if (source == NULL)
    return;
  gsm_snapshot_free (source->current);
  gsm_snapshot_free (source->previous);
  source->current = source->previous = NULL;
  source->previous_by_pid.clear ();
}

gint64
gsm_proc_source_boot_time_s (GsmProcSource *source)
{
  return source->boot_time_s;
}

guint
gsm_proc_source_figures (GsmProcSource   *source,
                         GsmProcWhose     whose,
                         uid_t            uid,
                         guint            cpu_count,
                         gboolean         solaris_mode,
                         GsmProcFigures **out)
{
  size_t count = 0, strings_len = 0;

  *out = NULL;
  if (source->current == NULL)
    return 0;

  const gsm_process *records = gsm_snapshot_processes (source->current, &count);
  const gsm_sample_stats *stats = gsm_snapshot_stats (source->current);
  const gsm_sample_stats *previous_stats = source->previous != NULL ? gsm_snapshot_stats (source->previous) : NULL;
  GsmProcWindow window = {};

  window.current_at_ns = stats->sampled_at_ns;
  window.previous_at_ns = previous_stats != NULL ? previous_stats->sampled_at_ns : 0;
  window.cpu_count = MAX (cpu_count, 1u);
  window.solaris_mode = solaris_mode;
  window.boot_time_s = source->boot_time_s;
  window.clock_ticks_per_s = source->clock_ticks_per_s;
  window.strings = gsm_snapshot_strings (source->current, &strings_len);
  window.strings_len = strings_len;

  GsmProcFigures *figures = g_new (GsmProcFigures, MAX (count, (size_t) 1));
  guint written = 0;

  for (size_t i = 0; i < count; i++)
    {
      if (!gsm_proc_source_selects (whose, uid, &records[i]))
        continue;

      auto found = source->previous_by_pid.find (records[i].pid);

      window.previous = found != source->previous_by_pid.end () ? found->second : NULL;
      gsm_proc_figures_from_record (&window, &records[i], &figures[written++]);
    }

  std::sort (figures, figures + written,
             [](const GsmProcFigures &a, const GsmProcFigures &b) { return a.pid < b.pid; });

  *out = figures;
  return written;
}
