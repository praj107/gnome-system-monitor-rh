// SPDX-License-Identifier: GPL-2.0-or-later
//! The process walk: one record per live process under a procfs root,
//! keyed by (pid, start_ticks), which survives pid reuse. Per process, `stat`,
//! `status`, `schedstat` and `cgroup` are each opened, read once
//! with a single pread and closed: three syscalls a file, no directory open.
//!
//! Kernel input is untrusted and processes come and go mid-read, so:
//!  - a process that vanishes mid-read (ENOENT, or ESRCH, which std reports
//!    as `error.Unexpected`, or an empty read) is counted in `exited` and
//!    gets no record, never a record of zeros;
//!  - a file that cannot be read (EACCES, EISDIR, EIO) leaves that part of
//!    the record null and is counted in `unreadable`;
//!  - a file that is cut short, too long or malformed leaves that part null
//!    and is counted in `malformed`;
//!  - `stat` is the key: when it cannot be read or parsed the process cannot
//!    be keyed and is not listed (its failure is counted).
//! `schedstat` is absent on kernels built without CONFIG_SCHED_INFO, so a
//! missing `schedstat` is "unknown", not "exited"; the read order puts
//! `cgroup`, which always exists, last so a vanish is still caught.
const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const stat_file = @import("stat.zig");
const status_file = @import("status.zig");
const schedstat_file = @import("schedstat.zig");
const cgroup_file = @import("cgroup.zig");
const numbers = @import("numbers.zig");

pub const File = enum { stat, status, schedstat, cgroup };
pub const file_count = @typeInfo(File).@"enum".fields.len;

pub const Process = struct {
    pid: u32,
    /// With `pid`, the process's identity: a reused pid has a new value.
    start_ticks: u64,
    comm_bytes: [stat_file.comm_max]u8,
    comm_len: u8,
    state: u8,
    ppid: u32,
    pgrp: u32,
    session: u32,
    tty_nr: i32,
    flags: u32,
    utime_ticks: u64,
    stime_ticks: u64,
    num_threads: u32,
    /// Null parts below mean the file could not be read or parsed.
    status: ?status_file.Status,
    schedstat: ?schedstat_file.Schedstat,
    /// The cgroup v2 path; null when unreadable or there is no v2 line.
    /// Owned by the snapshot.
    cgroup_path: ?[]const u8,

    pub fn comm(self: *const Process) []const u8 {
        return self.comm_bytes[0..self.comm_len];
    }

    pub fn isKernelThread(self: *const Process) bool {
        return stat_file.isKernelThread(self.flags);
    }
};

pub const Counts = struct {
    /// Processes listed.
    listed: u32 = 0,
    /// Processes that vanished while being read.
    exited: u32 = 0,
    /// Per file, indexed by `File`.
    unreadable: [file_count]u32 = @splat(0),
    malformed: [file_count]u32 = @splat(0),
};

pub const WalkError = Allocator.Error || Io.Dir.Iterator.Error;

/// Holds the result of the latest `walk`. Reusing one snapshot across
/// refreshes keeps its memory: the second walk allocates nothing unless the
/// process count grew.
pub const Snapshot = struct {
    gpa: Allocator,
    strings: std.heap.ArenaAllocator,
    processes: std.ArrayList(Process) = .empty,
    counts: Counts = .{},

    pub fn init(gpa: Allocator) Snapshot {
        return .{ .gpa = gpa, .strings = .init(gpa) };
    }

    pub fn deinit(self: *Snapshot) void {
        self.processes.deinit(self.gpa);
        self.strings.deinit();
    }

    pub fn find(self: *const Snapshot, pid: u32) ?*const Process {
        for (self.processes.items) |*process| if (process.pid == pid) return process;
        return null;
    }

    /// Replaces the snapshot with the processes under `root` (a directory
    /// opened with `.iterate = true`; `/proc` in production, a fixture tree
    /// in tests). Records are sorted by pid.
    pub fn walk(self: *Snapshot, io: Io, root: Io.Dir) WalkError!void {
        self.processes.clearRetainingCapacity();
        _ = self.strings.reset(.retain_capacity);
        self.counts = .{};

        var entries = root.iterate();
        while (try entries.next(io)) |entry| {
            if (entry.kind != .directory and entry.kind != .unknown) continue;
            const pid = numbers.pidFromName(entry.name) orelse continue;
            self.readProcess(io, root, pid) catch |err| switch (err) {
                error.Vanished => self.counts.exited += 1,
                error.OutOfMemory => return error.OutOfMemory,
            };
        }
        std.mem.sort(Process, self.processes.items, {}, byPid);
        self.counts.listed = @intCast(self.processes.items.len);
    }

    fn byPid(_: void, a: Process, b: Process) bool {
        return a.pid < b.pid;
    }

    const ReadError = error{ Vanished, OutOfMemory };

    // 8 KiB holds a status file with room (about 1.5 KiB today); a read that
    // fills its buffer is refused as too long, not parsed as if whole.
    const read_buffer_bytes = 8192;

    fn readProcess(self: *Snapshot, io: Io, root: Io.Dir, pid: u32) ReadError!void {
        var buffer: [read_buffer_bytes]u8 = undefined;

        const stat_bytes = (try self.readFile(io, root, pid, .stat, &buffer)) orelse return;
        const st = self.parsed(.stat, stat_file.parse(stat_bytes)) orelse return;
        if (st.pid != pid) {
            self.counts.malformed[@intFromEnum(File.stat)] += 1;
            return;
        }

        var process: Process = .{
            .pid = pid,
            .start_ticks = st.start_ticks,
            .comm_bytes = @splat(0),
            .comm_len = @intCast(st.comm.len),
            .state = st.state,
            .ppid = st.ppid,
            .pgrp = st.pgrp,
            .session = st.session,
            .tty_nr = st.tty_nr,
            .flags = st.flags,
            .utime_ticks = st.utime_ticks,
            .stime_ticks = st.stime_ticks,
            .num_threads = st.num_threads,
            .status = null,
            .schedstat = null,
            .cgroup_path = null,
        };
        @memcpy(process.comm_bytes[0..st.comm.len], st.comm);

        if (try self.readFile(io, root, pid, .status, &buffer)) |bytes| {
            process.status = self.parsed(.status, status_file.parse(bytes));
        }
        if (try self.readFile(io, root, pid, .schedstat, &buffer)) |bytes| {
            process.schedstat = self.parsed(.schedstat, schedstat_file.parse(bytes));
        }
        if (try self.readFile(io, root, pid, .cgroup, &buffer)) |bytes| {
            if (self.parsed(.cgroup, cgroup_file.parse(bytes))) |path| {
                if (path) |text| process.cgroup_path = try self.strings.allocator().dupe(u8, text);
            }
        }

        try self.processes.append(self.gpa, process);
    }

    /// The bytes of one file, null when it could not be read (counted),
    /// error.Vanished when the process is gone.
    fn readFile(self: *Snapshot, io: Io, root: Io.Dir, pid: u32, file: File, buffer: []u8) error{Vanished}!?[]const u8 {
        // schedstat may be absent on a live process (see the header).
        const absence_means_exit = file != .schedstat;

        var path_buffer: [32]u8 = undefined;
        const path = std.fmt.bufPrint(&path_buffer, "{d}/{s}", .{ pid, @tagName(file) }) catch
            return self.unreadable(file);

        const handle = root.openFile(io, path, .{}) catch |err| switch (err) {
            error.FileNotFound => return if (absence_means_exit) error.Vanished else self.unreadable(file),
            else => return self.unreadable(file),
        };
        defer handle.close(io);

        const length = handle.readPositional(io, &.{buffer}, 0) catch |err| switch (err) {
            // ESRCH on a task that exited after the open is not in std's
            // error set; it arrives as Unexpected.
            error.Unexpected => return if (absence_means_exit) error.Vanished else self.unreadable(file),
            else => return self.unreadable(file),
        };
        if (length == 0) return if (absence_means_exit) error.Vanished else self.unreadable(file);
        if (length == buffer.len) {
            self.counts.malformed[@intFromEnum(file)] += 1;
            return null;
        }
        return buffer[0..length];
    }

    fn unreadable(self: *Snapshot, file: File) ?[]const u8 {
        self.counts.unreadable[@intFromEnum(file)] += 1;
        return null;
    }

    fn parsed(self: *Snapshot, file: File, result: anytype) ?@typeInfo(@TypeOf(result)).error_union.payload {
        return result catch {
            self.counts.malformed[@intFromEnum(file)] += 1;
            return null;
        };
    }
};

// ---- tests ----------------------------------------------------------

const testing = std.testing;

fn openFixtures() !Io.Dir {
    return Io.Dir.cwd().openDir(testing.io, "testdata/proc", .{ .iterate = true });
}

const stat_tail = " S 1 1 1 0 -1 4194560 100 0 2 0 7 3 0 0 20 0 4 0 555 8192 55 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0 0 0 0 0 0 0 0 0\n";

fn put(dir: Io.Dir, sub_path: []const u8, data: []const u8) !void {
    try dir.writeFile(testing.io, .{ .sub_path = sub_path, .data = data });
}

fn putProcess(dir: Io.Dir, pid: []const u8, comm: []const u8) !void {
    var path: [64]u8 = undefined;
    try dir.createDirPath(testing.io, pid);
    var stat_text: [200]u8 = undefined;
    try put(dir, try std.fmt.bufPrint(&path, "{s}/stat", .{pid}), try std.fmt.bufPrint(&stat_text, "{s} ({s}){s}", .{ pid, comm, stat_tail }));
    try put(dir, try std.fmt.bufPrint(&path, "{s}/status", .{pid}), "Uid:\t1000\t1000\t1000\t1000\nVmSize:\t   400 kB\nRssAnon:\t    10 kB\nRssFile:\t    20 kB\nRssShmem:\t    30 kB\nVmSwap:\t     0 kB\n");
    try put(dir, try std.fmt.bufPrint(&path, "{s}/schedstat", .{pid}), "2000 300 4\n");
    try put(dir, try std.fmt.bufPrint(&path, "{s}/cgroup", .{pid}), "0::/app.slice/x.service\n");
}

test "the walk over a captured tree lists kernel threads and user processes keyed by pid and start" {
    var root = try openFixtures();
    defer root.close(testing.io);
    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, root);

    try testing.expectEqual(@as(u32, 5), snapshot.counts.listed);
    try testing.expectEqual(@as(u32, 0), snapshot.counts.exited);
    const init_process = snapshot.find(1) orelse return error.TestUnexpectedResult;
    try testing.expect(!init_process.isKernelThread());
    try testing.expect(init_process.start_ticks > 0);
    try testing.expectEqualStrings("/init.scope", init_process.cgroup_path.?);
    const kthreadd = snapshot.find(2) orelse return error.TestUnexpectedResult;
    try testing.expect(kthreadd.isKernelThread());
    try testing.expectEqualStrings("kthreadd", kthreadd.comm());
    try testing.expectEqual(@as(?u64, null), kthreadd.status.?.rss_anon_bytes);

    const worker = snapshot.find(103116) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("kworker/4:2-events", worker.comm());
    try testing.expect(worker.isKernelThread());

    // Values below are the captured files' own (testdata/proc/*/).
    try testing.expectEqual(@as(u64, 9), init_process.start_ticks);
    try testing.expectEqual(@as(?u64, 5044 * 1024), init_process.status.?.rss_anon_bytes);
    try testing.expectEqual(@as(u64, 2_816_627_283), init_process.schedstat.?.run_ns);
    const sleeper = snapshot.find(344260) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("sleep", sleeper.comm());
    try testing.expectEqual(@as(u64, 484977), sleeper.start_ticks);
    try testing.expectEqual(@as(u32, 344259), sleeper.ppid);
    try testing.expectEqual(@as(?u64, 112 * 1024), sleeper.status.?.rss_anon_bytes);
    try testing.expectEqual(@as(?u64, 2044 * 1024), sleeper.status.?.rss_file_bytes);
    try testing.expectEqual(@as(?u32, 1000), sleeper.status.?.uid_real);
    try testing.expectEqual(@as(u64, 482125), sleeper.schedstat.?.run_ns);
    try testing.expectEqual(@as(u64, 71565), sleeper.schedstat.?.run_delay_ns);
    try testing.expectEqual(@as(?u64, 8344 * 1024), sleeper.status.?.vm_size_bytes);
    try testing.expect(std.mem.endsWith(u8, sleeper.cgroup_path.?, ".scope"));
    const shell = snapshot.find(344261) orelse return error.TestUnexpectedResult;
    try testing.expectEqualStrings("bash", shell.comm());
    try testing.expectEqual(@as(?u64, 288 * 1024), shell.status.?.rss_anon_bytes);

    var previous: u32 = 0;
    for (snapshot.processes.items) |process| {
        try testing.expect(process.pid > previous);
        previous = process.pid;
    }
}

test "a process whose files disappear after stat is exited, not a record of zeros" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "10", "whole");
    try tmp.dir.createDirPath(testing.io, "11");
    try put(tmp.dir, "11/stat", "11 (gone)" ++ stat_tail);

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);

    try testing.expectEqual(@as(u32, 1), snapshot.counts.listed);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.exited);
    try testing.expect(snapshot.find(11) == null);
    const whole = snapshot.find(10).?;
    try testing.expectEqual(@as(?u64, 10 * 1024), whole.status.?.rss_anon_bytes);
    try testing.expectEqual(@as(u64, 2000), whole.schedstat.?.run_ns);
    try testing.expectEqual(@as(?u64, 400 * 1024), whole.status.?.vm_size_bytes);
}

test "a process directory with no stat at all is exited" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try tmp.dir.createDirPath(testing.io, "12");
    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqual(@as(u32, 0), snapshot.counts.listed);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.exited);
}

test "a file that cannot be read leaves that part null and is counted, never zero" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "20", "locked");
    // A directory where status should be: opening works, reading fails (EISDIR).
    try tmp.dir.deleteFile(testing.io, "20/status");
    try tmp.dir.createDirPath(testing.io, "20/status");

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);

    const process = snapshot.find(20).?;
    try testing.expectEqual(@as(?status_file.Status, null), process.status);
    try testing.expectEqual(@as(u64, 2000), process.schedstat.?.run_ns);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.unreadable[@intFromEnum(File.status)]);
    try testing.expectEqual(@as(u32, 0), snapshot.counts.exited);
}

test "a missing schedstat is unknown on a live process, not an exit" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "30", "nosched");
    try tmp.dir.deleteFile(testing.io, "30/schedstat");

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);

    const process = snapshot.find(30).?;
    try testing.expectEqual(@as(?schedstat_file.Schedstat, null), process.schedstat);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.unreadable[@intFromEnum(File.schedstat)]);
    try testing.expectEqual(@as(u32, 0), snapshot.counts.exited);
}

test "a malformed file is counted and leaves that part null; a malformed stat drops the process" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "40", "bad");
    try put(tmp.dir, "40/schedstat", "1 2 three\n");
    try putProcess(tmp.dir, "41", "worse");
    try put(tmp.dir, "41/stat", "41 (worse) S 1\n");
    try putProcess(tmp.dir, "42", "liar");
    try put(tmp.dir, "42/stat", "43 (liar)" ++ stat_tail);

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);

    try testing.expectEqual(@as(u32, 1), snapshot.counts.listed);
    try testing.expectEqual(@as(?schedstat_file.Schedstat, null), snapshot.find(40).?.schedstat);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.malformed[@intFromEnum(File.schedstat)]);
    try testing.expectEqual(@as(u32, 2), snapshot.counts.malformed[@intFromEnum(File.stat)]);
}

test "entries that are not pids are skipped" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "50", "real");
    try tmp.dir.createDirPath(testing.io, "self");
    try tmp.dir.createDirPath(testing.io, "sys");
    try tmp.dir.createDirPath(testing.io, "0123");
    try put(tmp.dir, "stat", "not a process\n");
    try put(tmp.dir, "60", "a file named like a pid");

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.listed);
    try testing.expectEqual(@as(u32, 0), snapshot.counts.exited);
}

test "a comm with spaces and parentheses survives the walk" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "70", "a) (b c");
    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqualStrings("a) (b c", snapshot.find(70).?.comm());
}

test "a second walk replaces the first and a reused snapshot gives the same answer" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "80", "first");
    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);
    try putProcess(tmp.dir, "81", "second");
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqual(@as(u32, 2), snapshot.counts.listed);
    try tmp.dir.deleteTree(testing.io, "80");
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.listed);
    try testing.expectEqualStrings("/app.slice/x.service", snapshot.find(81).?.cgroup_path.?);
}

test "a status file that fills the read buffer is refused as too long" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putProcess(tmp.dir, "90", "big");
    const filler = try testing.allocator.alloc(u8, Snapshot.read_buffer_bytes + 10);
    defer testing.allocator.free(filler);
    @memset(filler, 'x');
    filler[filler.len - 1] = '\n';
    try put(tmp.dir, "90/status", filler);

    var snapshot: Snapshot = .init(testing.allocator);
    defer snapshot.deinit();
    try snapshot.walk(testing.io, tmp.dir);
    try testing.expectEqual(@as(?status_file.Status, null), snapshot.find(90).?.status);
    try testing.expectEqual(@as(u32, 1), snapshot.counts.malformed[@intFromEnum(File.status)]);
}

test "fuzz: a walk over arbitrary walk file contents never panics or leaks" {
    try testing.fuzz({}, struct {
        fn run(_: void, smith: *testing.Smith) anyerror!void {
            var tmp = testing.tmpDir(.{ .iterate = true });
            defer tmp.cleanup();
            try tmp.dir.createDirPath(testing.io, "5");
            var content: [192]u8 = undefined;
            inline for (.{ "stat", "status", "schedstat", "cgroup" }) |name| {
                const length = smith.slice(&content);
                try put(tmp.dir, "5/" ++ name, content[0..length]);
            }
            var snapshot: Snapshot = .init(testing.allocator);
            defer snapshot.deinit();
            try snapshot.walk(testing.io, tmp.dir);
            const stat_index = @intFromEnum(File.stat);
            const outcomes = snapshot.counts.listed + snapshot.counts.exited +
                snapshot.counts.unreadable[stat_index] + snapshot.counts.malformed[stat_index];
            try testing.expectEqual(@as(u32, 1), outcomes);
        }
    }.run, .{});
}
