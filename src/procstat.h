/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per-CPU busy time from /proc/stat, keyed by kernel CPU number. Follows
 * proc_stat(5): the file has one "cpu" line for the whole machine and one
 * "cpuN" line for each online CPU, N being the kernel's CPU number, then
 * the cumulative time in USER_HZ ticks:
 *
 *   user nice system idle iowait irq softirq steal guest guest_nice
 *
 * An offline CPU has no line at all, so lines are never matched to graph
 * slots by position (libgtop's xcpu arrays are, which shows the wrong CPU
 * once any CPU is offline). guest and guest_nice are already counted inside
 * user and nice, so they are not added again.
 *
 * Definitions, per CPU:
 *   idle_ticks  = idle + iowait
 *   total_ticks = user + nice + system + idle + iowait + irq + softirq + steal
 *   busy_ticks  = total_ticks - idle_ticks
 *   busy %      = 100 * delta(busy_ticks) / delta(total_ticks)
 *
 * Older kernels print fewer fields; missing trailing fields count as 0
 * (at least user, nice, system and idle must be present). Pure functions,
 * no GTK.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  gboolean present;       /* the line was in the file */
  guint64 busy_ticks;
  guint64 idle_ticks;
} GsmCpuTicks;

/* Parse the file's text. cpus[0..n_cpus) receives the "cpuN" lines by N
 * (every entry not listed, or listed with N >= n_cpus, stays not present);
 * *aggregate receives the "cpu" line. Returns the number of "cpu" lines
 * that were malformed and skipped. */
guint gsm_procstat_parse (const char  *text,
                          GsmCpuTicks *aggregate,
                          GsmCpuTicks *cpus,
                          guint        n_cpus);

/* Read the file at path (normally "/proc/stat") and parse it. FALSE, with
 * everything not present, when the file cannot be read. */
gboolean gsm_procstat_read (const char  *path,
                            GsmCpuTicks *aggregate,
                            GsmCpuTicks *cpus,
                            guint        n_cpus);

/* Busy share of the interval between two readings of the same CPU, in
 * percent (0 to 100). FALSE ("no sample") when either reading is not
 * present (first sample, offline CPU), when a counter went backwards (a
 * hotplugged CPU restarts at 0; iowait may also step back) or when no ticks
 * elapsed. *percent is left untouched then. */
gboolean gsm_cpu_ticks_busy_percent (const GsmCpuTicks *before,
                                     const GsmCpuTicks *after,
                                     double            *percent);

/* The number of graph slots the CPUs need: the highest CPU number in a
 * kernel CPU list (the text of /sys/devices/system/cpu/present, e.g. "0-15",
 * "0-3,8-11" or "0"; trailing newline allowed) plus one, so that every
 * present CPU, offline ones included, has a slot indexed by its number.
 * 0 when the text is empty or malformed. Documentation/ABI/stable/
 * sysfs-devices-system-cpu. */
guint gsm_cpu_slots_from_list (const char *text);

/* Read <sysfs_root>/devices/system/cpu/present (sysfs_root is normally
 * "/sys"); falls back to online_cpus when the file is absent or malformed,
 * and never returns less than 1. */
guint gsm_cpu_slot_count (const char *sysfs_root,
                          guint       online_cpus);

G_END_DECLS
