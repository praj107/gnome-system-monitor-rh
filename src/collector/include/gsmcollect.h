/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gsmcollect.h: the C interface of the collector, version 1.
 *
 * The collector reads the kernel's own counters (proc_pid_stat(5),
 * proc_pid_status(5), /proc/meminfo, the pressure files of
 * Documentation/accounting/psi.rst, and the cgroup v2 files of
 * Documentation/admin-guide/cgroup-v2.rst) and hands them over as
 * immutable snapshots of plain structs.
 *
 * The rules this interface keeps:
 *
 *  - A collector is used from one thread at a time. A snapshot is read-only
 *    and may be read from any thread once returned, until it is freed.
 *  - Every record starts with `uint32_t struct_size`. The library fills
 *    records in the layout of the api_version the consumer asked for, so a
 *    consumer built against this header indexes the arrays directly; a later
 *    version only appends fields (and `known` bits).
 *  - Unknown is a bit, not a sentinel. Each record has `uint64_t known`,
 *    one bit per optional field. A field whose bit is clear was not
 *    available (file absent, unreadable or malformed, or the kernel does
 *    not print it): its value is 0 and must not be shown as 0.
 *  - Counters are cumulative, exactly as the kernel keeps them, in the unit
 *    named in the field (ns, usec, bytes, ticks, plain counts). Pages are
 *    converted to bytes. Rates and window shares are computed by the
 *    consumer from two snapshots with gsm_rate_per_s() and
 *    gsm_pressure_share() below, so every consumer uses the same arithmetic.
 *  - A process is identified across snapshots by (pid, start_ticks): a
 *    reused pid has a new start time.
 *  - Strings are bytes, never assumed to be UTF-8. `comm` is a fixed
 *    NUL-terminated buffer; a cgroup path is an (offset, length) into the
 *    snapshot's string table (each string is also followed by a NUL).
 *  - Memory belongs to the library: snapshots are allocated and freed by
 *    it, never with the consumer's allocator.
 *  - Errors are negative errno values.
 */
#ifndef GSMCOLLECT_H
#define GSMCOLLECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GSM_COLLECT_API_VERSION 1

/* The version the library was built with; compare with the macro above. */
uint32_t gsm_collect_api_version (void);

typedef struct gsm_collector gsm_collector;
typedef struct gsm_snapshot gsm_snapshot;

/* ---- configuration ---------------------------------------------------- */

/* Readers, for gsm_collect_config.readers (a bit mask). */
#define GSM_READ_PROCESSES (1u << 0)
#define GSM_READ_MEMORY    (1u << 1)
#define GSM_READ_PRESSURE  (1u << 2)
#define GSM_READ_CGROUPS   (1u << 3)
#define GSM_READ_ALL       0xFu
#define GSM_READER_COUNT   4

typedef struct gsm_collect_config
{
  uint32_t struct_size;        /* sizeof (gsm_collect_config) */
  uint32_t api_version;        /* GSM_COLLECT_API_VERSION */
  uint32_t readers;            /* GSM_READ_* mask; 0 means GSM_READ_ALL */
  uint32_t reserved;
  const char *procfs_root;     /* NULL: "/proc" */
  const char *sysfs_root;      /* NULL: "/sys" (no reader uses it yet) */
  const char *cgroupfs_root;   /* NULL: "/sys/fs/cgroup" */
} gsm_collect_config;

/* Makes a collector. NULL config means all defaults; the paths are copied.
 * Returns 0 and the collector in *out, or a negative errno and NULL in *out:
 * -EINVAL (struct_size too small, api_version newer than the library's,
 * unknown reader bits, an empty or over-long root, NULL out) or -ENOMEM. */
int             gsm_collect_open (const gsm_collect_config *config,
                                  gsm_collector           **out);

/* gsm_collect_open without the reason: NULL on any failure. */
gsm_collector * gsm_collect_new (const gsm_collect_config *config);
void            gsm_collect_free (gsm_collector *collector);

/* Reads everything the configuration asks for. Returns 0 and a new
 * snapshot in *out, or a negative errno (-ENOMEM; or the first reader error
 * when every requested reader failed). One reader failing while another
 * works is not an error: the failed reader's records are absent and its
 * errno is in gsm_sample_stats.reader_errno. A process or cgroup that
 * vanished mid-read is not an error either; it is counted. */
int             gsm_collect_sample (gsm_collector *collector,
                                    gsm_snapshot **out);
void            gsm_snapshot_free (gsm_snapshot *snapshot);

/* A static, English description of a (negative or positive) errno value
 * this library returns. Never NULL. */
const char *    gsm_collect_strerror (int err);

/* ---- processes -------------------------------------------------------- */

/* gsm_process.known */
#define GSM_PROCESS_HAS_RSS_ANON      (1ULL << 0)  /* status: RssAnon */
#define GSM_PROCESS_HAS_RSS_FILE      (1ULL << 1)  /* status: RssFile */
#define GSM_PROCESS_HAS_RSS_SHMEM     (1ULL << 2)  /* status: RssShmem */
#define GSM_PROCESS_HAS_VM_SWAP       (1ULL << 3)  /* status: VmSwap */
#define GSM_PROCESS_HAS_VM_SIZE       (1ULL << 4)  /* status: VmSize */
#define GSM_PROCESS_HAS_UID           (1ULL << 5)  /* status: Uid, real */
#define GSM_PROCESS_HAS_RUN_NS        (1ULL << 6)  /* schedstat field 1 */
#define GSM_PROCESS_HAS_RUN_DELAY_NS  (1ULL << 7)  /* schedstat field 2 */
#define GSM_PROCESS_HAS_TIMESLICES    (1ULL << 8)  /* schedstat field 3 */
#define GSM_PROCESS_HAS_CGROUP        (1ULL << 9)  /* cgroup v2 path in the string table */

/* gsm_process.flags: the kernel's task flags (proc_pid_stat(5), field 9). */
#define GSM_PF_KTHREAD 0x00200000u

typedef struct gsm_process
{
  uint32_t struct_size;
  uint32_t pid;
  uint64_t known;
  uint64_t start_ticks;        /* clock ticks after boot; with pid, the identity */
  uint64_t utime_ticks;        /* cumulative, clock ticks (sysconf(_SC_CLK_TCK)) */
  uint64_t stime_ticks;
  uint64_t rss_anon_bytes;     /* private resident memory */
  uint64_t rss_file_bytes;     /* resident file-backed pages, possibly shared */
  uint64_t rss_shmem_bytes;    /* resident shared-memory pages */
  uint64_t vm_swap_bytes;
  uint64_t vm_size_bytes;      /* address space, not memory use */
  uint64_t run_ns;             /* cumulative time on a CPU */
  uint64_t run_delay_ns;       /* cumulative time waiting on a run queue */
  uint64_t timeslices;
  uint32_t ppid;
  uint32_t pgrp;
  uint32_t session;
  int32_t tty_nr;
  uint32_t flags;
  uint32_t num_threads;
  uint32_t uid_real;
  uint32_t cgroup_offset;      /* into the string table; valid with HAS_CGROUP */
  uint32_t cgroup_len;
  uint8_t state;               /* R, S, D, Z, T, t, X ... as an ASCII letter */
  uint8_t comm_len;
  uint8_t reserved[6];
  char comm[64];               /* NUL-terminated; up to 63 bytes, may hold spaces */
} gsm_process;

/* ---- system memory ---------------------------------------------------- */

/* gsm_memory.known: raw /proc/meminfo keys (kB in the file, bytes here) */
#define GSM_MEMORY_HAS_MEM_TOTAL        (1ULL << 0)
#define GSM_MEMORY_HAS_MEM_FREE         (1ULL << 1)
#define GSM_MEMORY_HAS_MEM_AVAILABLE    (1ULL << 2)
#define GSM_MEMORY_HAS_BUFFERS          (1ULL << 3)
#define GSM_MEMORY_HAS_CACHED           (1ULL << 4)
#define GSM_MEMORY_HAS_SHMEM            (1ULL << 5)
#define GSM_MEMORY_HAS_ANON_PAGES       (1ULL << 6)
#define GSM_MEMORY_HAS_S_RECLAIMABLE    (1ULL << 7)
#define GSM_MEMORY_HAS_S_UNRECLAIM      (1ULL << 8)
#define GSM_MEMORY_HAS_K_RECLAIMABLE    (1ULL << 9)
#define GSM_MEMORY_HAS_KERNEL_STACK     (1ULL << 10)
#define GSM_MEMORY_HAS_PAGE_TABLES      (1ULL << 11)
#define GSM_MEMORY_HAS_SEC_PAGE_TABLES  (1ULL << 12)
#define GSM_MEMORY_HAS_PERCPU           (1ULL << 13)
#define GSM_MEMORY_HAS_VMALLOC_USED     (1ULL << 14)
#define GSM_MEMORY_HAS_DIRTY            (1ULL << 15)
#define GSM_MEMORY_HAS_WRITEBACK        (1ULL << 16)
/* used_bytes = MemTotal - MemAvailable (needs both, and available <= total) */
#define GSM_MEMORY_HAS_USED             (1ULL << 17)
/* The partition of MemTotal into rows that say what each byte is; all nine
 * partition_* fields are known together or not at all. */
#define GSM_MEMORY_HAS_PARTITION        (1ULL << 18)

typedef struct gsm_memory
{
  uint32_t struct_size;
  uint32_t reserved;
  uint64_t known;
  uint64_t mem_total_bytes;
  uint64_t mem_free_bytes;
  uint64_t mem_available_bytes;
  uint64_t buffers_bytes;
  uint64_t cached_bytes;
  uint64_t shmem_bytes;
  uint64_t anon_pages_bytes;
  uint64_t s_reclaimable_bytes;
  uint64_t s_unreclaim_bytes;
  uint64_t k_reclaimable_bytes;
  uint64_t kernel_stack_bytes;
  uint64_t page_tables_bytes;
  uint64_t sec_page_tables_bytes;
  uint64_t percpu_bytes;
  uint64_t vmalloc_used_bytes;
  uint64_t dirty_bytes;
  uint64_t writeback_bytes;
  uint64_t used_bytes;
  uint64_t partition_total_bytes;
  uint64_t partition_application_private_bytes;  /* AnonPages */
  uint64_t partition_ram_backed_files_bytes;     /* Shmem */
  uint64_t partition_file_cache_bytes;           /* Cached - Shmem + Buffers */
  uint64_t partition_kernel_reclaimable_bytes;   /* max (SReclaimable, KReclaimable) */
  uint64_t partition_kernel_fixed_bytes;         /* SUnreclaim + KernelStack + PageTables
                                                    + SecPageTables + Percpu + VmallocUsed
                                                    (approximate: VmallocUsed may overlap) */
  uint64_t partition_free_bytes;                 /* MemFree */
  uint64_t partition_unaccounted_bytes;          /* total minus the rows, else 0 */
  uint64_t partition_overlap_bytes;              /* the rows minus total, else 0 */
} gsm_memory;

/* ---- pressure --------------------------------------------------------- */

/* One line of a pressure file ("some" or "full"). */
typedef struct gsm_pressure_line
{
  float avg10_percent;
  float avg60_percent;
  float avg300_percent;
  uint32_t reserved;
  uint64_t total_us;           /* cumulative stall time */
} gsm_pressure_line;

#define GSM_PRESSURE_CPU    0
#define GSM_PRESSURE_MEMORY 1
#define GSM_PRESSURE_IO     2
#define GSM_PRESSURE_RESOURCE_COUNT 3

/* gsm_pressure.known */
#define GSM_PRESSURE_HAS_SOME (1ULL << 0)
#define GSM_PRESSURE_HAS_FULL (1ULL << 1)  /* absent from cpu on kernels before 5.13 */

/* The system-wide /proc/pressure/{cpu,memory,io}: when the pressure reader
 * runs there are always GSM_PRESSURE_RESOURCE_COUNT records, indexed by
 * `resource`; a file the kernel does not provide has known == 0. */
typedef struct gsm_pressure
{
  uint32_t struct_size;
  uint32_t resource;           /* GSM_PRESSURE_* */
  uint64_t known;
  gsm_pressure_line some;
  gsm_pressure_line full;
} gsm_pressure;

/* ---- cgroups ---------------------------------------------------------- */

/* gsm_cgroup.known */
#define GSM_CGROUP_HAS_PARENT                  (1ULL << 0)
#define GSM_CGROUP_HAS_MEMORY_CURRENT          (1ULL << 1)
#define GSM_CGROUP_HAS_MEMORY_PEAK             (1ULL << 2)
/* memory.max / memory.high: HAS_* means a finite limit, value valid;
 * *_UNLIMITED means the file says "max" (value 0); neither is unknown. */
#define GSM_CGROUP_HAS_MEMORY_MAX              (1ULL << 3)
#define GSM_CGROUP_MEMORY_MAX_UNLIMITED        (1ULL << 4)
#define GSM_CGROUP_HAS_MEMORY_HIGH             (1ULL << 5)
#define GSM_CGROUP_MEMORY_HIGH_UNLIMITED       (1ULL << 6)
#define GSM_CGROUP_HAS_ANON                    (1ULL << 7)   /* memory.stat */
#define GSM_CGROUP_HAS_FILE                    (1ULL << 8)
#define GSM_CGROUP_HAS_SHMEM                   (1ULL << 9)
#define GSM_CGROUP_HAS_KERNEL                  (1ULL << 10)
#define GSM_CGROUP_HAS_SOCK                    (1ULL << 11)
#define GSM_CGROUP_HAS_SLAB_RECLAIMABLE        (1ULL << 12)
#define GSM_CGROUP_HAS_SLAB_UNRECLAIMABLE      (1ULL << 13)
#define GSM_CGROUP_HAS_FILE_MAPPED             (1ULL << 14)
#define GSM_CGROUP_HAS_FILE_DIRTY              (1ULL << 15)
#define GSM_CGROUP_HAS_WORKINGSET_REFAULT_ANON (1ULL << 16)
#define GSM_CGROUP_HAS_WORKINGSET_REFAULT_FILE (1ULL << 17)
#define GSM_CGROUP_HAS_PGMAJFAULT              (1ULL << 18)
#define GSM_CGROUP_HAS_EVENTS_HIGH             (1ULL << 19)  /* memory.events */
#define GSM_CGROUP_HAS_EVENTS_MAX              (1ULL << 20)
#define GSM_CGROUP_HAS_EVENTS_OOM              (1ULL << 21)
#define GSM_CGROUP_HAS_EVENTS_OOM_KILL         (1ULL << 22)
#define GSM_CGROUP_HAS_CPU_USAGE_USEC          (1ULL << 23)  /* cpu.stat */
#define GSM_CGROUP_HAS_CPU_USER_USEC           (1ULL << 24)
#define GSM_CGROUP_HAS_CPU_SYSTEM_USEC         (1ULL << 25)
#define GSM_CGROUP_HAS_CPU_NR_THROTTLED        (1ULL << 26)  /* only where the cpu controller is on */
#define GSM_CGROUP_HAS_CPU_THROTTLED_USEC      (1ULL << 27)
#define GSM_CGROUP_HAS_PIDS_CURRENT            (1ULL << 28)
#define GSM_CGROUP_HAS_MEMORY_PRESSURE_SOME    (1ULL << 29)
#define GSM_CGROUP_HAS_MEMORY_PRESSURE_FULL    (1ULL << 30)
#define GSM_CGROUP_HAS_CPU_PRESSURE_SOME       (1ULL << 31)
#define GSM_CGROUP_HAS_CPU_PRESSURE_FULL       (1ULL << 32)
#define GSM_CGROUP_HAS_IO_PRESSURE_SOME        (1ULL << 33)
#define GSM_CGROUP_HAS_IO_PRESSURE_FULL        (1ULL << 34)

/* Records come parents before children, siblings by name. */
typedef struct gsm_cgroup
{
  uint32_t struct_size;
  uint32_t parent;             /* index into the cgroup array; valid with HAS_PARENT */
  uint32_t path_offset;        /* into the string table: "/" for the root, "/system.slice/x.service" */
  uint32_t path_len;
  uint64_t known;
  uint64_t memory_current_bytes;
  uint64_t memory_peak_bytes;
  uint64_t memory_max_bytes;
  uint64_t memory_high_bytes;
  uint64_t anon_bytes;
  uint64_t file_bytes;
  uint64_t shmem_bytes;
  uint64_t kernel_bytes;
  uint64_t sock_bytes;
  uint64_t slab_reclaimable_bytes;
  uint64_t slab_unreclaimable_bytes;
  uint64_t file_mapped_bytes;
  uint64_t file_dirty_bytes;
  uint64_t workingset_refault_anon;  /* count */
  uint64_t workingset_refault_file;  /* count: page-cache thrashing */
  uint64_t pgmajfault;               /* count */
  uint64_t events_high;              /* counts */
  uint64_t events_max;
  uint64_t events_oom;
  uint64_t events_oom_kill;
  uint64_t cpu_usage_usec;           /* cumulative */
  uint64_t cpu_user_usec;
  uint64_t cpu_system_usec;
  uint64_t cpu_nr_throttled;
  uint64_t cpu_throttled_usec;
  uint64_t pids_current;             /* processes and threads in the group and below */
  gsm_pressure_line memory_pressure_some;
  gsm_pressure_line memory_pressure_full;
  gsm_pressure_line cpu_pressure_some;
  gsm_pressure_line cpu_pressure_full;
  gsm_pressure_line io_pressure_some;
  gsm_pressure_line io_pressure_full;
} gsm_cgroup;

/* ---- the snapshot ----------------------------------------------------- */

typedef struct gsm_sample_stats
{
  uint32_t struct_size;
  uint32_t readers_run;              /* GSM_READ_* that were asked for */
  uint64_t sampled_at_ns;            /* CLOCK_MONOTONIC, taken before the reads */
  uint32_t processes_exited;         /* vanished while being read: no record, not zeros */
  uint32_t process_files_unreadable; /* per-process files that exist but could not be read */
  uint32_t process_files_malformed;  /* ... cut short, too long or not parseable */
  uint32_t cgroups_vanished;
  uint32_t cgroup_files_unreadable;
  uint32_t cgroup_files_malformed;
  int32_t reader_errno[GSM_READER_COUNT];  /* 0, or the negative errno that stopped the
                                              reader (indexed by the bit position of GSM_READ_*) */
  uint32_t reserved[2];
} gsm_sample_stats;

/* The record arrays: a pointer valid until gsm_snapshot_free and the number
 * of records in *count. The pointer is NULL when *count is 0. At most one
 * gsm_memory; GSM_PRESSURE_RESOURCE_COUNT gsm_pressure when the reader ran. */
const gsm_process *      gsm_snapshot_processes (const gsm_snapshot *snapshot,
                                                 size_t             *count);
const gsm_cgroup *       gsm_snapshot_cgroups (const gsm_snapshot *snapshot,
                                               size_t             *count);
const gsm_memory *       gsm_snapshot_memory (const gsm_snapshot *snapshot,
                                              size_t             *count);
const gsm_pressure *     gsm_snapshot_pressure (const gsm_snapshot *snapshot,
                                                size_t             *count);

/* The string table that gsm_process.cgroup_* and gsm_cgroup.path_* point
 * into, and its length in bytes. */
const char *             gsm_snapshot_strings (const gsm_snapshot *snapshot,
                                               size_t             *length);
const gsm_sample_stats * gsm_snapshot_stats (const gsm_snapshot *snapshot);

/* ---- rates and shares ------------------------------------------------- */

/* Rate of a cumulative counter between two snapshots, in counter units per
 * second. Returns 1 and stores it in *out, or returns 0 (unknown, *out
 * untouched) when the window is empty or the counter went backwards (a
 * recreated cgroup, a reused pid, a reboot). */
static inline int
gsm_rate_per_s (uint64_t previous,
                uint64_t current,
                uint64_t previous_at_ns,
                uint64_t current_at_ns,
                double  *out)
{
  if (current_at_ns <= previous_at_ns || current < previous)
    return 0;
  *out = (double) (current - previous) * 1e9 / (double) (current_at_ns - previous_at_ns);
  return 1;
}

/* The share of a window during which tasks were stalled, in percent:
 * delta total_us / delta wall_us * 100, from two cumulative `total_us`
 * values. Returns 1 or 0 (unknown) as above. It is not clamped: the two
 * reads are never simultaneous, so a result slightly over 100 is possible. */
static inline int
gsm_pressure_share (uint64_t previous_total_us,
                    uint64_t current_total_us,
                    uint64_t previous_at_ns,
                    uint64_t current_at_ns,
                    double  *percent)
{
  if (current_at_ns <= previous_at_ns || current_total_us < previous_total_us)
    return 0;
  *percent = (double) (current_total_us - previous_total_us) * 100000.0 /
             (double) (current_at_ns - previous_at_ns);
  return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* GSMCOLLECT_H */
