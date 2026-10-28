// SPDX-License-Identifier: GPL-2.0-or-later
//! gsmcollect-probe: walk a procfs root once and print what it found and
//! what it cost.
//!
//!   gsmcollect-probe --once [--root DIR] [--show PID]
//!
//! Prints the process count, the vanished count, per-file unreadable and
//! malformed counts, and the walk's elapsed nanoseconds. `--show PID` also
//! prints that process's record.
const std = @import("std");
const gsm = @import("gsmcollect");

const usage = "usage: gsmcollect-probe --once [--root DIR] [--show PID]\n";

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buffer: [4096]u8 = undefined;
    var stdout = std.Io.File.stdout().writerStreaming(io, &out_buffer);
    const out = &stdout.interface;
    defer out.flush() catch {};

    const args = try init.minimal.args.toSlice(init.arena.allocator());
    var root_path: []const u8 = "/proc";
    var show_pid: ?u32 = null;
    var once = false;
    var index: usize = 1;
    while (index < args.len) : (index += 1) {
        const arg = args[index];
        if (std.mem.eql(u8, arg, "--once")) {
            once = true;
        } else if (std.mem.eql(u8, arg, "--root") and index + 1 < args.len) {
            index += 1;
            root_path = args[index];
        } else if (std.mem.eql(u8, arg, "--show") and index + 1 < args.len) {
            index += 1;
            show_pid = gsm.numbers.unsigned(u32, args[index]) catch return fail(out, "bad pid\n");
        } else return fail(out, usage);
    }
    if (!once) return fail(out, usage);

    var root = std.Io.Dir.cwd().openDir(io, root_path, .{ .iterate = true }) catch |err|
        return failFmt(out, "cannot open {s}: {s}\n", .{ root_path, @errorName(err) });
    defer root.close(io);

    var snapshot: gsm.Snapshot = .init(init.gpa);
    defer snapshot.deinit();

    const started = std.Io.Clock.awake.now(io);
    try snapshot.walk(io, root);
    const elapsed_ns = started.durationTo(std.Io.Clock.awake.now(io)).nanoseconds;

    const counts = snapshot.counts;
    try out.print("root: {s}\nprocesses: {d}\nexited: {d}\nelapsed_ns: {d}\n", .{ root_path, counts.listed, counts.exited, elapsed_ns });
    inline for (@typeInfo(gsm.File).@"enum".fields) |field| {
        try out.print("{s}: unreadable {d} malformed {d}\n", .{ field.name, counts.unreadable[field.value], counts.malformed[field.value] });
    }
    if (show_pid) |pid| try show(out, &snapshot, pid);
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
