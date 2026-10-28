/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * argv[1] is the fixtures directory, argv[2] the fake NVML library built
 * from fake-nvml.c. The sysfs-gpu-* fixtures are minimal /sys/bus/pci/devices
 * trees: an Intel display device, an NVIDIA GPU at 0000:01:00.0 (plus its
 * HDMI audio function, which is not a GPU) and, in two of them, a second
 * NVIDIA GPU at 0000:02:00.0.
 */

#include <dlfcn.h>
#include <glib.h>

#include "../nvml.h"

#define MIB (1024ull * 1024ull)

static const char *fixtures_dir;
static const char *fake_library;
static void *fake_handle;

static void (*fake_reset) (void);
static unsigned int (*fake_init_count) (void);
static unsigned int (*fake_query_count) (unsigned int);
static void (*fake_fail_memory) (int);

static GsmNvml *
open_nvml (const char *fixture)
{
  g_autofree char *root = g_build_filename (fixtures_dir, fixture, NULL);

  fake_reset ();
  return gsm_nvml_new (fake_library, root);
}

static void
test_lists_only_nvidia_display_devices (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-mixed");
  GsmGpuDevice device;

  g_assert_cmpuint (gsm_nvml_device_count (nvml), ==, 2);
  gsm_nvml_read_device (nvml, 0, &device);
  g_assert_cmpstr (device.pci_address, ==, "0000:01:00.0");
  gsm_nvml_read_device (nvml, 1, &device);
  g_assert_cmpstr (device.pci_address, ==, "0000:02:00.0");
  gsm_nvml_free (nvml);
}

static void
test_active_device_reports_utilisation_and_memory (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-mixed");
  GsmGpuDevice device;

  gsm_nvml_read_device (nvml, 0, &device);
  g_assert_cmpint (device.state, ==, GSM_GPU_STATE_ACTIVE);
  g_assert_true (device.util_known);
  g_assert_cmpuint (device.util_percent, ==, 37);
  g_assert_true (device.mem_known);
  g_assert_cmpuint (device.mem_total_bytes, ==, 6141 * MIB);
  g_assert_cmpuint (device.mem_used_bytes, ==, 1203 * MIB);
  g_assert_cmpstr (device.reason, ==, "");
  gsm_nvml_free (nvml);
}

static void
test_suspended_device_gets_no_query_and_reads_powered_down (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-mixed");
  GsmGpuDevice device;

  gsm_nvml_read_device (nvml, 0, &device);
  gsm_nvml_read_device (nvml, 1, &device);

  g_assert_cmpint (device.state, ==, GSM_GPU_STATE_POWERED_DOWN);
  g_assert_false (device.util_known);
  g_assert_false (device.mem_known);
  g_assert_nonnull (g_strstr_len (device.reason, -1, "suspended"));
  g_assert_cmpuint (fake_query_count (0), >, 0);
  g_assert_cmpuint (fake_query_count (1), ==, 0);
  gsm_nvml_free (nvml);
}

static void
test_nvml_is_not_initialised_while_every_gpu_is_suspended (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-suspended");
  GsmGpuDevice device;
  GHashTable *processes;

  g_assert_true (gsm_nvml_available (nvml, NULL));
  gsm_nvml_read_device (nvml, 0, &device);
  g_assert_cmpint (device.state, ==, GSM_GPU_STATE_POWERED_DOWN);
  processes = gsm_nvml_read_processes (nvml);
  g_assert_null (processes);
  g_assert_cmpuint (fake_init_count (), ==, 0);
  g_assert_cmpuint (fake_query_count (0), ==, 0);
  gsm_nvml_free (nvml);
}

static void
test_missing_library_is_unavailable_with_a_reason_not_zero (void)
{
  g_autofree char *root = g_build_filename (fixtures_dir, "sysfs-gpu-mixed", NULL);
  GsmNvml *nvml = gsm_nvml_new ("/nonexistent/libnvidia-ml.so.1", root);
  const char *reason = NULL;
  GsmGpuDevice device;

  g_assert_false (gsm_nvml_available (nvml, &reason));
  g_assert_nonnull (g_strstr_len (reason, -1, "not installed"));
  gsm_nvml_read_device (nvml, 0, &device);
  g_assert_cmpint (device.state, ==, GSM_GPU_STATE_UNKNOWN);
  g_assert_false (device.util_known);
  g_assert_false (device.mem_known);
  g_assert_cmpstr (device.reason, !=, "");
  g_assert_null (gsm_nvml_read_processes (nvml));
  gsm_nvml_free (nvml);
}

static void
test_machine_without_nvidia_gpu_is_unavailable (void)
{
  g_autofree char *root = g_build_filename (fixtures_dir, "sysfs-plain", NULL);
  GsmNvml *nvml = gsm_nvml_new (fake_library, root);
  const char *reason = NULL;

  g_assert_false (gsm_nvml_available (nvml, &reason));
  g_assert_nonnull (g_strstr_len (reason, -1, "no NVIDIA GPU"));
  g_assert_cmpuint (gsm_nvml_device_count (nvml), ==, 0);
  gsm_nvml_free (nvml);
}

static void
test_failed_memory_query_is_unknown_not_zero (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-mixed");
  GsmGpuDevice device;

  fake_fail_memory (1);
  gsm_nvml_read_device (nvml, 0, &device);
  g_assert_cmpint (device.state, ==, GSM_GPU_STATE_ACTIVE);
  g_assert_true (device.util_known);
  g_assert_false (device.mem_known);
  g_assert_nonnull (g_strstr_len (device.reason, -1, "nvmlDeviceGetMemoryInfo failed: Unknown Error"));
  gsm_nvml_free (nvml);
}

static void
test_process_memory_and_utilisation (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-mixed");
  GHashTable *processes = gsm_nvml_read_processes (nvml);
  GsmGpuProcess *p;

  g_assert_nonnull (processes);

  /* in both the compute and graphics lists: counted once; newest sample wins */
  p = g_hash_table_lookup (processes, GUINT_TO_POINTER (1111));
  g_assert_nonnull (p);
  g_assert_true (p->mem_known);
  g_assert_cmpuint (p->mem_bytes, ==, 500 * MIB);
  g_assert_true (p->util_known);
  g_assert_cmpuint (p->util_percent, ==, 40);

  /* NVML could not say how much memory it holds, and has no utilisation sample */
  p = g_hash_table_lookup (processes, GUINT_TO_POINTER (2222));
  g_assert_nonnull (p);
  g_assert_false (p->mem_known);
  g_assert_false (p->util_known);

  p = g_hash_table_lookup (processes, GUINT_TO_POINTER (3333));
  g_assert_nonnull (p);
  g_assert_cmpuint (p->mem_bytes, ==, 200 * MIB);
  g_assert_cmpuint (p->util_percent, ==, 7);

  g_assert_null (g_hash_table_lookup (processes, GUINT_TO_POINTER (9999)));

  /* the suspended GPU was not asked about its processes either */
  g_assert_cmpuint (fake_query_count (1), ==, 0);
  g_hash_table_destroy (processes);
  gsm_nvml_free (nvml);
}

static void
test_process_memory_sums_over_active_devices (void)
{
  GsmNvml *nvml = open_nvml ("sysfs-gpu-both-active");
  GHashTable *processes = gsm_nvml_read_processes (nvml);
  GsmGpuProcess *p;

  g_assert_nonnull (processes);
  p = g_hash_table_lookup (processes, GUINT_TO_POINTER (1111));
  g_assert_nonnull (p);
  g_assert_cmpuint (p->mem_bytes, ==, 900 * MIB);
  g_hash_table_destroy (processes);
  gsm_nvml_free (nvml);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, ==, 3);
  fixtures_dir = argv[1];
  fake_library = argv[2];

  /* Keep our own reference to the fake so its counters are reachable. */
  fake_handle = dlopen (fake_library, RTLD_NOW);
  g_assert_nonnull (fake_handle);
  *(void **) (&fake_reset) = dlsym (fake_handle, "fake_nvml_reset");
  *(void **) (&fake_init_count) = dlsym (fake_handle, "fake_nvml_init_count");
  *(void **) (&fake_query_count) = dlsym (fake_handle, "fake_nvml_query_count");
  *(void **) (&fake_fail_memory) = dlsym (fake_handle, "fake_nvml_fail_memory");
  g_assert_nonnull (fake_reset);

  g_test_add_func ("/nvml/lists-only-nvidia-display-devices", test_lists_only_nvidia_display_devices);
  g_test_add_func ("/nvml/active-device-reports-utilisation-and-memory", test_active_device_reports_utilisation_and_memory);
  g_test_add_func ("/nvml/suspended-device-gets-no-query-and-reads-powered-down", test_suspended_device_gets_no_query_and_reads_powered_down);
  g_test_add_func ("/nvml/not-initialised-while-every-gpu-is-suspended", test_nvml_is_not_initialised_while_every_gpu_is_suspended);
  g_test_add_func ("/nvml/missing-library-is-unavailable-with-a-reason", test_missing_library_is_unavailable_with_a_reason_not_zero);
  g_test_add_func ("/nvml/machine-without-nvidia-gpu-is-unavailable", test_machine_without_nvidia_gpu_is_unavailable);
  g_test_add_func ("/nvml/failed-memory-query-is-unknown-not-zero", test_failed_memory_query_is_unknown_not_zero);
  g_test_add_func ("/nvml/process-memory-and-utilisation", test_process_memory_and_utilisation);
  g_test_add_func ("/nvml/process-memory-sums-over-active-devices", test_process_memory_sums_over_active_devices);

  return g_test_run ();
}
