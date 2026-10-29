/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The history graphs' scale, with rates above 2^32 bytes per second (the
 * old 32-bit history truncated those) and the rule that keeps the scale
 * from changing on every refresh.
 */

#include <glib.h>

#include "../graphscale.h"

#define KIB G_GUINT64_CONSTANT (1024)
#define MIB (KIB * 1024)
#define GIB (MIB * 1024)

static void
test_the_scale_is_never_below_one_kib (void)
{
  g_assert_cmpuint (gsm_graph_scale_round (0, FALSE, 0), ==, KIB);
  g_assert_cmpuint (gsm_graph_scale_round (1, FALSE, 0), ==, KIB);
}

static void
test_the_scale_has_headroom_and_one_significant_digit (void)
{
  /* 100.5 KiB * 1.1 = 110.55 KiB -> 111 KiB -> 200 KiB */
  g_assert_cmpuint (gsm_graph_scale_round (102912, FALSE, 0), ==, 200 * KIB);
  /* 10 MiB * 1.1 = 11 MiB -> 20 MiB */
  g_assert_cmpuint (gsm_graph_scale_round (10 * MIB, FALSE, 0), ==, 20 * MIB);
  /* 4 MiB * 1.1 = 4.4 MiB -> 5 MiB */
  g_assert_cmpuint (gsm_graph_scale_round (4 * MIB, FALSE, 0), ==, 5 * MIB);
}

static void
test_a_rate_above_four_gib_per_second_is_not_truncated (void)
{
  /* An NVMe drive reading 7 GB/s: 6 519 000 000 B/s * 1.1 = 7.17e9 B/s
   * = 6.68 GiB -> 7 GiB. A 32-bit value would have wrapped to about
   * 2.2 GB. */
  guint64 rate = G_GUINT64_CONSTANT (6519000000);
  guint64 scale = gsm_graph_scale_round (rate, FALSE, 0);

  g_assert_cmpuint (scale, ==, 7 * GIB);
  g_assert_cmpuint (scale, >, rate);
  g_assert_cmpuint (scale, >, G_MAXUINT32);
}

static void
test_an_absurd_rate_does_not_overflow (void)
{
  guint64 scale = gsm_graph_scale_round (G_MAXUINT64, FALSE, 0);

  g_assert_cmpuint (scale, >=, G_GUINT64_CONSTANT (1) << 60);
  scale = gsm_graph_scale_round (G_MAXUINT64, TRUE, 5);
  g_assert_cmpuint (scale, >=, G_GUINT64_CONSTANT (1) << 57);
}

static void
test_bits_scale_is_whole_ticks_of_a_round_step (void)
{
  /* 1 000 000 B/s = 8 Mbit/s over 4 ticks: 2 Mbit/s a tick (nice ceiling
   * of 2e6 is 2e6), 8 Mbit/s in all = 1 000 000 B/s. */
  g_assert_cmpuint (gsm_graph_scale_round (1000000, TRUE, 4), ==, 1000000);
  /* 1 100 000 B/s = 8.8 Mbit/s: 2.2 Mbit/s a tick -> 5 Mbit/s a tick,
   * 20 Mbit/s in all = 2 500 000 B/s. */
  g_assert_cmpuint (gsm_graph_scale_round (1100000, TRUE, 4), ==, 2500000);
  /* Idle: the floor is 10 kbit/s over 4 ticks: 2.5 kbit/s -> 5 kbit/s a
   * tick, 20 kbit/s = 2500 B/s. */
  g_assert_cmpuint (gsm_graph_scale_round (0, TRUE, 4), ==, 2500);
}

static void
test_a_higher_peak_raises_the_scale_at_once (void)
{
  guint64 history[] = { 100, 50 * MIB, 3 };

  g_assert_cmpuint (gsm_graph_scale_next (history, 3, 20 * MIB, FALSE, 0), ==, 60 * MIB);
}

static void
test_a_small_fall_keeps_the_scale (void)
{
  /* Peak 18 MiB rounds to 20 MiB, equal to the current scale. */
  guint64 history[] = { 18 * MIB, 1, 1 };

  g_assert_cmpuint (gsm_graph_scale_next (history, 3, 20 * MIB, FALSE, 0), ==, 20 * MIB);
  /* Peak 9 MiB rounds to 10 MiB, which is below 80 % of 20 MiB: rescale. */
  history[0] = 9 * MIB;
  g_assert_cmpuint (gsm_graph_scale_next (history, 3, 20 * MIB, FALSE, 0), ==, 10 * MIB);
}

static void
test_a_new_scale_just_below_the_current_one_is_kept (void)
{
  /* 5 MiB is 83 % of 6 MiB (not a scale the rounding makes, but the
   * hysteresis only compares): kept. */
  guint64 history[] = { 4 * MIB, 0 };

  g_assert_cmpuint (gsm_graph_scale_next (history, 2, 6 * MIB, FALSE, 0), ==, 6 * MIB);
}

static void
test_the_newest_value_counts_even_when_it_is_the_last_slot (void)
{
  guint64 history[] = { 0, 0, 0, 3 * GIB };

  g_assert_cmpuint (gsm_graph_scale_next (history, 4, KIB, FALSE, 0), >, 3 * GIB);
}

static void
test_an_empty_history_gives_the_floor (void)
{
  g_assert_cmpuint (gsm_graph_scale_next (NULL, 0, 0, FALSE, 0), ==, KIB);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/graphscale/never_below_one_kib", test_the_scale_is_never_below_one_kib);
  g_test_add_func ("/graphscale/headroom_and_one_significant_digit", test_the_scale_has_headroom_and_one_significant_digit);
  g_test_add_func ("/graphscale/rate_above_four_gib_is_not_truncated", test_a_rate_above_four_gib_per_second_is_not_truncated);
  g_test_add_func ("/graphscale/absurd_rate_does_not_overflow", test_an_absurd_rate_does_not_overflow);
  g_test_add_func ("/graphscale/bits_scale_is_whole_ticks", test_bits_scale_is_whole_ticks_of_a_round_step);
  g_test_add_func ("/graphscale/higher_peak_raises_at_once", test_a_higher_peak_raises_the_scale_at_once);
  g_test_add_func ("/graphscale/small_fall_keeps_the_scale", test_a_small_fall_keeps_the_scale);
  g_test_add_func ("/graphscale/scale_just_below_current_is_kept", test_a_new_scale_just_below_the_current_one_is_kept);
  g_test_add_func ("/graphscale/newest_value_counts", test_the_newest_value_counts_even_when_it_is_the_last_slot);
  g_test_add_func ("/graphscale/empty_history_gives_the_floor", test_an_empty_history_gives_the_floor);

  return g_test_run ();
}
