/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixtures meminfo-a.txt and meminfo-b.txt are real /proc/meminfo captures
 * from the development machine (2026-10-28), taken two seconds apart.
 * argv[1] is the fixtures directory.
 */

#include <glib.h>

#include "../meminfo.h"

static const char *fixtures_dir;

static char *
load_fixture (const char *name)
{
  char *path = g_build_filename (fixtures_dir, name, NULL);
  char *text = NULL;
  GError *error = NULL;

  g_file_get_contents (path, &text, NULL, &error);
  g_assert_no_error (error);
  g_free (path);
  return text;
}

static guint64
rows_sum (const GsmMemPartition *p)
{
  return p->application_private_bytes + p->ram_backed_files_bytes + p->file_cache_bytes
         + p->kernel_reclaimable_bytes + p->kernel_fixed_bytes + p->free_bytes
         + p->unaccounted_bytes;
}

static void
check_fixture (const char *name)
{
  char *text = load_fixture (name);
  GsmMemInfo info;
  GsmMemPartition p;

  gsm_meminfo_parse (text, &info);
  gsm_meminfo_partition (&info, &p);
  g_free (text);

  g_assert_true (p.valid);
  g_assert_cmpuint (p.total_bytes, ==, info.mem_total_bytes);
  g_assert_cmpuint (p.application_private_bytes, ==, info.anon_pages_bytes);
  g_assert_cmpuint (p.ram_backed_files_bytes, ==, info.shmem_bytes);
  g_assert_cmpuint (p.file_cache_bytes, ==, info.cached_bytes - info.shmem_bytes + info.buffers_bytes);
  g_assert_cmpuint (p.kernel_reclaimable_bytes, >=, info.s_reclaimable_bytes);
  g_assert_cmpuint (p.free_bytes, ==, info.mem_free_bytes);
  g_assert_cmpuint (p.overlap_bytes, ==, 0);
  /* The rows partition MemTotal. */
  g_assert_cmpuint (rows_sum (&p), ==, p.total_bytes);
}

static void
test_fixture_a_rows_sum_to_mem_total (void)
{
  check_fixture ("meminfo-a.txt");
}

static void
test_fixture_b_rows_sum_to_mem_total (void)
{
  check_fixture ("meminfo-b.txt");
}

static void
test_values_are_bytes_not_kib (void)
{
  GsmMemInfo info;

  gsm_meminfo_parse ("MemTotal:       1000 kB\nMemAvailable:    250 kB\n", &info);
  g_assert_cmpuint (info.mem_total_bytes, ==, 1000 * 1024);
  g_assert_cmpuint (info.mem_available_bytes, ==, 250 * 1024);
}

static void
test_missing_field_is_not_zero (void)
{
  GsmMemInfo info;
  GsmMemPartition p;

  gsm_meminfo_parse ("MemTotal: 1000 kB\nMemFree: 0 kB\n", &info);
  g_assert_cmpuint (info.mem_free_bytes, ==, 0);
  g_assert_cmpuint (info.mem_available_bytes, ==, GSM_MEMINFO_MISSING);
  g_assert_cmpuint (info.shmem_bytes, ==, GSM_MEMINFO_MISSING);

  gsm_meminfo_partition (&info, &p);
  g_assert_false (p.valid);
}

static void
test_empty_or_null_text_gives_all_missing (void)
{
  GsmMemInfo info;

  gsm_meminfo_parse (NULL, &info);
  g_assert_cmpuint (info.mem_total_bytes, ==, GSM_MEMINFO_MISSING);
  gsm_meminfo_parse ("", &info);
  g_assert_cmpuint (info.cached_bytes, ==, GSM_MEMINFO_MISSING);
  gsm_meminfo_parse ("garbage without colon\nMemTotal: notanumber kB\n", &info);
  g_assert_cmpuint (info.mem_total_bytes, ==, GSM_MEMINFO_MISSING);
}

static void
test_overlapping_rows_are_reported_not_negative (void)
{
  /* Synthetic: the rows add up to more than MemTotal (VmallocUsed overlap). */
  const char *text =
    "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 0 kB\nCached: 100 kB\nShmem: 50 kB\n"
    "AnonPages: 400 kB\nSReclaimable: 100 kB\nSUnreclaim: 100 kB\nKernelStack: 10 kB\n"
    "PageTables: 10 kB\nVmallocUsed: 300 kB\n";
  GsmMemInfo info;
  GsmMemPartition p;

  gsm_meminfo_parse (text, &info);
  gsm_meminfo_partition (&info, &p);
  g_assert_true (p.valid);
  g_assert_cmpuint (p.unaccounted_bytes, ==, 0);
  /* 400 + 50 + 50 + 100 + 420 + 100 = 1120 kB against 1000 kB */
  g_assert_cmpuint (p.overlap_bytes, ==, 120 * 1024);
}

static void
test_k_reclaimable_wins_when_larger (void)
{
  const char *text =
    "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 0 kB\nCached: 100 kB\nShmem: 50 kB\n"
    "AnonPages: 100 kB\nSReclaimable: 20 kB\nKReclaimable: 80 kB\nSUnreclaim: 10 kB\n"
    "KernelStack: 1 kB\nPageTables: 1 kB\n";
  GsmMemInfo info;
  GsmMemPartition p;

  gsm_meminfo_parse (text, &info);
  gsm_meminfo_partition (&info, &p);
  g_assert_true (p.valid);
  g_assert_cmpuint (p.kernel_reclaimable_bytes, ==, 80 * 1024);
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/meminfo/fixture-a-rows-sum-to-mem-total", test_fixture_a_rows_sum_to_mem_total);
  g_test_add_func ("/meminfo/fixture-b-rows-sum-to-mem-total", test_fixture_b_rows_sum_to_mem_total);
  g_test_add_func ("/meminfo/values-are-bytes-not-kib", test_values_are_bytes_not_kib);
  g_test_add_func ("/meminfo/missing-field-is-not-zero", test_missing_field_is_not_zero);
  g_test_add_func ("/meminfo/empty-or-null-text-gives-all-missing", test_empty_or_null_text_gives_all_missing);
  g_test_add_func ("/meminfo/overlapping-rows-are-reported-not-negative", test_overlapping_rows_are_reported_not_negative);
  g_test_add_func ("/meminfo/k-reclaimable-wins-when-larger", test_k_reclaimable_wins_when_larger);
  return g_test_run ();
}
