# gsmcollect: the collector

A small Zig 0.16 library that reads the kernel's own counters: one record per
live process from `/proc`, the system memory partition from `/proc/meminfo`,
pressure stall information, and the whole cgroup v2 tree. It is meant to
replace libgtop's per-process walk; it has no C header yet and is built on
its own. Every reader takes a root directory, so tests run against captured
trees.

Readers: [processes](#processes) · [system memory](#system-memory) ·
[pressure](#pressure) · [cgroup tree](#cgroup-tree)

## Processes

### What it reads

For each process, four files, each opened, read with one `pread` and closed
(12 syscalls per process, nothing else):

| File | Fields kept | Source of the definition |
|---|---|---|
| `stat` | pid, comm, state, ppid, pgrp, session, tty_nr, flags, utime/stime (ticks), num_threads, starttime (ticks) | `proc_pid_stat(5)` |
| `status` | `RssAnon`, `RssFile`, `RssShmem`, `VmSwap`, `VmSize` (kB to bytes), real uid | `proc_pid_status(5)` |
| `schedstat` | run time ns, run delay ns, timeslices | `Documentation/scheduler/sched-stats.rst` |
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
- `schedstat` is absent without `CONFIG_SCHED_INFO`; a missing one is
  `null`, not an exit (the `cgroup` read comes last and catches a vanish).
- `comm` is parsed from the **last** `)`: it may hold spaces and parentheses,
  and a kworker's is longer than 15 bytes (up to 63).
- Malformed input is an error value; the parsers never panic (fuzz tested).
- The walk takes a root directory, so tests run against captured trees.

### Cost

Measured 2026-10-28 on a 16-thread laptop with 482 processes, ReleaseFast:
a walk takes about 5.8 ms (5 runs: 5.6 to 6.1 ms), 12.0 syscalls per process
(`strace -c -f`: 1933 `openat`, 1932 `preadv`, 1933 `close`, 8 `getdents64`
for 483 entries), no subprocess, and **0 bytes written**. A reused
`Snapshot` allocates nothing on later walks unless the process count grows.
`statm` is not read: `status` carries `VmSize` and the Rss split.

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

## Build and test

```sh
export ZIG_LOCAL_CACHE_DIR=/dev/shm/gsmcollect-cache   # optional: build cache in RAM
zig build test                      # every test, over testdata/
zig build test -Dtest-filter=NAME   # only tests whose name contains NAME
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

`testdata/proc/<pid>/{stat,status,schedstat,cgroup}` was captured with
`tools/capture-fixtures.sh DEST --standard` (pid 1, pid 2, a kworker, and a
`sleep` and a shell the script starts and stops itself). The script refuses
any other file name. The fixtures are public: never add `cmdline`,
`environ`, `maps` or `fd`.
