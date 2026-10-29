/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsm-drive.h"

G_BEGIN_DECLS

#define GSM_TB ((uint64_t) 1000 * 1000 * 1000 * 1000)

/*
 * One endurance rating a maker publishes: an exact model string at one
 * capacity. `rated_tbw` is the maker's figure in terabytes (10^12 bytes)
 * written; `source_label` is a short name for the UI ("Samsung 980 PRO data
 * sheet Rev 2.1") and `source_url` the public document. None is ever NULL.
 */
typedef struct {
  const char *model_prefix;   /* start of the model string the drive reports */
  uint32_t    capacity_gb;    /* nominal decimal GB: 250, 500, 1000, ...      */
  uint32_t    rated_tbw;      /* maker's figure, terabytes written            */
  const char *source_label;
  const char *source_url;
} GsmDriveSpecRow;

/*
 * The maker's rating for this exact model and capacity, or NULL when the
 * maker publishes none that we have read: the common case, and an answer in
 * its own right ("not published"), never a guess from a vendor or family.
 * `size_bytes` is the drive's reported size; it is matched to the row's
 * nominal capacity within 3 %, because the same "1 TB" drive reports
 * 1,000,204,886,016 or 1,024,209,543,168 bytes. The row is static.
 */
const GsmDriveSpecRow *gsm_drive_spec_lookup (const char *model,
                                              uint64_t    size_bytes);

/* The table itself, so a test can check every row. */
size_t                 gsm_drive_spec_row_count (void);
const GsmDriveSpecRow *gsm_drive_spec_row       (size_t index);

/* gsm_drive_spec_lookup() as bytes; 0 when not published. */
uint64_t gsm_drive_spec_rated_tbw (const char *model,
                                   uint64_t    size);

/*
 * The endurance a drive's own wear implies, as a range. Controllers report
 * wear in whole percent, so `percent_used` = 12 means 12.00 to 12.99 % and
 * the endurance (bytes written / fraction worn) lies in
 * [written / 0.13, written / 0.12]. FALSE (and a zeroed range) when it
 * cannot be bounded: bytes written is zero or percent_used is below 1.
 */
typedef struct {
  uint64_t low_bytes;
  uint64_t high_bytes;
} GsmEnduranceRange;

gboolean gsm_drive_spec_implied_endurance (uint64_t           bytes_written,
                                           int                percent_used,
                                           GsmEnduranceRange *range);

/*
 * Decide what this drive's endurance is and where that number came from.
 *
 * Trust order, best first:
 *   1. what the user told us         (they can read their own datasheet)
 *   2. what the drive implies        (host writes / its own wear estimate)
 *   3. the maker's published rating  (exact model and capacity only)
 *
 * The drive's own controller is the only source that reflects the actual
 * flash in the actual device, including write amplification. A datasheet
 * figure is a marketing-adjacent number about a *class* of drive: on the WD
 * SN740 in this machine the two disagree by more than 3x. So we derive from
 * the drive whenever it has worn enough for its estimate to mean anything,
 * and fall back to the table only for drives too new to have said much yet.
 *
 * The single rated_tbw it sets is, when derived, the upper bound of the
 * implied range. Both truths are also kept apart on the drive: the maker's
 * rating (gsm_drive_get_maker_rated_tbw) and the implied range
 * (gsm_drive_get_implied_endurance), set whether or not the projection
 * follows them, so the UI can show both when they disagree.
 *
 * `user_override` is 0 when unset.
 */
void     gsm_drive_resolve_endurance (GsmDrive *drive,
                                      uint64_t  user_override);

/*
 * Years of life left at the given write rate, or -1 when we cannot say.
 * `bytes_per_day` should come from measured history, not from lifetime
 * averages — what the drive did last month predicts next month far better
 * than what it did when it was new.
 */
double   gsm_drive_project_years_left (GsmDrive *drive,
                                       double    bytes_per_day);

G_END_DECLS
