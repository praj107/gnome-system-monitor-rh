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

/* Returns a GPtrArray of GsmBlameRow*, biggest writer first. */
GPtrArray   *gsm_health_db_get_blame        (GsmHealthDb     *self,
                                             const char      *drive_id,
                                             GsmHistoryRange  range);

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
