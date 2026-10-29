/*
 * The history database.
 *
 * Everything genuinely novel about this feature depends on remembering: a
 * drive's SMART counters tell you where it is, but only a record over time
 * tells you where it is going, or which app took it there. This is a plain
 * local SQLite file — it never leaves the machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <sqlite3.h>

#include <gio/gio.h>

#include "gsm-drive-spec.h"
#include "gsm-health-db.h"

/* What the sampler has been told about one drive and when it last reached
 * storage, so an unchanged reading is not written again. */
typedef struct {
  char     *model;
  uint64_t  size;
  gboolean  removable;
  int64_t   identity_ts;

  gboolean  has_sample;
  int64_t   sample_ts;
  uint64_t  bytes_written;
  int       percent_used;
  int       temperature;
} DriveState;

typedef struct {
  int64_t day;
  int     health_percent;
  int     cycle_count;
} BatteryState;

/* One writer's bytes on one drive on one day, not yet in the database. */
typedef struct {
  char     *drive_id;
  int64_t   day;
  char     *name;
  uint64_t  wbytes;
  gboolean  exact;
} PendingBlame;

struct _GsmHealthDb {
  GObject parent_instance;

  sqlite3 *db;
  char *path;
  gboolean in_transaction;

  GsmHealthClock clock;
  gpointer clock_data;

  GHashTable *pending_blame;   /* "drive\nday\nname" -> PendingBlame */
  GHashTable *drive_state;     /* drive id -> DriveState */
  GHashTable *battery_state;   /* battery id -> BatteryState */
  int64_t last_roll_up;
};

G_DEFINE_FINAL_TYPE (GsmHealthDb, gsm_health_db, G_TYPE_OBJECT)

/* Retention, matching what the UI promises the user. */
#define RAW_RETENTION_SECS    (48 * 3600)
#define HOURLY_RETENTION_SECS (90 * 24 * 3600)
/* Daily rollups are kept indefinitely: one row per drive per day is a few
 * hundred KB per decade, and a multi-year wear curve is the whole point. */

#define SCHEMA_VERSION 1

/* Drive identity and an unchanged health reading are rewritten at most this
 * often: a "last seen" accurate to the hour is as honest as one accurate to
 * the second, and costs 24 page writes a day instead of thousands. */
#define DRIVE_REWRITE_SECS 3600

/* Folding old samples into rollups only has work to do when something has
 * aged out, which happens at the pace of the raw retention, not per flush. */
#define ROLL_UP_SECS 3600

/*
 * Write-ahead log bounds (https://www.sqlite.org/pragma.html): the log is
 * truncated to this size when a checkpoint resets it, and checkpoints run
 * after this many pages, so the file does not sit at several megabytes next
 * to a database of a few dozen kilobytes.
 */
#define WAL_SIZE_LIMIT_BYTES 524288
#define WAL_AUTOCHECKPOINT_PAGES 100

static const char *SCHEMA_SQL =
  /* Identity of every drive we have ever seen, keyed by serial so history
   * survives reboots, renaming and replugging. */
  "CREATE TABLE IF NOT EXISTS drive ("
  "  drive_id   TEXT PRIMARY KEY,"
  "  model      TEXT,"
  "  size       INTEGER,"
  "  removable  INTEGER DEFAULT 0,"
  "  user_tbw   INTEGER DEFAULT 0,"
  "  last_seen  INTEGER"
  ");"

  /* Raw SMART samples. */
  "CREATE TABLE IF NOT EXISTS drive_sample ("
  "  drive_id      TEXT NOT NULL,"
  "  ts            INTEGER NOT NULL,"
  "  bytes_written INTEGER,"
  "  percent_used  INTEGER,"
  "  temperature   INTEGER,"
  "  PRIMARY KEY (drive_id, ts)"
  ") WITHOUT ROWID;"

  /* Hourly and daily rollups. Same shape; a tier column would only make every
   * query carry a filter it does not need. */
  "CREATE TABLE IF NOT EXISTS drive_hourly ("
  "  drive_id      TEXT NOT NULL,"
  "  ts            INTEGER NOT NULL,"
  "  bytes_written INTEGER,"
  "  percent_used  INTEGER,"
  "  temperature   INTEGER,"
  "  PRIMARY KEY (drive_id, ts)"
  ") WITHOUT ROWID;"

  "CREATE TABLE IF NOT EXISTS drive_daily ("
  "  drive_id      TEXT NOT NULL,"
  "  ts            INTEGER NOT NULL,"
  "  bytes_written INTEGER,"
  "  percent_used  INTEGER,"
  "  temperature   INTEGER,"
  "  PRIMARY KEY (drive_id, ts)"
  ") WITHOUT ROWID;"

  /*
   * Blame is stored as per-day *deltas* accumulated per writer, not as
   * samples. The counters it comes from reset when a process exits, so a
   * total would be meaningless; only the increments survive. Aggregating on
   * write also keeps this table tiny regardless of sampling frequency.
   */
  "CREATE TABLE IF NOT EXISTS blame_daily ("
  "  drive_id TEXT NOT NULL,"
  "  day      INTEGER NOT NULL,"
  "  name     TEXT NOT NULL,"
  "  wbytes   INTEGER NOT NULL DEFAULT 0,"
  "  exact    INTEGER NOT NULL DEFAULT 0,"
  "  PRIMARY KEY (drive_id, day, name)"
  ") WITHOUT ROWID;"

  "CREATE TABLE IF NOT EXISTS battery_daily ("
  "  battery_id     TEXT NOT NULL,"
  "  day            INTEGER NOT NULL,"
  "  health_percent INTEGER,"
  "  cycle_count    INTEGER,"
  "  PRIMARY KEY (battery_id, day)"
  ") WITHOUT ROWID;"

  "CREATE TABLE IF NOT EXISTS meta ("
  "  key   TEXT PRIMARY KEY,"
  "  value TEXT"
  ");";


static void
drive_state_free (DriveState *state)
{
  g_free (state->model);
  g_free (state);
}


static void
pending_blame_free (PendingBlame *pending)
{
  g_free (pending->drive_id);
  g_free (pending->name);
  g_free (pending);
}


static int64_t
db_now (GsmHealthDb *self)
{
  if (self->clock != NULL)
    return self->clock (self->clock_data);

  return g_get_real_time () / G_USEC_PER_SEC;
}


void
gsm_health_db_set_clock (GsmHealthDb    *self,
                         GsmHealthClock  clock,
                         gpointer        user_data)
{
  g_return_if_fail (GSM_IS_HEALTH_DB (self));

  self->clock = clock;
  self->clock_data = user_data;
}


static void
gsm_health_db_finalize (GObject *object)
{
  GsmHealthDb *self = GSM_HEALTH_DB (object);

  if (self->db != NULL) {
    if (self->in_transaction)
      sqlite3_exec (self->db, "COMMIT", NULL, NULL, NULL);

    sqlite3_close (self->db);
    self->db = NULL;
  }

  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->pending_blame, g_hash_table_unref);
  g_clear_pointer (&self->drive_state, g_hash_table_unref);
  g_clear_pointer (&self->battery_state, g_hash_table_unref);

  G_OBJECT_CLASS (gsm_health_db_parent_class)->finalize (object);
}


static void
gsm_health_db_class_init (GsmHealthDbClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gsm_health_db_finalize;
}


static void
gsm_health_db_init (GsmHealthDb *self)
{
  self->pending_blame =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify) pending_blame_free);
  self->drive_state =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify) drive_state_free);
  self->battery_state =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}


static char *
default_db_path (void)
{
  g_autofree char *dir = g_build_filename (g_get_user_data_dir (),
                                           "gnome-system-monitor-rh",
                                           NULL);

  g_mkdir_with_parents (dir, 0700);

  return g_build_filename (dir, "health.db", NULL);
}


static gboolean
exec_sql (GsmHealthDb  *self,
          const char   *sql,
          GError      **error)
{
  char *sqlite_error = NULL;

  if (sqlite3_exec (self->db, sql, NULL, NULL, &sqlite_error) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "health database: %s", sqlite_error);
    sqlite3_free (sqlite_error);
    return FALSE;
  }

  return TRUE;
}


GsmHealthDb *
gsm_health_db_new (const char  *path,
                   GError     **error)
{
  g_autoptr (GsmHealthDb) self = g_object_new (GSM_TYPE_HEALTH_DB, NULL);

  self->path = path ? g_strdup (path) : default_db_path ();

  if (sqlite3_open (self->path, &self->db) != SQLITE_OK) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "cannot open %s: %s", self->path, sqlite3_errmsg (self->db));
    return NULL;
  }

  /*
   * The UI and the background sampler hold this file open at the same time.
   * WAL lets the UI read while the sampler writes, and a busy timeout means
   * a momentarily locked database waits rather than throwing.
   */
  sqlite3_busy_timeout (self->db, 3000);

  if (!exec_sql (self, "PRAGMA journal_mode=WAL;", error) ||
      !exec_sql (self, "PRAGMA synchronous=NORMAL;", error) ||
      !exec_sql (self, "PRAGMA journal_size_limit=" G_STRINGIFY (WAL_SIZE_LIMIT_BYTES) ";", error) ||
      !exec_sql (self, "PRAGMA wal_autocheckpoint=" G_STRINGIFY (WAL_AUTOCHECKPOINT_PAGES) ";", error) ||
      !exec_sql (self, SCHEMA_SQL, error))
    return NULL;

  {
    g_autofree char *version =
      g_strdup_printf ("INSERT OR IGNORE INTO meta (key, value) "
                       "VALUES ('schema_version', '%d');", SCHEMA_VERSION);

    if (!exec_sql (self, version, error))
      return NULL;
  }

  return g_steal_pointer (&self);
}


/* Batch a tick's worth of inserts so we pay one fsync, not dozens. */
static void
begin (GsmHealthDb *self)
{
  if (self->in_transaction)
    return;

  if (sqlite3_exec (self->db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK)
    self->in_transaction = TRUE;
}


gboolean
gsm_health_db_commit (GsmHealthDb  *self,
                      GError      **error)
{
  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), FALSE);

  if (!self->in_transaction)
    return TRUE;

  self->in_transaction = FALSE;

  return exec_sql (self, "COMMIT", error);
}


/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

static gboolean
drive_identity_changed (const DriveState *state,
                        GsmDrive         *drive,
                        int64_t           now)
{
  return state == NULL ||
         now - state->identity_ts >= DRIVE_REWRITE_SECS ||
         state->size != gsm_drive_get_size (drive) ||
         (state->removable != 0) != (gsm_drive_get_removable (drive) != 0) ||
         g_strcmp0 (state->model, gsm_drive_get_model (drive)) != 0;
}


static gboolean
drive_sample_due (const DriveState *state,
                  GsmDrive         *drive,
                  int64_t           now)
{
  return state == NULL ||
         !state->has_sample ||
         now - state->sample_ts >= DRIVE_REWRITE_SECS ||
         state->bytes_written != gsm_drive_get_bytes_written (drive) ||
         state->percent_used != gsm_drive_get_percent_used (drive) ||
         state->temperature != gsm_drive_get_temperature (drive);
}


void
gsm_health_db_record_drive (GsmHealthDb *self,
                            GsmDrive    *drive)
{
  sqlite3_stmt *stmt = NULL;
  const char *drive_id;
  DriveState *state;
  gboolean write_identity;
  gboolean write_sample;
  int64_t now;

  g_return_if_fail (GSM_IS_HEALTH_DB (self));
  g_return_if_fail (GSM_IS_DRIVE (drive));

  drive_id = gsm_drive_get_id (drive);
  if (drive_id == NULL)
    return;

  now = db_now (self);
  state = g_hash_table_lookup (self->drive_state, drive_id);

  /*
   * A health *sample* only makes sense when we actually read health.
   * Recording zeroes would draw a cliff to the floor in the wear chart every
   * time a drive was locked or absent.
   */
  write_identity = drive_identity_changed (state, drive, now);
  write_sample = !(gsm_drive_get_bytes_written (drive) == 0 &&
                   gsm_drive_get_percent_used (drive) < 0) &&
                 drive_sample_due (state, drive, now);

  if (!write_identity && !write_sample)
    return;

  if (state == NULL) {
    state = g_new0 (DriveState, 1);
    g_hash_table_replace (self->drive_state, g_strdup (drive_id), state);
  }

  begin (self);

  /*
   * The identity row is written whenever it changed or is an hour old — a
   * drive we cannot read health from still needs to be remembered, so a
   * removable that is currently unplugged (or a drive still awaiting an
   * unlock) can be shown with an honest "last seen" rather than vanishing
   * from the list entirely.
   */
  if (write_identity &&
      sqlite3_prepare_v2 (self->db,
                          "INSERT INTO drive (drive_id, model, size, removable, last_seen) "
                          "VALUES (?1, ?2, ?3, ?4, ?5) "
                          "ON CONFLICT (drive_id) DO UPDATE SET "
                          "  model=excluded.model,"
                          "  size=excluded.size,"
                          "  last_seen=excluded.last_seen",
                          -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (stmt, 2, gsm_drive_get_model (drive), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 3, (sqlite3_int64) gsm_drive_get_size (drive));
    sqlite3_bind_int (stmt, 4, gsm_drive_get_removable (drive));
    sqlite3_bind_int64 (stmt, 5, now);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);

    g_free (state->model);
    state->model = g_strdup (gsm_drive_get_model (drive));
    state->size = gsm_drive_get_size (drive);
    state->removable = gsm_drive_get_removable (drive) != 0;
    state->identity_ts = now;
  }

  if (!write_sample)
    return;

  stmt = NULL;
  if (sqlite3_prepare_v2 (self->db,
                          "INSERT OR REPLACE INTO drive_sample "
                          "(drive_id, ts, bytes_written, percent_used, temperature) "
                          "VALUES (?1, ?2, ?3, ?4, ?5)",
                          -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, now);
    sqlite3_bind_int64 (stmt, 3, (sqlite3_int64) gsm_drive_get_bytes_written (drive));
    sqlite3_bind_int (stmt, 4, gsm_drive_get_percent_used (drive));
    sqlite3_bind_int (stmt, 5, gsm_drive_get_temperature (drive));
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);

    state->has_sample = TRUE;
    state->sample_ts = now;
    state->bytes_written = gsm_drive_get_bytes_written (drive);
    state->percent_used = gsm_drive_get_percent_used (drive);
    state->temperature = gsm_drive_get_temperature (drive);
  }
}


void
gsm_health_db_record_blame (GsmHealthDb *self,
                            const char  *drive_id,
                            const char  *name,
                            uint64_t     delta_wbytes,
                            gboolean     exact)
{
  g_autofree char *key = NULL;
  PendingBlame *pending;
  int64_t day;

  g_return_if_fail (GSM_IS_HEALTH_DB (self));

  if (drive_id == NULL || name == NULL || delta_wbytes == 0)
    return;

  /* The day is taken now, when the delta is read, so a flush after midnight
   * still fills the bucket of the day the bytes were written in. */
  day = db_now (self) / 86400;
  key = g_strdup_printf ("%s\n%" G_GINT64_FORMAT "\n%s", drive_id, day, name);

  pending = g_hash_table_lookup (self->pending_blame, key);
  if (pending == NULL) {
    pending = g_new0 (PendingBlame, 1);
    pending->drive_id = g_strdup (drive_id);
    pending->day = day;
    pending->name = g_strdup (name);
    pending->exact = exact;
    g_hash_table_insert (self->pending_blame, g_steal_pointer (&key), pending);
  } else {
    /* `exact` is sticky-pessimistic: one heuristic contribution makes the
     * whole row an estimate, so the UI never overstates its confidence. */
    pending->exact = pending->exact && exact;
  }

  pending->wbytes += delta_wbytes;
}


static void
write_pending_blame (GsmHealthDb *self)
{
  GHashTableIter iter;
  PendingBlame *pending;
  sqlite3_stmt *stmt = NULL;

  if (g_hash_table_size (self->pending_blame) == 0)
    return;

  /* Added to the stored day's total; `exact` combined with MIN against what
   * earlier flushes stored, for the same reason as above. */
  if (sqlite3_prepare_v2 (self->db,
                          "INSERT INTO blame_daily (drive_id, day, name, wbytes, exact) "
                          "VALUES (?1, ?2, ?3, ?4, ?5) "
                          "ON CONFLICT (drive_id, day, name) DO UPDATE SET "
                          "  wbytes = wbytes + excluded.wbytes,"
                          "  exact  = MIN (exact, excluded.exact)",
                          -1, &stmt, NULL) != SQLITE_OK)
    return;

  g_hash_table_iter_init (&iter, self->pending_blame);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &pending)) {
    sqlite3_bind_text (stmt, 1, pending->drive_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 2, pending->day);
    sqlite3_bind_text (stmt, 3, pending->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 4, (sqlite3_int64) pending->wbytes);
    sqlite3_bind_int (stmt, 5, pending->exact ? 1 : 0);
    sqlite3_step (stmt);
    sqlite3_reset (stmt);
  }

  sqlite3_finalize (stmt);
}


void
gsm_health_db_record_battery (GsmHealthDb *self,
                              const char  *battery_id,
                              int          health_percent,
                              int          cycle_count)
{
  sqlite3_stmt *stmt = NULL;
  BatteryState *state;
  int64_t day;

  g_return_if_fail (GSM_IS_HEALTH_DB (self));

  if (battery_id == NULL || health_percent < 0)
    return;

  day = db_now (self) / 86400;

  /* Battery wear moves on the scale of weeks; one row per day is plenty, and
   * the same reading is not written twice. */
  state = g_hash_table_lookup (self->battery_state, battery_id);
  if (state != NULL && state->day == day &&
      state->health_percent == health_percent &&
      state->cycle_count == cycle_count)
    return;

  begin (self);

  if (sqlite3_prepare_v2 (self->db,
                          "INSERT OR REPLACE INTO battery_daily "
                          "(battery_id, day, health_percent, cycle_count) "
                          "VALUES (?1, ?2, ?3, ?4)",
                          -1, &stmt, NULL) != SQLITE_OK)
    return;

  sqlite3_bind_text (stmt, 1, battery_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, day);
  sqlite3_bind_int (stmt, 3, health_percent);
  sqlite3_bind_int (stmt, 4, cycle_count);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  if (state == NULL) {
    state = g_new0 (BatteryState, 1);
    g_hash_table_replace (self->battery_state, g_strdup (battery_id), state);
  }
  state->day = day;
  state->health_percent = health_percent;
  state->cycle_count = cycle_count;
}


/*
 * Write everything held in memory in one short transaction and note when.
 * The write transaction is open only inside this call, so the UI's own
 * writes (a rated TBW typed into a drive card) never wait on the sampler.
 */
gboolean
gsm_health_db_flush (GsmHealthDb  *self,
                     GError      **error)
{
  sqlite3_stmt *stmt = NULL;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), FALSE);

  begin (self);

  write_pending_blame (self);

  if (sqlite3_prepare_v2 (self->db,
                          "INSERT OR REPLACE INTO meta (key, value) "
                          "VALUES ('last_flush', ?1)",
                          -1, &stmt, NULL) == SQLITE_OK) {
    g_autofree char *value =
      g_strdup_printf ("%" G_GINT64_FORMAT, db_now (self));

    sqlite3_bind_text (stmt, 1, value, -1, SQLITE_TRANSIENT);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
  }

  if (!gsm_health_db_commit (self, error)) {
    sqlite3_exec (self->db, "ROLLBACK", NULL, NULL, NULL);

    /* Keep the pending deltas for the next flush, but forget what we
     * believe reached storage so identity and samples are written again. */
    g_hash_table_remove_all (self->drive_state);
    g_hash_table_remove_all (self->battery_state);
    return FALSE;
  }

  g_hash_table_remove_all (self->pending_blame);

  return TRUE;
}


int64_t
gsm_health_db_get_last_flush (GsmHealthDb *self)
{
  sqlite3_stmt *stmt = NULL;
  int64_t out = 0;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), 0);

  if (sqlite3_prepare_v2 (self->db,
                          "SELECT CAST (value AS INTEGER) FROM meta "
                          " WHERE key = 'last_flush'",
                          -1, &stmt, NULL) != SQLITE_OK)
    return 0;

  if (sqlite3_step (stmt) == SQLITE_ROW)
    out = sqlite3_column_int64 (stmt, 0);

  sqlite3_finalize (stmt);

  return out > 0 ? out : 0;
}


/* ------------------------------------------------------------------ */
/* Rollups                                                             */
/* ------------------------------------------------------------------ */

void
gsm_health_db_roll_up (GsmHealthDb *self)
{
  int64_t now;
  g_autofree char *sql = NULL;

  g_return_if_fail (GSM_IS_HEALTH_DB (self));

  now = db_now (self);

  if (self->last_roll_up != 0 && now - self->last_roll_up < ROLL_UP_SECS)
    return;
  self->last_roll_up = now;

  begin (self);

  /*
   * Fold aged-out raw samples into hourly buckets, then drop them.
   *
   * bytes_written and percent_used are monotonic lifetime counters, so the
   * right summary of an hour is its *last* value, not its average — averaging
   * a counter would drag the curve backwards and understate wear. MAX() gives
   * us that last value for free. Temperature is a genuine instantaneous
   * reading, so there the peak is what matters: a drive that touched 70C for
   * ten minutes is the interesting fact, not its comfortable hourly mean.
   */
  sql = g_strdup_printf (
    "INSERT OR REPLACE INTO drive_hourly (drive_id, ts, bytes_written, percent_used, temperature) "
    "SELECT drive_id, (ts / 3600) * 3600, MAX (bytes_written), MAX (percent_used), MAX (temperature) "
    "  FROM drive_sample WHERE ts < %" G_GINT64_FORMAT
    "  GROUP BY drive_id, ts / 3600;"
    "DELETE FROM drive_sample WHERE ts < %" G_GINT64_FORMAT ";",
    now - RAW_RETENTION_SECS, now - RAW_RETENTION_SECS);

  sqlite3_exec (self->db, sql, NULL, NULL, NULL);
  g_clear_pointer (&sql, g_free);

  sql = g_strdup_printf (
    "INSERT OR REPLACE INTO drive_daily (drive_id, ts, bytes_written, percent_used, temperature) "
    "SELECT drive_id, (ts / 86400) * 86400, MAX (bytes_written), MAX (percent_used), MAX (temperature) "
    "  FROM drive_hourly WHERE ts < %" G_GINT64_FORMAT
    "  GROUP BY drive_id, ts / 86400;"
    "DELETE FROM drive_hourly WHERE ts < %" G_GINT64_FORMAT ";",
    now - HOURLY_RETENTION_SECS, now - HOURLY_RETENTION_SECS);

  sqlite3_exec (self->db, sql, NULL, NULL, NULL);
}


/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

void
gsm_blame_row_free (GsmBlameRow *row)
{
  if (row == NULL)
    return;

  g_free (row->name);
  g_free (row);
}


/* How far back a given range reaches. The tier a row lives in depends only on
 * its age, not on which range is being asked for, so the query spans all
 * three and this just picks the cutoff. */
static int64_t
range_since (GsmHealthDb *self, GsmHistoryRange range)
{
  int64_t now = db_now (self);

  switch (range) {
    case GSM_HISTORY_RANGE_DAY:
      return now - (24 * 3600);
    case GSM_HISTORY_RANGE_MONTH:
      return now - (30 * 24 * 3600);
    case GSM_HISTORY_RANGE_YEAR:
    default:
      return now - (365 * 24 * 3600);
  }
}


GArray *
gsm_health_db_get_history (GsmHealthDb     *self,
                           const char      *drive_id,
                           GsmHistoryRange  range)
{
  GArray *points = g_array_new (FALSE, FALSE, sizeof (GsmHistoryPoint));
  sqlite3_stmt *stmt = NULL;
  int64_t since = range_since (self, range);
  g_autofree char *sql = NULL;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), points);
  g_return_val_if_fail (drive_id != NULL, points);

  /*
   * Union all three tiers rather than reading only the one that matches the
   * window. Data does not reach drive_hourly until it is 48 hours old, so a
   * 30-day query against that table alone would show nothing at all for the
   * first two days of tracking — the chart would sit empty precisely when a
   * new user is looking at it to see whether the feature works.
   *
   * The tiers never overlap (rollup deletes what it promotes), so a plain
   * UNION ALL cannot double-count.
   *
   * The table name comes from our own fixed set, never from user input, so
   * interpolating it is safe; values are still bound.
   */
  sql = g_strdup_printf (
    "SELECT ts, bytes_written, percent_used, temperature FROM drive_sample "
    "  WHERE drive_id = ?1 AND ts >= ?2 "
    "UNION ALL "
    "SELECT ts, bytes_written, percent_used, temperature FROM drive_hourly "
    "  WHERE drive_id = ?1 AND ts >= ?2 "
    "UNION ALL "
    "SELECT ts, bytes_written, percent_used, temperature FROM drive_daily "
    "  WHERE drive_id = ?1 AND ts >= ?2 "
    "ORDER BY ts ASC");

  if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return points;

  sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, since);

  while (sqlite3_step (stmt) == SQLITE_ROW) {
    GsmHistoryPoint p;

    p.ts = sqlite3_column_int64 (stmt, 0);
    p.bytes_written = (uint64_t) sqlite3_column_int64 (stmt, 1);
    p.percent_used = sqlite3_column_int (stmt, 2);
    p.temperature = sqlite3_column_int (stmt, 3);

    g_array_append_val (points, p);
  }

  sqlite3_finalize (stmt);

  return points;
}


GPtrArray *
gsm_health_db_get_blame (GsmHealthDb     *self,
                         const char      *drive_id,
                         GsmHistoryRange  range)
{
  GPtrArray *rows =
    g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_blame_row_free);
  sqlite3_stmt *stmt = NULL;
  int64_t today = db_now (self) / 86400;
  int days;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), rows);
  g_return_val_if_fail (drive_id != NULL, rows);

  switch (range) {
    case GSM_HISTORY_RANGE_DAY:   days = 1;   break;
    case GSM_HISTORY_RANGE_MONTH: days = 30;  break;
    case GSM_HISTORY_RANGE_YEAR:
    default:                      days = 365; break;
  }

  if (sqlite3_prepare_v2 (self->db,
                          "SELECT name, SUM (wbytes) AS total, MIN (exact) "
                          "  FROM blame_daily "
                          " WHERE drive_id = ?1 AND day >= ?2 "
                          " GROUP BY name ORDER BY total DESC LIMIT 25",
                          -1, &stmt, NULL) != SQLITE_OK)
    return rows;

  sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, today - days + 1);

  while (sqlite3_step (stmt) == SQLITE_ROW) {
    GsmBlameRow *row = g_new0 (GsmBlameRow, 1);

    row->name = g_strdup ((const char *) sqlite3_column_text (stmt, 0));
    row->wbytes = (uint64_t) sqlite3_column_int64 (stmt, 1);
    row->exact = sqlite3_column_int (stmt, 2) != 0;

    g_ptr_array_add (rows, row);
  }

  sqlite3_finalize (stmt);

  return rows;
}


/*
 * We need at least this much history before a write rate means anything.
 * Extrapolating a decade-long projection from an hour of a laptop being idle
 * (or from an hour of a big build) would be worse than saying nothing.
 */
#define MIN_HISTORY_SECS (6 * 3600)

double
gsm_health_db_get_write_rate (GsmHealthDb *self,
                              const char  *drive_id,
                              int          days)
{
  sqlite3_stmt *stmt = NULL;
  int64_t now = db_now (self);
  int64_t since = now - ((int64_t) days * 24 * 3600);
  double rate = -1;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), -1);
  g_return_val_if_fail (drive_id != NULL, -1);

  /*
   * Take the first and last lifetime-written counters in the window and
   * divide the difference by the time actually spanned — not by the nominal
   * window. If the machine was off for three weeks of a thirty-day window,
   * the rate is over the days we actually observed, otherwise we would
   * understate it badly and promise the drive a lifetime it will not have.
   *
   * The three tiers are unioned because a 30-day window straddles them.
   */
  if (sqlite3_prepare_v2 (self->db,
                          "WITH w AS ("
                          "  SELECT ts, bytes_written FROM drive_sample "
                          "   WHERE drive_id = ?1 AND ts >= ?2 AND bytes_written > 0"
                          "  UNION ALL"
                          "  SELECT ts, bytes_written FROM drive_hourly "
                          "   WHERE drive_id = ?1 AND ts >= ?2 AND bytes_written > 0"
                          "  UNION ALL"
                          "  SELECT ts, bytes_written FROM drive_daily "
                          "   WHERE drive_id = ?1 AND ts >= ?2 AND bytes_written > 0"
                          ")"
                          "SELECT MIN (ts), MAX (ts), MIN (bytes_written), MAX (bytes_written) "
                          "  FROM w",
                          -1, &stmt, NULL) != SQLITE_OK)
    return -1;

  sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, since);

  if (sqlite3_step (stmt) == SQLITE_ROW &&
      sqlite3_column_type (stmt, 0) != SQLITE_NULL) {
    int64_t first_ts = sqlite3_column_int64 (stmt, 0);
    int64_t last_ts = sqlite3_column_int64 (stmt, 1);
    uint64_t first_bytes = (uint64_t) sqlite3_column_int64 (stmt, 2);
    uint64_t last_bytes = (uint64_t) sqlite3_column_int64 (stmt, 3);
    int64_t span = last_ts - first_ts;

    if (span >= MIN_HISTORY_SECS && last_bytes >= first_bytes) {
      double bytes = (double) (last_bytes - first_bytes);
      double span_days = (double) span / 86400.0;

      rate = bytes / span_days;
    }
  }

  sqlite3_finalize (stmt);

  return rate;
}


uint64_t
gsm_health_db_get_user_tbw (GsmHealthDb *self,
                            const char  *drive_id)
{
  sqlite3_stmt *stmt = NULL;
  uint64_t out = 0;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), 0);

  if (sqlite3_prepare_v2 (self->db,
                          "SELECT user_tbw FROM drive WHERE drive_id = ?1",
                          -1, &stmt, NULL) != SQLITE_OK)
    return 0;

  sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);

  if (sqlite3_step (stmt) == SQLITE_ROW)
    out = (uint64_t) sqlite3_column_int64 (stmt, 0);

  sqlite3_finalize (stmt);

  return out;
}


void
gsm_health_db_set_user_tbw (GsmHealthDb *self,
                            const char  *drive_id,
                            uint64_t     rated_tbw)
{
  sqlite3_stmt *stmt = NULL;

  g_return_if_fail (GSM_IS_HEALTH_DB (self));
  g_return_if_fail (drive_id != NULL);

  begin (self);

  if (sqlite3_prepare_v2 (self->db,
                          "INSERT INTO drive (drive_id, user_tbw) VALUES (?1, ?2) "
                          "ON CONFLICT (drive_id) DO UPDATE SET user_tbw = excluded.user_tbw",
                          -1, &stmt, NULL) != SQLITE_OK)
    return;

  sqlite3_bind_text (stmt, 1, drive_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64 (stmt, 2, (sqlite3_int64) rated_tbw);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  gsm_health_db_commit (self, NULL);
}


int64_t
gsm_health_db_get_oldest_sample (GsmHealthDb *self)
{
  sqlite3_stmt *stmt = NULL;
  int64_t out = 0;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), 0);

  if (sqlite3_prepare_v2 (self->db,
                          "SELECT MIN (t) FROM ("
                          "  SELECT MIN (ts) AS t FROM drive_daily"
                          "  UNION ALL SELECT MIN (ts) FROM drive_hourly"
                          "  UNION ALL SELECT MIN (ts) FROM drive_sample)",
                          -1, &stmt, NULL) != SQLITE_OK)
    return 0;

  if (sqlite3_step (stmt) == SQLITE_ROW &&
      sqlite3_column_type (stmt, 0) != SQLITE_NULL)
    out = sqlite3_column_int64 (stmt, 0);

  sqlite3_finalize (stmt);

  return out;
}


uint64_t
gsm_health_db_get_size_bytes (GsmHealthDb *self)
{
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileInfo) info = NULL;

  g_return_val_if_fail (GSM_IS_HEALTH_DB (self), 0);

  file = g_file_new_for_path (self->path);
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info == NULL)
    return 0;

  return (uint64_t) g_file_info_get_size (info);
}


void
gsm_health_db_clear (GsmHealthDb *self)
{
  g_return_if_fail (GSM_IS_HEALTH_DB (self));

  gsm_health_db_commit (self, NULL);

  /* The user asked for this record to be gone, so actually give the space
   * back rather than leaving it in free pages. */
  sqlite3_exec (self->db,
                "DELETE FROM drive_sample;"
                "DELETE FROM drive_hourly;"
                "DELETE FROM drive_daily;"
                "DELETE FROM blame_daily;"
                "DELETE FROM battery_daily;",
                NULL, NULL, NULL);
  sqlite3_exec (self->db, "VACUUM", NULL, NULL, NULL);
}
