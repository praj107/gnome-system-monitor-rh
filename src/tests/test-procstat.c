/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Hand-made /proc/stat files with small round numbers, so every expected
 * figure can be checked by hand; argv[1] is the fixtures directory.
 *   procstat-normal.txt      four CPUs, every field set
 *   procstat-offline-cpu.txt CPU 2 is offline: no "cpu2" line
 *   procstat-guest.txt       guest and guest_nice set (already inside
 *                            user and nice)
 *   procstat-malformed.txt   damaged lines among good ones
 */

#include <glib.h>

#include "../procstat.h"

#define SLOTS 8

static const char *fixtures_dir;

static void
read_fixture (const char  *name,
              GsmCpuTicks *aggregate,
              GsmCpuTicks *cpus)
{
  char *path = g_build_filename (fixtures_dir, name, NULL);

  g_assert_true (gsm_procstat_read (path, aggregate, cpus, SLOTS));
  g_free (path);
}

static void
test_cpus_are_keyed_by_their_number (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];

  read_fixture ("procstat-normal.txt", &aggregate, cpus);

  /* cpu0: total = 100+20+30+2000+100+10+15+5 = 2280, idle = 2000+100 */
  g_assert_true (cpus[0].present);
  g_assert_cmpuint (cpus[0].idle_ticks, ==, 2100);
  g_assert_cmpuint (cpus[0].busy_ticks, ==, 180);
  /* cpu3: total = 3045, idle = 2400 */
  g_assert_true (cpus[3].present);
  g_assert_cmpuint (cpus[3].idle_ticks, ==, 2400);
  g_assert_cmpuint (cpus[3].busy_ticks, ==, 645);
  g_assert_false (cpus[4].present);
  /* The machine line: 1000+200+300+8000+400+50+60+70 = 10080, idle 8400 */
  g_assert_true (aggregate.present);
  g_assert_cmpuint (aggregate.idle_ticks, ==, 8400);
  g_assert_cmpuint (aggregate.busy_ticks, ==, 1680);
}

static void
test_an_offline_cpu_has_no_sample_and_does_not_shift_the_others (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];

  read_fixture ("procstat-offline-cpu.txt", &aggregate, cpus);

  g_assert_true (cpus[0].present);
  g_assert_true (cpus[1].present);
  g_assert_false (cpus[2].present);
  /* The third line of the file is CPU 3, not CPU 2. */
  g_assert_true (cpus[3].present);
  g_assert_cmpuint (cpus[3].idle_ticks, ==, 2210);
  g_assert_cmpuint (cpus[3].busy_ticks, ==, 300 + 60 + 90 + 10 + 15 + 20);
}

static void
test_guest_time_is_not_added_to_the_total_again (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];

  read_fixture ("procstat-guest.txt", &aggregate, cpus);

  /* user 1000 + nice 100 + system 200 are busy; idle 5000; the 800 and 80
   * of guest time are inside user and nice already. */
  g_assert_cmpuint (cpus[0].busy_ticks, ==, 1300);
  g_assert_cmpuint (cpus[0].idle_ticks, ==, 5000);
}

static void
test_irq_softirq_and_steal_count_as_busy_and_iowait_as_idle (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];
  GsmCpuTicks before = { TRUE, 0, 0 };
  double percent = -1.0;

  read_fixture ("procstat-normal.txt", &aggregate, cpus);

  /* cpu1 since boot: busy = 200+40+60+10+15+20 = 345, idle = 1900+90 */
  g_assert_true (gsm_cpu_ticks_busy_percent (&before, &cpus[1], &percent));
  g_assert_cmpfloat_with_epsilon (percent, 100.0 * 345 / (345 + 1990), 1e-9);
}

static void
test_malformed_lines_are_skipped_and_counted (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];
  char *path = g_build_filename (fixtures_dir, "procstat-malformed.txt", NULL);
  char *text = NULL;
  guint n_malformed;

  g_assert_true (g_file_get_contents (path, &text, NULL, NULL));
  n_malformed = gsm_procstat_parse (text, &aggregate, cpus, SLOTS);

  /* cpu1 (not a number), cpu2 (too short) and cpuz (bad name) */
  g_assert_cmpuint (n_malformed, ==, 3);
  g_assert_true (cpus[0].present);
  g_assert_false (cpus[1].present);
  g_assert_false (cpus[2].present);
  g_assert_true (cpus[3].present);
  /* cpu7 is the last of the 8 slots asked for */
  g_assert_true (cpus[7].present);
  g_assert_true (aggregate.present);
  g_free (text);
  g_free (path);
}

static void
test_cpus_beyond_the_slots_are_ignored (void)
{
  GsmCpuTicks aggregate, cpus[2];
  const char *text = "cpu 1 1 1 1\ncpu0 1 1 1 1\ncpu1 1 1 1 1\ncpu2 1 1 1 1\n";

  g_assert_cmpuint (gsm_procstat_parse (text, &aggregate, cpus, 2), ==, 0);
  g_assert_true (cpus[0].present);
  g_assert_true (cpus[1].present);
}

static void
test_an_unreadable_file_leaves_everything_unknown (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];

  g_assert_false (gsm_procstat_read ("/nonexistent/stat", &aggregate, cpus, SLOTS));
  g_assert_false (aggregate.present);
  g_assert_false (cpus[0].present);
}

static void
test_an_overflowing_line_is_malformed_not_wrapped (void)
{
  GsmCpuTicks aggregate, cpus[SLOTS];
  const char *text = "cpu0 18446744073709551615 1 0 0\n";

  g_assert_cmpuint (gsm_procstat_parse (text, &aggregate, cpus, SLOTS), ==, 1);
  g_assert_false (cpus[0].present);
}

static void
test_busy_percent_is_the_busy_share_of_the_interval (void)
{
  GsmCpuTicks before = { TRUE, 1000, 3000 };
  GsmCpuTicks after = { TRUE, 1025, 3075 };
  double percent = -1.0;

  /* 25 busy ticks of 100 elapsed */
  g_assert_true (gsm_cpu_ticks_busy_percent (&before, &after, &percent));
  g_assert_cmpfloat_with_epsilon (percent, 25.0, 1e-9);
}

static void
test_a_fully_idle_interval_is_a_known_zero (void)
{
  GsmCpuTicks before = { TRUE, 1000, 3000 };
  GsmCpuTicks after = { TRUE, 1000, 3100 };
  double percent = -1.0;

  g_assert_true (gsm_cpu_ticks_busy_percent (&before, &after, &percent));
  g_assert_cmpfloat (percent, ==, 0.0);
}

static void
test_a_first_sample_or_an_offline_cpu_is_no_sample (void)
{
  GsmCpuTicks absent = { FALSE, 0, 0 };
  GsmCpuTicks present = { TRUE, 10, 10 };
  double percent = -1.0;

  g_assert_false (gsm_cpu_ticks_busy_percent (&absent, &present, &percent));
  g_assert_false (gsm_cpu_ticks_busy_percent (&present, &absent, &percent));
  g_assert_cmpfloat (percent, ==, -1.0);
}

static void
test_a_counter_that_went_backwards_is_no_sample (void)
{
  GsmCpuTicks before = { TRUE, 1000, 3000 };
  GsmCpuTicks busy_back = { TRUE, 999, 3100 };
  GsmCpuTicks idle_back = { TRUE, 1100, 2999 };
  GsmCpuTicks restarted = { TRUE, 5, 20 };
  double percent = -1.0;

  g_assert_false (gsm_cpu_ticks_busy_percent (&before, &busy_back, &percent));
  g_assert_false (gsm_cpu_ticks_busy_percent (&before, &idle_back, &percent));
  g_assert_false (gsm_cpu_ticks_busy_percent (&before, &restarted, &percent));
}

static void
test_no_elapsed_ticks_is_no_sample_not_a_division_by_zero (void)
{
  GsmCpuTicks before = { TRUE, 1000, 3000 };
  double percent = -1.0;

  g_assert_false (gsm_cpu_ticks_busy_percent (&before, &before, &percent));
}

static void
test_slots_cover_the_highest_present_cpu_number (void)
{
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0-15\n"), ==, 16);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0-3,8-11\n"), ==, 12);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0"), ==, 1);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0,2,5\n"), ==, 6);
}

static void
test_a_malformed_present_list_has_no_slots (void)
{
  g_assert_cmpuint (gsm_cpu_slots_from_list (""), ==, 0);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("\n"), ==, 0);
  g_assert_cmpuint (gsm_cpu_slots_from_list (NULL), ==, 0);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0-"), ==, 0);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("a-b"), ==, 0);
  g_assert_cmpuint (gsm_cpu_slots_from_list ("0-3;5"), ==, 0);
}

static void
test_slot_count_reads_the_present_file_not_the_online_count (void)
{
  char *root = g_build_filename (fixtures_dir, "sysfs-cpus-offline", NULL);

  /* present is 0-11 and online 0-3,8-11 (CPUs 4-7 offline): counting the
   * eight online CPUs would give eight slots and leave CPUs 8-11 with
   * none. */
  g_assert_cmpuint (gsm_cpu_slot_count (root, 8), ==, 12);
  g_free (root);
}

static void
test_slot_count_falls_back_to_the_online_count (void)
{
  g_assert_cmpuint (gsm_cpu_slot_count ("/nonexistent", 6), ==, 6);
  g_assert_cmpuint (gsm_cpu_slot_count ("/nonexistent", 0), ==, 1);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, ==, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/procstat/cpus_are_keyed_by_their_number", test_cpus_are_keyed_by_their_number);
  g_test_add_func ("/procstat/offline_cpu_has_no_sample_and_does_not_shift_the_others",
                   test_an_offline_cpu_has_no_sample_and_does_not_shift_the_others);
  g_test_add_func ("/procstat/guest_time_is_not_added_again", test_guest_time_is_not_added_to_the_total_again);
  g_test_add_func ("/procstat/irq_softirq_steal_busy_iowait_idle",
                   test_irq_softirq_and_steal_count_as_busy_and_iowait_as_idle);
  g_test_add_func ("/procstat/malformed_lines_are_skipped_and_counted", test_malformed_lines_are_skipped_and_counted);
  g_test_add_func ("/procstat/cpus_beyond_the_slots_are_ignored", test_cpus_beyond_the_slots_are_ignored);
  g_test_add_func ("/procstat/unreadable_file_is_unknown", test_an_unreadable_file_leaves_everything_unknown);
  g_test_add_func ("/procstat/overflowing_line_is_malformed", test_an_overflowing_line_is_malformed_not_wrapped);
  g_test_add_func ("/procstat/busy_percent_is_the_busy_share", test_busy_percent_is_the_busy_share_of_the_interval);
  g_test_add_func ("/procstat/fully_idle_interval_is_a_known_zero", test_a_fully_idle_interval_is_a_known_zero);
  g_test_add_func ("/procstat/first_sample_or_offline_cpu_is_no_sample",
                   test_a_first_sample_or_an_offline_cpu_is_no_sample);
  g_test_add_func ("/procstat/counter_that_went_backwards_is_no_sample",
                   test_a_counter_that_went_backwards_is_no_sample);
  g_test_add_func ("/procstat/no_elapsed_ticks_is_no_sample", test_no_elapsed_ticks_is_no_sample_not_a_division_by_zero);

  g_test_add_func ("/procstat/slots_cover_the_highest_present_cpu", test_slots_cover_the_highest_present_cpu_number);
  g_test_add_func ("/procstat/malformed_present_list_has_no_slots", test_a_malformed_present_list_has_no_slots);
  g_test_add_func ("/procstat/slot_count_reads_the_present_file", test_slot_count_reads_the_present_file_not_the_online_count);
  g_test_add_func ("/procstat/slot_count_falls_back_to_online", test_slot_count_falls_back_to_the_online_count);

  return g_test_run ();
}
