# gsmcollect: the process collector

A small Zig 0.16 library that walks `/proc` once and returns one record per
live process. It is the first slice of the collector that will replace
libgtop's per-process walk; it has no C header yet and is built on its own.

## What it reads

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

## Guarantees

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

## Cost

Measured 2026-10-28 on a 16-thread laptop with 482 processes, ReleaseFast:
a walk takes about 5.8 ms (5 runs: 5.6 to 6.1 ms), 12.0 syscalls per process
(`strace -c -f`: 1933 `openat`, 1932 `preadv`, 1933 `close`, 8 `getdents64`
for 483 entries), no subprocess, and **0 bytes written**. A reused
`Snapshot` allocates nothing on later walks unless the process count grows.
`statm` is not read: `status` carries `VmSize` and the Rss split.

## Build and test

```sh
export ZIG_LOCAL_CACHE_DIR=/dev/shm/gsmcollect-cache   # optional: build cache in RAM
zig build test                      # every test, over testdata/
zig build test -Dtest-filter=NAME   # only tests whose name contains NAME
zig build -Doptimize=ReleaseFast    # zig-out/bin/gsmcollect-probe
zig-out/bin/gsmcollect-probe --once [--root DIR] [--show PID]
zig fmt --check .
```

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

`testdata/proc/<pid>/{stat,status,schedstat,cgroup}` was captured with
`tools/capture-fixtures.sh DEST --standard` (pid 1, pid 2, a kworker, and a
`sleep` and a shell the script starts and stops itself). The script refuses
any other file name. The fixtures are public: never add `cmdline`,
`environ`, `maps` or `fd`.
