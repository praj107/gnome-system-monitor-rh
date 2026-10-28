// SPDX-License-Identifier: GPL-2.0-or-later
//! The cgroup v2 tree: one record per group under a cgroupfs root, with the
//! group's memory, CPU, task count, event counters and pressure, read from
//! the interface files in Documentation/admin-guide/cgroup-v2.rst. Group
//! figures come from the group's own counters (they sum to the root's), never
//! from adding up processes.
//!
//! Kernel input is untrusted and groups come and go mid-walk, so:
//!  - a file that does not exist leaves its field null: a controller that is
//!    not enabled for the group, or a root group (which has no
//!    `memory.current`), is "not measured", never zero;
//!  - a file that cannot be read, or is cut short, too long or malformed,
//!    leaves its field null and is counted per file;
//!  - a group that was removed during the walk (its directory is gone, or it
//!    answers ENOENT to everything) is counted in `vanished` and gets no
//!    record. The tell is `cgroup.controllers`, which every live group has,
//!    the root included; it is consulted only when some other file was
//!    absent or unreadable, so a healthy group costs no extra syscalls;
//!  - nesting deeper than `max_depth` is cut off and counted.
//!
//! Each group costs one directory open, two `getdents64`, a close, and for
//! each of the 11 files an open, a pread and a close.
const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;
const readfile = @import("readfile.zig");
const files = @import("cgroup_files.zig");
const pressure_file = @import("pressure.zig");

/// The files read per group; also the index for `Counts`.
pub const File = enum {
    memory_current,
    memory_peak,
    memory_max,
    memory_high,
    memory_stat,
    memory_events,
    cpu_stat,
    pids_current,
    memory_pressure,
    cpu_pressure,
    io_pressure,
};
pub const file_count = @typeInfo(File).@"enum".fields.len;

const file_names = [file_count][]const u8{
    "memory.current",
    "memory.peak",
    "memory.max",
    "memory.high",
    "memory.stat",
    "memory.events",
    "cpu.stat",
    "pids.current",
    "memory.pressure",
    "cpu.pressure",
    "io.pressure",
};

/// Every live group has this file, the root included.
const liveness_file = "cgroup.controllers";

/// Deeper than any real hierarchy; stops a runaway or hostile tree.
pub const max_depth = 64;

pub const Group = struct {
    /// The cgroup path: "/" for the root, "/user.slice/user-1000.slice" below.
    /// Owned by the tree.
    path: []const u8,
    /// Index of the parent in `Tree.groups`; null for the first group of the
    /// walk. Parents come before their children.
    parent: ?u32,
    memory_current_bytes: ?u64 = null,
    memory_peak_bytes: ?u64 = null,
    memory_max: ?files.Limit = null,
    memory_high: ?files.Limit = null,
    memory_stat: ?files.MemoryStat = null,
    memory_events: ?files.MemoryEvents = null,
    cpu_stat: ?files.CpuStat = null,
    /// `pids.current`: processes and threads in the group and below.
    pids_current: ?u64 = null,
    memory_pressure: ?pressure_file.Pressure = null,
    cpu_pressure: ?pressure_file.Pressure = null,
    io_pressure: ?pressure_file.Pressure = null,
};

pub const Counts = struct {
    /// Groups listed.
    groups: u32 = 0,
    /// Groups removed while being read.
    vanished: u32 = 0,
    /// Groups whose children were not walked because of `max_depth`.
    too_deep: u32 = 0,
    /// Directory listings that failed on a live group.
    directory_errors: u32 = 0,
    /// Per file, indexed by `File`. Absent files are not counted: that is
    /// normal. These are files that exist but could not be used.
    unreadable: [file_count]u32 = @splat(0),
    malformed: [file_count]u32 = @splat(0),
};

pub const WalkError = Allocator.Error || error{
    /// `under` is not an absolute cgroup path without "." or ".." parts.
    InvalidPath,
    /// `under` does not exist (it may have just been removed).
    GroupNotFound,
    /// `under` exists but cannot be opened.
    CannotOpen,
};

/// Holds the result of the latest `walk`. Reusing a tree keeps its memory.
pub const Tree = struct {
    gpa: Allocator,
    strings: std.heap.ArenaAllocator,
    groups: std.ArrayList(Group) = .empty,
    counts: Counts = .{},

    pub fn init(gpa: Allocator) Tree {
        return .{ .gpa = gpa, .strings = .init(gpa) };
    }

    pub fn deinit(self: *Tree) void {
        self.groups.deinit(self.gpa);
        self.strings.deinit();
    }

    pub fn find(self: *const Tree, path: []const u8) ?*const Group {
        for (self.groups.items) |*group| if (std.mem.eql(u8, group.path, path)) return group;
        return null;
    }

    /// Replaces the tree with the groups at and below `under`, a cgroup path
    /// ("/" for everything) relative to `base`, the cgroupfs mount
    /// (`/sys/fs/cgroup` in production, a fixture tree in tests). Groups are
    /// listed parents first, siblings in name order.
    pub fn walk(self: *Tree, io: Io, base: Io.Dir, under: []const u8) WalkError!void {
        self.groups.clearRetainingCapacity();
        _ = self.strings.reset(.retain_capacity);
        self.counts = .{};

        const root_path = try normalized(under);
        var start = base.openDir(io, if (root_path.len == 1) "." else root_path[1..], .{ .iterate = true }) catch |err| switch (err) {
            error.FileNotFound => return error.GroupNotFound,
            else => return error.CannotOpen,
        };
        defer start.close(io);

        var walker: Walker = .{ .tree = self, .io = io };
        try walker.visit(start, try self.strings.allocator().dupe(u8, root_path), null, 0);
        self.counts.groups = @intCast(self.groups.items.len);
    }
};

/// "/", or `under` without its trailing slashes; "." and ".." parts and
/// empty parts are refused.
fn normalized(under: []const u8) error{InvalidPath}![]const u8 {
    if (under.len == 0 or under[0] != '/') return error.InvalidPath;
    const trimmed = std.mem.trimEnd(u8, under, "/");
    if (trimmed.len == 0) return "/";
    var parts = std.mem.splitScalar(u8, trimmed[1..], '/');
    while (parts.next()) |part| {
        if (part.len == 0 or std.mem.eql(u8, part, ".") or std.mem.eql(u8, part, "..")) return error.InvalidPath;
    }
    return trimmed;
}

// A memory.stat is about 2 KiB today; a read that fills the buffer is
// refused as too long, not parsed as if whole.
const read_buffer_bytes = 16384;

const Walker = struct {
    tree: *Tree,
    io: Io,
    buffer: [read_buffer_bytes]u8 = undefined,

    fn visit(self: *Walker, dir: Io.Dir, path: []const u8, parent: ?u32, depth: u32) Allocator.Error!void {
        const tree = self.tree;
        const group = (try self.readGroup(dir, path, parent)) orelse {
            tree.counts.vanished += 1;
            return;
        };
        const index: u32 = @intCast(tree.groups.items.len);
        try tree.groups.append(tree.gpa, group);

        var children: std.ArrayList([]const u8) = .empty;
        defer children.deinit(tree.gpa);
        var entries = dir.iterate();
        while (true) {
            const entry = entries.next(self.io) catch {
                tree.counts.directory_errors += 1;
                break;
            } orelse break;
            if (entry.kind != .directory and entry.kind != .unknown) continue;
            const prefix = if (path.len == 1) "" else path;
            try children.append(tree.gpa, try std.fmt.allocPrint(tree.strings.allocator(), "{s}/{s}", .{ prefix, entry.name }));
        }
        if (children.items.len == 0) return;
        if (depth >= max_depth) {
            tree.counts.too_deep += 1;
            return;
        }
        std.mem.sort([]const u8, children.items, {}, lessThan);

        const name_start = (if (path.len == 1) 0 else path.len) + 1;
        for (children.items) |child_path| {
            var child = dir.openDir(self.io, child_path[name_start..], .{ .iterate = true }) catch |err| switch (err) {
                error.FileNotFound => {
                    tree.counts.vanished += 1;
                    continue;
                },
                // A plain file reported with an unknown type.
                error.NotDir => continue,
                else => {
                    tree.counts.directory_errors += 1;
                    continue;
                },
            };
            defer child.close(self.io);
            try self.visit(child, child_path, index, depth + 1);
        }
    }

    fn lessThan(_: void, a: []const u8, b: []const u8) bool {
        return std.mem.lessThan(u8, a, b);
    }

    /// The group's record, or null when it was removed during the read.
    fn readGroup(self: *Walker, dir: Io.Dir, path: []const u8, parent: ?u32) Allocator.Error!?Group {
        var group: Group = .{ .path = path, .parent = parent };
        var local: Counts = .{};
        var trouble = false;

        group.memory_current_bytes = self.load(dir, .memory_current, files.parseCount, &local, &trouble);
        group.memory_peak_bytes = self.load(dir, .memory_peak, files.parseCount, &local, &trouble);
        group.memory_max = self.load(dir, .memory_max, files.parseLimit, &local, &trouble);
        group.memory_high = self.load(dir, .memory_high, files.parseLimit, &local, &trouble);
        group.memory_stat = self.load(dir, .memory_stat, files.parseMemoryStat, &local, &trouble);
        group.memory_events = self.load(dir, .memory_events, files.parseMemoryEvents, &local, &trouble);
        group.cpu_stat = self.load(dir, .cpu_stat, files.parseCpuStat, &local, &trouble);
        group.pids_current = self.load(dir, .pids_current, files.parseCount, &local, &trouble);
        group.memory_pressure = self.load(dir, .memory_pressure, pressure_file.parse, &local, &trouble);
        group.cpu_pressure = self.load(dir, .cpu_pressure, pressure_file.parse, &local, &trouble);
        group.io_pressure = self.load(dir, .io_pressure, pressure_file.parse, &local, &trouble);

        // Only a group that really is gone may be called vanished; failures
        // seen while it was dying are not counted against the files.
        if (trouble and !self.alive(dir)) return null;
        for (0..file_count) |index| {
            self.tree.counts.unreadable[index] += local.unreadable[index];
            self.tree.counts.malformed[index] += local.malformed[index];
        }
        return group;
    }

    /// False only when the group's directory answers ENOENT; any other
    /// failure to ask is not evidence of removal.
    fn alive(self: *Walker, dir: Io.Dir) bool {
        const handle = dir.openFile(self.io, liveness_file, .{}) catch |err| return err != error.FileNotFound;
        handle.close(self.io);
        return true;
    }

    fn Parsed(comptime parseFn: anytype) type {
        return @typeInfo(@typeInfo(@TypeOf(parseFn)).@"fn".return_type.?).error_union.payload;
    }

    /// Reads and parses one file; null when absent, unreadable or malformed.
    /// `trouble` is set when absence or a read failure might mean removal.
    fn load(self: *Walker, dir: Io.Dir, comptime file: File, comptime parseFn: anytype, local: *Counts, trouble: *bool) ?Parsed(parseFn) {
        const index = @intFromEnum(file);
        switch (readfile.read(self.io, dir, file_names[index], &self.buffer)) {
            .bytes => |bytes| return parseFn(bytes) catch {
                local.malformed[index] += 1;
                return null;
            },
            .absent => {
                trouble.* = true;
                return null;
            },
            .unreadable => {
                trouble.* = true;
                local.unreadable[index] += 1;
                return null;
            },
            .too_long => {
                local.malformed[index] += 1;
                return null;
            },
        }
    }
};

// ---- tests ----------------------------------------------------------

const testing = std.testing;

const agents = "/user.slice/user-1000.slice/user@1000.service/agents.slice";

fn put(dir: Io.Dir, sub_path: []const u8, data: []const u8) !void {
    try dir.writeFile(testing.io, .{ .sub_path = sub_path, .data = data });
}

/// A live group: the liveness file plus a few real-looking counters.
fn putGroup(dir: Io.Dir, sub_path: []const u8, memory_current: []const u8) !void {
    var path: [256]u8 = undefined;
    const prefix = if (sub_path.len == 0) "" else "/";
    if (sub_path.len > 0) try dir.createDirPath(testing.io, sub_path);
    try put(dir, try std.fmt.bufPrint(&path, "{s}{s}cgroup.controllers", .{ sub_path, prefix }), "cpu memory pids\n");
    try put(dir, try std.fmt.bufPrint(&path, "{s}{s}memory.current", .{ sub_path, prefix }), memory_current);
}

fn indexOf(tree: *const Tree, path: []const u8) !u32 {
    for (tree.groups.items, 0..) |group, index| if (std.mem.eql(u8, group.path, path)) return @intCast(index);
    return error.TestUnexpectedResult;
}

test "the file table names the kernel files in the order of the File enum" {
    inline for (@typeInfo(File).@"enum".fields) |field| {
        var expected: [field.name.len]u8 = undefined;
        for (field.name, 0..) |byte, at| expected[at] = if (byte == '_') '.' else byte;
        try testing.expectEqualStrings(&expected, file_names[field.value]);
    }
}

test "a walk over a captured tree reads each group's own counters" {
    var base = try Io.Dir.cwd().openDir(testing.io, "testdata/cgroup", .{});
    defer base.close(testing.io);
    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, base, "/");

    try testing.expectEqual(@as(u32, 11), tree.counts.groups);
    try testing.expectEqual(@as(u32, 0), tree.counts.vanished);
    for (0..file_count) |index| {
        try testing.expectEqual(@as(u32, 0), tree.counts.unreadable[index]);
        try testing.expectEqual(@as(u32, 0), tree.counts.malformed[index]);
    }

    // Parents first, siblings in name order.
    try testing.expectEqualStrings("/", tree.groups.items[0].path);
    try testing.expectEqual(@as(?u32, null), tree.groups.items[0].parent);
    for (tree.groups.items[1..]) |group| try testing.expect(group.parent.? < try indexOf(&tree, group.path));
    try testing.expect(try indexOf(&tree, "/system.slice") < try indexOf(&tree, "/user.slice"));

    // Values below are the captured files' own (testdata/cgroup/...).
    const scope = tree.find(agents ++ "/agents-alpha.slice/job-alpha-386902.scope") orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(?u64, 4385796096), scope.memory_current_bytes);
    try testing.expectEqual(@as(?u64, 4386430976), scope.memory_peak_bytes);
    try testing.expectEqual(files.Limit{ .bytes = 8589934592 }, scope.memory_max.?);
    try testing.expectEqual(files.Limit.unlimited, scope.memory_high.?);
    try testing.expectEqual(@as(?u64, 3), scope.pids_current);
    try testing.expectEqual(@as(?u64, 1323053056), scope.memory_stat.?.anon_bytes);
    try testing.expectEqual(@as(?u64, 2838020096), scope.memory_stat.?.shmem_bytes);
    try testing.expectEqual(@as(?u64, 0), scope.memory_events.?.oom_kill);
    try testing.expectEqual(@as(?u64, 362176024), scope.cpu_stat.?.usage_usec);
    try testing.expectEqual(@as(?u64, 206656), scope.cpu_pressure.?.some.total_us);
    try testing.expectEqual(@as(?u64, 72426), scope.cpu_pressure.?.full.?.total_us);
    try testing.expectEqual(@as(u64, 0), scope.memory_pressure.?.some.total_us);
    // This scope's cpu.stat has no throttle counters: unknown, not zero.
    try testing.expectEqual(@as(?u64, null), scope.cpu_stat.?.nr_throttled);
}

test "the root has no memory.current and a group without the memory controller has no memory files" {
    var base = try Io.Dir.cwd().openDir(testing.io, "testdata/cgroup", .{});
    defer base.close(testing.io);
    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, base, "/");

    const root = tree.find("/").?;
    try testing.expectEqual(@as(?u64, null), root.memory_current_bytes);
    try testing.expectEqual(@as(?files.Limit, null), root.memory_max);
    try testing.expectEqual(@as(?u64, null), root.pids_current);
    try testing.expect(root.memory_stat != null);
    try testing.expect(root.cpu_stat != null);
    try testing.expect(root.memory_pressure.?.full != null);

    const udev = tree.find("/system.slice/systemd-udevd.service/udev") orelse return error.TestUnexpectedResult;
    try testing.expectEqual(@as(?u64, null), udev.memory_current_bytes);
    try testing.expectEqual(@as(?files.MemoryStat, null), udev.memory_stat);
    try testing.expectEqual(@as(?files.MemoryEvents, null), udev.memory_events);
    try testing.expectEqual(@as(?u64, null), udev.pids_current);
    try testing.expect(udev.cpu_stat != null);
    try testing.expect(udev.cpu_pressure != null);
    // Not measured is not vanished.
    try testing.expectEqual(@as(u32, 0), tree.counts.vanished);
}

test "walking under a path lists that group and its descendants with absolute paths" {
    var base = try Io.Dir.cwd().openDir(testing.io, "testdata/cgroup", .{});
    defer base.close(testing.io);
    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, base, agents);

    try testing.expectEqual(@as(u32, 4), tree.counts.groups);
    try testing.expectEqualStrings(agents, tree.groups.items[0].path);
    try testing.expectEqualStrings(agents ++ "/agents-alpha.slice", tree.groups.items[1].path);
    try testing.expectEqualStrings(agents ++ "/agents-alpha.slice/job-alpha-386902.scope", tree.groups.items[2].path);
    try testing.expectEqualStrings(agents ++ "/agents-beta.slice", tree.groups.items[3].path);
    try testing.expectEqual(@as(?u64, 7049871360), tree.groups.items[0].memory_current_bytes);

    // A trailing slash names the same group.
    try tree.walk(testing.io, base, agents ++ "/");
    try testing.expectEqual(@as(u32, 4), tree.counts.groups);
}

test "an under path that is relative, climbs, or does not exist is refused" {
    var base = try Io.Dir.cwd().openDir(testing.io, "testdata/cgroup", .{});
    defer base.close(testing.io);
    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try testing.expectError(error.InvalidPath, tree.walk(testing.io, base, "user.slice"));
    try testing.expectError(error.InvalidPath, tree.walk(testing.io, base, ""));
    try testing.expectError(error.InvalidPath, tree.walk(testing.io, base, "/user.slice/../system.slice"));
    try testing.expectError(error.InvalidPath, tree.walk(testing.io, base, "/user.slice//x"));
    try testing.expectError(error.InvalidPath, tree.walk(testing.io, base, "/./x"));
    try testing.expectError(error.GroupNotFound, tree.walk(testing.io, base, "/no.slice"));
    try testing.expectEqual(@as(usize, 0), tree.groups.items.len);
}

test "a group removed mid-walk is counted as vanished, not listed and not zeroed" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putGroup(tmp.dir, "", "0\n");
    try putGroup(tmp.dir, "a.slice", "4096\n");
    // A removed group answers ENOENT to every file, including cgroup.controllers.
    try tmp.dir.createDirPath(testing.io, "gone.slice");
    try put(tmp.dir, "gone.slice/stale.txt", "not a cgroup file\n");

    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, tmp.dir, "/");

    try testing.expectEqual(@as(u32, 2), tree.counts.groups);
    try testing.expectEqual(@as(u32, 1), tree.counts.vanished);
    try testing.expect(tree.find("/gone.slice") == null);
    try testing.expectEqual(@as(?u64, 4096), tree.find("/a.slice").?.memory_current_bytes);
    // The vanished group's missing files are not charged to anyone.
    for (0..file_count) |index| try testing.expectEqual(@as(u32, 0), tree.counts.unreadable[index]);
}

test "a group that merely lacks files is listed with null fields, not vanished" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try tmp.dir.createDirPath(testing.io, "bare.slice");
    try put(tmp.dir, "bare.slice/cgroup.controllers", "\n");

    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, tmp.dir, "/bare.slice");
    try testing.expectEqual(@as(u32, 1), tree.counts.groups);
    try testing.expectEqual(@as(u32, 0), tree.counts.vanished);
    const bare = tree.groups.items[0];
    try testing.expectEqual(@as(?u64, null), bare.memory_current_bytes);
    try testing.expectEqual(@as(?files.CpuStat, null), bare.cpu_stat);
}

test "a file that cannot be read or parsed leaves that field null and is counted per file" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putGroup(tmp.dir, "", "100\n");
    try put(tmp.dir, "memory.peak", "lots\n");
    // A directory where memory.max should be: it opens, the read fails (EISDIR).
    try tmp.dir.createDirPath(testing.io, "memory.max");
    try put(tmp.dir, "cpu.stat", "usage_usec 5\nuser_usec 3\nsystem_usec 2\n");
    try put(tmp.dir, "memory.pressure", "some avg10=0.00 avg60=0.00 avg300=0.00 total=9\n");
    const filler = try testing.allocator.alloc(u8, read_buffer_bytes + 10);
    defer testing.allocator.free(filler);
    @memset(filler, 'x');
    filler[filler.len - 1] = '\n';
    try put(tmp.dir, "memory.stat", filler);

    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, tmp.dir, "/");

    const group = tree.groups.items[0];
    try testing.expectEqual(@as(?u64, 100), group.memory_current_bytes);
    try testing.expectEqual(@as(?u64, null), group.memory_peak_bytes);
    try testing.expectEqual(@as(?files.Limit, null), group.memory_max);
    try testing.expectEqual(@as(?files.MemoryStat, null), group.memory_stat);
    try testing.expectEqual(@as(?u64, 5), group.cpu_stat.?.usage_usec);
    try testing.expectEqual(@as(?u64, 9), group.memory_pressure.?.some.total_us);
    try testing.expectEqual(@as(?pressure_file.Line, null), group.memory_pressure.?.full);
    try testing.expectEqual(@as(u32, 1), tree.counts.malformed[@intFromEnum(File.memory_peak)]);
    try testing.expectEqual(@as(u32, 1), tree.counts.unreadable[@intFromEnum(File.memory_max)]);
    try testing.expectEqual(@as(u32, 1), tree.counts.malformed[@intFromEnum(File.memory_stat)]);
    // The directory standing in for memory.max is also walked as a child group,
    // and a child without cgroup.controllers is a removed group.
    try testing.expectEqual(@as(u32, 1), tree.counts.vanished);
    try testing.expectEqual(@as(u32, 1), tree.counts.groups);
}

test "a hierarchy deeper than the limit is cut off and counted" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    var path: std.ArrayList(u8) = .empty;
    defer path.deinit(testing.allocator);
    try putGroup(tmp.dir, "", "1\n");
    for (0..max_depth + 3) |_| {
        try path.appendSlice(testing.allocator, "d/");
        try putGroup(tmp.dir, path.items[0 .. path.items.len - 1], "1\n");
    }

    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, tmp.dir, "/");
    try testing.expectEqual(@as(u32, max_depth + 1), tree.counts.groups);
    try testing.expectEqual(@as(u32, 1), tree.counts.too_deep);
}

test "a second walk replaces the first and a reused tree gives the same answer" {
    var tmp = testing.tmpDir(.{ .iterate = true });
    defer tmp.cleanup();
    try putGroup(tmp.dir, "", "1\n");
    try putGroup(tmp.dir, "a.slice", "2\n");
    var tree: Tree = .init(testing.allocator);
    defer tree.deinit();
    try tree.walk(testing.io, tmp.dir, "/");
    try testing.expectEqual(@as(u32, 2), tree.counts.groups);
    try putGroup(tmp.dir, "b.slice", "3\n");
    try tree.walk(testing.io, tmp.dir, "/");
    try testing.expectEqual(@as(u32, 3), tree.counts.groups);
    try tmp.dir.deleteTree(testing.io, "a.slice");
    try tree.walk(testing.io, tmp.dir, "/");
    try testing.expectEqual(@as(u32, 2), tree.counts.groups);
    try testing.expect(tree.find("/a.slice") == null);
    try testing.expectEqual(@as(?u64, 3), tree.find("/b.slice").?.memory_current_bytes);
}

test "fuzz: a walk over arbitrary group file contents never panics or leaks" {
    try testing.fuzz({}, struct {
        fn run(_: void, smith: *testing.Smith) anyerror!void {
            var tmp = testing.tmpDir(.{ .iterate = true });
            defer tmp.cleanup();
            try tmp.dir.createDirPath(testing.io, "g");
            try put(tmp.dir, "cgroup.controllers", "cpu\n");
            try put(tmp.dir, "g/cgroup.controllers", "cpu\n");
            var content: [160]u8 = undefined;
            inline for (file_names) |name| {
                const length = smith.slice(&content);
                try put(tmp.dir, "g/" ++ name, content[0..length]);
            }
            var tree: Tree = .init(testing.allocator);
            defer tree.deinit();
            try tree.walk(testing.io, tmp.dir, "/");
            try testing.expectEqual(@as(u32, 2), tree.counts.groups);
        }
    }.run, .{});
}
