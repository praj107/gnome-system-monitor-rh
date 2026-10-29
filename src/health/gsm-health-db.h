/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#include <glib-object.h>

#include "gsm-drive.h"

G_BEGIN_DECLS

#define GSM_TYPE_HEALTH_DB (gsm_health_db_get_type ())
G_DECLARE_FINAL_TYPE (GsmHealthDb, gsm_health_db, GSM, HEALTH_DB, GObject)

/* Which rollup tier a query should read from. */
typedef enum {
  GSM_HISTORY_RANGE_DAY,     /* last 24h  — raw samples    */
  GSM_HISTORY_RANGE_MONTH,   /* last 30d  — hourly rollups */
  GSM_HISTORY_RANGE_YEAR,    /* last 365d — daily rollups  */
} GsmHistoryRange;

/* One point on a wear/TBW chart. */
typedef struct {
  int64_t  ts;             /* unix seconds     */
  uint64_t bytes_written;
  int      percent_used;   /* -1 unknown       */
  int      temperature;    /* -1 unknown       */
} GsmHistoryPoint;

/* One row of the blame chart: how much a named app wrote to one drive. */
typedef struct {
  char    *name;
  uint64_t wbytes;
  gboolean exact;          /* FALSE => attributed by heuristic, label it */
} GsmBlameRow;

void         gsm_blame_row_free (GsmBlameRow *row);

/*
 * How a stored row of bytes was arrived at (the `basis` column). `kernel`
 * and `residue` come from cgroup io.stat and are exact; `procfs` is
 * /proc/<pid>/io, which names the bytes but not the device, so which drive
 * they landed on is inferred. The `coredump-*` and `journald-lines` bases
 * are reserved for rows that reassign a proxy writer's bytes to whoever
 * caused them (the `via` column names the proxy); those are estimates or
 * per-event figures, not the kernel's.
 */
#define GSM_HEALTH_BASIS_KERNEL               "kernel"
#define GSM_HEALTH_BASIS_RESIDUE              "residue"
#define GSM_HEALTH_BASIS_PROCFS               "procfs"
#define GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL "coredump-proportional"
#define GSM_HEALTH_BASIS_COREDUMP_EXACT       "coredump-exact"
#define GSM_HEALTH_BASIS_JOURNALD_LINES       "journald-lines"

/* One stored row of the blame table, as written: nothing summed away. */
typedef struct
{
  char    *name;
  char    *via;            /* proxy writer for an on-behalf row, else "" */
  char    *basis;          /* GSM_HEALTH_BASIS_* */
  uint64_t wbytes;
} GsmBlameDetail;

void          gsm_blame_detail_free (GsmBlameDetail *row);

/*
 * Open (creating if needed) the history database. Path defaults to
 * $XDG_DATA_HOME/gnome-system-monitor-rh/health.db; pass an explicit path for
 * tests. Both the UI and the sampler open the same file, so every write is
 * wrapped in a transaction and WAL is enabled.
 */
GsmHealthDb *gsm_health_db_new              (const char  *path,
                                             GError     **error);

/*
 * Where "now" comes from, in unix seconds. NULL (the default) is the wall
 * clock; tests inject their own so a simulated hour runs in milliseconds.
 */
typedef int64_t (*GsmHealthClock) (gpointer user_data);

void         gsm_health_db_set_clock        (GsmHealthDb    *self,
                                             GsmHealthClock  clock,
                                             gpointer        user_data);

/* --- writing (sampler side) --- */

/*
 * Remember a drive and, when health was read, one sample of it. Both rows
 * are skipped when nothing changed since they were last written and the last
 * write is under an hour old. Opens a transaction that stays open until
 * gsm_health_db_commit() or gsm_health_db_flush().
 */
void         gsm_health_db_record_drive     (GsmHealthDb *self,
                                             GsmDrive    *drive);

/*
 * Record a *delta* of bytes written by `name` to one drive since the last
 * sample. Deltas, not totals: the underlying counters die with the cgroup or
 * process, so only the increments are durable. The delta is held in memory,
 * in today's bucket, until gsm_health_db_flush().
 */
void         gsm_health_db_record_blame     (GsmHealthDb *self,
                                             const char  *drive_id,
                                             const char  *name,
                                             uint64_t     delta_wbytes,
                                             gboolean     exact);

/*
 * The same with the row's full identity: `via` is "" for bytes charged to
 * `name` by the kernel, or the proxy writer for bytes reassigned to `name`;
 * `basis` is one of GSM_HEALTH_BASIS_*. gsm_health_db_record_blame() is this
 * with an empty via and basis kernel (exact) or procfs (not exact).
 *
 * A flush writes at most the 20 largest rows per drive, day and kind
 * (charged-to or on-behalf), and folds the rest into one row named "other".
 */
void gsm_health_db_record_blame_row (GsmHealthDb *self,
                                     const char  *drive_id,
                                     const char  *name,
                                     const char  *via,
                                     const char  *basis,
                                     uint64_t     delta_wbytes);

void         gsm_health_db_record_battery   (GsmHealthDb *self,
                                             const char  *battery_id,
                                             int          health_percent,
                                             int          cycle_count);

/* Fold raw samples into hourly rollups and hourly into daily, then drop what
 * has aged out. Cheap; safe to call on every sampler tick. */
void         gsm_health_db_roll_up          (GsmHealthDb *self);

gboolean     gsm_health_db_commit           (GsmHealthDb  *self,
                                             GError      **error);

/*
 * Write the accumulated blame deltas and any open drive or battery rows in
 * one short transaction, and record the time in the meta table. Returns FALSE
 * (keeping the deltas for the next flush) when the commit fails.
 */
gboolean     gsm_health_db_flush            (GsmHealthDb  *self,
                                             GError      **error);

/* When the record was last flushed (unix seconds), or 0 when it never was. */
int64_t      gsm_health_db_get_last_flush   (GsmHealthDb *self);

/* --- reading (UI side) --- */

/* Returns an array of GsmHistoryPoint, oldest first. */
GArray      *gsm_health_db_get_history      (GsmHealthDb     *self,
                                             const char      *drive_id,
                                             GsmHistoryRange  range);

/*
 * Returns a GPtrArray of GsmBlameRow*, biggest writer first: the bytes the
 * kernel charged (and /proc attributed), summed over basis per name.
 * `exact` is derived: every contributing row is kernel or residue.
 * On-behalf rows are left out, since they re-divide bytes already counted
 * under their proxy writer.
 */
GPtrArray   *gsm_health_db_get_blame        (GsmHealthDb     *self,
                                             const char      *drive_id,
                                             GsmHistoryRange  range);

/*
 * Every stored charged-to and on-behalf row for this drive over the range,
 * summed per (name, via, basis), biggest first. Returns GsmBlameDetail*.
 */
GPtrArray * gsm_health_db_get_blame_detail (GsmHealthDb    *self,
                                            const char     *drive_id,
                                            GsmHistoryRange range);

/*
 * Average bytes/day written to this drive over the trailing window, measured
 * from recorded history. Returns -1 when there is not yet enough history to
 * say — a projection from two hours of data would be worthless, and we would
 * rather show nothing than a confident fiction.
 */
double       gsm_health_db_get_write_rate   (GsmHealthDb *self,
                                             const char  *drive_id,
                                             int          days);

/* Endurance the user typed in for this drive, or 0. */
uint64_t     gsm_health_db_get_user_tbw     (GsmHealthDb *self,
                                             const char  *drive_id);
void         gsm_health_db_set_user_tbw     (GsmHealthDb *self,
                                             const char  *drive_id,
                                             uint64_t     rated_tbw);

/* How far back the history actually goes, and what it costs on disk — both
 * shown in the UI so the user can see exactly what is being kept. */
int64_t      gsm_health_db_get_oldest_sample (GsmHealthDb *self);
uint64_t     gsm_health_db_get_size_bytes    (GsmHealthDb *self);
void         gsm_health_db_clear             (GsmHealthDb *self);

G_END_DECLS
