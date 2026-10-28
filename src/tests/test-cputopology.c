/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fixture sysfs-hybrid holds the small topology files copied from the
 * development machine (2026-10-28, kernel 7.0, 6 P-cores x 2 threads and
 * 4 E-cores; there is no topology/core_type on that kernel). The other
 * sysfs-* trees are hand-made shapes. argv[1] is the fixtures directory.
 */

#include <glib.h>

#include "../cputopology.h"

static const char *fixtures_dir;

static void
read_tree (const char     *name,
           guint           n_cpus,
           GsmCpuTopology *out)
{
  char *root = g_build_filename (fixtures_dir, name, NULL);

  gsm_cpu_topology_read (root, n_cpus, out);
  g_free (root);
}

static char *
all_labels (const GsmCpuTopology *topology)
{
  GString *text = g_string_new (NULL);

  for (guint cpu = 0; cpu < topology->n_cpus; cpu++)
    {
      char *label = gsm_cpu_topology_label (topology, cpu);

      g_string_append_printf (text, "%s%s", cpu ? " " : "", label);
      g_free (label);
    }
  return g_string_free (text, FALSE);
}

static void
test_hybrid_machine_labels_threads_and_efficiency_cores (void)
{
  GsmCpuTopology topology;
  char *labels;

  read_tree ("sysfs-hybrid", 16, &topology);
  labels = all_labels (&topology);
  g_assert_cmpstr (labels, ==, "P1a P1b P2a P2b P3a P3b P4a P4b P5a P5b P6a P6b E1 E2 E3 E4");
  g_free (labels);

  /* CPU 3 is the second thread of P-core 2 and shares it with CPU 2. */
  g_assert_cmpint (topology.cpus[3].kind, ==, GSM_CPU_KIND_PERFORMANCE);
  g_assert_cmpuint (topology.cpus[3].core_number, ==, 2);
  g_assert_cmpuint (topology.cpus[3].thread_index, ==, 1);
  g_assert_cmpuint (topology.cpus[3].thread_count, ==, 2);
  g_assert_cmpuint (topology.cpus[3].first_cpu, ==, 2);
  g_assert_cmpint (topology.cpus[15].kind, ==, GSM_CPU_KIND_EFFICIENCY);
  g_assert_cmpuint (topology.cpus[15].thread_count, ==, 1);
  gsm_cpu_topology_clear (&topology);
}

static void
test_non_hybrid_machine_gets_plain_cpu_numbers (void)
{
  GsmCpuTopology topology;
  char *labels;

  read_tree ("sysfs-plain", 4, &topology);
  labels = all_labels (&topology);
  g_assert_cmpstr (labels, ==, "CPU 0 CPU 1 CPU 2 CPU 3");
  g_free (labels);
  g_assert_cmpint (topology.cpus[0].kind, ==, GSM_CPU_KIND_UNKNOWN);
  gsm_cpu_topology_clear (&topology);
}

static void
test_missing_sysfs_root_gives_plain_cpu_numbers (void)
{
  GsmCpuTopology topology;
  char *labels;

  gsm_cpu_topology_read ("/nonexistent/sysfs", 3, &topology);
  labels = all_labels (&topology);
  g_assert_cmpstr (labels, ==, "CPU 0 CPU 1 CPU 2");
  g_free (labels);
  gsm_cpu_topology_clear (&topology);
}

static void
test_one_hybrid_file_alone_is_not_a_hybrid_machine (void)
{
  GsmCpuTopology topology;
  char *labels;

  read_tree ("sysfs-core-only", 1, &topology);
  labels = all_labels (&topology);
  g_assert_cmpstr (labels, ==, "CPU 0");
  g_free (labels);
  gsm_cpu_topology_clear (&topology);
}

static void
test_cpu_with_missing_or_inconsistent_siblings_is_unknown_not_guessed (void)
{
  GsmCpuTopology topology;
  char *labels;

  /* cpu_core 0-1, cpu_atom 2-3. CPU 0 is fine; CPU 1 claims a sibling that
   * is an E-core; CPU 2 is fine; CPU 3 has no siblings file. */
  read_tree ("sysfs-partial", 4, &topology);
  labels = all_labels (&topology);
  g_assert_cmpstr (labels, ==, "P1a CPU 1 E1 CPU 3");
  g_free (labels);
  gsm_cpu_topology_clear (&topology);
}

static void
test_cpu_list_parser_reads_ranges_and_singles (void)
{
  gboolean members[12];

  g_assert_true (gsm_cpu_list_parse ("0-3,8,10-11\n", members, 12));
  for (guint i = 0; i < 12; i++)
    g_assert_cmpint (members[i], ==, (i <= 3 || i == 8 || i >= 10));

  /* CPUs beyond the table are ignored, not an error. */
  g_assert_true (gsm_cpu_list_parse ("10-40", members, 12));
  g_assert_true (members[10] && members[11]);
}

static void
test_cpu_list_parser_rejects_malformed_text (void)
{
  gboolean members[4];
  const char *bad[] = { "", "\n", NULL, "a-b", "3-1", "1,,2", "1,", "1-2-3", "1 2" };

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_assert_false (gsm_cpu_list_parse (bad[i], members, 4));
      for (guint m = 0; m < 4; m++)
        g_assert_false (members[m]);
    }
}

int
main (int    argc,
      char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);
  fixtures_dir = argv[1];

  g_test_add_func ("/cputopology/hybrid-machine-labels-threads-and-efficiency-cores", test_hybrid_machine_labels_threads_and_efficiency_cores);
  g_test_add_func ("/cputopology/non-hybrid-machine-gets-plain-cpu-numbers", test_non_hybrid_machine_gets_plain_cpu_numbers);
  g_test_add_func ("/cputopology/missing-sysfs-root-gives-plain-cpu-numbers", test_missing_sysfs_root_gives_plain_cpu_numbers);
  g_test_add_func ("/cputopology/one-hybrid-file-alone-is-not-hybrid", test_one_hybrid_file_alone_is_not_a_hybrid_machine);
  g_test_add_func ("/cputopology/missing-or-inconsistent-siblings-is-unknown", test_cpu_with_missing_or_inconsistent_siblings_is_unknown_not_guessed);
  g_test_add_func ("/cputopology/list-parser-reads-ranges-and-singles", test_cpu_list_parser_reads_ranges_and_singles);
  g_test_add_func ("/cputopology/list-parser-rejects-malformed-text", test_cpu_list_parser_rejects_malformed_text);
  return g_test_run ();
}
