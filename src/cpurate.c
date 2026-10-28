/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See cpurate.h for the definition.
 */

#include "cpurate.h"

GsmCpuRate
gsm_cpu_rate (gboolean read_ok,
              gboolean same_process,
              guint64  previous_cpu_ticks,
              guint64  cpu_ticks,
              guint64  total_ticks,
              guint    cpu_scale)
{
  GsmCpuRate rate = { FALSE, 0.0 };
  double percent;

  /* Each guard is a case where a number would be invented. */
  if (!read_ok || !same_process)
    return rate;

  /* An unsigned subtraction here would wrap to about 1.8e19. */
  if (cpu_ticks < previous_cpu_ticks)
    return rate;

  if (total_ticks == 0)
    return rate;

  percent = (double) (cpu_ticks - previous_cpu_ticks) * cpu_scale / total_ticks;

  rate.known = TRUE;
  rate.percent = MIN (percent, (double) cpu_scale);

  return rate;
}

gboolean
gsm_cpu_baseline_carries (gboolean read_ok,
                          guint64  kept_start_time,
                          guint64  kept_ticks,
                          guint64  start_time,
                          guint64  ticks)
{
  return read_ok && kept_start_time == start_time && ticks >= kept_ticks;
}
