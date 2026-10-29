/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The health sampler's write budget, measured rather than eyeballed.
 *
 * Bytes are counted by a counting SQLite VFS (https://www.sqlite.org/vfs.html)
 * that wraps the default one and adds up every xWrite: the database, the
 * write-ahead log and any journal all reach storage through it, so nothing
 * SQLite writes by itself escapes the count, checkpoints included. The one
 * file it cannot see is the write-ahead log's shared-memory index
 * (https://www.sqlite.org/walformat.html#walindexformat), which SQLite maps
 * into memory instead of writing; each commit may dirty up to two of its 4 KiB
 * pages, and that allowance is added to the figure.
 *
 * The same synthetic machine (ten writers, one drive whose lifetime counter
 * moves, one battery) is run twice through a clock the test controls: once
 * through the sampler's pattern (read every 15 s, write every 10 minutes) and
 * once by replaying the SQL of the earlier pattern (one commit per 15 s
 * tick), as the figure to beat. Both are warmed up for 49 simulated hours
 * first so the hourly and daily roll-ups are in their steady state, then
 * measured over 24 consecutive hours.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "gsm-drive.h"
#include "gsm-health-db.h"

#define TICK_SECS         15
#define FLUSH_SECS        600
#define WARMUP_HOURS      49
#define MEASURED_HOURS    24
#define WRITER_COUNT      10
#define BUDGET_BYTES      (256 * 1024)
#define SHM_ALLOWANCE     (2 * 4096)

/* ------------------------------------------------------------------ */
/* A counting VFS                                                      */
/* ------------------------------------------------------------------ */

static sqlite3_vfs *real_vfs;
static sqlite3_vfs counting_vfs;
static sqlite3_io_methods counting_io;
static guint64 vfs_write_bytes;
static guint64 vfs_write_calls;

typedef struct {
  sqlite3_file base;
  sqlite3_file real[1];       /* the real file follows, szOsFile long */
} CountingFile;

#define REAL(file) (((CountingFile *) (file))->real)

static int
count_close (sqlite3_file *file)
{
  int result = REAL (file)->pMethods->xClose (REAL (file));

  file->pMethods = NULL;

  return result;
}

static int
count_read (sqlite3_file *file, void *buf, int amount, sqlite3_int64 offset)
{
  return REAL (file)->pMethods->xRead (REAL (file), buf, amount, offset);
}

static int
count_write (sqlite3_file *file, const void *buf, int amount, sqlite3_int64 offset)
{
  vfs_write_bytes += (guint64) amount;
  vfs_write_calls++;

  return REAL (file)->pMethods->xWrite (REAL (file), buf, amount, offset);
}

static int
count_truncate (sqlite3_file *file, sqlite3_int64 size)
{
  return REAL (file)->pMethods->xTruncate (REAL (file), size);
}

static int
count_sync (sqlite3_file *file, int flags)
{
  return REAL (file)->pMethods->xSync (REAL (file), flags);
}

static int
count_file_size (sqlite3_file *file, sqlite3_int64 *size)
{
  return REAL (file)->pMethods->xFileSize (REAL (file), size);
}

static int
count_lock (sqlite3_file *file, int level)
{
  return REAL (file)->pMethods->xLock (REAL (file), level);
}

static int
count_unlock (sqlite3_file *file, int level)
{
  return REAL (file)->pMethods->xUnlock (REAL (file), level);
}

static int
count_check_reserved (sqlite3_file *file, int *out)
{
  return REAL (file)->pMethods->xCheckReservedLock (REAL (file), out);
}

static int
count_file_control (sqlite3_file *file, int op, void *arg)
{
  return REAL (file)->pMethods->xFileControl (REAL (file), op, arg);
}

static int
count_sector_size (sqlite3_file *file)
{
  return REAL (file)->pMethods->xSectorSize (REAL (file));
}

static int
count_device_characteristics (sqlite3_file *file)
{
  return REAL (file)->pMethods->xDeviceCharacteristics (REAL (file));
}

static int
count_shm_map (sqlite3_file *file, int page, int size, int extend, void volatile **out)
{
  return REAL (file)->pMethods->xShmMap (REAL (file), page, size, extend, out);
}

static int
count_shm_lock (sqlite3_file *file, int offset, int n, int flags)
{
  return REAL (file)->pMethods->xShmLock (REAL (file), offset, n, flags);
}

static void
count_shm_barrier (sqlite3_file *file)
{
  REAL (file)->pMethods->xShmBarrier (REAL (file));
}

static int
count_shm_unmap (sqlite3_file *file, int delete_flag)
{
  return REAL (file)->pMethods->xShmUnmap (REAL (file), delete_flag);
}

static int
count_open (sqlite3_vfs *vfs, const char *name, sqlite3_file *file,
            int flags, int *out_flags)
{
  int result;

  (void) vfs;

  memset (file, 0, sizeof (CountingFile));
  result = real_vfs->xOpen (real_vfs, name, REAL (file), flags, out_flags);

  if (result == SQLITE_OK && REAL (file)->pMethods != NULL)
    file->pMethods = &counting_io;

  return result;
}

static int
count_delete (sqlite3_vfs *vfs, const char *name, int sync_dir)
{
  (void) vfs;
  return real_vfs->xDelete (real_vfs, name, sync_dir);
}

static int
count_access (sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
  (void) vfs;
  return real_vfs->xAccess (real_vfs, name, flags, out);
}

static int
count_full_pathname (sqlite3_vfs *vfs, const char *name, int n, char *out)
{
  (void) vfs;
  return real_vfs->xFullPathname (real_vfs, name, n, out);
}

static int
count_randomness (sqlite3_vfs *vfs, int n, char *out)
{
  (void) vfs;
  return real_vfs->xRandomness (real_vfs, n, out);
}

static int
count_sleep (sqlite3_vfs *vfs, int micros)
{
  (void) vfs;
  return real_vfs->xSleep (real_vfs, micros);
}

static int
count_current_time (sqlite3_vfs *vfs, double *out)
{
  (void) vfs;
  return real_vfs->xCurrentTime (real_vfs, out);
}

static int
count_last_error (sqlite3_vfs *vfs, int n, char *out)
{
  (void) vfs;
  return real_vfs->xGetLastError (real_vfs, n, out);
}

static void
install_counting_vfs (void)
{
  if (real_vfs != NULL)
    return;

  real_vfs = sqlite3_vfs_find (NULL);
  g_assert_nonnull (real_vfs);

  counting_io.iVersion = 2;
  counting_io.xClose = count_close;
  counting_io.xRead = count_read;
  counting_io.xWrite = count_write;
  counting_io.xTruncate = count_truncate;
  counting_io.xSync = count_sync;
  counting_io.xFileSize = count_file_size;
  counting_io.xLock = count_lock;
  counting_io.xUnlock = count_unlock;
  counting_io.xCheckReservedLock = count_check_reserved;
  counting_io.xFileControl = count_file_control;
  counting_io.xSectorSize = count_sector_size;
  counting_io.xDeviceCharacteristics = count_device_characteristics;
  counting_io.xShmMap = count_shm_map;
  counting_io.xShmLock = count_shm_lock;
  counting_io.xShmBarrier = count_shm_barrier;
  counting_io.xShmUnmap = count_shm_unmap;

  counting_vfs.iVersion = 1;
  counting_vfs.szOsFile = (int) (sizeof (CountingFile) - sizeof (sqlite3_file) +
                                 (gsize) real_vfs->szOsFile);
  counting_vfs.mxPathname = real_vfs->mxPathname;
  counting_vfs.zName = "gsm-counting";
  counting_vfs.xOpen = count_open;
  counting_vfs.xDelete = count_delete;
  counting_vfs.xAccess = count_access;
  counting_vfs.xFullPathname = count_full_pathname;
  counting_vfs.xRandomness = count_randomness;
  counting_vfs.xSleep = count_sleep;
  counting_vfs.xCurrentTime = count_current_time;
  counting_vfs.xGetLastError = count_last_error;

  g_assert_cmpint (sqlite3_vfs_register (&counting_vfs, 1), ==, SQLITE_OK);
}

/* ------------------------------------------------------------------ */
/* A clock and a synthetic machine                                     */
/* ------------------------------------------------------------------ */

static int64_t sim_now;

static int64_t
sim_clock (gpointer data)
{
  (void) data;

  return sim_now;
}

/* 2026-10-01 00:00:00 UTC, so a 73-hour run crosses several midnights. */
#define SIM_START 1790812800
#define SIM_DAY_BEFORE 20727   /* SIM_START / 86400 */
#define SIM_DAY_AFTER  20728

static guint32 rng_state;

static guint32
next_random (void)
{
  rng_state = rng_state * 1664525u + 1013904223u;

  return rng_state >> 8;
}

static const char *writer_names[WRITER_COUNT] = {
  "firefox", "code", "gnome-shell", "systemd-journald", "tracker-miner",
  "docker", "kworker", "pipewire", "cc1plus", "claude",
};

/* Writers 0-4 are named by cgroup accounting (exact); 5-9 by /proc/<pid>/io
 * (a heuristic, so estimated). */
static gboolean
writer_is_exact (int writer)
{
  return writer < 5;
}

static uint64_t drive_lifetime_bytes;
static uint64_t blame_total[WRITER_COUNT];

/* What one 15 s tick reads from the machine: a delta per writer, most
 * ticks, and a drive counter that keeps climbing. */
static uint64_t
draw_delta (int writer)
{
  guint32 r = next_random ();

  if (r % 10 < 4)
    return 0;

  return (uint64_t) (1 + (r >> 4) % 4000) * 1024 * (uint64_t) (writer + 1);
}

static GsmDrive *
make_drive (void)
{
  GsmDriveInfo info = { 0 };
  GsmDrive *drive = gsm_drive_new ("SERIAL-0001");

  info.model = (char *) "Test NVMe 1TB";
  info.serial = (char *) "SERIAL-0001";
  info.present = TRUE;
  info.size = 1000204886016ull;
  info.bytes_written = drive_lifetime_bytes;
  info.temperature = 38 + (int) (drive_lifetime_bytes >> 33) % 5;
  info.percent_used = 3;
  info.spare_percent = -1;
  info.spare_threshold = -1;
  info.smart_source = GSM_SMART_SOURCE_UDISKS;
  gsm_drive_apply_info (drive, &info);

  return drive;
}

/* ------------------------------------------------------------------ */
/* Scratch files                                                       */
/* ------------------------------------------------------------------ */

/* The runtime directory is tmpfs; the test never writes to the disk itself. */
static char *
make_test_dir (void)
{
  g_autofree char *template = NULL;
  const char *base = g_get_user_runtime_dir ();
  char *dir;

  if (base == NULL || !g_file_test (base, G_FILE_TEST_IS_DIR))
    base = "/dev/shm";

  template = g_build_filename (base, "gsm-health-budget-XXXXXX", NULL);
  dir = g_mkdtemp (template);
  g_assert_nonnull (dir);

  return g_strdup (dir);
}

static void
remove_test_dir (char *dir)
{
  static const char *const suffixes[] = { "", "-wal", "-shm", "-journal" };

  for (guint i = 0; i < G_N_ELEMENTS (suffixes); i++) {
    g_autofree char *name = g_strdup_printf ("health.db%s", suffixes[i]);
    g_autofree char *path = g_build_filename (dir, name, NULL);

    g_remove (path);
  }

  g_rmdir (dir);
  g_free (dir);
}

typedef struct {
  char          *dir;
  char          *path;
  GsmHealthDb   *db;
} Fixture;

static void
fixture_open (Fixture *f)
{
  g_autoptr (GError) error = NULL;

  f->dir = make_test_dir ();
  f->path = g_build_filename (f->dir, "health.db", NULL);
  sim_now = SIM_START + 12 * 3600;

  f->db = gsm_health_db_new (f->path, &error);
  g_assert_no_error (error);
  gsm_health_db_set_clock (f->db, sim_clock, NULL);
}

static void
fixture_close (Fixture *f)
{
  g_clear_object (&f->db);
  g_clear_pointer (&f->path, g_free);
  remove_test_dir (g_steal_pointer (&f->dir));
}

static sqlite3 *
open_raw (const char *path)
{
  sqlite3 *raw = NULL;

  g_assert_cmpint (sqlite3_open (path, &raw), ==, SQLITE_OK);
  sqlite3_busy_timeout (raw, 0);

  return raw;
}

static int
count_rows (sqlite3 *raw, const char *sql)
{
  sqlite3_stmt *stmt = NULL;
  int out = -1;

  g_assert_cmpint (sqlite3_prepare_v2 (raw, sql, -1, &stmt, NULL), ==, SQLITE_OK);
  if (sqlite3_step (stmt) == SQLITE_ROW)
    out = sqlite3_column_int (stmt, 0);
  sqlite3_finalize (stmt);

  return out;
}

/* ------------------------------------------------------------------ */
/* The sampler's pattern                                               */
/* ------------------------------------------------------------------ */

static void
sampler_tick (GsmHealthDb *db, gboolean flush_now)
{
  sim_now += TICK_SECS;
  drive_lifetime_bytes += 2ull * 1024 * 1024;

  for (int w = 0; w < WRITER_COUNT; w++) {
    uint64_t delta = draw_delta (w);

    blame_total[w] += delta;
    gsm_health_db_record_blame (db, "SERIAL-0001", writer_names[w], delta,
                                writer_is_exact (w));
  }

  if (flush_now) {
    g_autoptr (GsmDrive) drive = make_drive ();
    g_autoptr (GError) error = NULL;

    gsm_health_db_record_drive (db, drive);
    gsm_health_db_record_battery (db, "BAT0", 91, 212);
    gsm_health_db_roll_up (db);
    g_assert_true (gsm_health_db_flush (db, &error));
    g_assert_no_error (error);
  }
}

/* Runs `hours` simulated hours; returns the number of flushes. */
static guint
run_sampler_hours (GsmHealthDb *db, guint hours)
{
  guint ticks = hours * 3600 / TICK_SECS;
  guint flushes = 0;

  for (guint i = 1; i <= ticks; i++) {
    gboolean flush_now = (i % (FLUSH_SECS / TICK_SECS)) == 0;

    sampler_tick (db, flush_now);
    if (flush_now)
      flushes++;
  }

  return flushes;
}

/* ------------------------------------------------------------------ */
/* The previous pattern, replayed as SQL                               */
/* ------------------------------------------------------------------ */

static void
old_tick (sqlite3 *raw)
{
  sqlite3_stmt *stmt = NULL;
  int64_t day;
  char *sql;

  sim_now += TICK_SECS;
  drive_lifetime_bytes += 2ull * 1024 * 1024;
  day = sim_now / 86400;

  g_assert_cmpint (sqlite3_exec (raw, "BEGIN", NULL, NULL, NULL), ==, SQLITE_OK);

  sql = sqlite3_mprintf (
    "INSERT INTO drive (drive_id, model, size, removable, last_seen) "
    "VALUES ('SERIAL-0001', 'Test NVMe 1TB', 1000204886016, 0, %lld) "
    "ON CONFLICT (drive_id) DO UPDATE SET model=excluded.model,"
    " size=excluded.size, last_seen=excluded.last_seen;"
    "INSERT OR REPLACE INTO drive_sample "
    "(drive_id, ts, bytes_written, percent_used, temperature) "
    "VALUES ('SERIAL-0001', %lld, %llu, 3, 40);"
    "INSERT OR REPLACE INTO battery_daily "
    "(battery_id, day, health_percent, cycle_count) "
    "VALUES ('BAT0', %lld, 91, 212);",
    (long long) sim_now, (long long) sim_now,
    (unsigned long long) drive_lifetime_bytes, (long long) day);
  sqlite3_exec (raw, sql, NULL, NULL, NULL);
  sqlite3_free (sql);

  g_assert_cmpint (sqlite3_prepare_v2 (raw,
                                       "INSERT INTO blame_daily (drive_id, day, name, wbytes, exact) "
                                       "VALUES ('SERIAL-0001', ?1, ?2, ?3, ?4) "
                                       "ON CONFLICT (drive_id, day, name) DO UPDATE SET "
                                       "  wbytes = wbytes + excluded.wbytes,"
                                       "  exact  = MIN (exact, excluded.exact)",
                                       -1, &stmt, NULL), ==, SQLITE_OK);
  for (int w = 0; w < WRITER_COUNT; w++) {
    uint64_t delta = draw_delta (w);

    if (delta == 0)
      continue;

    blame_total[w] += delta;
    sqlite3_bind_int64 (stmt, 1, day);
    sqlite3_bind_text (stmt, 2, writer_names[w], -1, SQLITE_STATIC);
    sqlite3_bind_int64 (stmt, 3, (sqlite3_int64) delta);
    sqlite3_bind_int (stmt, 4, writer_is_exact (w));
    sqlite3_step (stmt);
    sqlite3_reset (stmt);
  }
  sqlite3_finalize (stmt);

  sql = sqlite3_mprintf (
    "INSERT OR REPLACE INTO drive_hourly (drive_id, ts, bytes_written, percent_used, temperature) "
    "SELECT drive_id, (ts / 3600) * 3600, MAX (bytes_written), MAX (percent_used), MAX (temperature) "
    "  FROM drive_sample WHERE ts < %lld GROUP BY drive_id, ts / 3600;"
    "DELETE FROM drive_sample WHERE ts < %lld;"
    "INSERT OR REPLACE INTO drive_daily (drive_id, ts, bytes_written, percent_used, temperature) "
    "SELECT drive_id, (ts / 86400) * 86400, MAX (bytes_written), MAX (percent_used), MAX (temperature) "
    "  FROM drive_hourly WHERE ts < %lld GROUP BY drive_id, ts / 86400;"
    "DELETE FROM drive_hourly WHERE ts < %lld;",
    (long long) (sim_now - 48 * 3600), (long long) (sim_now - 48 * 3600),
    (long long) (sim_now - 90ll * 24 * 3600), (long long) (sim_now - 90ll * 24 * 3600));
  sqlite3_exec (raw, sql, NULL, NULL, NULL);
  sqlite3_free (sql);

  g_assert_cmpint (sqlite3_exec (raw, "COMMIT", NULL, NULL, NULL), ==, SQLITE_OK);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void
reset_machine (void)
{
  rng_state = 12345;
  drive_lifetime_bytes = 5ull * 1000 * 1000 * 1000 * 1000 / 100;
  memset (blame_total, 0, sizeof blame_total);
  sim_now = SIM_START;
}

static void
test_a_simulated_hour_stays_within_the_write_budget (void)
{
  Fixture f = { 0 };
  guint64 before_bytes;
  guint64 first_hour_bytes;
  guint64 total_bytes;
  guint warm_flushes;
  guint measured_flushes = 0;

  reset_machine ();
  fixture_open (&f);
  sim_now = SIM_START;

  warm_flushes = run_sampler_hours (f.db, WARMUP_HOURS);
  g_assert_cmpuint (warm_flushes, >, 0);

  before_bytes = vfs_write_bytes;
  measured_flushes = run_sampler_hours (f.db, 1);
  first_hour_bytes = vfs_write_bytes - before_bytes +
                     (guint64) measured_flushes * SHM_ALLOWANCE;

  before_bytes = vfs_write_bytes;
  measured_flushes = run_sampler_hours (f.db, MEASURED_HOURS);
  total_bytes = vfs_write_bytes - before_bytes +
                (guint64) measured_flushes * SHM_ALLOWANCE;

  g_print ("# new pattern: %" G_GUINT64_FORMAT " bytes in a simulated hour, "
           "%" G_GUINT64_FORMAT " bytes per hour averaged over %d hours "
           "(budget %d)\n",
           first_hour_bytes, total_bytes / MEASURED_HOURS, MEASURED_HOURS,
           BUDGET_BYTES);

  g_assert_cmpuint (first_hour_bytes, <=, BUDGET_BYTES);
  g_assert_cmpuint (total_bytes / MEASURED_HOURS, <=, BUDGET_BYTES);

  fixture_close (&f);
}

static void
test_the_previous_per_tick_commit_pattern_is_far_over_the_budget (void)
{
  Fixture f = { 0 };
  sqlite3 *raw;
  guint64 before_bytes;
  guint64 total_bytes;
  guint ticks;

  reset_machine ();
  fixture_open (&f);
  sim_now = SIM_START;

  raw = open_raw (f.path);
  g_assert_cmpint (sqlite3_exec (raw, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL),
                   ==, SQLITE_OK);

  ticks = WARMUP_HOURS * 3600 / TICK_SECS;
  for (guint i = 0; i < ticks; i++)
    old_tick (raw);

  before_bytes = vfs_write_bytes;
  ticks = MEASURED_HOURS * 3600 / TICK_SECS;
  for (guint i = 0; i < ticks; i++)
    old_tick (raw);

  /* The commits of the old pattern are one per tick, so the shared-memory
   * allowance scales with them. */
  total_bytes = vfs_write_bytes - before_bytes + (guint64) ticks * SHM_ALLOWANCE;

  g_print ("# per-tick baseline: %" G_GUINT64_FORMAT " bytes per hour averaged "
           "over %d hours (%" G_GUINT64_FORMAT " without the shared-memory "
           "allowance)\n", total_bytes / MEASURED_HOURS, MEASURED_HOURS,
           (total_bytes - (guint64) ticks * SHM_ALLOWANCE) / MEASURED_HOURS);

  g_assert_cmpuint (total_bytes / MEASURED_HOURS, >, 4ull * BUDGET_BYTES);

  sqlite3_close (raw);
  fixture_close (&f);
}

static void
test_the_readers_return_what_the_sampler_wrote (void)
{
  Fixture f = { 0 };
  g_autoptr (GPtrArray) blame = NULL;
  g_autoptr (GArray) history = NULL;
  double rate;

  reset_machine ();
  fixture_open (&f);
  sim_now = SIM_START;

  run_sampler_hours (f.db, 30);

  /* Nothing newer than the last flush is expected: 30 h is a whole number
   * of flushes, so everything read has been written. */
  g_assert_cmpint (gsm_health_db_get_last_flush (f.db), ==, sim_now);

  blame = gsm_health_db_get_blame (f.db, "SERIAL-0001", GSM_HISTORY_RANGE_YEAR);
  g_assert_cmpuint (blame->len, ==, WRITER_COUNT);

  for (guint i = 0; i < blame->len; i++) {
    GsmBlameRow *row = g_ptr_array_index (blame, i);

    for (int w = 0; w < WRITER_COUNT; w++) {
      if (g_strcmp0 (row->name, writer_names[w]) != 0)
        continue;

      g_assert_cmpuint (row->wbytes, ==, blame_total[w]);
      g_assert_cmpint (row->exact, ==, writer_is_exact (w));
    }
  }

  history = gsm_health_db_get_history (f.db, "SERIAL-0001", GSM_HISTORY_RANGE_MONTH);
  g_assert_cmpuint (history->len, >, 100);

  rate = gsm_health_db_get_write_rate (f.db, "SERIAL-0001", 30);
  g_assert_cmpfloat (rate, >, 0);

  g_assert_cmpint (gsm_health_db_get_oldest_sample (f.db), >, 0);

  fixture_close (&f);
}

static void
test_blame_is_held_in_memory_until_the_flush (void)
{
  Fixture f = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) blame = NULL;
  sqlite3 *raw;
  guint64 before_bytes;

  reset_machine ();
  fixture_open (&f);
  raw = open_raw (f.path);

  g_assert_cmpint (gsm_health_db_get_last_flush (f.db), ==, 0);

  before_bytes = vfs_write_bytes;
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "firefox", 4096, TRUE);
  g_assert_cmpuint (vfs_write_bytes, ==, before_bytes);
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM blame_daily"), ==, 0);

  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_no_error (error);
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM blame_daily"), ==, 1);
  g_assert_cmpint (gsm_health_db_get_last_flush (f.db), ==, sim_now);

  /* The sampler never holds a write transaction between flushes: another
   * connection (the window's own write of a rated TBW) can take it at once,
   * with a busy timeout of zero. */
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "firefox", 4096, TRUE);
  g_assert_cmpint (sqlite3_exec (raw, "BEGIN IMMEDIATE", NULL, NULL, NULL), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (raw, "COMMIT", NULL, NULL, NULL), ==, SQLITE_OK);

  sqlite3_close (raw);
  fixture_close (&f);
}

static void
test_a_flush_after_midnight_fills_the_day_the_bytes_were_written_in (void)
{
  Fixture f = { 0 };
  sqlite3 *raw;
  g_autoptr (GPtrArray) day_after = NULL;
  g_autoptr (GPtrArray) both = NULL;
  g_autoptr (GError) error = NULL;
  int64_t midnight = SIM_START + 86400;

  reset_machine ();
  fixture_open (&f);

  sim_now = midnight - 10;
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "backup", 100, TRUE);

  sim_now = midnight + 20;
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "backup", 50, TRUE);

  sim_now = midnight + 30;
  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_no_error (error);

  day_after = gsm_health_db_get_blame (f.db, "SERIAL-0001", GSM_HISTORY_RANGE_DAY);
  g_assert_cmpuint (day_after->len, ==, 1);
  g_assert_cmpuint (((GsmBlameRow *) g_ptr_array_index (day_after, 0))->wbytes, ==, 50);

  both = gsm_health_db_get_blame (f.db, "SERIAL-0001", GSM_HISTORY_RANGE_MONTH);
  g_assert_cmpuint (both->len, ==, 1);
  g_assert_cmpuint (((GsmBlameRow *) g_ptr_array_index (both, 0))->wbytes, ==, 150);

  /* The reader counts everything from a day onward, so the two buckets
   * are read straight from the table. */
  raw = open_raw (f.path);
  g_assert_cmpint (count_rows (raw, "SELECT wbytes FROM blame_daily WHERE day = "
                               G_STRINGIFY (SIM_DAY_BEFORE)), ==, 100);
  g_assert_cmpint (count_rows (raw, "SELECT wbytes FROM blame_daily WHERE day = "
                               G_STRINGIFY (SIM_DAY_AFTER)), ==, 50);
  sqlite3_close (raw);

  fixture_close (&f);
}

static gboolean
blame_exact (GsmHealthDb *db)
{
  g_autoptr (GPtrArray) rows =
    gsm_health_db_get_blame (db, "SERIAL-0001", GSM_HISTORY_RANGE_DAY);

  g_assert_cmpuint (rows->len, ==, 1);

  return ((GsmBlameRow *) g_ptr_array_index (rows, 0))->exact;
}

static void
test_one_estimated_contribution_keeps_the_row_estimated (void)
{
  Fixture f = { 0 };
  g_autoptr (GError) error = NULL;

  reset_machine ();
  fixture_open (&f);

  /* Within one flush: exact, estimated, exact. */
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, TRUE);
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, FALSE);
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, TRUE);
  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_false (blame_exact (f.db));

  /* Across flushes: an exact contribution cannot make it exact again. */
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, TRUE);
  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_false (blame_exact (f.db));

  fixture_close (&f);

  /* And the other way: a row that starts exact stays exact while every
   * contribution is exact, and turns estimated on the first guess. */
  fixture_open (&f);
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, TRUE);
  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_true (blame_exact (f.db));
  gsm_health_db_record_blame (f.db, "SERIAL-0001", "app", 10, FALSE);
  g_assert_true (gsm_health_db_flush (f.db, &error));
  g_assert_false (blame_exact (f.db));

  fixture_close (&f);
}

static void
test_an_unchanged_drive_reading_is_not_written_again_within_the_hour (void)
{
  Fixture f = { 0 };
  g_autoptr (GError) error = NULL;
  sqlite3 *raw;

  reset_machine ();
  drive_lifetime_bytes = 1000;
  fixture_open (&f);
  raw = open_raw (f.path);

  for (int i = 0; i < 3; i++) {
    g_autoptr (GsmDrive) drive = make_drive ();

    gsm_health_db_record_drive (f.db, drive);
    g_assert_true (gsm_health_db_flush (f.db, &error));
    sim_now += FLUSH_SECS;
  }
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM drive_sample"), ==, 1);

  /* A changed counter is a new sample. */
  drive_lifetime_bytes += 1 << 20;
  {
    g_autoptr (GsmDrive) drive = make_drive ();

    gsm_health_db_record_drive (f.db, drive);
    g_assert_true (gsm_health_db_flush (f.db, &error));
  }
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM drive_sample"), ==, 2);

  /* An hour later the same reading is written again, so the chart has no
   * gap and "last seen" is no staler than an hour. */
  sim_now += 3600;
  {
    g_autoptr (GsmDrive) drive = make_drive ();

    gsm_health_db_record_drive (f.db, drive);
    g_assert_true (gsm_health_db_flush (f.db, &error));
  }
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM drive_sample"), ==, 3);
  g_assert_cmpint (count_rows (raw, "SELECT COUNT (*) FROM drive WHERE last_seen = "
                               "(SELECT MAX (ts) FROM drive_sample)"), ==, 1);

  sqlite3_close (raw);
  fixture_close (&f);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  install_counting_vfs ();

  g_test_add_func ("/health-budget/a simulated hour stays within the write budget",
                   test_a_simulated_hour_stays_within_the_write_budget);
  g_test_add_func ("/health-budget/the previous per-tick commit pattern is far over the budget",
                   test_the_previous_per_tick_commit_pattern_is_far_over_the_budget);
  g_test_add_func ("/health-budget/the readers return what the sampler wrote",
                   test_the_readers_return_what_the_sampler_wrote);
  g_test_add_func ("/health-budget/blame is held in memory until the flush",
                   test_blame_is_held_in_memory_until_the_flush);
  g_test_add_func ("/health-budget/a flush after midnight fills the day the bytes were written in",
                   test_a_flush_after_midnight_fills_the_day_the_bytes_were_written_in);
  g_test_add_func ("/health-budget/one estimated contribution keeps the row estimated",
                   test_one_estimated_contribution_keeps_the_row_estimated);
  g_test_add_func ("/health-budget/an unchanged drive reading is not written again within the hour",
                   test_an_unchanged_drive_reading_is_not_written_again_within_the_hour);

  return g_test_run ();
}
