/*
 * Drive endurance: what the datasheet claims, what the drive itself implies,
 * and how many years are left.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include "gsm-drive-spec.h"

/*
 * A deliberately small table. Endurance ratings are per capacity *and* per
 * model, there are thousands of SKUs, and a stale guess is worse than no
 * guess — so this covers common families only, and everything else falls back
 * to what the drive says about itself.
 *
 * `tbw_per_tb` is the rated TBW scaled to drive capacity, which is how
 * manufacturers actually spec a family (a 2TB model of the same drive gets
 * roughly double the rating of the 1TB one).
 */
typedef struct {
  const char *match;        /* case-insensitive substring of the model  */
  unsigned    tbw_per_tb;   /* rated terabytes written, per TB of size  */
} DriveSpec;

static const DriveSpec drive_specs[] = {
  /* Samsung consumer NVMe */
  { "Samsung SSD 980 PRO",  600 },
  { "Samsung SSD 980",      300 },
  { "Samsung SSD 990 PRO",  600 },
  { "Samsung SSD 970 EVO",  600 },
  { "Samsung SSD 970 PRO", 1200 },
  { "Samsung SSD 870 EVO",  600 },
  { "Samsung SSD 860 EVO",  600 },
  { "Samsung SSD 850 EVO",  150 },

  /* WD / SanDisk */
  { "WD_BLACK SN850",       600 },
  { "WD_BLACK SN770",       600 },
  { "WDS",                  400 },
  { "WD Blue SN570",        300 },
  { "WD PC SN740",          400 },
  { "WD PC SN730",          400 },

  /* Crucial / Micron */
  { "CT500MX500",           180 },
  { "CT1000MX500",          360 },
  { "CT1000P3",             220 },
  { "CT1000X9",             300 },
  { "CT2000P3",             440 },

  /* Kingston, Intel, SK hynix */
  { "KINGSTON SA2000",      600 },
  { "KINGSTON SNV2S",       320 },
  { "INTEL SSDPEK",         100 },
  { "SKHynix",              600 },
  { "Solidigm",             400 },
};


uint64_t
gsm_drive_spec_rated_tbw (const char *model,
                          uint64_t    size)
{
  g_autofree char *folded = NULL;
  double size_tb;

  if (model == NULL || *model == '\0' || size == 0)
    return 0;

  folded = g_utf8_casefold (model, -1);
  size_tb = (double) size / (double) GSM_TB;

  for (gsize i = 0; i < G_N_ELEMENTS (drive_specs); i++) {
    g_autofree char *needle = g_utf8_casefold (drive_specs[i].match, -1);

    if (strstr (folded, needle) != NULL)
      return (uint64_t) (drive_specs[i].tbw_per_tb * size_tb * (double) GSM_TB);
  }

  return 0;
}


/*
 * Below this much wear the controller's percent_used is too coarse to divide
 * by: it is reported in whole percent, so at 1% the implied endurance carries
 * a ±50% error and at 2% still ±25%. Deriving from it would produce a
 * confident-looking number built on rounding noise.
 */
#define MIN_WEAR_TO_DERIVE 5


void
gsm_drive_resolve_endurance (GsmDrive *drive,
                             uint64_t  user_override)
{
  int percent_used;
  uint64_t written;
  uint64_t spec;

  g_return_if_fail (GSM_IS_DRIVE (drive));

  if (user_override > 0) {
    gsm_drive_set_endurance (drive, user_override, GSM_ENDURANCE_USER);
    return;
  }

  percent_used = gsm_drive_get_percent_used (drive);
  written = gsm_drive_get_bytes_written (drive);

  if (percent_used >= MIN_WEAR_TO_DERIVE && written > 0) {
    uint64_t implied = (uint64_t) ((double) written / ((double) percent_used / 100.0));

    gsm_drive_set_endurance (drive, implied, GSM_ENDURANCE_DERIVED);
    return;
  }

  spec = gsm_drive_spec_rated_tbw (gsm_drive_get_model (drive),
                                   gsm_drive_get_size (drive));
  if (spec > 0) {
    gsm_drive_set_endurance (drive, spec, GSM_ENDURANCE_SPEC_DB);
    return;
  }

  gsm_drive_set_endurance (drive, 0, GSM_ENDURANCE_UNKNOWN);
}


double
gsm_drive_project_years_left (GsmDrive *drive,
                              double    bytes_per_day)
{
  int percent_used;
  uint64_t rated;
  uint64_t written;
  double remaining_bytes;

  g_return_val_if_fail (GSM_IS_DRIVE (drive), -1);

  if (bytes_per_day <= 0)
    return -1;

  percent_used = gsm_drive_get_percent_used (drive);
  rated = gsm_drive_get_rated_tbw (drive);
  written = gsm_drive_get_bytes_written (drive);

  if (rated == 0)
    return -1;

  /*
   * Prefer the drive's own wear estimate to compute what is left. It already
   * accounts for write amplification and for wear the host never saw, so
   * "88% of the endurance remains" is a better basis than subtracting host
   * writes from a rating the drive may not agree with in the first place.
   */
  if (percent_used >= 0 && percent_used <= 100) {
    remaining_bytes = (double) rated * ((100.0 - percent_used) / 100.0);
  } else if (written < rated) {
    remaining_bytes = (double) (rated - written);
  } else {
    return 0;
  }

  if (remaining_bytes <= 0)
    return 0;

  return remaining_bytes / bytes_per_day / 365.25;
}
