// SPDX-License-Identifier: GPL-2.0-or-later
//! gsmcollect-probe: run one reader of the collector once and print what it
//! found and what it cost (elapsed nanoseconds around the read and parse).
//!
//!   gsmcollect-probe --once [--root DIR] [--show PID]
//!       the process walk over a procfs root (default /proc); --show PID also
//!       prints that process's record
//!   gsmcollect-probe --memory [--root DIR]
//!       <root>/meminfo: the raw fields and the partition of MemTotal
//!   gsmcollect-probe --pressure [--root DIR] [--window-ms N]
//!       <root>/pressure/{cpu,memory,io}; with a window, reads twice N ms
//!       apart and prints the stall share from the totals
//!   gsmcollect-probe --cgroups [--cgroup-root DIR] [--under PATH] [--list]
//!       the cgroup tree (default root /sys/fs/cgroup, default PATH "/");
//!       --under implies --list, which prints one line per group
const std = @import("std");
const gsm = @import("gsmcollect");

const usage =
    \\usage: gsmcollect-probe --once [--root DIR] [--show PID]
    \\       gsmcollect-probe --memory [--root DIR]
    \\       gsmcollect-probe --pressure [--root DIR] [--window-ms N]
    \\       gsmcollect-probe --cgroups [--cgroup-root DIR] [--under PATH] [--list]
    \\
;

const Mode = enum { none, once, memory, pressure, cgroups };

const Options = struct {
    mode: Mode = .none,
    root_path: []const u8 = "/proc",
    cgroup_root_path: []const u8 = "/sys/fs/cgroup",
    under: []const u8 = "/",
    show_pid: ?u32 = null,
    window_ms: u32 = 0,
    list: bool = false,
};

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writerStreaming(io, &out_buffer);
    const out = &stdout.interface;
    defer out.flush() catch {};

    const args = try init.minimal.args.toSlice(init.arena.allocator());
    var options: Options = .{};
    var index: usize = 1;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        const value: ?[]const u8 = if (index + 1 < args.len) args[index + 1] else null;
        if (std.mem.eql(u8, arg, "--once")) {
            options.mode = .once;
        } else if (std.mem.eql(u8, arg, "--memory")) {
            options.mode = .memory;
        } else if (std.mem.eql(u8, arg, "--pressure")) {
            options.mode = .pressure;
        } else if (std.mem.eql(u8, arg, "--cgroups")) {
            options.mode = .cgroups;
        } else if (std.mem.eql(u8, arg, "--list")) {
            options.list = true;
        } else if (std.mem.eql(u8, arg, "--root") and value != null) {
            index += 1;
            options.root_path = value.?;
        } else if (std.mem.eql(u8, arg, "--cgroup-root") and value != null) {
            index += 1;
            options.cgroup_root_path = value.?;
        } else if (std.mem.eql(u8, arg, "--under") and value != null) {
            index += 1;
            options.under = value.?;
            options.list = true;
        } else if (std.mem.eql(u8, arg, "--show") and value != null) {
            index += 1;
            options.show_pid = gsm.numbers.unsigned(u32, value.?) catch return fail(out, "bad pid\n");
        } else if (std.mem.eql(u8, arg, "--window-ms") and value != null) {
            index += 1;
            options.window_ms = gsm.numbers.unsigned(u32, value.?) catch return fail(out, "bad window\n");
        } else return fail(out, usage);
    }

    switch (options.mode) {
        .none => return fail(out, usage),
        .once => try probeProcesses(init, out, options),
        .memory => try probeMemory(io, out, options),
        .pressure => try probePressure(io, out, options),
        .cgroups => try probeCgroups(init, out, options),
    }
}

fn nowNs(io: std.Io) std.Io.Timestamp {
    return std.Io.Clock.awake.now(io);
}

fn probeProcesses(init: std.process.Init, out: *std.Io.Writer, options: Options) !void {
    const io = init.io;
    var root = std.Io.Dir.cwd().openDir(io, options.root_path, .{ .iterate = true }) catch |err|
        return failFmt(out, "cannot open {s}: {s}\n", .{ options.root_path, @errorName(err) });
    defer root.close(io);

    var snapshot: gsm.Snapshot = .init(init.gpa);
    defer snapshot.deinit();

    const started = nowNs(io);
    try snapshot.walk(io, root);
    const elapsed_ns = started.durationTo(nowNs(io)).nanoseconds;

    const counts = snapshot.counts;
    try out.print("root: {s}\nprocesses: {d}\nexited: {d}\nelapsed_ns: {d}\n", .{ options.root_path, counts.listed, counts.exited, elapsed_ns });
    inline for (@typeInfo(gsm.File).@"enum".fields) |field| {
        try out.print("{s}: unreadable {d} malformed {d}\n", .{ field.name, counts.unreadable[field.value], counts.malformed[field.value] });
    }
    if (options.show_pid) |pid| try show(out, &snapshot, pid);
}

fn show(out: *std.Io.Writer, snapshot: *const gsm.Snapshot, pid: u32) !void {
    const process = snapshot.find(pid) orelse return out.print("pid {d}: not listed\n", .{pid});
    try out.print("pid {d} comm {s} start_ticks {d} state {c} kthread {}\n", .{ pid, process.comm(), process.start_ticks, process.state, process.isKernelThread() });
    try out.print("utime_ticks {d} stime_ticks {d} threads {d}\n", .{ process.utime_ticks, process.stime_ticks, process.num_threads });
    if (process.status) |s| {
        try out.print("RssAnon_bytes {?d} RssFile_bytes {?d} RssShmem_bytes {?d} VmSwap_bytes {?d} VmSize_bytes {?d} uid {?d}\n", .{ s.rss_anon_bytes, s.rss_file_bytes, s.rss_shmem_bytes, s.vm_swap_bytes, s.vm_size_bytes, s.uid_real });
    } else try out.writeAll("status: unknown\n");
    if (process.schedstat) |s| {
        try out.print("run_ns {d} run_delay_ns {d} timeslices {d}\n", .{ s.run_ns, s.run_delay_ns, s.timeslices });
    } else try out.writeAll("schedstat: unknown\n");
    try out.print("cgroup {?s}\n", .{process.cgroup_path});
}

/// Reads <dir>/<sub_path> into `buffer`; exits with a message when the file
/// cannot be used, so the probe never prints a number it does not have.
fn readOrFail(io: std.Io, out: *std.Io.Writer, dir: std.Io.Dir, sub_path: []const u8, buffer: []u8) ![]const u8 {
    switch (gsm.readfile.read(io, dir, sub_path, buffer)) {
        .bytes => |bytes| return bytes,
        .absent => try failFmt(out, "{s}: absent\n", .{sub_path}),
        .unreadable => try failFmt(out, "{s}: unreadable\n", .{sub_path}),
        .too_long => try failFmt(out, "{s}: too long for the buffer\n", .{sub_path}),
    }
    unreachable;
}

fn probeMemory(io: std.Io, out: *std.Io.Writer, options: Options) !void {
    var root = std.Io.Dir.cwd().openDir(io, options.root_path, .{}) catch |err|
        return failFmt(out, "cannot open {s}: {s}\n", .{ options.root_path, @errorName(err) });
    defer root.close(io);
    var buffer: [16384]u8 = undefined;

    const started = nowNs(io);
    const bytes = try readOrFail(io, out, root, "meminfo", &buffer);
    const info = gsm.meminfo.parse(bytes) catch |err|
        return failFmt(out, "meminfo: {s}\n", .{@errorName(err)});
    const partition = gsm.meminfo.partition(info);
    const elapsed_ns = started.durationTo(nowNs(io)).nanoseconds;

    try out.print("root: {s}\nelapsed_ns: {d}\n", .{ options.root_path, elapsed_ns });
    inline for (@typeInfo(gsm.meminfo.Meminfo).@"struct".fields) |field| {
        try out.print("{s}: ", .{field.name});
        try printOptional(out, @field(info, field.name));
        try out.writeByte('\n');
    }
    try out.writeAll("used_bytes (MemTotal - MemAvailable): ");
    try printOptional(out, gsm.meminfo.usedBytes(info));
    try out.writeByte('\n');
    const p = partition orelse return out.writeAll("partition: unknown (a field the rows need is missing)\n");
    inline for (@typeInfo(gsm.meminfo.Partition).@"struct".fields) |field| {
        try out.print("partition.{s}: {d}\n", .{ field.name, @field(p, field.name) });
    }
}

fn probePressure(io: std.Io, out: *std.Io.Writer, options: Options) !void {
    var root = std.Io.Dir.cwd().openDir(io, options.root_path, .{}) catch |err|
        return failFmt(out, "cannot open {s}: {s}\n", .{ options.root_path, @errorName(err) });
    defer root.close(io);

    const resources = [_][]const u8{ "cpu", "memory", "io" };
    var first: [resources.len]?gsm.pressure.Pressure = @splat(null);
    var second: [resources.len]?gsm.pressure.Pressure = @splat(null);

    const started = nowNs(io);
    for (resources, 0..) |name, at| first[at] = try readPressure(io, out, root, name);
    const elapsed_ns = started.durationTo(nowNs(io)).nanoseconds;
    const first_at = nowNs(io);
    var wall_us: u64 = 0;
    if (options.window_ms > 0) {
        try io.sleep(std.Io.Duration.fromMilliseconds(options.window_ms), .awake);
        for (resources, 0..) |name, at| second[at] = try readPressure(io, out, root, name);
        wall_us = @intCast(@divTrunc(first_at.durationTo(nowNs(io)).nanoseconds, 1000));
    }

    try out.print("root: {s}\nelapsed_ns (three reads and parses): {d}\n", .{ options.root_path, elapsed_ns });
    if (options.window_ms > 0) try out.print("window_us: {d}\n", .{wall_us});
    for (resources, 0..) |name, at| {
        const now = first[at] orelse continue;
        try printLine(out, name, "some", now.some);
        if (now.full) |full| try printLine(out, name, "full", full) else try out.print("{s} full: absent\n", .{name});
        const later = second[at] orelse continue;
        try out.print("{s} some stall_percent over window: ", .{name});
        try printPercent(out, gsm.pressure.stallPercent(now.some, later.some, wall_us));
        if (now.full != null and later.full != null) {
            try out.print("{s} full stall_percent over window: ", .{name});
            try printPercent(out, gsm.pressure.stallPercent(now.full.?, later.full.?, wall_us));
        }
    }
}

fn readPressure(io: std.Io, out: *std.Io.Writer, root: std.Io.Dir, name: []const u8) !gsm.pressure.Pressure {
    var path_buffer: [32]u8 = undefined;
    var buffer: [1024]u8 = undefined;
    const path = try std.fmt.bufPrint(&path_buffer, "pressure/{s}", .{name});
    const bytes = try readOrFail(io, out, root, path, &buffer);
    return gsm.pressure.parse(bytes) catch |err| {
        try failFmt(out, "{s}: {s}\n", .{ path, @errorName(err) });
        unreachable;
    };
}

fn printLine(out: *std.Io.Writer, name: []const u8, kind: []const u8, line: gsm.pressure.Line) !void {
    try out.print("{s} {s}: avg10 {d:.2} avg60 {d:.2} avg300 {d:.2} total_us {d}\n", .{ name, kind, line.avg10_percent, line.avg60_percent, line.avg300_percent, line.total_us });
}

fn printPercent(out: *std.Io.Writer, value: ?f64) !void {
    if (value) |percent| try out.print("{d:.4}\n", .{percent}) else try out.writeAll("unknown\n");
}

fn printOptional(out: *std.Io.Writer, value: ?u64) !void {
    if (value) |number| try out.print("{d}", .{number}) else try out.writeAll("unknown");
}

fn probeCgroups(init: std.process.Init, out: *std.Io.Writer, options: Options) !void {
    const io = init.io;
    var base = std.Io.Dir.cwd().openDir(io, options.cgroup_root_path, .{}) catch |err|
        return failFmt(out, "cannot open {s}: {s}\n", .{ options.cgroup_root_path, @errorName(err) });
    defer base.close(io);

    var tree: gsm.cgroup_tree.Tree = .init(init.gpa);
    defer tree.deinit();

    const started = nowNs(io);
    tree.walk(io, base, options.under) catch |err|
        return failFmt(out, "cannot walk {s}: {s}\n", .{ options.under, @errorName(err) });
    const elapsed_ns = started.durationTo(nowNs(io)).nanoseconds;

    const counts = tree.counts;
    try out.print("cgroup_root: {s}\nunder: {s}\ngroups: {d}\nvanished: {d}\ntoo_deep: {d}\ndirectory_errors: {d}\nelapsed_ns: {d}\n", .{
        options.cgroup_root_path, options.under, counts.groups, counts.vanished, counts.too_deep, counts.directory_errors, elapsed_ns,
    });
    inline for (@typeInfo(gsm.cgroup_tree.File).@"enum".fields) |field| {
        try out.print("{s}: unreadable {d} malformed {d}\n", .{ field.name, counts.unreadable[field.value], counts.malformed[field.value] });
    }

    // The first group is the one walked from; its children are the top level.
    var top_level: u32 = 0;
    var top_level_measured: u32 = 0;
    var top_level_memory_current_bytes: u64 = 0;
    for (tree.groups.items) |group| {
        if (group.parent != 0) continue;
        top_level += 1;
        const current = group.memory_current_bytes orelse continue;
        top_level_measured += 1;
        top_level_memory_current_bytes += current;
    }
    try out.print("top_level_groups: {d} (memory.current measured on {d})\ntop_level_memory_current_bytes: {d}\n", .{ top_level, top_level_measured, top_level_memory_current_bytes });
    if (std.mem.eql(u8, options.under, "/")) try compareWithMeminfo(io, out, options, top_level_memory_current_bytes);

    if (!options.list) return;
    for (tree.groups.items) |group| {
        try out.print("group {s} memory_current_bytes ", .{group.path});
        try printOptional(out, group.memory_current_bytes);
        try out.writeAll(" pids ");
        try printOptional(out, group.pids_current);
        try out.writeAll(" cpu_usage_usec ");
        try printOptional(out, if (group.cpu_stat) |stat| stat.usage_usec else null);
        try out.writeAll(" memory_some_avg10 ");
        if (group.memory_pressure) |p| try out.print("{d:.2}\n", .{p.some.avg10_percent}) else try out.writeAll("unknown\n");
    }
}

/// The sum of the top-level groups' memory.current against what the kernel
/// says is in use. Reported as figures; the gap is the root group's own
/// charges and memory charged to no group.
fn compareWithMeminfo(io: std.Io, out: *std.Io.Writer, options: Options, top_level_bytes: u64) !void {
    var root = std.Io.Dir.cwd().openDir(io, options.root_path, .{}) catch return;
    defer root.close(io);
    var buffer: [16384]u8 = undefined;
    const bytes = switch (gsm.readfile.read(io, root, "meminfo", &buffer)) {
        .bytes => |text| text,
        else => return,
    };
    const info = gsm.meminfo.parse(bytes) catch return;
    const total = info.mem_total_bytes orelse return;
    const free = info.mem_free_bytes orelse return;
    const cached = info.cached_bytes orelse return;
    const buffers = info.buffers_bytes orelse return;
    const shmem = info.shmem_bytes orelse return;
    if (free > total or cached < shmem) return;

    const used_bytes = total - free;
    // Page cache that is not shmem (shmem is charged to groups like anon).
    const page_cache_bytes = cached - shmem +| buffers;
    try out.print("meminfo_total_minus_free_bytes: {d}\n", .{used_bytes});
    try out.print("top_level_sum_over_total_minus_free: {d:.4}\n", .{ratio(top_level_bytes, used_bytes)});
    if (page_cache_bytes <= used_bytes) {
        const without_cache = used_bytes - page_cache_bytes;
        try out.print("meminfo_total_minus_free_minus_page_cache_bytes: {d}\n", .{without_cache});
        try out.print("top_level_sum_over_total_minus_free_minus_page_cache: {d:.4}\n", .{ratio(top_level_bytes, without_cache)});
    }
}

fn ratio(numerator: u64, denominator: u64) f64 {
    if (denominator == 0) return 0;
    return @as(f64, @floatFromInt(numerator)) / @as(f64, @floatFromInt(denominator));
}

fn fail(out: *std.Io.Writer, message: []const u8) !void {
    try out.writeAll(message);
    try out.flush();
    std.process.exit(2);
}

fn failFmt(out: *std.Io.Writer, comptime format: []const u8, args: anytype) !void {
    try out.print(format, args);
    try out.flush();
    std.process.exit(2);
}
