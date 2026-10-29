# gsmcollect: the collector

A small Zig 0.16 library that reads the kernel's own counters: one record per
live process from `/proc`, the system memory partition from `/proc/meminfo`,
pressure stall information, and the whole cgroup v2 tree. It is meant to
replace libgtop's per-process walk, and reaches C and C++ code through the
[C interface](#c-interface) in `include/gsmcollect.h`. Every reader takes a
root directory, so tests run against captured trees.

Readers: [processes](#processes) · [system memory](#system-memory) ·
[pressure](#pressure) · [cgroup tree](#cgroup-tree) · [C interface](#c-interface)

## Processes

### What it reads

For each process, five files, each opened, read with one `pread` and closed
(at most 15 syscalls per process, nothing else; 13 when `io` is refused):

| File | Fields kept | Source of the definition |
|---|---|---|
| `stat` | pid, comm, state, ppid, pgrp, session, tty_nr, flags, utime/stime (ticks), priority and nice (fields 18 and 19), num_threads, starttime (ticks) | `proc_pid_stat(5)` |
| `status` | `RssAnon`, `RssFile`, `RssShmem`, `VmSwap`, `VmSize` (kB to bytes), real uid | `proc_pid_status(5)` |
| `schedstat` | run time ns, run delay ns, timeslices | `Documentation/scheduler/sched-stats.rst` |
| `io` | `rchar`, `wchar`, `syscr`, `syscw`, `read_bytes`, `write_bytes`, `cancelled_write_bytes`, known together | `proc_pid_io(5)` |
| `cgroup` | the cgroup v2 path (`0::`); v1 lines ignored | `Documentation/admin-guide/cgroup-v2.rst` |

A record is keyed by `(pid, start_ticks)`: a reused pid has a new start time.
Kernel threads are recognised by `PF_KTHREAD` (`0x00200000`) in `flags`.
Nothing else is read: no `cmdline`, `environ`, `maps` or `fd`.

### Guarantees

- A process that vanishes mid-read is counted in `exited` and gets **no
  record**, never a record of zeros.
- A file that cannot be read (permission, I/O) leaves that part of the record
  `null` and is counted in `unreadable[file]`; one that is cut short, too
  long (8 KiB buffer) or malformed is `null` and counted in `malformed[file]`.
  Unknown is never zero.
- `stat` is the key. If it cannot be read or parsed the process is not listed
  and the failure is counted.
- `schedstat` is absent without `CONFIG_SCHED_INFO`, and `io` without
  `CONFIG_TASK_IO_ACCOUNTING`; a missing one is `null`, not an exit (the
  `cgroup` read comes last and catches a vanish).
- `io` is refused (EACCES at the open) for other users' processes, about two
  thirds of them on a workstation. That is the normal case: the counters are
  unknown (not zero) and the refusal is counted in `unreadable[io]`, which
  the C interface reports as `process_io_unreadable`, apart from the
  unexpected `process_files_unreadable`. A refused open is one syscall.
- `comm` is parsed from the **last** `)`: it may hold spaces and parentheses,
  and a kworker's is longer than 15 bytes (up to 63).
- Malformed input is an error value; the parsers never panic (fuzz tested).
- The walk takes a root directory, so tests run against captured trees.
- `Counts.file_syscalls` counts the `openat`, `pread` and `close` calls of the
  per-process reads (a failed `openat` counts; the directory listing does
  not), and a test asserts it per process: exactly 15 with all five files, 13
  where `io` is absent or refused.

### Cost

Measured 2026-10-28 on a 16-thread laptop with 482 processes, ReleaseFast:
a walk takes about 5.8 ms (5 runs: 5.6 to 6.1 ms), 12.0 syscalls per process
(`strace -c -f`: 1933 `openat`, 1932 `preadv`, 1933 `close`, 8 `getdents64`
for 483 entries), no subprocess, and **0 bytes written**. A reused
`Snapshot` allocates nothing on later walks unless the process count grows.
`statm` is not read: `status` carries `VmSize` and the Rss split.

Measured again 2026-10-29 with `io` added (16-thread laptop, 523 processes,
ReleaseSafe): **13.8 file syscalls per process** (`strace -c -f` on
`gsmcollect-probe --once`: 2616 `openat`, 327 of them the refused `io` of
other users' processes, 2288 `preadv`, 2289 `close`, 9 `getdents64`; the
probe's own `file_syscalls` counter says 7191 for 523 processes). A refused
`io` costs 1 syscall and a read one 3, so the figure sits between 13 and 15.
Still **0 bytes written**, no subprocess.

The walk's CPU time (thread CPU time, median of five walks) depends on the
machine's state, not only on the code. Per 500 processes, 2026-10-29:

| Condition | ms per 500 |
|---|---:|
| quiet machine | 8.3 |
| load average about 5, unpinned | 10.4 to 13.3 |
| load 4.7, pinned to a P-core (cores at 3.1 of 4.9 GHz) | 15.5 to 16.1 |
| load 4.7, pinned to an E-core | 24.2 to 25.0 |

So the 10 ms per 500 processes is a design target for a quiet machine, and
the figure is reported, not asserted. Both budgets are tests:

- `zig build test` asserts the file syscalls per process over the fixture
  tree: exactly 15 with all five files, 13 where `io` is absent or refused.
- `zig build budget` (ReleaseSafe on the live `/proc`; skipped under 100
  processes or when the thread CPU clock cannot be read) asserts the walk
  costs **at most 1.3 times a baseline measured in the same run**: the
  baseline makes the syscalls the walk cannot avoid, the listing of `/proc`
  and an open, one pread and a close of the same five files of the same
  processes, and parses nothing, so the ratio is the collector's own work
  over the kernel's. Walk and baseline alternate on one thread pinned to
  the CPU it started on (its own affinity, restored afterwards), and the
  median of 9 per-pair ratios is asserted. Each run prints both absolute
  medians and the ratio. Measured on 2026-10-29 at load average 5.9: ratios
  1.15 to 1.21 over five runs (walk 18.9 to 20.2 ms per 500, baseline 15.9
  to 16.8). Without the listing in the baseline the ratios were 1.22 to
  1.28: listing `/proc` is about 6% of the kernel's share. Meson runs the
  step as the suite `collector-budget`.

## System memory

`meminfo.zig` reads `/proc/meminfo` (one read, about 20 µs measured) into
named byte fields (the file's kB times 1024; a key that is not in the file is
`null`, never zero) and partitions `MemTotal` into rows that say what each
byte is. The definitions are those of `Documentation/filesystems/proc.rst`
and the GUI's `src/meminfo.c`, and both give the same rows on the same file
(the two captured `/proc/meminfo` files are tested here with the same
assertions as in the C suite):

| Row | Formula |
|---|---|
| application private | `AnonPages` |
| RAM-backed files | `Shmem` (tmpfs, shared memory; not reclaimable without swap) |
| file cache | `Cached - Shmem + Buffers` |
| kernel reclaimable | `max(SReclaimable, KReclaimable)` |
| kernel fixed | `SUnreclaim + KernelStack + PageTables + SecPageTables + Percpu + VmallocUsed` (approximate: `VmallocUsed` may overlap) |
| free | `MemFree` |
| unaccounted / overlap | `MemTotal` minus the rows above, or the excess when they exceed it; never negative |

`partition` returns `null` when a field the rows need is missing (a row of
zeros would look like an answer). `usedBytes` is `MemTotal - MemAvailable`.
A known key whose value is not `<digits> kB` is an error; other keys are not
looked at. **Cost:** 3 syscalls, 0 bytes written.

## Pressure

`pressure.zig` parses one pressure file (`/proc/pressure/{cpu,memory,io}` or a
cgroup's `*.pressure`, `Documentation/accounting/psi.rst`): a `some` line and
an optional `full` line with `avg10/60/300` (percent) and `total` (µs). A
missing `full` (CPU on kernels before 5.13) is `null`. `stallPercent` gives
the share of a window from two samples, `Δtotal_us / Δwall_us × 100`; it is
unknown (`null`) for an empty window or a counter that went backwards, and it
is not clamped (two reads are never simultaneous). **Cost:** 3 syscalls per
file, 0 bytes written; the three system files read and parse in 33 to 60 µs (3 runs).

## Cgroup tree

`cgroup_tree.zig` walks a cgroupfs mount (`/sys/fs/cgroup`, or a subtree of
it with `under`) and returns one `Group` per cgroup, parents before children,
siblings by name. Per group, from the group's own counters
(`Documentation/admin-guide/cgroup-v2.rst`; the groups' totals add up to the
root's, which summing processes does not):

| File | Fields kept |
|---|---|
| `memory.current`, `memory.peak` | bytes |
| `memory.max`, `memory.high` | bytes, or unlimited |
| `memory.stat` | `anon`, `file`, `shmem`, `kernel`, `sock`, `slab_reclaimable`, `slab_unreclaimable`, `file_mapped`, `file_dirty` (bytes); `workingset_refault_anon`, `workingset_refault_file`, `pgmajfault` (counts) |
| `memory.events` | `high`, `max`, `oom`, `oom_kill` (counts) |
| `cpu.stat` | `usage_usec`, `user_usec`, `system_usec`, `nr_throttled`, `throttled_usec` |
| `pids.current` | processes and threads |
| `memory.pressure`, `cpu.pressure`, `io.pressure` | as in Pressure above |

- A file that does not exist leaves its field `null`: the root has no
  `memory.current`, and a group without the cpu controller has no
  `nr_throttled` (live example in the fixtures). Not measured is not zero.
- A file that exists but cannot be read, or is cut short, over 16 KiB or
  malformed, is `null` and counted per file in `Counts`.
- A group removed during the walk is **counted in `vanished` and not listed**.
  The tell is `cgroup.controllers`, which every live group has (the root
  too): it is opened only when another file of the group was absent or
  unreadable, so a healthy group costs nothing extra, and the failures seen
  while it was dying are not charged to the files. A directory without
  `cgroup.controllers` is therefore taken as removed.
- Nesting deeper than 64 is cut off and counted in `too_deep`.
- `under` must be an absolute cgroup path without `.`, `..` or empty parts.

**Cost**, measured 2026-10-28 on the 16-thread laptop, ReleaseFast: the whole
tree of 127 groups takes 8.9 to 10.4 ms (5 runs), a subtree of 8 groups
0.63 ms. `strace -c -f` of one whole-tree run: 5052 syscalls in total
(1529 `openat`, 1385 `preadv`, 1516 `close`, 333 `getdents64`, 127 `lseek`),
about 12 per group for the files plus about 3 for the directory, no
subprocess, 0 bytes written. Memory is one 16 KiB read buffer plus the group
records and path strings, reused by a later walk. A refresh of the whole
tree every second would use about 1% of one CPU; walk only the subtree you
show where you can.

## Workload classes

`src/classify.zig` puts every cgroup path and every process into one of nine
classes (Kernel, Init, System services, Session services, Applications,
Login session, Agent sessions, Containers and VMs, Unclassified) by an
ordered rule table: an array of `Rule` records, first match wins, each with a
name, a class, a confidence (high, medium, low) and the kind of key it
extracts. The names are `classify.rules[i].name`. It is not yet part of the C
interface: nothing in `gsmcollect.h` or the walk changes.

Two passes, both pure functions of what the walk already read (**zero
syscalls**):

- `classifyPath(path)` uses the cgroup path alone, with systemd's naming:
  the slices of systemd.special(7), `app[-LAUNCHER]-APPID-RANDOM.scope` and
  `app[-LAUNCHER]-APPID[@RANDOM].service` from the desktop-environment
  conventions, `session-ID.scope` from logind, and the cgroup names that
  docker, libpod, kubepods and LXC give containers.
- `classifyProcesses(gpa, processes, out)` classifies a whole snapshot. It
  adds kernel threads (`PF_KTHREAD`, or pid 2), pid 1, a process with no
  usable cgroup (the uid and the controlling terminal stand in; an unknown uid
  is never guessed), and the **ancestry overlay**: a process whose ancestors
  within the snapshot, or itself, run a program with `comm` `claude`,
  `opencode` or `pi`, and whose path class is Applications, Session services
  or Login session, becomes an Agent session keyed by the outermost such
  ancestor's `(pid, start_ticks)`. A parent that started after its child is a
  reused pid and is not followed; a cycle or a self-parent ends the climb.
  Ancestry is resolved once per process (linear), through one hash map and
  one array of `2 * n` indexes, the only allocations, made only when some
  process is an agent program.

A `Classification` holds the class, the rule index, the confidence and the
key. Keys are slices of the input (a cgroup path or a comm), still escaped as
the directory holds them; `unit_path.unescape` decodes `\xNN` into a caller's
buffer. A slice whose whole subtree is one class carries that class (`system.slice`
and the slices under it, `machine.slice`, the top-level `docker` and `lxc`,
the user manager's `session.slice` and `background.slice`, `agents.slice`
and `agents-P.slice` with the project as key, an application's
`app-APPID.slice`), because memory charged to a removed unit moves to its
parent and a class total must find it there. A node that holds several
classes (the root, `user.slice`, `user-N.slice`, a user manager's directory,
its `app.slice`) is **mixed**: `Classification.mixed` is set, it is neither a
class nor Unclassified, and totals recurse into its children. A process is
never mixed: one found in such a node is Unclassified. Units inside an
application's slice (a terminal's `vte-spawn-*.scope`) are that application at
medium confidence, the layout being the launcher's convention.

`src/unit_path.zig` derives the unit, the user unit, the login session id and
the owner uid from a path the way systemd does (`cg_path_get_unit`,
`cg_path_get_user_unit`, `cg_path_get_session`, `cg_path_get_owner_uid` in
src/basic/cgroup-util.c at v255, re-implemented), without a syscall: it
replaces libsystemd's `sd_pid_get_unit` family, which opens `/proc/PID/cgroup`
again for each call.

Tests: table tests over `testdata/cgroup-paths.txt` (the directory paths of
a workstation's cgroup tree, private names replaced, plus hand-written
shapes of the cases the capture lacked), a synthetic process tree (agent
scopes and jobs, an orphaned agent shell, a session inside an editor's scope,
a nested `claude -p`, a ppid cycle, a reused pid), the fixture walk, an
allocation budget, the mixed nodes and single-class slices, and three fuzz tests (arbitrary path bytes, arbitrary
process tables, and `unit_path`'s).

Cost, measured 2026-10-29 on the development workstation (ReleaseSafe, the
live `/proc` of 510 processes walked once, then classified 200 times, median
of 9 timings): **277 microseconds of thread CPU per 500 processes**, about 2.5%
of the walk's own 11 ms, with two allocations per pass and none when no
process is an agent program; 0 bytes written. That machine's processes fell
into Kernel 237, Init 1, System services 82, Session services 39,
Applications 30, Login session 88, Agent sessions 33, Unclassified 0; its 135
cgroups into 4 mixed, 0 Unclassified, 1 Init, 74 System services, 33 Session
services, 3 Applications, 2 Login session, 17 Agent sessions and 1 Containers
and VMs.

## C interface

`include/gsmcollect.h` (C11, `<stdint.h>` and `<stddef.h>` only,
`GSM_COLLECT_API_VERSION 2`) is the contract; the Zig side takes its types
and `known` bits from the header at build time, so they cannot drift.

```c
gsm_collector *c = gsm_collect_new(NULL);            /* defaults: /proc, /sys/fs/cgroup, all readers */
gsm_snapshot *snap;
int err = gsm_collect_sample(c, &snap);              /* 0, or a negative errno */
size_t n; const gsm_process *p = gsm_snapshot_processes(snap, &n);
/* p[i].known & GSM_PROCESS_HAS_RSS_ANON says whether p[i].rss_anon_bytes is a measurement */
gsm_snapshot_free(snap); gsm_collect_free(c);
```

- **A collector is used from one thread at a time**; a snapshot is
  read-only and may be read from any thread until it is freed. Snapshots are
  allocated and freed by the library (Zig's own allocator, never libc's), and a
  collector reuses its walk buffers between samples.
- **Records** are arrays of plain structs (`gsm_process`, `gsm_cgroup`,
  `gsm_memory`, `gsm_pressure`), each starting with `uint32_t struct_size`.
  `GSM_COLLECT_API_VERSION` counts record layouts and a layout only appends
  to the one before. The library fills the layout of the `api_version` the
  caller asked for (0 means the newest): a layout-1 caller gets `gsm_process`
  records of 216 bytes and a `gsm_sample_stats` with `struct_size` 64,
  holding exactly layout 1's fields; layout 2 adds `priority` and `nice`
  (`stat` fields 18 and 19) and the seven `/proc/<pid>/io` counters under
  `GSM_PROCESS_HAS_IO` (280 bytes), and `process_io_unreadable` in the stats.
  The library writes the full record and hands over its first `struct_size`
  bytes at that stride.
- **Unknown is a bit, not a sentinel.** Each record has `uint64_t known`;
  a field whose bit is clear is 0 and must not be shown as 0. A kernel
  thread has no `GSM_PROCESS_HAS_RSS_ANON`; a cgroup without a memory
  controller has no `GSM_CGROUP_HAS_MEMORY_CURRENT`; `memory.max` of `max` is
  `GSM_CGROUP_MEMORY_MAX_UNLIMITED`, a different bit from a finite limit.
- **Raw counters plus `sampled_at_ns`** (CLOCK_MONOTONIC, taken before the
  reads). Rates and stall shares are computed from two snapshots with the
  header's `gsm_rate_per_s` and `gsm_pressure_share`, which return 0 (unknown)
  for an empty window or a counter that went backwards. A process is the same
  across snapshots when `(pid, start_ticks)` match.
- **Strings:** `comm` is a NUL-terminated `char[64]`; a cgroup path is an
  offset and length into `gsm_snapshot_strings()` (also NUL-terminated;
  a process in a cgroup of the tree shares the tree's string). Bytes, not
  assumed UTF-8.
- **Errors** are negative errno values. A process or cgroup that vanished
  mid-read is not an error (`gsm_sample_stats.processes_exited`,
  `cgroups_vanished`). One reader failing while another works is not an error
  of the sample either: its records are absent and its errno is in
  `reader_errno`; only when every requested reader fails does
  `gsm_collect_sample` return it. `gsm_collect_strerror` names the values.
- **Build:** Meson runs `zig build library` (Zig exactly 0.16.0, checked at
  configure time) for `x86_64-linux-gnu`, `-Dcpu=baseline` (only SSE2 is
  required, so a packaged binary runs on any x86-64 machine), `ReleaseSafe`,
  position-independent, and registers three test suites, `collector-zig`, `collector-budget` (the
  overhead budgets on the live machine) and
  `collector-c` (a plain C program that links the library and checks a
  snapshot over the fixture trees, `tests/c/fixture/proc` and
  `testdata/cgroup`, then once over the live `/proc`). Nothing is installed.
- **Link notes:** the archive holds one object and exports only `gsm_*`
  symbols. It has no libc inside: the final link supplies `memcpy`, `memmove`,
  `memset`, `getauxval` and `__tls_get_addr` from the C library and `__divti3`
  from libgcc or compiler-rt, as for any C code. Stack probes stay on: Zig's
  probe routine is carried in the object as a local (not exported) symbol,
  because Zig's compiler-rt is not bundled.
- **Opening:** `gsm_collect_open(config, &collector)` returns 0 or a negative
  errno (`-EINVAL` for a bad configuration, `-ENOMEM`); `gsm_collect_new` is
  the same without the reason. The collector owns the read buffer of the
  files `c_api.zig` reads itself; the process and cgroup walkers still keep
  their 8 and 16 KiB read buffers, and the process sort its cache, on the
  stack, which is why the probes matter.
- **Cost** of one full `gsm_collect_sample` (all four readers), measured
  2026-10-28 on the 16-thread laptop with 492 processes and 127 cgroups,
  ReleaseSafe, best of 20 in one process: 12.4 ms (3 runs: 12.43, 12.44, 12.57).
  That is about 1.2% of one CPU at a one-second refresh. The snapshot then held
  216 bytes per process (280 in layout 2, so about 29% more for the process
  array: 143 KB for 510 processes), 376 per cgroup and a 6.4 KB string table, about
  190 KB in all; there is no subprocess and **0 bytes are written**.

## Build and test

```sh
export ZIG_LOCAL_CACHE_DIR=/dev/shm/gsmcollect-cache   # optional: build cache in RAM
zig build library                   # zig-out/lib/libgsmcollect.a and zig-out/include/gsmcollect.h
zig build test                      # every test, over testdata/
zig build test -Dtest-filter=NAME   # only tests whose name contains NAME
zig build budget                    # the walk's CPU time on the live /proc (ReleaseSafe)
zig build -Doptimize=ReleaseFast    # zig-out/bin/gsmcollect-probe
zig-out/bin/gsmcollect-probe --once [--root DIR] [--show PID]
zig-out/bin/gsmcollect-probe --memory [--root DIR]
zig-out/bin/gsmcollect-probe --pressure [--root DIR] [--window-ms N]
zig-out/bin/gsmcollect-probe --cgroups [--cgroup-root DIR] [--under PATH] [--list]
zig fmt --check .
```

The tests are plain `test` blocks with `std.testing`, so `zig build test` is
always the whole suite. When a checkout of the
[ztest](https://github.com/praj107/ztest) runner is placed at
`vendor/ztest` (it is not part of this repository), `zig build ztest` runs
the same tests under it, with its filtering, isolation, timeouts and
reports.

### Fuzzing

Every parser and the walk has a `test "fuzz: ..."`. Zig 0.16.0's default test
runner does not compile under `--fuzz`, and fuzzing needs ReleaseSafe, so:

```sh
zig build test -Doptimize=ReleaseSafe -Dfuzz-runner \
    -Dtest-filter="fuzz: arbitrary stat bytes" --fuzz=500K
```

`tools/fuzz_test_runner.zig` is Zig's runner with a one-line fix (header says
which); `--fuzz=N` fuzzes only the first test matching the filter, so run one
filter per fuzz test.

## Fixtures

`testdata/system/` holds two `/proc/meminfo` captures and the pressure files
(`meminfo-a.txt`, `pressure-cpu-no-full.txt`, ...), taken from the
development machine. `testdata/cgroup/` is a captured slice of a cgroup
tree (the root, a service whose child has no memory controller, and an
agent session scope), made with `tools/capture-cgroup-fixtures.sh DEST
CGROUP_PATH...`, which copies each path and its ancestors and only the 12
files the collector reads. The vanished-group and unreadable-file cases are
built in a temporary directory, because git does not keep empty directories.

`testdata/proc/<pid>/{stat,status,schedstat,io,cgroup}` was captured with
`tools/capture-fixtures.sh DEST --standard` (pid 1, pid 2, a kworker, and a
`sleep` and a shell the script starts and stops itself). Only the two own-user
processes have an `io` file, as for any unprivileged capture: the other
three (pid 1, pid 2, a kworker) exercise the unknown case. The script refuses
any other file name. The fixtures are public: never add `cmdline`,
`environ`, `maps` or `fd`.
