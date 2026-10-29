/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reassigning a proxy writer's bytes to the units that caused them. The
 * journal is replaced by synthetic records fed through the module's source
 * seam, so the expected numbers are fixed: no unit, path or user of any
 * machine appears here. Per drive and writer, the rows must add up to the
 * bytes charged, exactly.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "gsm-health-db.h"
#include "gsm-onbehalf.h"

#define DRIVE "SERIAL-0001"

typedef struct
{
  char    *name;
  char    *via;
  char    *basis;
  uint64_t wbytes;
} Row;

static void
row_free (Row *row)
{
  g_free (row->name);
  g_free (row->via);
  g_free (row->basis);
  g_free (row);
}

static void
collect (const char *drive_id,
         const char *name,
         const char *via,
         const char *basis,
         uint64_t    wbytes,
         gpointer    data)
{
  Row *row = g_new0 (Row, 1);

  (void) drive_id;
  row->name = g_strdup (name);
  row->via = g_strdup (via);
  row->basis = g_strdup (basis);
  row->wbytes = wbytes;
  g_ptr_array_add (data, row);
}

static GPtrArray *
new_rows (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
}

static uint64_t
rows_total (GPtrArray *rows,
            const char *via)
{
  uint64_t total = 0;

  for (guint i = 0; i < rows->len; i++)
    {
      const Row *row = g_ptr_array_index (rows, i);

      if (g_strcmp0 (row->via, via) == 0)
        total += row->wbytes;
    }

  return total;
}

static uint64_t
row_bytes (GPtrArray  *rows,
           const char *name,
           const char *basis)
{
  uint64_t total = 0;

  for (guint i = 0; i < rows->len; i++)
    {
      const Row *row = g_ptr_array_index (rows, i);

      if (g_strcmp0 (row->name, name) == 0 && (basis == NULL || g_strcmp0 (row->basis, basis) == 0))
        total += row->wbytes;
    }

  return total;
}

/* ---- a synthetic journal ---- */

typedef struct
{
  GPtrArray *records;
  guint      next;
  guint      reads;
} Fake;

static GsmJournalRecord *
fake_next (gpointer data)
{
  Fake *fake = data;
  GsmJournalRecord *source;
  GsmJournalRecord *copy;

  if (fake->next >= fake->records->len)
    return NULL;

  source = g_ptr_array_index (fake->records, fake->next++);
  fake->reads++;
  copy = g_new0 (GsmJournalRecord, 1);
  copy->message_id = g_strdup (source->message_id);
  copy->unit = g_strdup (source->unit);
  copy->user_unit = g_strdup (source->user_unit);
  copy->about_unit = g_strdup (source->about_unit);
  copy->about_user_unit = g_strdup (source->about_user_unit);
  copy->coredump_unit = g_strdup (source->coredump_unit);
  copy->coredump_user_unit = g_strdup (source->coredump_user_unit);
  copy->coredump_cgroup = g_strdup (source->coredump_cgroup);
  copy->coredump_proc_maps = g_strdup (source->coredump_proc_maps);
  copy->io_write_bytes = g_strdup (source->io_write_bytes);
  return copy;
}

static char *
fake_end_cursor (gpointer data)
{
  (void) data;
  return g_strdup ("end");
}

static void
fake_close (gpointer data)
{
  Fake *fake = data;

  g_ptr_array_unref (fake->records);
  g_free (fake);
}

static Fake *
new_fake (GsmJournalSource *source)
{
  Fake *fake = g_new0 (Fake, 1);

  fake->records = g_ptr_array_new_with_free_func ((GDestroyNotify) gsm_journal_record_free);
  source->next = fake_next;
  source->end_cursor = fake_end_cursor;
  source->close = fake_close;
  source->data = fake;
  return fake;
}

static void
add_crash (Fake       *fake,
           const char *instance,
           const char *user_unit,
           const char *unit,
           const char *cgroup,
           const char *maps)
{
  GsmJournalRecord *record = g_new0 (GsmJournalRecord, 1);

  record->message_id = g_strdup (GSM_MESSAGE_ID_COREDUMP);
  record->unit = g_strdup (instance);
  record->coredump_user_unit = g_strdup (user_unit);
  record->coredump_unit = g_strdup (unit);
  record->coredump_cgroup = g_strdup (cgroup);
  record->coredump_proc_maps = g_strdup (maps);
  g_ptr_array_add (fake->records, record);
}

static void
add_resources (Fake       *fake,
               const char *instance,
               const char *write_bytes)
{
  GsmJournalRecord *record = g_new0 (GsmJournalRecord, 1);

  record->message_id = g_strdup (GSM_MESSAGE_ID_RESOURCES);
  record->unit = g_strdup ("init.scope");
  record->about_unit = g_strdup (instance);
  record->io_write_bytes = g_strdup (write_bytes);
  g_ptr_array_add (fake->records, record);
}

static void
add_line (Fake       *fake,
          const char *unit,
          const char *user_unit,
          const char *about_unit,
          const char *about_user_unit)
{
  GsmJournalRecord *record = g_new0 (GsmJournalRecord, 1);

  record->unit = g_strdup (unit);
  record->user_unit = g_strdup (user_unit);
  record->about_unit = g_strdup (about_unit);
  record->about_user_unit = g_strdup (about_user_unit);
  g_ptr_array_add (fake->records, record);
}

/* A maps text whose dumped size is `anon_bytes` of anonymous private memory. */
static char *
maps_of (uint64_t anon_bytes)
{
  return g_strdup_printf ("7f0000000000-%lx rw-p 00000000 00:00 0 \n"
                          "7ffc00000000-7ffc00004000 r--p 00000000 00:00 0   [vvar]\n"
                          "7f4000200000-7f4000300000 r--p 00200000 08:01 777  /demo/lib.so\n",
                          0x7f0000000000UL + anon_bytes);
}

/* ---- the tests ---- */

static void
test_a_cgroup_is_recognised_as_a_proxy_writers_subtree (void)
{
  GsmProxyWriter writer;

  g_assert_true (gsm_on_behalf_proxy_for_cgroup ("system.slice/system-systemd\\x2dcoredump.slice", &writer));
  g_assert_cmpint (writer, ==, GSM_PROXY_COREDUMP);
  g_assert_true (gsm_on_behalf_proxy_for_cgroup (
                   "system.slice/system-systemd\\x2dcoredump.slice/systemd-coredump@0-1-0.service", &writer));
  g_assert_cmpint (writer, ==, GSM_PROXY_COREDUMP);
  g_assert_true (gsm_on_behalf_proxy_for_cgroup ("system.slice/systemd-journald.service", &writer));
  g_assert_cmpint (writer, ==, GSM_PROXY_JOURNALD);

  g_assert_false (gsm_on_behalf_proxy_for_cgroup ("system.slice", &writer));
  g_assert_false (gsm_on_behalf_proxy_for_cgroup ("system.slice/systemd-journald.service.d", &writer));
  g_assert_false (gsm_on_behalf_proxy_for_cgroup ("user.slice", &writer));
  g_assert_false (gsm_on_behalf_proxy_for_cgroup (NULL, &writer));
}

static void
test_the_estimate_counts_what_the_default_filter_dumps (void)
{
  const char *maps =
    "55d0c0a00000-55d0c0a21000 rw-p 00000000 00:00 0                          [heap]\n"
    "7f1000000000-7f1000100000 rw-s 00000000 00:01 123 /dev/zero (deleted)\n"
    "7f2000000000-7f2000010000 rw-s 00000000 00:01 5 /memfd:demo (deleted)\n"
    "7f3000000000-7f3000010000 rw-s 00000000 00:01 9 /SYSV00000000 (deleted)\n"
    "7f4000000000-7f4000200000 r-xp 00000000 08:01 777 /demo/lib.so\n"
    "7f4000200000-7f4000300000 r--p 00200000 08:01 777 /demo/lib.so\n"
    "7ffc00000000-7ffc00004000 r--p 00000000 00:00 0   [vvar]\n"
    "7ffc00004000-7ffc00006000 r-xp 00000000 00:00 0   [vdso]\n"
    "7f5000000000-7f5000010000 rw-s 00000000 08:01 31 /demo/shared-file\n"
    "not a maps line\n";

  g_assert_cmpuint (gsm_on_behalf_estimate_core_bytes (maps), ==,
                    0x21000 + 0x100000 + 0x10000 + 0x10000 + 4096);
  g_assert_cmpuint (gsm_on_behalf_estimate_core_bytes (""), ==, 0);
  g_assert_cmpuint (gsm_on_behalf_estimate_core_bytes (NULL), ==, 0);
}

static void
test_crash_bytes_follow_the_estimated_core_sizes (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *big = maps_of(3 * 1048576);
  g_autofree char *small = maps_of(1048576);

  add_crash (fake, "demo-handler@1.service", "demo-875820.scope", "user@1000.service", NULL, big);
  add_crash (fake, "demo-handler@2.service", "other-tool.service", NULL, NULL, small);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 4000000);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 1000003);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (rows_total (rows, gsm_proxy_writer_name (GSM_PROXY_COREDUMP)), ==, 5000003);
  /* 3:1 by core size, the odd byte to the larger remainder. */
  g_assert_cmpuint (row_bytes (rows, "demo", GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL), ==, 3750002);
  g_assert_cmpuint (row_bytes (rows, "other-tool.service", GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL), ==, 1250001);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_an_exact_figure_for_a_crash_beats_the_estimate (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *maps = maps_of(1048576);

  add_crash (fake, "demo-handler@1.service", "alpha.service", NULL, NULL, maps);
  add_crash (fake, "demo-handler@2.service", "beta.service", NULL, NULL, maps);
  add_resources (fake, "demo-handler@1.service", "700000");
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 1000000);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, "alpha.service", GSM_HEALTH_BASIS_COREDUMP_EXACT), ==, 700000);
  g_assert_cmpuint (row_bytes (rows, "beta.service", GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL), ==, 300000);
  g_assert_cmpuint (rows_total (rows, gsm_proxy_writer_name (GSM_PROXY_COREDUMP)), ==, 1000000);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_exact_figures_larger_than_the_charge_are_scaled_to_fit (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();

  add_crash (fake, "demo-handler@1.service", "alpha.service", NULL, NULL, NULL);
  add_crash (fake, "demo-handler@2.service", "beta.service", NULL, NULL, NULL);
  add_resources (fake, "demo-handler@1.service", "900");
  add_resources (fake, "demo-handler@2.service", "300");
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 600);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, "alpha.service", GSM_HEALTH_BASIS_COREDUMP_EXACT), ==, 450);
  g_assert_cmpuint (row_bytes (rows, "beta.service", GSM_HEALTH_BASIS_COREDUMP_EXACT), ==, 150);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_the_originator_is_the_user_unit_then_the_unit_then_the_cgroup (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *maps = maps_of(1048576);

  add_crash (fake, "h@1.service", "first.service", "second.service", "/user.slice/third.service", maps);
  add_crash (fake, "h@2.service", NULL, "second.service", "/user.slice/third.service", maps);
  add_crash (fake, "h@3.service", NULL, NULL, "/user.slice/third.service", maps);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 3000);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, "first.service", NULL), ==, 1000);
  g_assert_cmpuint (row_bytes (rows, "second.service", NULL), ==, 1000);
  g_assert_cmpuint (row_bytes (rows, "third.service", NULL), ==, 1000);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_an_agent_job_scope_is_grouped_by_its_project (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *maps = maps_of(1048576);

  add_crash (fake, "h@1.service", "job-demo\\x2dproj-4242.scope", "user@1000.service",
             "/user.slice/user-1000.slice/user@1000.service/agents.slice/agents-demo\\x2dproj.slice/job-demo\\x2dproj-4242.scope",
             maps);
  add_crash (fake, "h@2.service", "job-demo\\x2dproj-4243.scope", NULL,
             "/user.slice/user-1000.slice/user@1000.service/agents.slice/agents-demo\\x2dproj.slice/job-demo\\x2dproj-4243.scope",
             maps);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 2000);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, "agent jobs: demo-proj", NULL), ==, 2000);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_journald_bytes_split_by_line_counts_with_systemds_lines_counted_for_the_unit (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *maps = maps_of(1048576);

  /* 3 lines from alpha itself, 1 about it from PID 1, 2 from a user unit
   * about beta, 1 by beta, 2 with no unit. */
  for (int i = 0; i < 3; i++)
    add_line (fake, "alpha.service", NULL, NULL, NULL);
  add_line (fake, "init.scope", NULL, "alpha.service", NULL);
  add_line (fake, "user@1000.service", NULL, NULL, "beta.service");
  add_line (fake, "user@1000.service", NULL, NULL, "beta.service");
  add_line (fake, NULL, "beta.service", NULL, NULL);
  add_line (fake, NULL, NULL, NULL, NULL);
  add_line (fake, NULL, NULL, NULL, NULL);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_JOURNALD, 9000);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, "alpha.service", GSM_HEALTH_BASIS_JOURNALD_LINES), ==, 4000);
  g_assert_cmpuint (row_bytes (rows, "beta.service", GSM_HEALTH_BASIS_JOURNALD_LINES), ==, 3000);
  g_assert_cmpuint (row_bytes (rows, GSM_ON_BEHALF_NO_UNIT, GSM_HEALTH_BASIS_JOURNALD_LINES), ==, 2000);
  g_assert_cmpuint (rows_total (rows, gsm_proxy_writer_name (GSM_PROXY_JOURNALD)), ==, 9000);

  (void) maps;
  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_lines_about_a_crash_handler_count_for_the_crasher (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();
  g_autofree char *maps = maps_of(1048576);

  /* "Started" is logged before the crash record it belongs to. */
  add_line (fake, "init.scope", NULL, "demo-handler@7.service", NULL);
  add_crash (fake, "demo-handler@7.service", "crasher.service", NULL, NULL, maps);
  add_line (fake, "demo-handler@7.service", NULL, NULL, NULL);
  add_line (fake, "init.scope", NULL, "demo-handler@7.service", NULL);
  add_line (fake, "quiet.service", NULL, NULL, NULL);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_JOURNALD, 500);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  /* 4 lines for the crasher (two Started, the record, the handler's own), 1 quiet. */
  g_assert_cmpuint (row_bytes (rows, "crasher.service", NULL), ==, 400);
  g_assert_cmpuint (row_bytes (rows, "quiet.service", NULL), ==, 100);

  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_rows_sum_exactly_to_the_charge_for_awkward_numbers (void)
{
  for (uint64_t charged = 1; charged < 5000; charged += 211)
    {
      GsmJournalSource source = { 0 };
      Fake *fake = new_fake (&source);
      GsmOnBehalf *ob = gsm_on_behalf_new ();
      g_autoptr (GPtrArray) rows = new_rows ();

      for (int unit = 0; unit < 7; unit++)
        for (int line = 0; line <= unit; line++)
          {
            g_autofree char *name = g_strdup_printf ("unit-%d.service", unit);

            add_line (fake, name, NULL, NULL, NULL);
          }
      gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_JOURNALD, charged);
      gsm_on_behalf_charge (ob, "SERIAL-0002", GSM_PROXY_JOURNALD, charged + 1);
      gsm_on_behalf_settle (ob, &source, collect, rows);

      g_assert_cmpuint (rows_total (rows, gsm_proxy_writer_name (GSM_PROXY_JOURNALD)), ==, 2 * charged + 1);
      fake_close (fake);
      gsm_on_behalf_free (ob);
    }
}

static void
test_without_journal_access_the_bytes_are_one_unknown_row (void)
{
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();

  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 777);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_JOURNALD, 333);
  gsm_on_behalf_settle (ob, NULL, collect, rows);

  g_assert_cmpuint (rows->len, ==, 2);
  g_assert_cmpuint (row_bytes (rows, GSM_ON_BEHALF_NO_ACCESS, GSM_HEALTH_BASIS_COREDUMP_PROPORTIONAL), ==, 777);
  g_assert_cmpuint (row_bytes (rows, GSM_ON_BEHALF_NO_ACCESS, GSM_HEALTH_BASIS_JOURNALD_LINES), ==, 333);
  gsm_on_behalf_free (ob);
}

static void
test_charged_bytes_with_no_matching_record_are_unknown_not_dropped (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();

  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_COREDUMP, 900);
  gsm_on_behalf_charge (ob, DRIVE, GSM_PROXY_JOURNALD, 100);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (row_bytes (rows, GSM_ON_BEHALF_NO_CRASH_RECORD, NULL), ==, 900);
  g_assert_cmpuint (row_bytes (rows, GSM_ON_BEHALF_NO_JOURNAL_ENTRY, NULL), ==, 100);
  fake_close (fake);
  gsm_on_behalf_free (ob);
}

static void
test_the_journal_is_not_read_when_nothing_was_charged (void)
{
  GsmJournalSource source = { 0 };
  Fake *fake = new_fake (&source);
  GsmOnBehalf *ob = gsm_on_behalf_new ();
  g_autoptr (GPtrArray) rows = new_rows ();

  add_line (fake, "alpha.service", NULL, NULL, NULL);
  gsm_on_behalf_settle (ob, &source, collect, rows);

  g_assert_cmpuint (fake->reads, ==, 0);
  g_assert_cmpuint (rows->len, ==, 0);
  fake_close (fake);
  gsm_on_behalf_free (ob);
}

/* ---- the history database ---- */

typedef struct
{
  char *dir;
  char *path;
} Scratch;

static Scratch
scratch_new (void)
{
  Scratch s;
  g_autofree char *template = g_build_filename (g_get_user_runtime_dir (), "gsm-onbehalf-XXXXXX", NULL);

  s.dir = g_mkdtemp (g_steal_pointer (&template));
  g_assert_nonnull (s.dir);
  s.path = g_build_filename (s.dir, "health.db", NULL);
  return s;
}

static void
scratch_free (Scratch *s)
{
  const char *suffixes[] = { "", "-wal", "-shm" };

  for (guint i = 0; i < G_N_ELEMENTS (suffixes); i++)
    {
      g_autofree char *file = g_strconcat (s->path, suffixes[i], NULL);

      g_remove (file);
    }
  g_rmdir (s->dir);
  g_free (s->dir);
  g_free (s->path);
}

static int64_t
fixed_clock (gpointer data)
{
  return *(int64_t *) data;
}

static void
test_a_version_2_database_gains_power_on_hours_as_unknown (void)
{
  Scratch s = scratch_new ();
  sqlite3 *raw = NULL;
  g_autoptr (GsmHealthDb) db = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GArray) points = NULL;
  g_autofree char *version = NULL;
  int64_t now = 1700000000;

  g_assert_cmpint (sqlite3_open (s.path, &raw), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (raw,
                                 "CREATE TABLE drive_sample (drive_id TEXT NOT NULL, ts INTEGER NOT NULL,"
                                 " bytes_written INTEGER, percent_used INTEGER, temperature INTEGER,"
                                 " PRIMARY KEY (drive_id, ts)) WITHOUT ROWID;"
                                 "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
                                 "INSERT INTO meta VALUES ('schema_version', '2');"
                                 "INSERT INTO drive_sample VALUES ('OLD', 1699999000, 5, 1, 30);",
                                 NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (raw);

  db = gsm_health_db_new (s.path, &error);
  g_assert_no_error (error);
  gsm_health_db_set_clock (db, fixed_clock, &now);

  points = gsm_health_db_get_history (db, "OLD", GSM_HISTORY_RANGE_DAY);
  g_assert_cmpuint (points->len, ==, 1);
  g_assert_cmpint (g_array_index (points, GsmHistoryPoint, 0).power_on_hours, ==, -1);

  version = gsm_health_db_get_meta (db, "schema_version");
  g_assert_cmpstr (version, ==, "3");

  g_clear_object (&db);
  scratch_free (&s);
}

static void
test_power_on_hours_are_stored_per_sample_and_zero_means_unknown (void)
{
  Scratch s = scratch_new ();
  g_autoptr (GsmHealthDb) db = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GArray) points = NULL;
  g_autoptr (GsmDrive) drive = gsm_drive_new ("NEW");
  GsmDriveInfo info = { 0 };
  int64_t now = 1700000000;

  info.serial = g_strdup ("NEW");
  info.model = g_strdup ("Demo");
  info.bytes_written = 4096;
  info.percent_used = 2;
  info.temperature = 31;
  info.power_on_hours = 1234;
  gsm_drive_apply_info (drive, &info);

  db = gsm_health_db_new (s.path, &error);
  g_assert_no_error (error);
  gsm_health_db_set_clock (db, fixed_clock, &now);
  gsm_health_db_record_drive (db, drive);

  now += 7200;
  info.power_on_hours = 0;
  info.bytes_written = 8192;
  {
    g_autoptr (GsmDrive) second = gsm_drive_new ("NEW");

    gsm_drive_apply_info (second, &info);
    gsm_health_db_record_drive (db, second);
  }
  g_assert_true (gsm_health_db_flush (db, &error));

  points = gsm_health_db_get_history (db, "NEW", GSM_HISTORY_RANGE_DAY);
  g_assert_cmpuint (points->len, ==, 2);
  g_assert_cmpint (g_array_index (points, GsmHistoryPoint, 0).power_on_hours, ==, 1234);
  g_assert_cmpint (g_array_index (points, GsmHistoryPoint, 1).power_on_hours, ==, -1);

  g_free (info.serial);
  g_free (info.model);
  g_clear_object (&db);
  scratch_free (&s);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

#define T(name, fn) g_test_add_func ("/health-onbehalf/" name, fn)
  T ("a cgroup is recognised as a proxy writer's subtree", test_a_cgroup_is_recognised_as_a_proxy_writers_subtree);
  T ("the estimate counts what the default filter dumps", test_the_estimate_counts_what_the_default_filter_dumps);
  T ("crash bytes follow the estimated core sizes", test_crash_bytes_follow_the_estimated_core_sizes);
  T ("an exact figure for a crash beats the estimate", test_an_exact_figure_for_a_crash_beats_the_estimate);
  T ("exact figures larger than the charge are scaled to fit", test_exact_figures_larger_than_the_charge_are_scaled_to_fit);
  T ("the originator is the user unit, then the unit, then the cgroup", test_the_originator_is_the_user_unit_then_the_unit_then_the_cgroup);
  T ("an agent job scope is grouped by its project", test_an_agent_job_scope_is_grouped_by_its_project);
  T ("journald bytes split by line counts with systemd's lines counted for the unit", test_journald_bytes_split_by_line_counts_with_systemds_lines_counted_for_the_unit);
  T ("lines about a crash handler count for the crasher", test_lines_about_a_crash_handler_count_for_the_crasher);
  T ("rows sum exactly to the charge for awkward numbers", test_rows_sum_exactly_to_the_charge_for_awkward_numbers);
  T ("without journal access the bytes are one unknown row", test_without_journal_access_the_bytes_are_one_unknown_row);
  T ("charged bytes with no matching record are unknown, not dropped", test_charged_bytes_with_no_matching_record_are_unknown_not_dropped);
  T ("the journal is not read when nothing was charged", test_the_journal_is_not_read_when_nothing_was_charged);
  T ("a version 2 database gains power_on_hours as unknown", test_a_version_2_database_gains_power_on_hours_as_unknown);
  T ("power-on hours are stored per sample and zero means unknown", test_power_on_hours_are_stored_per_sample_and_zero_means_unknown);
#undef T

  return g_test_run ();
}
