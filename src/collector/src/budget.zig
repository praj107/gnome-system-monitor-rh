// SPDX-License-Identifier: GPL-2.0-or-later
//! The process walk's cost on the live /proc, asserted rather than eyeballed.
//! Built ReleaseSafe by `zig build budget` (the optimisation the Meson build
//! ships); it is not part of `zig build test`, which runs in Debug over fixtures.
//!
//! Thread CPU time of a /proc walk depends on which core it runs on and on
//! the package's power limit, which other work on the machine sets, so a fixed
//! number of milliseconds passes or fails on what else is running. The budget
//! is therefore a ratio measured in the same run: the walk's CPU time against
//! a baseline that makes only the syscalls the walk must make (the listing of
//! /proc, then open, one pread into a buffer of the walk's size and close for
//! the same five files of the same processes) and parses nothing. The two are interleaved on one
//! thread pinned to the CPU it started on, and the median of the per-pair
//! ratios must stay at or under 1.3: the collector's own work adds at most
//! 30% to the kernel's. Both absolute medians are printed, not asserted. The
//! per-process syscall count is asserted over fixtures in walk.zig.
//!
//! The second budget is the classification that layout 3 adds: the class of
//! every process of the walk, and the unit, user unit, session and owner its
//! cgroup path names, are pure functions of what the walk read (no syscall).
//! Its cost is the ratio of that work to the walk's own CPU time, from the
//! same pair of runs on the same thread, median of the pairs, at most 0.1.
const std = @import("std");
const linux = std.os.linux;
const Io = std.Io;
const walk = @import("walk.zig");
const classify = @import("classify.zig");
const unit_path = @import("unit_path.zig");

const pairs_measured = 9;
/// Fewer processes than this means a build sandbox with a bare /proc, where
/// the figure says nothing about a real machine.
const processes_needed = 100;
const max_ratio = 1.3;
const max_classify_ratio = 0.1;
const baseline_files = [_][]const u8{ "stat", "status", "schedstat", "io", "cgroup" };

/// CPU time of the calling thread, or null when the clock cannot be read (a
/// zero would read as an elapsed time of nothing, which would pass).
fn threadCpuNs() ?u64 {
    var ts: linux.timespec = undefined;
    if (linux.errno(linux.clock_gettime(.THREAD_CPUTIME_ID, &ts)) != .SUCCESS) return null;
    return @as(u64, @intCast(ts.sec)) * std.time.ns_per_s + @as(u64, @intCast(ts.nsec));
}

/// The bare syscalls of the walk for the processes it listed, without
/// parsing: the directory listing (the kernel's work too, so it belongs to
/// the baseline, not to the collector's share), then the file reads. The
/// buffer is the walk's read buffer size.
fn baselinePass(io: Io, root: Io.Dir, snapshot: *const walk.Snapshot) void {
    var entries = root.iterate();
    while (entries.next(io) catch null) |_| {}

    var buffer: [walk.Snapshot.read_buffer_bytes]u8 = undefined;
    var path_buffer: [32]u8 = undefined;
    for (snapshot.processes.items) |process| {
        inline for (baseline_files) |name| {
            const path = std.fmt.bufPrint(&path_buffer, "{d}/" ++ name, .{process.pid}) catch unreachable;
            if (root.openFile(io, path, .{})) |handle| {
                defer handle.close(io);
                _ = handle.readPositional(io, &.{&buffer}, 0) catch 0;
            } else |_| {}
        }
    }
}

/// Pins the calling thread to the CPU it is on, and restores its old
/// affinity on `release`. Returns null (unpinned) when the kernel refuses.
/// It changes only this thread's own CPU mask: no signal, no other task.
const Pin = struct {
    saved: linux.cpu_set_t,

    fn acquire() ?Pin {
        var saved: linux.cpu_set_t = undefined;
        if (linux.sched_getaffinity(0, @sizeOf(linux.cpu_set_t), &saved) != 0) return null;
        // The kernel stores a 32-bit CPU number: start from 0 so the rest is clean.
        var cpu: usize = 0;
        if (linux.getcpu(&cpu, null) != 0) return null;
        if (cpu >= linux.CPU_SETSIZE) return null;
        var only: linux.cpu_set_t = @splat(0);
        const bits = @bitSizeOf(usize);
        only[cpu / bits] = @as(usize, 1) << @intCast(cpu % bits);
        linux.sched_setaffinity(0, &only) catch return null;
        return .{ .saved = saved };
    }

    fn release(self: *Pin) void {
        linux.sched_setaffinity(0, &self.saved) catch {};
    }
};

fn median(comptime T: type, values: []T) T {
    std.mem.sort(T, values, {}, std.sort.asc(T));
    return values[values.len / 2];
}

test "budget: the walk of the live /proc costs at most 1.3 times the bare syscalls (median of 9 interleaved pairs)" {
    const io = std.testing.io;
    var root = Io.Dir.cwd().openDir(io, "/proc", .{ .iterate = true }) catch return error.SkipZigTest;
    defer root.close(io);

    var snapshot: walk.Snapshot = .init(std.testing.allocator);
    defer snapshot.deinit();
    // The first walk grows the snapshot's buffers and warms the page cache.
    try snapshot.walk(io, root);
    if (snapshot.counts.listed < processes_needed) return error.SkipZigTest;
    baselinePass(io, root, &snapshot);

    var pin = Pin.acquire();
    defer if (pin) |*held| held.release();

    var ratios: [pairs_measured]f64 = undefined;
    var walk_ns: [pairs_measured]u64 = undefined;
    var baseline_ns: [pairs_measured]u64 = undefined;
    var listed_total: u64 = 0;
    for (0..pairs_measured) |pair| {
        const walk_started = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");
        try snapshot.walk(io, root);
        const walk_ended = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");
        const baseline_started = walk_ended;
        baselinePass(io, root, &snapshot);
        const baseline_ended = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");

        walk_ns[pair] = walk_ended - walk_started;
        baseline_ns[pair] = baseline_ended - baseline_started;
        // A clock that did not advance cannot give a ratio, and must not pass.
        if (walk_ns[pair] == 0 or baseline_ns[pair] == 0) return skip("the thread CPU clock did not advance");
        ratios[pair] = @as(f64, @floatFromInt(walk_ns[pair])) / @as(f64, @floatFromInt(baseline_ns[pair]));
        listed_total += snapshot.counts.listed;
    }
    const listed = @as(f64, @floatFromInt(listed_total)) / pairs_measured;
    const per_500 = 500.0 / listed / 1e6;
    const ratio = median(f64, &ratios);

    std.debug.print(
        "walk budget: {d:.0} processes, {s}; walk median {d:.2} ms per 500, bare-syscall baseline median {d:.2} ms per 500, ratio {d:.3} (allowed {d:.1})\n",
        .{
            listed,
            if (pin != null) "pinned to one CPU" else "not pinned",
            @as(f64, @floatFromInt(median(u64, &walk_ns))) * per_500,
            @as(f64, @floatFromInt(median(u64, &baseline_ns))) * per_500,
            ratio,
            max_ratio,
        },
    );
    try std.testing.expect(ratio <= max_ratio);
}

fn skip(reason: []const u8) error{SkipZigTest} {
    std.debug.print("walk budget skipped: {s}\n", .{reason});
    return error.SkipZigTest;
}

/// What the C interface derives from a process's cgroup path besides the
/// class, so the measured work is the work layout 3 does.
fn pathFields(path: []const u8) usize {
    var seen: usize = 0;
    if (unit_path.unit(path)) |name| seen += name.len;
    if (unit_path.userUnit(path)) |name| seen += name.len;
    if (unit_path.session(path)) |name| seen += name.len;
    if (unit_path.ownerUid(path)) |uid| seen += uid;
    return seen;
}

test "budget: classifying the walk's processes costs at most 0.1 of the walk (median of 9 interleaved pairs)" {
    const io = std.testing.io;
    const gpa = std.testing.allocator;
    var root = Io.Dir.cwd().openDir(io, "/proc", .{ .iterate = true }) catch return error.SkipZigTest;
    defer root.close(io);

    var snapshot: walk.Snapshot = .init(gpa);
    defer snapshot.deinit();
    var classes: std.ArrayList(classify.Classification) = .empty;
    defer classes.deinit(gpa);
    try snapshot.walk(io, root);
    if (snapshot.counts.listed < processes_needed) return error.SkipZigTest;
    // The first classification grows the buffer and warms the code.
    try classes.resize(gpa, snapshot.processes.items.len);
    try classify.classifyProcesses(gpa, snapshot.processes.items, classes.items);

    var pin = Pin.acquire();
    defer if (pin) |*held| held.release();

    var ratios: [pairs_measured]f64 = undefined;
    var walk_ns: [pairs_measured]u64 = undefined;
    var classify_ns: [pairs_measured]u64 = undefined;
    var listed_total: u64 = 0;
    for (0..pairs_measured) |pair| {
        const walk_started = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");
        try snapshot.walk(io, root);
        const walk_ended = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");

        // The buffer is sized outside the timed part, as the collector keeps it between samples.
        try classes.resize(gpa, snapshot.processes.items.len);
        const classify_started = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");
        try classify.classifyProcesses(gpa, snapshot.processes.items, classes.items);
        var seen: usize = 0;
        for (snapshot.processes.items) |process| {
            if (process.cgroup_path) |path| seen +%= pathFields(path);
        }
        std.mem.doNotOptimizeAway(seen);
        const classify_ended = threadCpuNs() orelse return skip("CLOCK_THREAD_CPUTIME_ID is unavailable");

        walk_ns[pair] = walk_ended - walk_started;
        classify_ns[pair] = classify_ended - classify_started;
        if (walk_ns[pair] == 0) return skip("the thread CPU clock did not advance");
        ratios[pair] = @as(f64, @floatFromInt(classify_ns[pair])) / @as(f64, @floatFromInt(walk_ns[pair]));
        listed_total += snapshot.counts.listed;
    }
    const listed = @as(f64, @floatFromInt(listed_total)) / pairs_measured;
    const per_500 = 500.0 / listed / 1e3;
    const ratio = median(f64, &ratios);

    std.debug.print(
        "classification budget: {d:.0} processes, {s}; classification median {d:.1} us per 500, walk median {d:.0} us per 500, ratio {d:.4} (allowed {d:.1})\n",
        .{
            listed,
            if (pin != null) "pinned to one CPU" else "not pinned",
            @as(f64, @floatFromInt(median(u64, &classify_ns))) * per_500,
            @as(f64, @floatFromInt(median(u64, &walk_ns))) * per_500,
            ratio,
            max_classify_ratio,
        },
    );
    try std.testing.expect(ratio <= max_classify_ratio);
}
