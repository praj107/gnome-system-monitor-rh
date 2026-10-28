/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixtures pressure-{cpu,memory,io}-{a,b}.txt are real /proc/pressure
 * captures from the development machine (2026-10-28), b taken about two
 * seconds after a. pressure-cpu-no-full.txt is the single-line shape older
 * kernels give for /proc/pressure/cpu. argv[1] is the fixtures directory.
 */

#include <glib.h>

#include "../psi.h"

static const char *fixtures_dir;

static void
read_fixture (const char *name,
              GsmPsi     *out)
{
  char *path = g_build_filename (fixtures_dir, name, NULL);

  g_assert_true (gsm_psi_read (path, out));
  g_free (path);
}

static void
test_parses_some_and_full_lines (void)
{
  GsmPsi psi;

  read_fixture ("pressure-cpu-a.txt", &psi);
  g_assert_true (psi.some.present);
  g_assert_cmpuint (psi.some.total_us, ==, 74156353);
  g_assert_cmpfloat_with_epsilon (psi.some.avg10, 0.12, 1e-9);
  g_assert_true (psi.full.present);
  g_assert_cmpuint (psi.full.total_us, ==, 0);

  read_fixture ("pressure-io-a.txt", &psi);
  g_assert_cmpuint (psi.some.total_us, ==, 10939815);
  g_assert_cmpuint (psi.full.total_us, ==, 9556574);
}

static void
test_missing_full_line_is_unknown_not_zero (void)
{
  GsmPsi psi;
  double percent = -1.0;

  read_fixture ("pressure-cpu-no-full.txt", &psi);
  g_assert_true (psi.some.present);
  g_assert_false (psi.full.present);
  g_assert_false (gsm_psi_window_share (&psi.full, &psi.full, 1000000, &percent));
  g_assert_cmpfloat (percent, ==, -1.0);
}

static void
test_window_share_is_stall_microseconds_over_window_microseconds (void)
{
  GsmPsi a, b;
  double percent = -1.0;

  read_fixture ("pressure-cpu-a.txt", &a);
  read_fixture ("pressure-cpu-b.txt", &b);

  /* (74257091 - 74156353) us stalled in a 2 s window = 5.0369 % */
  g_assert_true (gsm_psi_window_share (&a.some, &b.some, 2000000, &percent));
  g_assert_cmpfloat_with_epsilon (percent, 100.0 * 100738.0 / 2000000.0, 1e-9);

  read_fixture ("pressure-io-a.txt", &a);
  read_fixture ("pressure-io-b.txt", &b);
  g_assert_true (gsm_psi_window_share (&a.full, &b.full, 2000000, &percent));
  g_assert_cmpfloat_with_epsilon (percent, 100.0 * 460.0 / 2000000.0, 1e-9);
}

static void
test_a_counter_that_did_not_move_is_zero_percent (void)
{
  GsmPsi a, b;
  double percent = -1.0;

  read_fixture ("pressure-memory-a.txt", &a);
  read_fixture ("pressure-memory-b.txt", &b);
  g_assert_true (gsm_psi_window_share (&a.some, &b.some, 2000000, &percent));
  g_assert_cmpfloat (percent, ==, 0.0);
  g_assert_true (gsm_psi_window_share (&a.full, &b.full, 2000000, &percent));
  g_assert_cmpfloat (percent, ==, 0.0);
}

static void
test_unusable_windows_are_unknown (void)
{
  GsmPsi a, b;
  double percent = -1.0;

  read_fixture ("pressure-cpu-a.txt", &a);
  read_fixture ("pressure-cpu-b.txt", &b);

  g_assert_false (gsm_psi_window_share (&a.some, &b.some, 0, &percent));
  g_assert_false (gsm_psi_window_share (&a.some, &b.some, -5, &percent));
  /* A counter that went backwards (reset, or the samples swapped). */
  g_assert_false (gsm_psi_window_share (&b.some, &a.some, 2000000, &percent));
  g_assert_cmpfloat (percent, ==, -1.0);
}

static void
test_share_is_capped_at_one_hundred_percent (void)
{
  GsmPsiLine before = { TRUE, 0.0, 1000 };
  GsmPsiLine after = { TRUE, 0.0, 3000000 };
  double percent = -1.0;

  /* Timer jitter can make the stall delta exceed the measured window. */
  g_assert_true (gsm_psi_window_share (&before, &after, 2000000, &percent));
  g_assert_cmpfloat (percent, ==, 100.0);
}

static void
test_unreadable_or_malformed_text_is_not_a_reading (void)
{
  GsmPsi psi;

  g_assert_false (gsm_psi_read ("/nonexistent/pressure/memory", &psi));
  g_assert_false (psi.some.present);
  g_assert_false (gsm_psi_parse (NULL, &psi));
  g_assert_false (gsm_psi_parse ("", &psi));
  g_assert_false (gsm_psi_parse ("some avg10=oops total=12\n", &psi));
  g_assert_false (gsm_psi_parse ("some avg10=1.0 avg60=0 avg300=0\n", &psi)); /* no total */
  g_assert_false (psi.some.present);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/psi/parses-some-and-full-lines", test_parses_some_and_full_lines);
  g_test_add_func ("/psi/missing-full-line-is-unknown-not-zero", test_missing_full_line_is_unknown_not_zero);
  g_test_add_func ("/psi/window-share-is-stall-us-over-window-us", test_window_share_is_stall_microseconds_over_window_microseconds);
  g_test_add_func ("/psi/counter-that-did-not-move-is-zero-percent", test_a_counter_that_did_not_move_is_zero_percent);
  g_test_add_func ("/psi/unusable-windows-are-unknown", test_unusable_windows_are_unknown);
  g_test_add_func ("/psi/share-is-capped-at-100-percent", test_share_is_capped_at_one_hundred_percent);
  g_test_add_func ("/psi/unreadable-or-malformed-text-is-not-a-reading", test_unreadable_or_malformed_text_is_not_a_reading);
  return g_test_run ();
}
