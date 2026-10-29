// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/<pid>/schedstat`, Documentation/scheduler/sched-stats.rst: three
//! numbers, "time spent on the cpu" (ns), "time spent waiting on a runqueue"
//! (ns), "# of timeslices run on this cpu". Readable unprivileged and
//! without `kernel.sched_schedstats` (verified on kernel 7.0, 2026-10-28).
//! The statistics are per task: `/proc/<pid>/schedstat` is the first thread's
//! alone, not the thread group's (a 77-thread JVM read 8 ms here against
//! 513 s of `stat` utime + stime). A process's CPU time is `stat`'s.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

pub const Schedstat = struct {
    run_ns: u64,
    run_delay_ns: u64,
    timeslices: u64,
};

pub fn parse(bytes: []const u8) Error!Schedstat {
    try numbers.requireComplete(bytes);
    var pieces = std.mem.tokenizeAny(u8, bytes, " \n");
    const run_ns = try numbers.unsigned(u64, pieces.next() orelse return error.MissingField);
    const run_delay_ns = try numbers.unsigned(u64, pieces.next() orelse return error.MissingField);
    const timeslices = try numbers.unsigned(u64, pieces.next() orelse return error.MissingField);
    return .{ .run_ns = run_ns, .run_delay_ns = run_delay_ns, .timeslices = timeslices };
}

test "a schedstat line gives run time, run delay and timeslices" {
    const s = try parse("1500000000 25000 77\n");
    try std.testing.expectEqual(@as(u64, 1_500_000_000), s.run_ns);
    try std.testing.expectEqual(@as(u64, 25_000), s.run_delay_ns);
    try std.testing.expectEqual(@as(u64, 77), s.timeslices);
}

test "a process that never ran has zeros, which are real" {
    const s = try parse("0 0 0\n");
    try std.testing.expectEqual(@as(u64, 0), s.run_ns);
}

test "a schedstat with a missing field, a non-number or no newline is an error" {
    try std.testing.expectError(error.MissingField, parse("1 2\n"));
    try std.testing.expectError(error.Malformed, parse("1 -2 3\n"));
    try std.testing.expectError(error.Malformed, parse("1 2 three\n"));
    try std.testing.expectError(error.Truncated, parse("1 2 3"));
}

test "a run time of the largest u64 is kept and one more is malformed" {
    const s = try parse("18446744073709551615 0 0\n");
    try std.testing.expectEqual(std.math.maxInt(u64), s.run_ns);
    try std.testing.expectError(error.Malformed, parse("18446744073709551616 0 0\n"));
}

test "fuzz: arbitrary schedstat bytes never panic" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [96]u8 = undefined;
            const length = smith.slice(&input);
            _ = parse(input[0..length]) catch return;
        }
    }.run, .{});
}
