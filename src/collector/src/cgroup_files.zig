// SPDX-License-Identifier: GPL-2.0-or-later
//! The cgroup v2 interface files this collector reads, one parser each:
//! Documentation/admin-guide/cgroup-v2.rst.
//!
//!  - single-value files: `memory.current`, `memory.peak`, `pids.current`
//!    (a number), `memory.max`, `memory.high` (a number or "max");
//!  - flat-keyed files ("key value" lines): `memory.stat`, `memory.events`,
//!    `cpu.stat`. Only the keys named below are kept; a key that is not in the
//!    file is `null`, never zero (a controller that is off, or an older
//!    kernel, simply has no such line). A kept key whose value is not a
//!    number, or that appears twice, makes the whole file an error.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

/// A limit that is a number of bytes or the word "max" (no limit).
pub const Limit = union(enum) {
    unlimited,
    bytes: u64,
};

/// One decimal number and a newline: `memory.current`, `memory.peak`,
/// `pids.current`.
pub fn parseCount(bytes: []const u8) Error!u64 {
    return numbers.unsigned(u64, try oneLine(bytes));
}

/// `memory.max` and `memory.high`: "max" or a number of bytes.
pub fn parseLimit(bytes: []const u8) Error!Limit {
    const text = try oneLine(bytes);
    if (std.mem.eql(u8, text, "max")) return .unlimited;
    return .{ .bytes = try numbers.unsigned(u64, text) };
}

fn oneLine(bytes: []const u8) Error![]const u8 {
    try numbers.requireComplete(bytes);
    const text = bytes[0 .. bytes.len - 1];
    if (std.mem.indexOfScalar(u8, text, '\n') != null) return error.Malformed;
    return text;
}

/// `memory.stat`: sizes in bytes, event counters as plain counts.
pub const MemoryStat = struct {
    anon_bytes: ?u64 = null,
    file_bytes: ?u64 = null,
    shmem_bytes: ?u64 = null,
    kernel_bytes: ?u64 = null,
    sock_bytes: ?u64 = null,
    slab_reclaimable_bytes: ?u64 = null,
    slab_unreclaimable_bytes: ?u64 = null,
    file_mapped_bytes: ?u64 = null,
    file_dirty_bytes: ?u64 = null,
    /// Pages re-faulted from swap/zram after eviction (count).
    workingset_refault_anon: ?u64 = null,
    /// Page-cache pages re-read after eviction (count): memory thrashing.
    workingset_refault_file: ?u64 = null,
    /// Major page faults (count).
    pgmajfault: ?u64 = null,
};
const memory_stat_keys = [_][]const u8{
    "anon",       "file",                    "shmem",                   "kernel",
    "sock",       "slab_reclaimable",        "slab_unreclaimable",      "file_mapped",
    "file_dirty", "workingset_refault_anon", "workingset_refault_file", "pgmajfault",
};

pub fn parseMemoryStat(bytes: []const u8) Error!MemoryStat {
    return parseKeyed(MemoryStat, &memory_stat_keys, bytes);
}

/// `memory.events`: how many times each threshold was hit (counts).
pub const MemoryEvents = struct {
    high: ?u64 = null,
    max: ?u64 = null,
    oom: ?u64 = null,
    oom_kill: ?u64 = null,
};
const memory_events_keys = [_][]const u8{ "high", "max", "oom", "oom_kill" };

pub fn parseMemoryEvents(bytes: []const u8) Error!MemoryEvents {
    return parseKeyed(MemoryEvents, &memory_events_keys, bytes);
}

/// `cpu.stat`: CPU time in microseconds. `nr_throttled` and `throttled_usec`
/// are written only where the cpu controller is enabled for the group.
pub const CpuStat = struct {
    usage_usec: ?u64 = null,
    user_usec: ?u64 = null,
    system_usec: ?u64 = null,
    nr_throttled: ?u64 = null,
    throttled_usec: ?u64 = null,
};
const cpu_stat_keys = [_][]const u8{ "usage_usec", "user_usec", "system_usec", "nr_throttled", "throttled_usec" };

pub fn parseCpuStat(bytes: []const u8) Error!CpuStat {
    return parseKeyed(CpuStat, &cpu_stat_keys, bytes);
}

/// Parses "key value" lines into `Record`, whose i-th field takes the value
/// of `keys[i]`. Every line must be "key value"; only the wanted keys have
/// their value checked, so a new kernel's extra keys cannot spoil the file.
fn parseKeyed(comptime Record: type, comptime keys: []const []const u8, bytes: []const u8) Error!Record {
    const fields = @typeInfo(Record).@"struct".fields;
    comptime std.debug.assert(fields.len == keys.len);

    try numbers.requireComplete(bytes);
    var values: [keys.len]?u64 = @splat(null);
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |line| {
        const space = std.mem.indexOfScalar(u8, line, ' ') orelse return error.Malformed;
        const key = line[0..space];
        if (key.len == 0) return error.Malformed;
        inline for (keys, 0..) |wanted, index| {
            if (std.mem.eql(u8, key, wanted)) {
                if (values[index] != null) return error.Malformed;
                values[index] = try numbers.unsigned(u64, line[space + 1 ..]);
            }
        }
    }

    var record: Record = .{};
    inline for (fields, 0..) |field, index| @field(record, field.name) = values[index];
    return record;
}

// ---- tests ----------------------------------------------------------

const testing = std.testing;

test "a count file is one number and a newline" {
    try testing.expectEqual(@as(u64, 3966554112), try parseCount("3966554112\n"));
    try testing.expectEqual(@as(u64, 0), try parseCount("0\n"));
    try testing.expectError(error.Truncated, parseCount("12"));
    try testing.expectError(error.Malformed, parseCount("max\n"));
    try testing.expectError(error.Malformed, parseCount("1\n2\n"));
    try testing.expectError(error.Malformed, parseCount("\n"));
    try testing.expectError(error.Malformed, parseCount("-1\n"));
}

test "a limit is a number of bytes or max, which is no limit and not zero" {
    try testing.expectEqual(Limit.unlimited, try parseLimit("max\n"));
    try testing.expectEqual(Limit{ .bytes = 8589934592 }, try parseLimit("8589934592\n"));
    try testing.expectEqual(Limit{ .bytes = 0 }, try parseLimit("0\n"));
    try testing.expectError(error.Malformed, parseLimit("maximum\n"));
    try testing.expectError(error.Truncated, parseLimit("max"));
}

test "memory.stat keeps the named keys in bytes and counts and leaves absent keys null" {
    const stat = try parseMemoryStat(
        "anon 905277440\nfile 2838020096\nkernel 221249536\nkernel_stack 1\nsock 0\nshmem 2838020096\n" ++
            "file_mapped 0\nfile_dirty 4096\nslab_reclaimable 61020208\nslab_unreclaimable 158094080\n" ++
            "workingset_refault_file 7\npgmajfault 3\nsome_future_key 1\n",
    );
    try testing.expectEqual(@as(?u64, 905277440), stat.anon_bytes);
    try testing.expectEqual(@as(?u64, 2838020096), stat.file_bytes);
    try testing.expectEqual(@as(?u64, 2838020096), stat.shmem_bytes);
    try testing.expectEqual(@as(?u64, 221249536), stat.kernel_bytes);
    try testing.expectEqual(@as(?u64, 0), stat.sock_bytes);
    try testing.expectEqual(@as(?u64, 4096), stat.file_dirty_bytes);
    try testing.expectEqual(@as(?u64, 61020208), stat.slab_reclaimable_bytes);
    try testing.expectEqual(@as(?u64, 158094080), stat.slab_unreclaimable_bytes);
    try testing.expectEqual(@as(?u64, 7), stat.workingset_refault_file);
    try testing.expectEqual(@as(?u64, 3), stat.pgmajfault);
    // Not in the file: unknown, not zero.
    try testing.expectEqual(@as(?u64, null), stat.workingset_refault_anon);
}

test "memory.events keeps high, max, oom and oom_kill" {
    const events = try parseMemoryEvents("low 0\nhigh 4\nmax 2\noom 1\noom_kill 1\noom_group_kill 0\nsock_throttled 0\n");
    try testing.expectEqual(@as(?u64, 4), events.high);
    try testing.expectEqual(@as(?u64, 2), events.max);
    try testing.expectEqual(@as(?u64, 1), events.oom);
    try testing.expectEqual(@as(?u64, 1), events.oom_kill);
}

test "cpu.stat of a group without the cpu controller has no throttle counters" {
    const stat = try parseCpuStat("usage_usec 121908751\nuser_usec 114832251\nsystem_usec 7076500\nnice_usec 0\ncore_sched.force_idle_usec 0\n");
    try testing.expectEqual(@as(?u64, 121908751), stat.usage_usec);
    try testing.expectEqual(@as(?u64, 114832251), stat.user_usec);
    try testing.expectEqual(@as(?u64, 7076500), stat.system_usec);
    try testing.expectEqual(@as(?u64, null), stat.nr_throttled);
    try testing.expectEqual(@as(?u64, null), stat.throttled_usec);
}

test "cpu.stat with the cpu controller on reports throttling, zero included" {
    const stat = try parseCpuStat("usage_usec 1\nuser_usec 1\nsystem_usec 0\nnr_periods 9\nnr_throttled 0\nthrottled_usec 0\n");
    try testing.expectEqual(@as(?u64, 0), stat.nr_throttled);
    try testing.expectEqual(@as(?u64, 0), stat.throttled_usec);
}

test "a keyed file with a bad wanted value, a repeat, a line without a value or a cut is an error" {
    try testing.expectError(error.Malformed, parseMemoryEvents("high x\n"));
    try testing.expectError(error.Malformed, parseMemoryEvents("high -1\n"));
    try testing.expectError(error.Malformed, parseMemoryEvents("high 1\nhigh 2\n"));
    try testing.expectError(error.Malformed, parseMemoryEvents("high\n"));
    try testing.expectError(error.Malformed, parseMemoryEvents("high 1\n\n"));
    try testing.expectError(error.Malformed, parseMemoryEvents(" 1\n"));
    try testing.expectError(error.Truncated, parseMemoryEvents("high 1"));
    try testing.expectError(error.Malformed, parseCpuStat("usage_usec 18446744073709551616\n"));
    // An unwanted key's value is not checked.
    const events = try parseMemoryEvents("whatever ???\nhigh 1\n");
    try testing.expectEqual(@as(?u64, 1), events.high);
}

test "fuzz: arbitrary cgroup file bytes never panic" {
    try testing.fuzz({}, struct {
        fn run(_: void, smith: *testing.Smith) anyerror!void {
            var input: [192]u8 = undefined;
            const length = smith.slice(&input);
            const bytes = input[0..length];
            _ = parseCount(bytes) catch {};
            _ = parseLimit(bytes) catch {};
            _ = parseMemoryStat(bytes) catch {};
            _ = parseMemoryEvents(bytes) catch {};
            _ = parseCpuStat(bytes) catch {};
        }
    }.run, .{});
}
