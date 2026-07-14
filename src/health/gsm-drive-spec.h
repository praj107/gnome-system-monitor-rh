/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include "gsm-drive.h"

G_BEGIN_DECLS

#define GSM_TB ((uint64_t) 1000 * 1000 * 1000 * 1000)

/*
 * Look up a drive's datasheet endurance rating by model. Returns 0 when we
 * have no entry — which is the common case, and is why the spec table is the
 * *last* resort rather than the first.
 */
uint64_t gsm_drive_spec_rated_tbw (const char *model,
                                   uint64_t    size);

/*
 * Decide what this drive's endurance is and where that number came from.
 *
 * Trust order, best first:
 *   1. what the user told us         (they can read their own datasheet)
 *   2. what the drive implies        (host writes / its own wear estimate)
 *   3. our shipped model table       (a guess from a name)
 *
 * The drive's own controller is the only source that reflects the actual
 * flash in the actual device, including write amplification. A datasheet
 * figure is a marketing-adjacent number about a *class* of drive: on the WD
 * SN740 in this machine the two disagree by more than 3x. So we derive from
 * the drive whenever it has worn enough for its estimate to mean anything,
 * and fall back to the table only for drives too new to have said much yet.
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
