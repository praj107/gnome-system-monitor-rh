/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Processes tab's figures from the collector library (gsmcollect.h)
 * instead of libgtop's per-process calls.
 *
 * Definitions, over proc_pid_stat(5), proc_pid_status(5), proc_pid_io(5) and
 * the scheduler's /proc/PID/schedstat:
 *
 *   Private (anon)  RssAnon
 *   Resident        RssAnon + RssFile + RssShmem
 *   Shared          RssFile + RssShmem
 *   Virtual         VmSize
 *   CPU time        utime + stime, in clock ticks
 *   CPU %           delta run_ns / delta wall ns * 100 between two snapshots,
 *                   a share of one logical CPU (the Irix convention) or, in
 *                   Solaris mode, of the whole machine; capped at the scale
 *   Disk read       read_bytes of /proc/PID/io
 *   Disk write      write_bytes - cancelled_write_bytes, 0 when more was
 *                   cancelled than written: a process that truncates pages
 *                   another process dirtied is credited with their
 *                   cancellation without ever having been charged for them
 *   Start time      boot time (btime of /proc/stat) + start_ticks / CLK_TCK
 *
 * A figure the kernel did not provide (a clear `known` bit) or that needs two
 * snapshots of the same process and has only one is flagged unknown in the
 * result; it is never 0.
 */

#pragma once

#include <glib.h>
#include <sys/types.h>

#include "gsmcollect.h"

G_BEGIN_DECLS

/* Which processes the list shows: the same selection as libgtop's
 * glibtop_get_proclist with GLIBTOP_KERN_PROC_ALL, that plus
 * GLIBTOP_EXCLUDE_IDLE, and GLIBTOP_KERN_PROC_UID. */
typedef enum
{
  GSM_PROC_WHOSE_ALL,
  GSM_PROC_WHOSE_ACTIVE,
  GSM_PROC_WHOSE_USER
} GsmProcWhose;

typedef struct
{
  pid_t     pid;
  pid_t     ppid;
  guint     status;               /* GLIBTOP_PROCESS_*, 0 for a state libgtop does not name */
  gboolean  uid_known;
  guint     uid;                  /* real uid, status: Uid */
  gint      nice;
  guint     num_threads;

  /* bytes; each valid only with its flag */
  gboolean  vmsize_known;
  gboolean  memres_known;
  gboolean  memshared_known;
  gboolean  mem_known;
  guint64   vmsize_bytes;
  guint64   memres_bytes;
  guint64   memshared_bytes;
  guint64   mem_bytes;

  guint64   cpu_time_ticks;
  gboolean  start_time_known;
  guint64   start_time_s;         /* seconds since the epoch */

  gboolean  cpu_known;
  double    cpu_percent;

  gboolean  io_readable;          /* /proc/PID/io was read: the totals are valid */
  gboolean  io_rate_valid;        /* and two snapshots of the same process gave a rate */
  guint64   disk_read_bytes;
  guint64   disk_write_bytes;
  guint64   disk_read_bytes_per_s;
  guint64   disk_write_bytes_per_s;

  /* A cgroup v2 path, pointing into the snapshot's string table (not
   * NUL-terminated at path_len); NULL when the kernel did not give one. */
  const char *cgroup_path;
  gsize       cgroup_path_len;
} GsmProcFigures;

/* What the mapping needs besides the record itself. */
typedef struct
{
  const gsm_process *previous;         /* the same pid in the previous snapshot, or NULL */
  guint64            previous_at_ns;   /* sampled_at_ns of that snapshot */
  guint64            current_at_ns;
  guint              cpu_count;        /* logical CPUs, at least 1 */
  gboolean           solaris_mode;
  gint64             boot_time_s;      /* btime; negative when unknown */
  glong              clock_ticks_per_s;
  const char        *strings;          /* the snapshot's string table */
  gsize              strings_len;
} GsmProcWindow;

void     gsm_proc_figures_from_record (const GsmProcWindow *window,
                                       const gsm_process   *record,
                                       GsmProcFigures      *figures);

gboolean gsm_proc_source_selects      (GsmProcWhose        whose,
                                       uid_t               uid,
                                       const gsm_process  *record);

/* The collector handle and the previous snapshot of the process list. */
typedef struct GsmProcSource GsmProcSource;

/* procfs_root NULL means "/proc". NULL when the collector cannot be opened. */
GsmProcSource *gsm_proc_source_new     (const char *procfs_root);
void           gsm_proc_source_free    (GsmProcSource *source);

/* Takes a snapshot, keeping the one before it as the baseline of the rates.
 * FALSE (and the old snapshots kept) when the collector could not read. */
gboolean       gsm_proc_source_sample  (GsmProcSource *source);

/* Forgets the snapshots, so the next sample has no baseline: used when the
 * list is not read through the collector for a while. */
void           gsm_proc_source_reset   (GsmProcSource *source);

/* Figures of the processes `whose` selects, sorted by pid, from the latest
 * sample. Returns the number written to *out (a g_new array, free it with
 * g_free); 0 and NULL before any sample. */
guint          gsm_proc_source_figures (GsmProcSource  *source,
                                        GsmProcWhose    whose,
                                        uid_t           uid,
                                        guint           cpu_count,
                                        gboolean        solaris_mode,
                                        GsmProcFigures **out);

/* Boot time from the btime line of <procfs_root>/stat; -1 when absent. */
gint64         gsm_proc_source_boot_time_s (GsmProcSource *source);

G_END_DECLS
