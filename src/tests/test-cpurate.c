/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixed numbers, no /proc: 100 ticks per second on one CPU is a total of
 * 100 ticks per second of interval.
 */

#include <glib.h>

#include "../cpurate.h"

static void
test_a_normal_sample_is_its_share_of_the_interval (void)
{
  /* 25 of 400 ticks (four CPUs, one second) of the machine, whole-machine
   * scale: 6.25 %; on the one-CPU scale (400) the same ticks are 25 %. */
  GsmCpuRate rate = gsm_cpu_rate (TRUE, TRUE, 1000, 1025, 400, 100);

  g_assert_true (rate.known);
  g_assert_cmpfloat_with_epsilon (rate.percent, 6.25, 1e-9);

  rate = gsm_cpu_rate (TRUE, TRUE, 1000, 1025, 400, 400);
  g_assert_true (rate.known);
  g_assert_cmpfloat_with_epsilon (rate.percent, 25.0, 1e-9);
}

static void
test_an_idle_process_is_a_known_zero (void)
{
  GsmCpuRate rate = gsm_cpu_rate (TRUE, TRUE, 500, 500, 400, 100);

  g_assert_true (rate.known);
  g_assert_cmpfloat (rate.percent, ==, 0.0);
}

static void
test_a_failed_read_is_unknown_not_a_wrapped_subtraction (void)
{
  /* A failed read reports 0 ticks: 0 - 1000 wraps to about 1.8e19. */
  GsmCpuRate rate = gsm_cpu_rate (FALSE, TRUE, 1000, 0, 400, 100);

  g_assert_false (rate.known);
}

static void
test_a_counter_that_went_backwards_is_unknown (void)
{
  GsmCpuRate rate = gsm_cpu_rate (TRUE, TRUE, 1000, 999, 400, 100);

  g_assert_false (rate.known);
}

static void
test_a_reused_pid_is_unknown_even_when_its_counter_is_larger (void)
{
  /* A different process under the old pid, with more CPU time than the
   * previous one had: the difference is real arithmetic but not a rate. */
  GsmCpuRate rate = gsm_cpu_rate (TRUE, FALSE, 1000, 90000, 400, 100);

  g_assert_false (rate.known);
}

static void
test_a_sample_over_the_cap_is_capped_and_still_known (void)
{
  GsmCpuRate rate = gsm_cpu_rate (TRUE, TRUE, 0, 5000, 400, 100);

  g_assert_true (rate.known);
  g_assert_cmpfloat (rate.percent, ==, 100.0);
}

static void
test_no_interval_is_unknown_not_a_division_by_zero (void)
{
  GsmCpuRate rate = gsm_cpu_rate (TRUE, TRUE, 1000, 1010, 0, 100);

  g_assert_false (rate.known);
}

static void
test_a_kept_counter_seeds_only_the_same_process (void)
{
  /* Same start time, counter ahead: carried over a tree rebuild. */
  g_assert_true (gsm_cpu_baseline_carries (TRUE, 777, 1000, 777, 1025));
  g_assert_true (gsm_cpu_baseline_carries (TRUE, 777, 1000, 777, 1000));
}

static void
test_a_kept_counter_of_a_reused_pid_seeds_nothing (void)
{
  /* Another start time under the same pid, even with a larger counter. */
  g_assert_false (gsm_cpu_baseline_carries (TRUE, 777, 1000, 888, 90000));
}

static void
test_a_kept_counter_that_is_ahead_or_unread_seeds_nothing (void)
{
  g_assert_false (gsm_cpu_baseline_carries (TRUE, 777, 1000, 777, 999));
  g_assert_false (gsm_cpu_baseline_carries (FALSE, 777, 1000, 0, 0));
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cpurate/normal-sample",
                   test_a_normal_sample_is_its_share_of_the_interval);
  g_test_add_func ("/cpurate/idle-is-known-zero",
                   test_an_idle_process_is_a_known_zero);
  g_test_add_func ("/cpurate/failed-read-is-unknown",
                   test_a_failed_read_is_unknown_not_a_wrapped_subtraction);
  g_test_add_func ("/cpurate/backwards-counter-is-unknown",
                   test_a_counter_that_went_backwards_is_unknown);
  g_test_add_func ("/cpurate/reused-pid-is-unknown",
                   test_a_reused_pid_is_unknown_even_when_its_counter_is_larger);
  g_test_add_func ("/cpurate/capped-and-known",
                   test_a_sample_over_the_cap_is_capped_and_still_known);
  g_test_add_func ("/cpurate/no-interval-is-unknown",
                   test_no_interval_is_unknown_not_a_division_by_zero);

  g_test_add_func ("/cpurate/kept-counter-same-process",
                   test_a_kept_counter_seeds_only_the_same_process);
  g_test_add_func ("/cpurate/kept-counter-reused-pid",
                   test_a_kept_counter_of_a_reused_pid_seeds_nothing);
  g_test_add_func ("/cpurate/kept-counter-ahead-or-unread",
                   test_a_kept_counter_that_is_ahead_or_unread_seeds_nothing);

  return g_test_run ();
}
