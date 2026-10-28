/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Links the collector's static library through gsmcollect.h and checks a
 * snapshot over captured /proc and cgroupfs trees, then once over the live
 * machine. No GLib: this is what a plain C consumer sees.
 *
 *   test-collector FIXTURE_PROC FIXTURE_CGROUP
 */
#define _DEFAULT_SOURCE
#include "gsmcollect.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                      \
        do {                                                                        \
            if (!(condition))                                                         \
              {                                                                       \
                fprintf (stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
                failures++;                                                           \
              }                                                                       \
          } while (0)

static const gsm_process *
find_process (const gsm_snapshot *snapshot,
              uint32_t            pid)
{
  size_t count = 0;
  const gsm_process *records = gsm_snapshot_processes (snapshot, &count);

  for (size_t i = 0; i < count; i++)
    if (records[i].pid == pid)
      return &records[i];
  return NULL;
}

static const gsm_cgroup *
find_cgroup (const gsm_snapshot *snapshot,
             const char         *path)
{
  size_t count = 0, length = 0;
  const gsm_cgroup *records = gsm_snapshot_cgroups (snapshot, &count);
  const char *strings = gsm_snapshot_strings (snapshot, &length);

  for (size_t i = 0; i < count; i++)
    {
      if (records[i].path_offset + (size_t) records[i].path_len > length)
        return NULL;
      if (records[i].path_len == strlen (path) &&
          memcmp (strings + records[i].path_offset, path, records[i].path_len) == 0)
        return &records[i];
    }
  return NULL;
}

static gsm_collector *
new_collector (const char *procfs_root,
               const char *cgroupfs_root,
               uint32_t    readers)
{
  gsm_collect_config config = {
    0
  };

  config.struct_size = sizeof config;
  config.api_version = GSM_COLLECT_API_VERSION;
  config.readers = readers;
  config.procfs_root = procfs_root;
  config.cgroupfs_root = cgroupfs_root;
  return gsm_collect_new (&config);
}

static void
check_fixture_snapshot (const char *proc_root,
                        const char *cgroup_root)
{
  gsm_collector *collector = new_collector (proc_root, cgroup_root, 0);
  gsm_snapshot *first = NULL, *second = NULL;
  size_t count = 0, length = 0;

  CHECK (collector != NULL);
  CHECK (gsm_collect_api_version () == GSM_COLLECT_API_VERSION);
  if (collector == NULL)
    return;
  CHECK (gsm_collect_sample (collector, &first) == 0);
  CHECK (gsm_collect_sample (collector, &second) == 0);
  if (first == NULL || second == NULL)
    {
      gsm_snapshot_free (first);
      gsm_snapshot_free (second);
      gsm_collect_free (collector);
      return;
    }

  /* counts: five processes, eleven cgroups, one memory record, three pressure */
  gsm_snapshot_processes (second, &count);
  CHECK (count == 5);
  gsm_snapshot_cgroups (second, &count);
  CHECK (count == 11);
  CHECK (gsm_snapshot_memory (second, &count) != NULL && count == 1);
  CHECK (gsm_snapshot_pressure (second, &count) != NULL && count == GSM_PRESSURE_RESOURCE_COUNT);
  CHECK (gsm_snapshot_stats (second)->processes_exited == 0);
  CHECK (gsm_snapshot_stats (second)->sampled_at_ns > gsm_snapshot_stats (first)->sampled_at_ns);

  /* one known process, field for field against testdata/proc/344260 */
  const gsm_process *sleeper = find_process (second, 344260);
  CHECK (sleeper != NULL);
  if (sleeper != NULL)
    {
      CHECK (sleeper->struct_size == sizeof (gsm_process));
      CHECK (strcmp (sleeper->comm, "sleep") == 0 && sleeper->comm_len == 5);
      CHECK (sleeper->state == 'S');
      CHECK (sleeper->ppid == 344259 && sleeper->pgrp == 344259 && sleeper->session == 344253);
      CHECK (sleeper->tty_nr == 0 && sleeper->flags == 4194304 && sleeper->num_threads == 1);
      CHECK (sleeper->start_ticks == 484977);
      CHECK (sleeper->utime_ticks == 0 && sleeper->stime_ticks == 0);
      CHECK (sleeper->rss_anon_bytes == 112u * 1024 && sleeper->rss_file_bytes == 2044u * 1024);
      CHECK (sleeper->vm_size_bytes == 8544256);
      /* VmSwap is 0 kB in the file: a real zero, so its bit is set */
      CHECK ((sleeper->known & GSM_PROCESS_HAS_VM_SWAP) != 0 && sleeper->vm_swap_bytes == 0);
      CHECK (sleeper->uid_real == 1000 && (sleeper->known & GSM_PROCESS_HAS_UID) != 0);
      CHECK (sleeper->run_ns == 482125 && sleeper->run_delay_ns == 71565 && sleeper->timeslices == 2);
      CHECK ((sleeper->known & GSM_PROCESS_HAS_CGROUP) != 0);
      const char *strings = gsm_snapshot_strings (second, &length);
      const char *expected =
        "/user.slice/user-1000.slice/user@1000.service/app.slice/app-com.visualstudio.CodeOSS-4247.scope";
      CHECK (sleeper->cgroup_len == strlen (expected));
      CHECK (sleeper->cgroup_offset + (size_t) sleeper->cgroup_len < length);
      CHECK (strcmp (strings + sleeper->cgroup_offset, expected) == 0);  /* NUL-terminated */
    }

  /* a kernel thread has no mm: its RSS is unknown, and says so */
  const gsm_process *kthreadd = find_process (second, 2);
  CHECK (kthreadd != NULL);
  if (kthreadd != NULL)
    {
      CHECK ((kthreadd->flags & GSM_PF_KTHREAD) != 0);
      CHECK ((kthreadd->known & GSM_PROCESS_HAS_RSS_ANON) == 0 && kthreadd->rss_anon_bytes == 0);
      CHECK ((kthreadd->known & GSM_PROCESS_HAS_VM_SIZE) == 0);
    }

  /* a cgroup path from the string table, with its counters */
  const gsm_cgroup *job = find_cgroup (second,
                                       "/user.slice/user-1000.slice/user@1000.service/agents.slice/agents-alpha.slice/job-alpha-386902.scope");
  CHECK (job != NULL);
  if (job != NULL)
    {
      CHECK ((job->known & GSM_CGROUP_HAS_MEMORY_CURRENT) != 0);
      CHECK (job->memory_current_bytes == 4385796096ULL);
      CHECK ((job->known & GSM_CGROUP_HAS_PARENT) != 0);
    }
  const gsm_cgroup *root = find_cgroup (second, "/");
  CHECK (root != NULL && (root->known & GSM_CGROUP_HAS_PARENT) == 0);
  CHECK (root != NULL && (root->known & GSM_CGROUP_HAS_MEMORY_CURRENT) == 0);  /* the root has none */

  /* memory */
  const gsm_memory *memory = gsm_snapshot_memory (second, &count);
  CHECK (memory != NULL && memory->mem_total_bytes == 32557452ULL * 1024);
  CHECK (memory != NULL && (memory->known & GSM_MEMORY_HAS_PARTITION) != 0);
  CHECK (memory != NULL && memory->used_bytes == memory->mem_total_bytes - memory->mem_available_bytes);

  /* rates over the two samples: the fixture does not change, so rates are 0, not unknown */
  if (sleeper != NULL)
    {
      const gsm_process *before = find_process (first, 344260);
      double rate = -1.0;
      CHECK (before != NULL);
      CHECK (gsm_rate_per_s (before->run_ns, sleeper->run_ns,
                             gsm_snapshot_stats (first)->sampled_at_ns,
                             gsm_snapshot_stats (second)->sampled_at_ns, &rate) == 1);
      CHECK (rate == 0.0);
    }
  {
    const gsm_pressure *before = gsm_snapshot_pressure (first, &count);
    const gsm_pressure *after = gsm_snapshot_pressure (second, &count);
    double share = -1.0;
    CHECK (before != NULL && after != NULL);
    CHECK (gsm_pressure_share (before[GSM_PRESSURE_MEMORY].some.total_us,
                               after[GSM_PRESSURE_MEMORY].some.total_us,
                               gsm_snapshot_stats (first)->sampled_at_ns,
                               gsm_snapshot_stats (second)->sampled_at_ns, &share) == 1);
    CHECK (share == 0.0);
  }

  gsm_snapshot_free (first);
  gsm_snapshot_free (second);
  gsm_collect_free (collector);
}

/* The records exactly as layout 1 of the header declared them (the fields of
 * gsm_process up to comm, and of gsm_sample_stats up to reserved): a consumer
 * built against layout 1 indexes the arrays with these. */
typedef struct
{
  uint32_t struct_size;
  uint32_t pid;
  uint64_t known;
  uint64_t start_ticks;
  uint64_t utime_ticks;
  uint64_t stime_ticks;
  uint64_t rss_anon_bytes;
  uint64_t rss_file_bytes;
  uint64_t rss_shmem_bytes;
  uint64_t vm_swap_bytes;
  uint64_t vm_size_bytes;
  uint64_t run_ns;
  uint64_t run_delay_ns;
  uint64_t timeslices;
  uint32_t ppid;
  uint32_t pgrp;
  uint32_t session;
  int32_t tty_nr;
  uint32_t flags;
  uint32_t num_threads;
  uint32_t uid_real;
  uint32_t cgroup_offset;
  uint32_t cgroup_len;
  uint8_t state;
  uint8_t comm_len;
  uint8_t reserved[6];
  char comm[64];
} gsm_process_layout1;

typedef struct
{
  uint32_t struct_size;
  uint32_t readers_run;
  uint64_t sampled_at_ns;
  uint32_t processes_exited;
  uint32_t process_files_unreadable;
  uint32_t process_files_malformed;
  uint32_t cgroups_vanished;
  uint32_t cgroup_files_unreadable;
  uint32_t cgroup_files_malformed;
  int32_t reader_errno[GSM_READER_COUNT];
  uint32_t reserved[2];
} gsm_sample_stats_layout1;

static gsm_collector *
new_collector_layout (const char *procfs_root,
                      const char *cgroupfs_root,
                      uint32_t    api_version)
{
  gsm_collect_config config = {
    0
  };

  config.struct_size = sizeof config;
  config.api_version = api_version;
  config.readers = GSM_READ_PROCESSES | GSM_READ_CGROUPS;
  config.procfs_root = procfs_root;
  config.cgroupfs_root = cgroupfs_root;
  return gsm_collect_new (&config);
}

static void
check_layouts (const char *proc_root,
               const char *cgroup_root)
{
  /* the sizes layout 1 shipped with, and layout 2 appending without padding */
  CHECK (sizeof (gsm_process_layout1) == 216);
  CHECK (sizeof (gsm_sample_stats_layout1) == 64);
  CHECK (offsetof (gsm_process, priority) == 212);
  CHECK (offsetof (gsm_process, comm) == offsetof (gsm_process_layout1, comm));
  CHECK (offsetof (gsm_process, io_rchar_bytes) % 8 == 0);
  CHECK (sizeof (gsm_process) == 280);
  CHECK (sizeof (gsm_sample_stats) == 72);
  CHECK (offsetof (gsm_sample_stats, process_io_unreadable) == sizeof (gsm_sample_stats_layout1));
  CHECK (GSM_COLLECT_API_VERSION == 2);

  gsm_collector *new_reader = new_collector_layout (proc_root, cgroup_root, 2);
  gsm_collector *old_reader = new_collector_layout (proc_root, cgroup_root, 1);
  gsm_snapshot *new_snapshot = NULL, *old_snapshot = NULL;

  CHECK (new_reader != NULL && old_reader != NULL);
  if (new_reader == NULL || old_reader == NULL)
    {
      gsm_collect_free (new_reader);
      gsm_collect_free (old_reader);
      return;
    }
  CHECK (gsm_collect_sample (new_reader, &new_snapshot) == 0);
  CHECK (gsm_collect_sample (old_reader, &old_snapshot) == 0);
  if (new_snapshot == NULL || old_snapshot == NULL)
    {
      gsm_snapshot_free (new_snapshot);
      gsm_snapshot_free (old_snapshot);
      gsm_collect_free (new_reader);
      gsm_collect_free (old_reader);
      return;
    }

  /* layout 2: nice, priority and io, read through the header */
  const gsm_process *sleeper = find_process (new_snapshot, 344260);
  const gsm_process *shell = find_process (new_snapshot, 344261);
  CHECK (sleeper != NULL && shell != NULL);
  if (sleeper != NULL && shell != NULL)
    {
      CHECK (sleeper->struct_size == sizeof (gsm_process));
      CHECK (sleeper->priority == 20 && sleeper->nice == 0);
      CHECK ((sleeper->known & GSM_PROCESS_HAS_IO) != 0);
      CHECK (sleeper->io_rchar_bytes == 4092 && sleeper->io_syscr == 9 && sleeper->io_write_bytes == 0);
      CHECK ((shell->known & GSM_PROCESS_HAS_IO) != 0);
      CHECK (shell->io_rchar_bytes == 1048576 && shell->io_wchar_bytes == 524288);
      CHECK (shell->io_syscr == 812 && shell->io_syscw == 301);
      CHECK (shell->io_read_bytes == 262144 && shell->io_write_bytes == 131072);
      CHECK (shell->io_cancelled_write_bytes == 4096);
    }

  /* a process with no io file in the capture: unknown, and not zero-as-known */
  const gsm_process *init_process = find_process (new_snapshot, 1);
  CHECK (init_process != NULL && (init_process->known & GSM_PROCESS_HAS_IO) == 0);
  CHECK (init_process != NULL && init_process->priority == 20);  /* stat is always there */
  /* three captured processes have no io file; that is not an unexpected failure */
  const gsm_sample_stats *new_stats = gsm_snapshot_stats (new_snapshot);
  CHECK (new_stats->struct_size == sizeof (gsm_sample_stats));
  CHECK (new_stats->process_io_unreadable == 3);
  CHECK (new_stats->process_files_unreadable == 0);

  /* layout 1: the old size and stride, the same processes in the same order */
  const gsm_sample_stats_layout1 *old_stats = (const gsm_sample_stats_layout1 *) gsm_snapshot_stats (old_snapshot);
  CHECK (old_stats->struct_size == sizeof (gsm_sample_stats_layout1));
  CHECK (old_stats->process_files_unreadable == 0);

  size_t old_count = 0, new_count = 0, old_length = 0, new_length = 0;
  const gsm_process_layout1 *old_records = (const gsm_process_layout1 *) gsm_snapshot_processes (old_snapshot, &old_count);
  const gsm_process *new_records = gsm_snapshot_processes (new_snapshot, &new_count);
  const char *old_strings = gsm_snapshot_strings (old_snapshot, &old_length);
  const char *new_strings = gsm_snapshot_strings (new_snapshot, &new_length);
  CHECK (old_count == 5 && new_count == 5);
  for (size_t i = 0; i < old_count && i < new_count; i++)
    {
      CHECK (old_records[i].struct_size == sizeof (gsm_process_layout1));
      CHECK (old_records[i].pid == new_records[i].pid);
      CHECK (old_records[i].start_ticks == new_records[i].start_ticks);
      CHECK (strcmp (old_records[i].comm, new_records[i].comm) == 0);
      CHECK ((old_records[i].known & GSM_PROCESS_HAS_IO) == 0);
      CHECK (old_records[i].cgroup_len == new_records[i].cgroup_len);
      CHECK (old_records[i].cgroup_offset + (size_t) old_records[i].cgroup_len < old_length);
      CHECK (new_records[i].cgroup_offset + (size_t) new_records[i].cgroup_len < new_length);
      CHECK (strcmp (old_strings + old_records[i].cgroup_offset,
                     new_strings + new_records[i].cgroup_offset) == 0);
    }

  gsm_snapshot_free (new_snapshot);
  gsm_snapshot_free (old_snapshot);
  gsm_collect_free (new_reader);
  gsm_collect_free (old_reader);
}

static void
check_helpers_and_errors (void)
{
  double value = -1.0;

  /* 200 counts in 2 s */
  CHECK (gsm_rate_per_s (100, 300, 1000000000ULL, 3000000000ULL, &value) == 1 && value == 100.0);
  /* a counter that went backwards, or no time between samples, is unknown */
  CHECK (gsm_rate_per_s (300, 100, 1, 2000000000ULL, &value) == 0);
  CHECK (gsm_rate_per_s (1, 2, 5, 5, &value) == 0);
  /* 500 ms stalled in a 2 s window */
  CHECK (gsm_pressure_share (0, 500000, 0, 2000000000ULL, &value) == 1 && value == 25.0);

  CHECK (strcmp (gsm_collect_strerror (-ENOMEM), "out of memory") == 0);
  CHECK (strlen (gsm_collect_strerror (-12345)) > 0);

  /* invalid configurations give no collector */
  gsm_collect_config config = {
    0
  };
  CHECK (gsm_collect_new (&config) == NULL);
  config.struct_size = sizeof config;
  config.api_version = GSM_COLLECT_API_VERSION + 1;
  CHECK (gsm_collect_new (&config) == NULL);

  /* gsm_collect_open says why */
  gsm_collector *opened = (gsm_collector *) 1;
  CHECK (gsm_collect_open (&config, &opened) == -EINVAL);
  CHECK (opened == NULL);
  config.api_version = GSM_COLLECT_API_VERSION;
  config.readers = 0x10;
  CHECK (gsm_collect_open (&config, &opened) == -EINVAL);
  CHECK (gsm_collect_open (NULL, NULL) == -EINVAL);
  config.readers = 0;
  CHECK (gsm_collect_open (&config, &opened) == 0 && opened != NULL);
  gsm_collect_free (opened);
  CHECK (gsm_collect_open (NULL, &opened) == 0 && opened != NULL);
  gsm_collect_free (opened);

  /* a procfs root that does not exist fails the only reader asked for */
  gsm_collector *collector = new_collector ("/nonexistent/proc", "/nonexistent/cgroup", GSM_READ_PROCESSES);
  gsm_snapshot *snapshot = (gsm_snapshot *) 1;
  CHECK (collector != NULL);
  CHECK (gsm_collect_sample (collector, &snapshot) == -ENOENT);
  CHECK (snapshot == NULL);
  gsm_collect_free (collector);
}

static size_t
count_proc_entries (void)
{
  DIR *dir = opendir ("/proc");
  size_t numeric = 0;
  struct dirent *entry;

  if (dir == NULL)
    return 0;
  while ((entry = readdir (dir)) != NULL)
    {
      const char *name = entry->d_name;
      if (!isdigit ((unsigned char) name[0]))
        continue;
      size_t i = 0;
      while (isdigit ((unsigned char) name[i]))
        {
          i++;
        }
      if (name[i] == '\0')
        numeric++;
    }
  closedir (dir);
  return numeric;
}

static void
check_live_machine (void)
{
  gsm_collector *collector = gsm_collect_new (NULL);
  gsm_snapshot *snapshot = NULL;
  size_t count = 0;

  CHECK (collector != NULL);
  if (collector == NULL)
    return;
  size_t before = count_proc_entries ();
  CHECK (gsm_collect_sample (collector, &snapshot) == 0);
  if (snapshot != NULL)
    {
      size_t listed = 0;
      size_t after = count_proc_entries ();
      gsm_snapshot_processes (snapshot, &listed);
      /* Processes come and go while the walk runs (a parallel build forks
       * hundreds a second), so the walk is checked against /proc read just
       * before and just after it, with a margin of a tenth: a real miscount
       * (a skipped directory, a parse failure) still falls outside. */
      size_t lo = before < after ? before : after;
      size_t hi = before < after ? after : before;
      size_t slack = lo / 10 + 5;
      CHECK (listed + slack >= lo && listed <= hi + slack);
      CHECK (find_process (snapshot, 1) != NULL);
      const gsm_memory *memory = gsm_snapshot_memory (snapshot, &count);
      CHECK (memory != NULL && memory->mem_total_bytes > 0);
      CHECK (gsm_snapshot_stats (snapshot)->reader_errno[0] == 0);
      gsm_snapshot_free (snapshot);
    }
  gsm_collect_free (collector);
}

int
main (int    argc,
      char **argv)
{
  if (argc != 3)
    {
      fprintf (stderr, "usage: %s FIXTURE_PROC FIXTURE_CGROUP\n", argv[0]);
      return 2;
    }
  check_fixture_snapshot (argv[1], argv[2]);
  check_layouts (argv[1], argv[2]);
  check_helpers_and_errors ();
  check_live_machine ();
  if (failures != 0)
    {
      fprintf (stderr, "%d check(s) failed\n", failures);
      return 1;
    }
  return 0;
}
