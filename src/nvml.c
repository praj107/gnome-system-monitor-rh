/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * See nvml.h. The declarations below are the parts of NVIDIA's NVML API
 * (NVML API Reference Guide, docs.nvidia.com/deploy/nvml-api) that this
 * file uses, with the layouts of nvml.h as of NVML 12; symbols are resolved
 * with dlsym and any that a driver lacks degrade only the figure that needs
 * them.
 */

#include "config.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nvml.h"

#define NVML_SUCCESS 0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_MEMORY_V2_VERSION (((unsigned int) sizeof (nvml_memory_v2_t)) | (2u << 24))
#define NVML_VALUE_NOT_AVAILABLE ((unsigned long long) -1)

#define NVIDIA_PCI_VENDOR 0x10de
#define PCI_CLASS_DISPLAY 0x03 /* VGA (0x0300) and 3D (0x0302) controllers */
#define MAX_PROCESSES 1024

typedef int nvml_return_t;
typedef void *nvml_device_t;

typedef struct
{
  unsigned int gpu;
  unsigned int memory;
} nvml_utilization_t;

typedef struct
{
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
} nvml_memory_t;

typedef struct
{
  unsigned int       version;
  unsigned long long total;
  unsigned long long reserved;
  unsigned long long free;
  unsigned long long used;
} nvml_memory_v2_t;

typedef struct
{
  char         bus_id_legacy[16];
  unsigned int domain;
  unsigned int bus;
  unsigned int device;
  unsigned int pci_device_id;
  unsigned int pci_sub_system_id;
  char         bus_id[32];
} nvml_pci_info_t;

typedef struct
{
  unsigned int       pid;
  unsigned long long used_gpu_memory;
  unsigned int       gpu_instance_id;
  unsigned int       compute_instance_id;
} nvml_process_info_t;

typedef struct
{
  unsigned int       pid;
  unsigned long long time_stamp;
  unsigned int       sm_util;
  unsigned int       mem_util;
  unsigned int       enc_util;
  unsigned int       dec_util;
} nvml_process_utilization_sample_t;

typedef nvml_return_t (*nvml_init_fn) (void);
typedef nvml_return_t (*nvml_shutdown_fn) (void);
typedef nvml_return_t (*nvml_get_count_fn) (unsigned int *count);
typedef nvml_return_t (*nvml_get_handle_fn) (unsigned int index, nvml_device_t *device);
typedef nvml_return_t (*nvml_get_pci_info_fn) (nvml_device_t device, nvml_pci_info_t *pci);
typedef nvml_return_t (*nvml_get_utilization_fn) (nvml_device_t device, nvml_utilization_t *utilization);
typedef nvml_return_t (*nvml_get_memory_fn) (nvml_device_t device, nvml_memory_t *memory);
typedef nvml_return_t (*nvml_get_memory_v2_fn) (nvml_device_t device, nvml_memory_v2_t *memory);
typedef nvml_return_t (*nvml_get_processes_fn) (nvml_device_t device, unsigned int *count, nvml_process_info_t *infos);
typedef nvml_return_t (*nvml_get_process_util_fn) (nvml_device_t device, nvml_process_utilization_sample_t *samples,
                                                   unsigned int *count, unsigned long long last_seen_time_stamp);
typedef const char *(*nvml_error_string_fn) (nvml_return_t result);

typedef struct
{
  char          address[16];
  unsigned int  domain;
  unsigned int  bus;
  unsigned int  device;
  nvml_device_t handle; /* NULL until NVML is initialised and lists this device */
} Slot;

struct _GsmNvml
{
  char *library_path;
  char *sysfs_root;

  gboolean scanned;
  GArray  *slots; /* of Slot, sorted by PCI address */

  gboolean load_tried;
  void    *library;
  char     load_error[160];

  nvml_init_fn init;
  nvml_shutdown_fn shutdown;
  nvml_get_count_fn get_count;
  nvml_get_handle_fn get_handle;
  nvml_get_pci_info_fn get_pci_info;
  nvml_get_utilization_fn get_utilization;
  nvml_get_memory_fn get_memory;
  nvml_get_memory_v2_fn get_memory_v2;
  nvml_get_processes_fn get_compute_processes;
  nvml_get_processes_fn get_graphics_processes;
  nvml_get_process_util_fn get_process_utilization;
  nvml_error_string_fn error_string;

  gboolean initialised;
};

/* ---------------------------------------------------------------- */
/* Library                                                           */
/* ---------------------------------------------------------------- */

static void
set_text (char       *dest,
          gsize       size,
          const char *format,
          ...) G_GNUC_PRINTF (3, 4);

static void
set_text (char       *dest,
          gsize       size,
          const char *format,
          ...)
{
  va_list args;

  va_start (args, format);
  g_vsnprintf (dest, size, format, args);
  va_end (args);
}

static const char *
error_text (GsmNvml      *nvml,
            nvml_return_t result)
{
  static char fallback[32];
  const char *text = nvml->error_string ? nvml->error_string (result) : NULL;

  if (text != NULL)
    return text;
  g_snprintf (fallback, sizeof fallback, "NVML error %d", result);
  return fallback;
}

static gboolean
load_library (GsmNvml *nvml)
{
  if (nvml->load_tried)
    return nvml->library != NULL;
  nvml->load_tried = TRUE;

  const char *path = nvml->library_path ? nvml->library_path : "libnvidia-ml.so.1";
  void *library = dlopen (path, RTLD_NOW | RTLD_LOCAL);

  if (library == NULL)
    {
      set_text (nvml->load_error, sizeof nvml->load_error, "NVML is not installed (%s)", path);
      return FALSE;
    }

#define REQUIRE(field, name) \
  do { \
    *(void **) (&nvml->field) = dlsym (library, name); \
    if (nvml->field == NULL) \
      { \
        set_text (nvml->load_error, sizeof nvml->load_error, "the NVML library lacks %s", name); \
        dlclose (library); \
        return FALSE; \
      } \
  } while (0)
#define OPTIONAL(field, name) *(void **) (&nvml->field) = dlsym (library, name)

  REQUIRE (init, "nvmlInit_v2");
  REQUIRE (get_count, "nvmlDeviceGetCount_v2");
  REQUIRE (get_handle, "nvmlDeviceGetHandleByIndex_v2");
  REQUIRE (get_pci_info, "nvmlDeviceGetPciInfo_v3");
  REQUIRE (get_utilization, "nvmlDeviceGetUtilizationRates");
  REQUIRE (get_memory, "nvmlDeviceGetMemoryInfo");
  OPTIONAL (shutdown, "nvmlShutdown");
  OPTIONAL (get_memory_v2, "nvmlDeviceGetMemoryInfo_v2");
  OPTIONAL (get_compute_processes, "nvmlDeviceGetComputeRunningProcesses_v3");
  OPTIONAL (get_graphics_processes, "nvmlDeviceGetGraphicsRunningProcesses_v3");
  OPTIONAL (get_process_utilization, "nvmlDeviceGetProcessUtilization");
  OPTIONAL (error_string, "nvmlErrorString");
#undef REQUIRE
#undef OPTIONAL

  nvml->library = library;
  return TRUE;
}

/* NVML is initialised only once a caller has an active device to ask about. */
static gboolean
ensure_initialised (GsmNvml *nvml,
                    char    *reason,
                    gsize    reason_size)
{
  if (nvml->initialised)
    return TRUE;

  if (!load_library (nvml))
    {
      g_strlcpy (reason, nvml->load_error, reason_size);
      return FALSE;
    }

  nvml_return_t rc = nvml->init ();

  if (rc != NVML_SUCCESS)
    {
      set_text (reason, reason_size, "nvmlInit failed: %s", error_text (nvml, rc));
      return FALSE;
    }
  nvml->initialised = TRUE;

  unsigned int count = 0;

  if (nvml->get_count (&count) != NVML_SUCCESS)
    return TRUE; /* initialised, but no device will map: each read says so */

  for (unsigned int i = 0; i < count; i++)
    {
      nvml_device_t handle = NULL;
      nvml_pci_info_t pci;

      if (nvml->get_handle (i, &handle) != NVML_SUCCESS)
        continue;
      memset (&pci, 0, sizeof pci);
      if (nvml->get_pci_info (handle, &pci) != NVML_SUCCESS)
        continue;

      for (guint s = 0; s < nvml->slots->len; s++)
        {
          Slot *slot = &g_array_index (nvml->slots, Slot, s);

          if (slot->domain == pci.domain && slot->bus == pci.bus && slot->device == pci.device)
            slot->handle = handle;
        }
    }
  return TRUE;
}

/* ---------------------------------------------------------------- */
/* sysfs                                                             */
/* ---------------------------------------------------------------- */

static gboolean
read_trimmed (const char *path,
              char      **out)
{
  char *text = NULL;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return FALSE;
  g_strstrip (text);
  *out = text;
  return TRUE;
}

static gboolean
read_hex_file (const char *path,
               guint64    *value)
{
  g_autofree char *text = NULL;
  char *end = NULL;

  if (!read_trimmed (path, &text) || text[0] == '\0')
    return FALSE;
  *value = g_ascii_strtoull (text, &end, 16);
  return end != text;
}

static gint
compare_slots (gconstpointer a,
               gconstpointer b)
{
  return strcmp (((const Slot *) a)->address, ((const Slot *) b)->address);
}

static void
scan_sysfs (GsmNvml *nvml)
{
  if (nvml->scanned)
    return;
  nvml->scanned = TRUE;

  g_autofree char *devices_dir = g_build_filename (nvml->sysfs_root, "bus", "pci", "devices", NULL);
  GDir *dir = g_dir_open (devices_dir, 0, NULL);

  if (dir == NULL)
    return;

  const char *name;

  while ((name = g_dir_read_name (dir)) != NULL)
    {
      unsigned int domain, bus, device, function;
      char trailing;
      Slot slot;

      if (sscanf (name, "%x:%x:%x.%x%c", &domain, &bus, &device, &function, &trailing) != 4 ||
          strlen (name) >= sizeof slot.address)
        continue;

      g_autofree char *vendor_path = g_build_filename (devices_dir, name, "vendor", NULL);
      g_autofree char *class_path = g_build_filename (devices_dir, name, "class", NULL);
      guint64 vendor, pci_class;

      if (!read_hex_file (vendor_path, &vendor) || vendor != NVIDIA_PCI_VENDOR)
        continue;
      if (!read_hex_file (class_path, &pci_class) || (pci_class >> 16) != PCI_CLASS_DISPLAY)
        continue;

      memset (&slot, 0, sizeof slot);
      g_strlcpy (slot.address, name, sizeof slot.address);
      slot.domain = domain;
      slot.bus = bus;
      slot.device = device;
      g_array_append_val (nvml->slots, slot);
    }
  g_dir_close (dir);
  g_array_sort (nvml->slots, compare_slots);
}

typedef enum
{
  POWER_QUERY,  /* active, or the kernel offers no runtime power state */
  POWER_DOWN,   /* suspended or suspending: asking would wake it */
  POWER_UNSURE  /* resuming or an unrecognised state */
} PowerGate;

static PowerGate
read_power_gate (GsmNvml    *nvml,
                 const Slot *slot,
                 char       *reason,
                 gsize       reason_size)
{
  g_autofree char *path = g_build_filename (nvml->sysfs_root, "bus", "pci", "devices", slot->address,
                                            "power", "runtime_status", NULL);
  g_autofree char *status = NULL;

  /* Without CONFIG_PM the file is absent and the device cannot be
   * runtime-suspended, so there is nothing to wake. */
  if (!read_trimmed (path, &status))
    return POWER_QUERY;

  if (g_strcmp0 (status, "active") == 0 || g_strcmp0 (status, "unsupported") == 0)
    return POWER_QUERY;

  if (g_strcmp0 (status, "suspended") == 0 || g_strcmp0 (status, "suspending") == 0)
    {
      set_text (reason, reason_size, "powered down (runtime_status: %s)", status);
      return POWER_DOWN;
    }

  set_text (reason, reason_size, "power state is \"%s\"; not querying", status);
  return POWER_UNSURE;
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

GsmNvml *
gsm_nvml_new (const char *library_path,
              const char *sysfs_root)
{
  GsmNvml *nvml = g_new0 (GsmNvml, 1);

  nvml->library_path = g_strdup (library_path);
  nvml->sysfs_root = g_strdup (sysfs_root ? sysfs_root : "/sys");
  nvml->slots = g_array_new (FALSE, TRUE, sizeof (Slot));
  return nvml;
}

void
gsm_nvml_free (GsmNvml *nvml)
{
  if (nvml == NULL)
    return;
  if (nvml->initialised && nvml->shutdown != NULL)
    nvml->shutdown ();
  if (nvml->library != NULL)
    dlclose (nvml->library);
  g_array_free (nvml->slots, TRUE);
  g_free (nvml->library_path);
  g_free (nvml->sysfs_root);
  g_free (nvml);
}

GsmNvml *
gsm_nvml_default (void)
{
  static GsmNvml *instance = NULL;

  if (instance == NULL)
    instance = gsm_nvml_new (NULL, NULL);
  return instance;
}

gboolean
gsm_nvml_available (GsmNvml     *nvml,
                    const char **reason_out)
{
  const char *reason = NULL;

  scan_sysfs (nvml);
  if (nvml->slots->len == 0)
    reason = "no NVIDIA GPU found in sysfs";
  else if (!load_library (nvml))
    reason = nvml->load_error;

  if (reason_out != NULL)
    *reason_out = reason;
  return reason == NULL;
}

guint
gsm_nvml_device_count (GsmNvml *nvml)
{
  scan_sysfs (nvml);
  return nvml->slots->len;
}

static void
fail_reason (GsmGpuDevice *out,
             const char   *what,
             GsmNvml      *nvml,
             nvml_return_t rc)
{
  if (out->reason[0] != '\0')
    return; /* keep the first cause */
  set_text (out->reason, sizeof out->reason, "%s failed: %s", what, error_text (nvml, rc));
}

static void
read_memory (GsmNvml      *nvml,
             nvml_device_t handle,
             GsmGpuDevice *out)
{
  /* The v2 call reports the memory the driver reserves for itself apart
   * from "used", which is the figure nvidia-smi calls memory.used; the v1
   * call folds the reservation into used. Prefer v2. */
  if (nvml->get_memory_v2 != NULL)
    {
      nvml_memory_v2_t memory_v2;

      memset (&memory_v2, 0, sizeof memory_v2);
      memory_v2.version = NVML_MEMORY_V2_VERSION;
      if (nvml->get_memory_v2 (handle, &memory_v2) == NVML_SUCCESS)
        {
          out->mem_known = TRUE;
          out->mem_total_bytes = memory_v2.total;
          out->mem_used_bytes = memory_v2.used;
          return;
        }
    }

  nvml_memory_t memory;

  memset (&memory, 0, sizeof memory);
  nvml_return_t rc = nvml->get_memory (handle, &memory);

  if (rc != NVML_SUCCESS)
    {
      fail_reason (out, "nvmlDeviceGetMemoryInfo", nvml, rc);
      return;
    }
  out->mem_known = TRUE;
  out->mem_total_bytes = memory.total;
  out->mem_used_bytes = memory.used;
}

void
gsm_nvml_read_device (GsmNvml      *nvml,
                      guint         index,
                      GsmGpuDevice *out)
{
  memset (out, 0, sizeof *out);
  out->state = GSM_GPU_STATE_UNKNOWN;

  scan_sysfs (nvml);
  if (index >= nvml->slots->len)
    {
      set_text (out->reason, sizeof out->reason, "no such GPU");
      return;
    }

  Slot *slot = &g_array_index (nvml->slots, Slot, index);

  g_strlcpy (out->pci_address, slot->address, sizeof out->pci_address);

  switch (read_power_gate (nvml, slot, out->reason, sizeof out->reason))
    {
      case POWER_DOWN:
        out->state = GSM_GPU_STATE_POWERED_DOWN;
        return;

      case POWER_UNSURE:
        return;

      case POWER_QUERY:
        break;
    }

  if (!ensure_initialised (nvml, out->reason, sizeof out->reason))
    return;

  /* ensure_initialised may have mapped handles into the array. */
  slot = &g_array_index (nvml->slots, Slot, index);
  if (slot->handle == NULL)
    {
      set_text (out->reason, sizeof out->reason, "NVML does not list %s", slot->address);
      return;
    }

  out->state = GSM_GPU_STATE_ACTIVE;

  nvml_utilization_t utilization;
  nvml_return_t rc;

  memset (&utilization, 0, sizeof utilization);
  rc = nvml->get_utilization (slot->handle, &utilization);
  if (rc == NVML_SUCCESS)
    {
      out->util_known = TRUE;
      out->util_percent = utilization.gpu;
    }
  else
    fail_reason (out, "nvmlDeviceGetUtilizationRates", nvml, rc);

  read_memory (nvml, slot->handle, out);
}

/* ---------------------------------------------------------------- */
/* Processes                                                         */
/* ---------------------------------------------------------------- */

static GsmGpuProcess *
process_entry (GHashTable *table,
               guint       pid)
{
  GsmGpuProcess *entry = g_hash_table_lookup (table, GUINT_TO_POINTER (pid));

  if (entry == NULL)
    {
      entry = g_new0 (GsmGpuProcess, 1);
      g_hash_table_insert (table, GUINT_TO_POINTER (pid), entry);
    }
  return entry;
}

/* Fill got[] with one device's process list (NVML reports how many it
 * needs when the buffer is short). Returns the number of entries, or -1. */
static int
list_processes (nvml_get_processes_fn   fn,
                nvml_device_t           handle,
                nvml_process_info_t    *infos)
{
  unsigned int count = MAX_PROCESSES;
  nvml_return_t rc = fn (handle, &count, infos);

  if (rc != NVML_SUCCESS || count > MAX_PROCESSES)
    return -1;
  return (int) count;
}

static void
add_device_processes (GsmNvml    *nvml,
                      Slot       *slot,
                      GHashTable *table)
{
  nvml_process_info_t compute[MAX_PROCESSES];
  nvml_process_info_t graphics[MAX_PROCESSES];
  nvml_process_info_t *lists[2] = { compute, graphics };
  int counts[2] = { -1, -1 };
  /* A process using both queues is listed twice with the same memory;
   * take the larger, never the sum. */
  GHashTable *device_memory = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

  if (nvml->get_compute_processes != NULL)
    counts[0] = list_processes (nvml->get_compute_processes, slot->handle, compute);
  if (nvml->get_graphics_processes != NULL)
    counts[1] = list_processes (nvml->get_graphics_processes, slot->handle, graphics);

  for (int l = 0; l < 2; l++)
    {
      for (int i = 0; i < counts[l]; i++)
        {
          const nvml_process_info_t *info = &lists[l][i];
          guint64 *best = g_hash_table_lookup (device_memory, GUINT_TO_POINTER (info->pid));

          if (best == NULL)
            {
              best = g_new0 (guint64, 2); /* [0] bytes, [1] known */
              g_hash_table_insert (device_memory, GUINT_TO_POINTER (info->pid), best);
            }
          if (info->used_gpu_memory == NVML_VALUE_NOT_AVAILABLE)
            continue;
          best[1] = 1;
          best[0] = MAX (best[0], (guint64) info->used_gpu_memory);
        }
    }

  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, device_memory);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      guint64 *best = value;
      GsmGpuProcess *entry = process_entry (table, GPOINTER_TO_UINT (key));

      if (best[1])
        {
          entry->mem_known = TRUE;
          entry->mem_bytes += best[0];
        }
    }
  g_hash_table_destroy (device_memory);

  if (nvml->get_process_utilization == NULL)
    return;

  /* The driver keeps a short history of per-process samples; with a
   * last-seen time of 0 it returns all of them, so keep the newest per pid.
   * No sample (NVML_ERROR_NOT_FOUND) leaves the figure unknown. */
  nvml_process_utilization_sample_t samples[MAX_PROCESSES];
  unsigned int sample_count = MAX_PROCESSES;

  if (nvml->get_process_utilization (slot->handle, samples, &sample_count, 0) != NVML_SUCCESS ||
      sample_count > MAX_PROCESSES)
    return;

  GHashTable *newest = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

  for (unsigned int i = 0; i < sample_count; i++)
    {
      unsigned long long *stamp = g_hash_table_lookup (newest, GUINT_TO_POINTER (samples[i].pid));
      GsmGpuProcess *entry = process_entry (table, samples[i].pid);

      if (stamp == NULL)
        {
          stamp = g_new0 (unsigned long long, 1);
          g_hash_table_insert (newest, GUINT_TO_POINTER (samples[i].pid), stamp);
        }
      else if (samples[i].time_stamp < *stamp)
        continue;

      *stamp = samples[i].time_stamp;
      entry->util_known = TRUE;
      entry->util_percent = MIN (samples[i].sm_util, 100u);
    }
  g_hash_table_destroy (newest);
}

GHashTable *
gsm_nvml_read_processes (GsmNvml *nvml)
{
  GHashTable *table = NULL;
  char reason[160];

  scan_sysfs (nvml);

  for (guint i = 0; i < nvml->slots->len; i++)
    {
      Slot *slot = &g_array_index (nvml->slots, Slot, i);

      reason[0] = '\0';
      if (read_power_gate (nvml, slot, reason, sizeof reason) != POWER_QUERY)
        continue;
      if (!ensure_initialised (nvml, reason, sizeof reason))
        continue;

      slot = &g_array_index (nvml->slots, Slot, i);
      if (slot->handle == NULL)
        continue;

      if (table == NULL)
        table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
      add_device_processes (nvml, slot, table);
    }
  return table;
}
