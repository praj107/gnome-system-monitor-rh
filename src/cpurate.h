/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A process's CPU share over one refresh interval, from two readings of its
 * cumulative CPU time (utime + stime of /proc/PID/stat, proc(5), fields 14
 * and 15, in clock ticks) and the machine's total CPU ticks over the same
 * interval (the summed fields of the first line of /proc/stat).
 *
 *   percent = (cpu_ticks - previous_cpu_ticks) * cpu_scale / total_ticks
 *
 * cpu_scale is 100 for "share of the whole machine" and 100 * CPU count for
 * "share of one CPU" (the Irix convention); the result is capped at it.
 *
 * A figure that cannot be told is unknown, never 0 and never a wrapped
 * subtraction: the counter could not be read, the process is not the one the
 * previous reading belonged to (the pid was reused: its start time differs),
 * the counter went backwards, or there was no interval to divide by. Pure
 * function, no GTK: the readings are parameters so tests use fixed numbers.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  gboolean known;   /* FALSE: show "—", leave out of totals */
  double   percent; /* meaningful only when known */
} GsmCpuRate;

GsmCpuRate gsm_cpu_rate (gboolean read_ok,
                         gboolean same_process,
                         guint64  previous_cpu_ticks,
                         guint64  cpu_ticks,
                         guint64  total_ticks,
                         guint    cpu_scale);

/*
 * May a counter kept from an earlier refresh serve as the baseline for a
 * ProcInfo that is being (re)built now? Only for the same process (same start
 * time) and a counter that has not gone backwards. A counter taken in the
 * very refresh that first sees a process is no baseline at all: the caller
 * leaves the rate unknown until the next refresh.
 */
gboolean gsm_cpu_baseline_carries (gboolean read_ok,
                                   guint64  kept_start_time,
                                   guint64  kept_ticks,
                                   guint64  start_time,
                                   guint64  ticks);

G_END_DECLS
