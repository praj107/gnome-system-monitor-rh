/*
 * Battery wear from sysfs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "gsm-battery.h"

#define POWER_SUPPLY_DIR "/sys/class/power_supply"


void
gsm_battery_info_free (GsmBatteryInfo *info)
{
  if (info == NULL)
    return;

  g_free (info->id);
  g_free (info->model);
  g_free (info->technology);
  g_free (info);
}


static char *
read_attr (const char *dir,
           const char *attr)
{
  g_autofree char *path = g_build_filename (dir, attr, NULL);
  g_autofree char *contents = NULL;

  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;

  return g_strchomp (g_steal_pointer (&contents));
}


static gboolean
read_int_attr (const char *dir,
               const char *attr,
               int64_t    *out)
{
  g_autofree char *value = read_attr (dir, attr);
  char *end = NULL;
  int64_t parsed;

  if (value == NULL || *value == '\0')
    return FALSE;

  parsed = g_ascii_strtoll (value, &end, 10);
  if (end == value)
    return FALSE;

  *out = parsed;

  return TRUE;
}


/*
 * Kernels expose capacity either as energy (µWh, needs no voltage) or as
 * charge (µAh, which must be multiplied by the design voltage to become
 * energy). Laptops differ, so try both rather than assuming.
 */
static gboolean
read_capacity (const char *dir,
               double     *full_wh,
               double     *design_wh)
{
  int64_t full = 0, design = 0, voltage = 0;

  if (read_int_attr (dir, "energy_full", &full) &&
      read_int_attr (dir, "energy_full_design", &design) &&
      design > 0) {
    *full_wh = (double) full / 1e6;
    *design_wh = (double) design / 1e6;
    return TRUE;
  }

  if (read_int_attr (dir, "charge_full", &full) &&
      read_int_attr (dir, "charge_full_design", &design) &&
      design > 0) {
    double volts = 0;

    if (read_int_attr (dir, "voltage_min_design", &voltage) && voltage > 0)
      volts = (double) voltage / 1e6;

    /* Without a voltage we can still report *relative* wear correctly, which
     * is the number that matters — we just cannot state it in watt-hours. */
    *full_wh = volts > 0 ? ((double) full / 1e6) * volts : (double) full / 1e6;
    *design_wh = volts > 0 ? ((double) design / 1e6) * volts : (double) design / 1e6;

    return TRUE;
  }

  return FALSE;
}


GPtrArray *
gsm_battery_collect (void)
{
  GPtrArray *out =
    g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_battery_info_free);
  g_autoptr (GDir) dir = g_dir_open (POWER_SUPPLY_DIR, 0, NULL);
  const char *name;

  if (dir == NULL)
    return out;

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *path = g_build_filename (POWER_SUPPLY_DIR, name, NULL);
    g_autofree char *type = read_attr (path, "type");
    GsmBatteryInfo *info;
    int64_t value;
    double full_wh = 0, design_wh = 0;

    /* Skip AC adapters and the USB-C power-delivery pseudo-supplies. */
    if (g_strcmp0 (type, "Battery") != 0)
      continue;

    info = g_new0 (GsmBatteryInfo, 1);
    info->id = g_strdup (name);
    info->model = read_attr (path, "model_name");
    info->technology = read_attr (path, "technology");
    info->present = TRUE;
    info->health_percent = -1;
    info->charge_percent = -1;
    info->cycle_count = -1;

    if (read_int_attr (path, "capacity", &value))
      info->charge_percent = (int) CLAMP (value, 0, 100);

    /* Some firmware reports 0 here rather than omitting the file; a battery
     * that has genuinely done zero cycles is indistinguishable, but reporting
     * "0 cycles" for a worn battery would be a lie, so treat 0 as unknown
     * only when the battery has clearly aged. */
    if (read_int_attr (path, "cycle_count", &value) && value >= 0)
      info->cycle_count = (int) value;

    if (read_capacity (path, &full_wh, &design_wh)) {
      info->full_wh = full_wh;
      info->design_wh = design_wh;

      /* Capacity can exceed the design figure on a new pack; clamping keeps
       * the meter from rendering above 100%. */
      info->health_percent =
        (int) CLAMP ((full_wh / design_wh) * 100.0, 0.0, 100.0);
    }

    if (info->cycle_count == 0 && info->health_percent >= 0 &&
        info->health_percent < 90)
      info->cycle_count = -1;

    g_ptr_array_add (out, info);
  }

  return out;
}
