/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Battery wear, from /sys/class/power_supply. Free and unprivileged, and the
 * same story as drive wear: a component quietly losing capacity that almost
 * no monitor bothers to show.
 */
typedef struct {
  char    *id;              /* e.g. "BAT1" — stable across reboots        */
  char    *model;
  char    *technology;

  int      health_percent;  /* full / design capacity, -1 unknown         */
  int      charge_percent;  /* current charge, -1 unknown                 */
  int      cycle_count;     /* -1 unknown                                 */

  double   full_wh;         /* current full capacity                      */
  double   design_wh;       /* as-new capacity                            */

  gboolean present;
} GsmBatteryInfo;

void       gsm_battery_info_free (GsmBatteryInfo *info);

/* Returns a GPtrArray of GsmBatteryInfo*. Empty on a desktop. */
GPtrArray *gsm_battery_collect   (void);

G_END_DECLS
