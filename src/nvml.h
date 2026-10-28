/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NVIDIA GPU figures from NVML, loaded with dlopen so the program neither
 * links nor requires it, without spawning nvidia-smi and without waking a
 * powered-down GPU.
 *
 * The NVML prototypes and structure layouts declared in nvml.c follow
 * NVIDIA's "NVML API Reference Guide" (docs.nvidia.com/deploy/nvml-api);
 * no NVIDIA header is vendored.
 *
 * Power: many laptops put the discrete GPU under PCI runtime power
 * management (Documentation/power/pci.rst; the state is the device's
 * power/runtime_status in sysfs). Any NVML query on a suspended GPU wakes
 * it, so the device is queried only while that file says "active"; a
 * suspended GPU reads as "powered down", which is an answer, not a zero.
 * NVML itself is initialised on the first refresh that finds an active
 * NVIDIA device.
 *
 * The sysfs root and the library path are parameters so tests use fixture
 * trees and a fake library. A figure that cannot be read is "unknown"
 * (its *_known flag is FALSE and reason says why), never 0.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  GSM_GPU_STATE_UNKNOWN,      /* not readable: see reason */
  GSM_GPU_STATE_ACTIVE,       /* queried; individual figures may be unknown */
  GSM_GPU_STATE_POWERED_DOWN  /* runtime-suspended: no NVML call was made */
} GsmGpuState;

typedef struct
{
  char        pci_address[16]; /* "0000:01:00.0" */
  GsmGpuState state;
  gboolean    util_known;
  guint       util_percent;     /* NVML utilization.gpu: share of the last sample period a kernel ran */
  gboolean    mem_known;
  guint64     mem_used_bytes;
  guint64     mem_total_bytes;
  char        reason[160];      /* why something is unknown or powered down; "" otherwise */
} GsmGpuDevice;

typedef struct
{
  gboolean mem_known;
  guint64  mem_bytes;           /* summed over devices; NVML's per-process figure */
  gboolean util_known;
  guint    util_percent;        /* SM utilisation from the driver's latest sample */
} GsmGpuProcess;

typedef struct _GsmNvml GsmNvml;

/* library_path NULL means "libnvidia-ml.so.1". sysfs_root is normally "/sys". */
GsmNvml *gsm_nvml_new (const char *library_path,
                       const char *sysfs_root);

void gsm_nvml_free (GsmNvml *nvml);

/* The process-wide instance on the real library and /sys. Never freed. */
GsmNvml *gsm_nvml_default (void);

/* TRUE when the library loads and sysfs lists at least one NVIDIA display
 * device. Loading the library does not initialise NVML or touch the GPU.
 * When FALSE, *reason_out (if given) points at text owned by nvml. */
gboolean gsm_nvml_available (GsmNvml     *nvml,
                             const char **reason_out);

guint gsm_nvml_device_count (GsmNvml *nvml);

/* Read device index (0 <= index < count). Always fills out. */
void gsm_nvml_read_device (GsmNvml      *nvml,
                           guint         index,
                           GsmGpuDevice *out);

/* pid (GUINT_TO_POINTER) -> GsmGpuProcess (g_free) for the processes NVML
 * lists on active devices. NULL when no device could be asked (powered
 * down, unavailable, or every call failed): every process is then unknown.
 * A process missing from a non-NULL table is "not reported", not 0. */
GHashTable *gsm_nvml_read_processes (GsmNvml *nvml);

G_END_DECLS
