/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * diskstats-machine.txt is /proc/diskstats from the development machine
 * (2026-10-28, kernel 7.0: eight loop devices, a SATA disk and an NVMe disk
 * with partitions, and zram swap); sysfs-disks/block holds the device
 * directories the kernel lists under /sys/block for them (each holds a stub
 * file so the tree can be committed). diskstats-mixed.txt is a hand-made
 * file with every shape the parser must handle. argv[1] is the fixtures
 * directory.
 */

#include <glib.h>

#include "../diskstats.h"

static const char *fixtures_dir;

static char *
fixture_path (const char *name)
{
  return g_build_filename (fixtures_dir, name, NULL);
}

static void
read_fixture (const char    *diskstats_name,
              GsmDiskTotals *out)
{
  char *diskstats = fixture_path (diskstats_name);
  char *sysfs = fixture_path ("sysfs-disks");

  g_assert_true (gsm_diskstats_read (diskstats, sysfs, out));
  g_free (diskstats);
  g_free (sysfs);
}

static void
test_machine_counts_only_the_two_whole_disks (void)
{
  GsmDiskTotals totals;

  read_fixture ("diskstats-machine.txt", &totals);

  /* sda read 40444 sectors, nvme0n1 read 13029889 and wrote 11418706; the
   * partitions repeat those sectors and loop/zram are RAM. */
  g_assert_cmpuint (totals.n_disks, ==, 2);
  g_assert_cmpuint (totals.n_malformed_lines, ==, 0);
  g_assert_cmpuint (totals.read_bytes, ==, G_GUINT64_CONSTANT (6692010496));
  g_assert_cmpuint (totals.written_bytes, ==, G_GUINT64_CONSTANT (5846377472));
}

static void
test_mixed_file_skips_stacked_ram_partition_and_unlisted_devices (void)
{
  GsmDiskTotals totals;

  read_fixture ("diskstats-mixed.txt", &totals);

  /* Counted: sda (100/50 sectors), sdb (200/70) and nvme0n1 (300/400), the
   * last on a line with 18 fields. Not counted: loop0, dm-0, md0, zram0
   * (by name), sda1 and ghost0 (no /sys/block directory). */
  g_assert_cmpuint (totals.n_disks, ==, 3);
  g_assert_cmpuint (totals.read_bytes, ==, 600 * 512);
  g_assert_cmpuint (totals.written_bytes, ==, 520 * 512);
}

static void
test_short_lines_and_non_numbers_are_malformed_not_zero (void)
{
  GsmDiskTotals totals;

  read_fixture ("diskstats-mixed.txt", &totals);

  /* sdc has too few fields and sde has "12x" for its sectors read: both
   * have /sys/block directories, and both are reported, not summed as 0. */
  g_assert_cmpuint (totals.n_malformed_lines, ==, 2);
}

static void
test_a_line_with_only_fourteen_fields_is_accepted (void)
{
  GsmDiskTotals totals;
  char *sysfs = fixture_path ("sysfs-disks");

  gsm_diskstats_parse ("8 0 sda 1 0 8 0 1 0 16 0 0 0 0\n", sysfs, &totals);
  g_assert_cmpuint (totals.n_disks, ==, 1);
  g_assert_cmpuint (totals.read_bytes, ==, 8 * 512);
  g_assert_cmpuint (totals.written_bytes, ==, 16 * 512);

  /* Thirteen fields is one short. */
  gsm_diskstats_parse ("8 0 sda 1 0 8 0 1 0 16 0 0 0\n", sysfs, &totals);
  g_assert_cmpuint (totals.n_disks, ==, 0);
  g_assert_cmpuint (totals.n_malformed_lines, ==, 1);
  g_free (sysfs);
}

static void
test_sector_counts_that_overflow_bytes_are_malformed (void)
{
  GsmDiskTotals totals;
  char *sysfs = fixture_path ("sysfs-disks");

  gsm_diskstats_parse ("8 0 sda 1 0 18446744073709551615 0 1 0 0 0 0 0 0\n", sysfs, &totals);
  g_assert_cmpuint (totals.n_disks, ==, 0);
  g_assert_cmpuint (totals.n_malformed_lines, ==, 1);
  g_free (sysfs);
}

static void
test_device_set_hash_ignores_line_order_and_tells_sets_apart (void)
{
  GsmDiskTotals forward, backward, one_less;
  char *sysfs = fixture_path ("sysfs-disks");

  gsm_diskstats_parse ("8 0 sda 1 0 8 0 1 0 8 0 0 0 0\n8 16 sdb 1 0 8 0 1 0 8 0 0 0 0\n", sysfs, &forward);
  gsm_diskstats_parse ("8 16 sdb 1 0 8 0 1 0 8 0 0 0 0\n8 0 sda 1 0 8 0 1 0 8 0 0 0 0\n", sysfs, &backward);
  gsm_diskstats_parse ("8 0 sda 1 0 8 0 1 0 8 0 0 0 0\n", sysfs, &one_less);

  g_assert_cmpuint (forward.device_set_hash, ==, backward.device_set_hash);
  g_assert_cmpuint (forward.device_set_hash, !=, one_less.device_set_hash);
  g_free (sysfs);
}

static void
test_unreadable_file_is_not_a_reading (void)
{
  GsmDiskTotals totals;
  char *sysfs = fixture_path ("sysfs-disks");
  char *missing = fixture_path ("no-such-diskstats");

  g_assert_false (gsm_diskstats_read (missing, sysfs, &totals));
  g_assert_cmpuint (totals.n_disks, ==, 0);

  /* An empty file is readable and has no disks: a reading of nothing. */
  gsm_diskstats_parse ("", sysfs, &totals);
  g_assert_cmpuint (totals.n_disks, ==, 0);
  g_assert_cmpuint (totals.n_malformed_lines, ==, 0);
  g_free (missing);
  g_free (sysfs);
}

static void
test_names_that_could_escape_the_sysfs_root_are_not_disks (void)
{
  char *sysfs = fixture_path ("sysfs-disks");

  g_assert_true (gsm_diskstats_is_physical_disk ("sda", sysfs));
  g_assert_false (gsm_diskstats_is_physical_disk ("", sysfs));
  g_assert_false (gsm_diskstats_is_physical_disk ("..", sysfs));
  g_assert_false (gsm_diskstats_is_physical_disk ("../block/sda", sysfs));
  g_assert_false (gsm_diskstats_is_physical_disk (NULL, sysfs));
  g_free (sysfs);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/diskstats/machine-counts-only-the-two-whole-disks", test_machine_counts_only_the_two_whole_disks);
  g_test_add_func ("/diskstats/mixed-file-skips-stacked-ram-partition-and-unlisted", test_mixed_file_skips_stacked_ram_partition_and_unlisted_devices);
  g_test_add_func ("/diskstats/short-lines-and-non-numbers-are-malformed", test_short_lines_and_non_numbers_are_malformed_not_zero);
  g_test_add_func ("/diskstats/fourteen-fields-is-the-minimum", test_a_line_with_only_fourteen_fields_is_accepted);
  g_test_add_func ("/diskstats/sector-overflow-is-malformed", test_sector_counts_that_overflow_bytes_are_malformed);
  g_test_add_func ("/diskstats/device-set-hash-ignores-order", test_device_set_hash_ignores_line_order_and_tells_sets_apart);
  g_test_add_func ("/diskstats/unreadable-file-is-not-a-reading", test_unreadable_file_is_not_a_reading);
  g_test_add_func ("/diskstats/escaping-names-are-not-disks", test_names_that_could_escape_the_sysfs_root_are_not_disks);
  return g_test_run ();
}
