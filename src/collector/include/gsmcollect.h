/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * gsmcollect.h: the C interface of the collector, layout 3.
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
 *  - Every record starts with `uint32_t struct_size`. GSM_COLLECT_API_VERSION
 *    counts record layouts, and a layout only appends fields (and `known`
 *    bits) to the one before. The library fills records in the layout of the
 *    api_version the consumer asked for in gsm_collect_config: a consumer
 *    built against layout 1 sets api_version = 1 and gets arrays whose
 *    stride and struct_size are layout 1's, holding exactly layout 1's
 *    fields; one built against this header sets GSM_COLLECT_API_VERSION and
 *    indexes the arrays directly. The first `struct_size` bytes of a record
 *    are always valid.
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

/* The newest record layout. Layout 1: the first release. Layout 2: gsm_process
 * gains priority, nice and the /proc/<pid>/io counters, gsm_sample_stats gains
 * process_io_unreadable. Layout 3: gsm_process and gsm_cgroup gain a workload
 * class and, for processes, the systemd unit, user unit, login session and
 * owner uid their cgroup path names. */
#define GSM_COLLECT_API_VERSION 3

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
  uint32_t api_version;        /* the record layout wanted, 1 to GSM_COLLECT_API_VERSION;
                                  0 means GSM_COLLECT_API_VERSION */
  uint32_t readers;            /* GSM_READ_* mask; 0 means GSM_READ_ALL */
  uint32_t reserved;
  const char *procfs_root;     /* NULL: "/proc" */
  const char *sysfs_root;      /* NULL: "/sys" (no reader uses it yet) */
  const char *cgroupfs_root;   /* NULL: "/sys/fs/cgroup" */
} gsm_collect_config;

/* Makes a collector. NULL config means all defaults; the paths are copied.
 * Returns 0 and the collector in *out, or a negative errno and NULL in *out:
 * -EINVAL (struct_size too small, api_version newer than the library's layout,
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
/* Layout 2: all seven io_* counters together (proc_pid_io(5)). Clear when the
 * file is absent (no task I/O accounting) or refused, which is the normal case
 * for other users' processes. */
#define GSM_PROCESS_HAS_IO            (1ULL << 10)
/* Layout 3. HAS_CLASS is set whenever the classifier ran (the processes
 * reader ran): workload_class, class_rule and class_confidence are valid; the
 * class key may still be empty. The unit, user unit and login session are
 * what the cgroup path names, so they need HAS_CGROUP as well; the owner uid
 * is that of the innermost user-UID.slice. HAS_AGENT: the outermost coding
 * agent among the process's ancestors (and itself), found by ancestry. */
#define GSM_PROCESS_HAS_CLASS         (1ULL << 11)
#define GSM_PROCESS_HAS_UNIT          (1ULL << 12)
#define GSM_PROCESS_HAS_USER_UNIT     (1ULL << 13)
#define GSM_PROCESS_HAS_SESSION       (1ULL << 14)
#define GSM_PROCESS_HAS_OWNER_UID     (1ULL << 15)
#define GSM_PROCESS_HAS_AGENT         (1ULL << 16)

/* The memory fields are 0 with their bits set, not unknown, for a process
 * that has no user address space: a kernel thread (GSM_PF_KTHREAD) or a
 * zombie or dead task (state 'Z' or 'X'), whose status file has no Vm* or Rss*
 * lines because there is no mm to print (proc_pid_status(5)). That holds only
 * when the status file was read and parsed; a refused, short or malformed
 * status stays unknown. Applies to rss_anon_bytes, rss_file_bytes,
 * rss_shmem_bytes, vm_swap_bytes and vm_size_bytes in every layout. */

/* Workload classes, gsm_process.workload_class and gsm_cgroup.workload_class
 * (indexes into the names of gsm_class_name). */
#define GSM_CLASS_KERNEL            0
#define GSM_CLASS_INIT              1
#define GSM_CLASS_SYSTEM_SERVICES   2
#define GSM_CLASS_SESSION_SERVICES  3
#define GSM_CLASS_APPLICATIONS      4
#define GSM_CLASS_LOGIN_SESSION     5
#define GSM_CLASS_AGENT_SESSIONS    6
#define GSM_CLASS_CONTAINERS_AND_VMS 7
#define GSM_CLASS_UNCLASSIFIED      8
#define GSM_CLASS_COUNT             9
/* A cgroup (never a process) that holds several classes: not a class. */
#define GSM_CLASS_MIXED             255

/* How sure the rule is, gsm_process.class_confidence. */
#define GSM_CONFIDENCE_NONE   0
#define GSM_CONFIDENCE_LOW    1
#define GSM_CONFIDENCE_MEDIUM 2
#define GSM_CONFIDENCE_HIGH   3
#define GSM_CONFIDENCE_COUNT  4

/* The number of classification rules; class_rule is an index below it. */
#define GSM_CLASS_RULE_COUNT  20

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
  /* ---- layout 2 starts here, at offset 212: layout 1's last 4 bytes were
   * trailing padding, so its 216-byte records never held anything there ---- */
  int32_t priority;            /* proc_pid_stat(5) field 18: 20 at nice 0, negative for real-time */
  int32_t nice;                /* proc_pid_stat(5) field 19: -20 to 19; priority and nice are
                                  always valid in a layout-2 record, `stat` being the key */
  uint32_t reserved2;          /* explicit padding: the counters below are 8-byte aligned */
  uint64_t io_rchar_bytes;     /* proc_pid_io(5) rchar: bytes passed to read(2) and similar,
                                  page cache included; valid with HAS_IO, as are the next six */
  uint64_t io_wchar_bytes;     /* wchar: bytes passed to write(2) and similar */
  uint64_t io_syscr;           /* syscr: read-like calls, a count */
  uint64_t io_syscw;           /* syscw: write-like calls, a count */
  uint64_t io_read_bytes;      /* read_bytes: bytes the storage layer was asked to fetch */
  uint64_t io_write_bytes;     /* write_bytes: bytes sent to the storage layer */
  uint64_t io_cancelled_write_bytes;  /* cancelled_write_bytes: writes undone by truncation */
  /* ---- layout 3 starts here, at offset 280; the record is 336 bytes ----
   * Every (offset, length) pair below is a slice of the string table, mostly of
   * this process's cgroup path, raw as the cgroup directory holds it (a
   * `\xNN` escape is possible: gsm_unescape_name decodes it). A slice is not
   * followed by a NUL of its own; use the length. */
  uint8_t workload_class;      /* GSM_CLASS_*; valid with HAS_CLASS, never GSM_CLASS_MIXED */
  uint8_t class_rule;          /* index of the rule that decided, below GSM_CLASS_RULE_COUNT
                                  (gsm_class_rule_name) */
  uint8_t class_confidence;    /* GSM_CONFIDENCE_* */
  uint8_t reserved3a;
  uint32_t owner_uid;          /* valid with HAS_OWNER_UID */
  uint32_t class_key_offset;   /* what the class is keyed by: the unit, the application id,
                                  the login session, the scope or the project. Empty (length 0)
                                  when the rule has no key, or when the key is not part of the
                                  path (an agent's key is a process: see agent_pid) */
  uint32_t class_key_length;
  uint32_t unit_offset;        /* the systemd unit that owns the path; valid with HAS_UNIT */
  uint32_t unit_length;
  uint32_t user_unit_offset;   /* the unit inside the user manager or login session; HAS_USER_UNIT */
  uint32_t user_unit_length;
  uint32_t login_session_offset;  /* the logind session id of session-ID.scope; HAS_SESSION */
  uint32_t login_session_length;
  uint32_t agent_pid;          /* with agent_start_ticks, the identity of the outermost agent
                                  program above this process; valid with HAS_AGENT */
  uint32_t reserved3b;
  uint64_t agent_start_ticks;
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
/* Layout 3: workload_class, class_rule, class_confidence and the key. Set
 * whenever the classifier ran (always, for a layout-3 consumer). */
#define GSM_CGROUP_HAS_CLASS                   (1ULL << 35)

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
  /* ---- layout 3 starts here: a layout 1 or 2 consumer's records end before it ---- */
  uint8_t workload_class;      /* GSM_CLASS_*, or GSM_CLASS_MIXED for a node that holds several
                                  classes (the root, user.slice, a user's slices and manager,
                                  app.slice); valid with HAS_CLASS */
  uint8_t class_rule;
  uint8_t class_confidence;
  uint8_t reserved3a;
  uint32_t class_key_offset;   /* a slice of this cgroup's path in the string table; empty
                                  (length 0) when the rule has no key */
  uint32_t class_key_length;
  uint32_t reserved3b;         /* explicit padding to 8 bytes */
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
  /* ---- layout 2 starts here (offset 64) ---- */
  uint32_t process_io_unreadable;    /* processes whose /proc/<pid>/io was refused or absent:
                                        normal for other users' processes, so not part of
                                        process_files_unreadable */
  uint32_t reserved3;
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

/* ---- names ------------------------------------------------------------ */

/* Static English strings, never NULL, valid for the life of the program. An
 * out-of-range value gives "unknown". gsm_class_name also names
 * GSM_CLASS_MIXED ("Mixed"). The rule names are those of the classification
 * policy, in rule order. */
const char * gsm_class_name (uint32_t workload_class);
const char * gsm_confidence_name (uint32_t confidence);
uint32_t     gsm_class_rule_count (void);
const char * gsm_class_rule_name (uint32_t rule);

/* Decodes the `\xNN` escapes systemd writes into cgroup directory names
 * (`gnome\x2dtweaks` is `gnome-tweaks`) from the `length` bytes at `name`
 * into `buffer`, and NUL-terminates it. Returns the decoded length, or -1
 * when `buffer_size` cannot hold the result and its NUL. Anything that is
 * not a well-formed escape is copied as it is. */
static inline long
gsm_unescape_name (const char *name,
                   size_t      length,
                   char       *buffer,
                   size_t      buffer_size)
{
  size_t out = 0;
  size_t i = 0;

  if (buffer_size == 0)
    return -1;
  while (i < length)
    {
      char byte = name[i];
      int advance = 1;

      if (out + 1 >= buffer_size)
        return -1;
      if (name[i] == '\\' && i + 3 < length && name[i + 1] == 'x')
        {
          int high = -1, low = -1;
          char h = name[i + 2], l = name[i + 3];

          if (h >= '0' && h <= '9')
            high = h - '0';
          else if (h >= 'a' && h <= 'f')
            high = h - 'a' + 10;
          else if (h >= 'A' && h <= 'F')
            high = h - 'A' + 10;
          if (l >= '0' && l <= '9')
            low = l - '0';
          else if (l >= 'a' && l <= 'f')
            low = l - 'a' + 10;
          else if (l >= 'A' && l <= 'F')
            low = l - 'A' + 10;
          if (high >= 0 && low >= 0)
            {
              byte = (char) (high * 16 + low);
              advance = 4;
            }
        }
      buffer[out++] = byte;
      i += (size_t) advance;
    }
  buffer[out] = '\0';
  return (long) out;
}

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
