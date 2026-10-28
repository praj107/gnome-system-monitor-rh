/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A stand-in for libnvidia-ml.so.1 with canned answers, for test-nvml.c.
 * Two devices: 0000:01:00.0 and 0000:02:00.0. Each query on a device is
 * counted so a test can prove a powered-down device was never asked.
 * The structures mirror NVIDIA's NVML API Reference Guide.
 */

#include <string.h>

#define NVML_SUCCESS 0
#define NVML_ERROR_UNKNOWN 999
#define NOT_AVAILABLE ((unsigned long long) -1)
#define MIB (1024ull * 1024ull)

typedef int nvml_return_t;
typedef struct { unsigned int gpu, memory; } utilization_t;
typedef struct { unsigned long long total, free, used; } memory_t;
typedef struct {
  char bus_id_legacy[16];
  unsigned int domain, bus, device, pci_device_id, pci_sub_system_id;
  char bus_id[32];
} pci_info_t;
typedef struct { unsigned int version; unsigned long long total, reserved, free, used; } memory_v2_t;
typedef struct { unsigned int pid; unsigned long long used_gpu_memory; unsigned int gpu_instance_id, compute_instance_id; } process_info_t;
typedef struct { unsigned int pid; unsigned long long time_stamp; unsigned int sm_util, mem_util, enc_util, dec_util; } sample_t;

static struct { unsigned int bus; unsigned int util; unsigned long long used_mib; } devices[2] = {
  { 0x01, 37, 1203 },
  { 0x02, 90, 3000 },
};
static unsigned int init_calls;
static unsigned int queries[2];
static int fail_memory;

static int
index_of (void *handle)
{
  return (int) ((char *) handle - (char *) devices) / (int) sizeof devices[0];
}

/* Test controls. */
void
fake_nvml_reset (void)
{
  init_calls = 0;
  memset (queries, 0, sizeof queries);
  fail_memory = 0;
}

unsigned int fake_nvml_init_count (void) { return init_calls; }
unsigned int fake_nvml_query_count (unsigned int device) { return queries[device]; }
void fake_nvml_fail_memory (int fail) { fail_memory = fail; }

/* The NVML surface. */
nvml_return_t nvmlInit_v2 (void) { init_calls++; return NVML_SUCCESS; }
nvml_return_t nvmlShutdown (void) { return NVML_SUCCESS; }
const char *nvmlErrorString (nvml_return_t result) { return result == NVML_ERROR_UNKNOWN ? "Unknown Error" : "Fake Error"; }

nvml_return_t
nvmlDeviceGetCount_v2 (unsigned int *count)
{
  *count = 2;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetHandleByIndex_v2 (unsigned int index, void **device)
{
  if (index > 1)
    return 2;
  *device = &devices[index];
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetPciInfo_v3 (void *device, pci_info_t *pci)
{
  int i = index_of (device);

  memset (pci, 0, sizeof *pci);
  pci->domain = 0;
  pci->bus = devices[i].bus;
  pci->device = 0;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetUtilizationRates (void *device, utilization_t *utilization)
{
  int i = index_of (device);

  queries[i]++;
  utilization->gpu = devices[i].util;
  utilization->memory = 10;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetMemoryInfo (void *device, memory_t *memory)
{
  int i = index_of (device);

  queries[i]++;
  if (fail_memory)
    return NVML_ERROR_UNKNOWN;
  memory->total = 6141 * MIB;
  /* v1 folds the 100 MiB the driver reserves into used */
  memory->used = (devices[i].used_mib + 100) * MIB;
  memory->free = memory->total - memory->used;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetMemoryInfo_v2 (void *device, memory_v2_t *memory)
{
  int i = index_of (device);

  queries[i]++;
  if (fail_memory)
    return NVML_ERROR_UNKNOWN;
  memory->total = 6141 * MIB;
  memory->reserved = 100 * MIB;
  memory->used = devices[i].used_mib * MIB;
  memory->free = memory->total - memory->used - memory->reserved;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetComputeRunningProcesses_v3 (void *device, unsigned int *count, process_info_t *infos)
{
  int i = index_of (device);

  queries[i]++;
  if (i == 0)
    {
      infos[0] = (process_info_t) { 1111, 500 * MIB, 0, 0 };
      infos[1] = (process_info_t) { 2222, NOT_AVAILABLE, 0, 0 };
      *count = 2;
    }
  else
    {
      infos[0] = (process_info_t) { 1111, 400 * MIB, 0, 0 };
      *count = 1;
    }
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetGraphicsRunningProcesses_v3 (void *device, unsigned int *count, process_info_t *infos)
{
  int i = index_of (device);

  queries[i]++;
  if (i == 0)
    {
      infos[0] = (process_info_t) { 1111, 500 * MIB, 0, 0 }; /* same process as in the compute list */
      infos[1] = (process_info_t) { 3333, 200 * MIB, 0, 0 };
      *count = 2;
    }
  else
    *count = 0;
  return NVML_SUCCESS;
}

nvml_return_t
nvmlDeviceGetProcessUtilization (void *device, sample_t *samples, unsigned int *count, unsigned long long last_seen)
{
  int i = index_of (device);

  (void) last_seen;
  queries[i]++;
  if (i != 0)
    {
      *count = 0;
      return 6; /* NVML_ERROR_NOT_FOUND */
    }
  samples[0] = (sample_t) { 1111, 20, 40, 0, 0, 0 };
  samples[1] = (sample_t) { 1111, 10, 5, 0, 0, 0 }; /* older sample for the same pid */
  samples[2] = (sample_t) { 3333, 20, 7, 0, 0, 0 };
  *count = 3;
  return NVML_SUCCESS;
}
