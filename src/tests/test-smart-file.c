/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reading the system SMART reader's smart.ini (schema 1, a GLib key file).
 * Every state of the file, and every status of a drive, is an explicit
 * result: a figure the reader did not write is unknown, never zero.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gsm-smart-file.h"

#define NOW 1700000000
#define MAX_AGE 1800

static char *dir;
static char *path;

static void
write_file (const char *text)
{
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
}

static GsmSmartFile *
load_text (const char *text,
           int64_t     now)
{
  write_file (text);
  return gsm_smart_file_load (path, now, MAX_AGE);
}

#define META(at) "[meta]\nschema=1\ngenerated_at=" #at "\nreader_version=1\n"

static void
test_a_missing_file_is_its_own_state (void)
{
  g_autofree char *absent = g_build_filename (dir, "absent.ini", NULL);
  GsmSmartFile *file = gsm_smart_file_load (absent, NOW, MAX_AGE);

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_MISSING);
  g_assert_cmpuint (file->drives->len, ==, 0);
  gsm_smart_file_free (file);
}

static void
test_an_unreadable_path_is_not_called_missing (void)
{
  GsmSmartFile *file = gsm_smart_file_load (dir, NOW, MAX_AGE);   /* a directory */

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_UNREADABLE);
  gsm_smart_file_free (file);
}

static void
test_a_file_that_is_not_a_key_file_is_malformed (void)
{
  GsmSmartFile *file = load_text ("this is { not a key file\n", NOW);

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_MALFORMED);
  gsm_smart_file_free (file);

  file = load_text ("[drive sda]\nstatus=ok\n", NOW);       /* no [meta] */
  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_MALFORMED);
  gsm_smart_file_free (file);

  file = load_text ("[meta]\nschema=1\ngenerated_at=soon\n", NOW);
  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_MALFORMED);
  gsm_smart_file_free (file);
}

static void
test_a_newer_schema_is_not_guessed_at (void)
{
  GsmSmartFile *file = load_text ("[meta]\nschema=2\ngenerated_at=1700000000\n"
                                  "[drive sda]\nstatus=ok\nbytes_written=5\n", NOW);

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_UNKNOWN_SCHEMA);
  g_assert_cmpuint (file->drives->len, ==, 0);
  gsm_smart_file_free (file);
}

static void
test_an_old_snapshot_is_stale_and_gives_no_figures (void)
{
  GsmSmartFile *file = load_text (META (1700000000)
                                  "[drive sda]\nserial=S1\nstatus=ok\nbytes_written=5\n",
                                  NOW + MAX_AGE + 1);
  GsmDriveInfo info = { 0 };

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_STALE);
  g_assert_cmpint (file->generated_at, ==, NOW);
  g_assert_cmpuint (file->drives->len, ==, 1);
  g_assert_false (gsm_smart_file_fill_info (file, gsm_smart_file_find (file, "S1"), &info));
  g_assert_cmpuint (info.bytes_written, ==, 0);
  gsm_smart_file_free (file);
}

static void
test_a_time_far_in_the_future_is_not_fresh (void)
{
  GsmSmartFile *file = load_text (META (1700009999) "[drive sda]\nstatus=ok\n", NOW);

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_MALFORMED);
  gsm_smart_file_free (file);
}

static void
test_a_fresh_drive_gives_its_figures_and_leaves_absent_ones_unknown (void)
{
  GsmSmartFile *file = load_text (META (1700000000)
                                  "\n[drive sda]\nmodel=Demo Disk\nserial=SER-1\nusb_id=0000:0000\n"
                                  "bridge=sat\nbasis=ata\nstatus=ok\n"
                                  "bytes_written=2048000000\npercent_used=3\n"
                                  "power_on_hours=4321\ntemperature_kelvin=310\n",
                                  NOW + 60);
  const GsmSmartFileDrive *drive = gsm_smart_file_find (file, "SER-1");
  GsmDriveInfo info = { 0 };

  g_assert_cmpint (file->state, ==, GSM_SMART_FILE_OK);
  g_assert_nonnull (drive);
  g_assert_cmpint (drive->status, ==, GSM_SMART_DRIVE_OK);
  g_assert_true (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_POWER_ON_HOURS));
  g_assert_false (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_MEDIA_ERRORS));
  g_assert_false (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_BYTES_READ));

  info.percent_used = -1;
  info.temperature = -1;
  g_assert_true (gsm_smart_file_fill_info (file, drive, &info));
  g_assert_cmpuint (info.bytes_written, ==, 2048000000ULL);
  g_assert_cmpuint (info.power_on_hours, ==, 4321);
  g_assert_cmpint (info.percent_used, ==, 3);
  g_assert_cmpint (info.temperature, ==, 37);
  g_assert_cmpuint (info.media_errors, ==, 0);
  g_assert_cmpint (info.smart_source, ==, GSM_SMART_SOURCE_HELPER);
  gsm_smart_file_free (file);
}

static void
test_every_status_other_than_ok_gives_no_figures (void)
{
  const char *words[] = { "asleep", "unsupported", "failed", "garbled", "from-the-future" };
  GsmSmartDriveStatus expect[] = { GSM_SMART_DRIVE_ASLEEP, GSM_SMART_DRIVE_UNSUPPORTED,
                                   GSM_SMART_DRIVE_FAILED, GSM_SMART_DRIVE_GARBLED,
                                   GSM_SMART_DRIVE_UNKNOWN_STATUS };

  for (guint i = 0; i < G_N_ELEMENTS (words); i++)
    {
      g_autofree char *text = g_strdup_printf (META (1700000000)
                                               "[drive sda]\nserial=S1\nstatus=%s\n"
                                               "bytes_written=999\nlast_ok_generated_at=1699999000\n",
                                               words[i]);
      GsmSmartFile *file = load_text (text, NOW);
      const GsmSmartFileDrive *drive = gsm_smart_file_find (file, "S1");
      GsmDriveInfo info = { 0 };

      g_assert_nonnull (drive);
      g_assert_cmpint (drive->status, ==, expect[i]);
      g_assert_cmpint (drive->last_ok_generated_at, ==, 1699999000);
      g_assert_false (gsm_smart_file_fill_info (file, drive, &info));
      g_assert_cmpuint (info.bytes_written, ==, 0);
      gsm_smart_file_free (file);
    }
}

static void
test_a_figure_that_is_not_a_number_is_unknown_not_zero (void)
{
  GsmSmartFile *file = load_text (META (1700000000)
                                  "[drive sda]\nserial=S1\nstatus=ok\n"
                                  "bytes_written=lots\npower_on_hours=-5\nmedia_errors=0\npercent_used=250\n",
                                  NOW);
  const GsmSmartFileDrive *drive = gsm_smart_file_find (file, "S1");
  GsmDriveInfo info = { 0 };

  g_assert_false (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_BYTES_WRITTEN));
  g_assert_false (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_POWER_ON_HOURS));
  g_assert_true (gsm_smart_figure_known (drive, GSM_SMART_FIGURE_MEDIA_ERRORS));      /* a real zero */

  info.percent_used = -1;
  g_assert_true (gsm_smart_file_fill_info (file, drive, &info));
  g_assert_cmpint (info.percent_used, ==, -1);                    /* 250 % is not believed */
  gsm_smart_file_free (file);
}

static void
test_a_drive_is_found_by_serial_or_usb_serial (void)
{
  GsmSmartFile *file = load_text (META (1700000000)
                                  "[drive sda]\nserial=AAA\nstatus=ok\n"
                                  "[drive sdb]\nusb_serial=BBB\nstatus=asleep\n",
                                  NOW);

  g_assert_cmpstr (gsm_smart_file_find (file, "AAA")->name, ==, "sda");
  g_assert_cmpstr (gsm_smart_file_find (file, "  BBB ")->name, ==, "sdb");
  g_assert_null (gsm_smart_file_find (file, "CCC"));
  g_assert_null (gsm_smart_file_find (file, ""));
  gsm_smart_file_free (file);
}

int
main (int   argc,
      char *argv[])
{
  g_autofree char *template = NULL;
  int result;

  g_test_init (&argc, &argv, NULL);

  template = g_build_filename (g_get_user_runtime_dir (), "gsm-smart-file-XXXXXX", NULL);
  dir = g_mkdtemp (g_steal_pointer (&template));
  g_assert_nonnull (dir);
  path = g_build_filename (dir, "smart.ini", NULL);

#define T(name, fn) g_test_add_func ("/smart-file/" name, fn)
  T ("a missing file is its own state", test_a_missing_file_is_its_own_state);
  T ("an unreadable path is not called missing", test_an_unreadable_path_is_not_called_missing);
  T ("a file that is not a key file is malformed", test_a_file_that_is_not_a_key_file_is_malformed);
  T ("a newer schema is not guessed at", test_a_newer_schema_is_not_guessed_at);
  T ("an old snapshot is stale and gives no figures", test_an_old_snapshot_is_stale_and_gives_no_figures);
  T ("a time far in the future is not fresh", test_a_time_far_in_the_future_is_not_fresh);
  T ("a fresh drive gives its figures and leaves absent ones unknown", test_a_fresh_drive_gives_its_figures_and_leaves_absent_ones_unknown);
  T ("every status other than ok gives no figures", test_every_status_other_than_ok_gives_no_figures);
  T ("a figure that is not a number is unknown, not zero", test_a_figure_that_is_not_a_number_is_unknown_not_zero);
  T ("a drive is found by serial or usb serial", test_a_drive_is_found_by_serial_or_usb_serial);
#undef T

  result = g_test_run ();

  g_remove (path);
  g_rmdir (dir);
  g_free (path);
  g_free (dir);
  return result;
}
