// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/<pid>/io`, proc_pid_io(5): seven `key: value` lines of cumulative
//! per-task I/O counters. `rchar`/`wchar` count bytes passed through read(2)
//! and write(2) (page cache and pipes included), `syscr`/`syscw` count those
//! calls, `read_bytes`/`write_bytes` count bytes the storage layer was asked
//! to fetch or send, and `cancelled_write_bytes` counts writes undone by
//! truncation.
//!
//! The kernel refuses the file (EACCES at open) for other users' processes,
//! and a kernel built without task I/O accounting has no such file, so a
//! missing or refused file is handled by the caller as "unknown". Here a key
//! the kernel did not print is an error, never zero: the seven counters are
//! known together or not at all. A key this parser does not know is ignored,
//! so a newer kernel that adds a line does not hide the rest.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

pub const ProcIo = struct {
    rchar_bytes: u64,
    wchar_bytes: u64,
    syscr: u64,
    syscw: u64,
    read_bytes: u64,
    write_bytes: u64,
    cancelled_write_bytes: u64,
};

const Key = enum { rchar, wchar, syscr, syscw, read_bytes, write_bytes, cancelled_write_bytes };
const key_count = @typeInfo(Key).@"enum".fields.len;

pub fn parse(bytes: []const u8) Error!ProcIo {
    try numbers.requireComplete(bytes);
    var values: [key_count]?u64 = @splat(null);
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |text| {
        const colon = std.mem.indexOfScalar(u8, text, ':') orelse return error.Malformed;
        const key = std.meta.stringToEnum(Key, text[0..colon]) orelse continue;
        const value = std.mem.trim(u8, text[colon + 1 ..], " \t");
        values[@intFromEnum(key)] = try numbers.unsigned(u64, value);
    }
    for (values) |value| if (value == null) return error.MissingField;
    return .{
        .rchar_bytes = values[@intFromEnum(Key.rchar)].?,
        .wchar_bytes = values[@intFromEnum(Key.wchar)].?,
        .syscr = values[@intFromEnum(Key.syscr)].?,
        .syscw = values[@intFromEnum(Key.syscw)].?,
        .read_bytes = values[@intFromEnum(Key.read_bytes)].?,
        .write_bytes = values[@intFromEnum(Key.write_bytes)].?,
        .cancelled_write_bytes = values[@intFromEnum(Key.cancelled_write_bytes)].?,
    };
}

const sample =
    "rchar: 323934931\nwchar: 323929600\nsyscr: 632687\nsyscw: 632675\n" ++
    "read_bytes: 4096\nwrite_bytes: 323932160\ncancelled_write_bytes: 8192\n";

test "an io file gives the seven counters" {
    const counters = try parse(sample);
    try std.testing.expectEqual(@as(u64, 323934931), counters.rchar_bytes);
    try std.testing.expectEqual(@as(u64, 323929600), counters.wchar_bytes);
    try std.testing.expectEqual(@as(u64, 632687), counters.syscr);
    try std.testing.expectEqual(@as(u64, 632675), counters.syscw);
    try std.testing.expectEqual(@as(u64, 4096), counters.read_bytes);
    try std.testing.expectEqual(@as(u64, 323932160), counters.write_bytes);
    try std.testing.expectEqual(@as(u64, 8192), counters.cancelled_write_bytes);
}

test "a process that did no I/O has zeros, which are real" {
    const counters = try parse("rchar: 0\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n");
    try std.testing.expectEqual(@as(u64, 0), counters.write_bytes);
}

test "the order of the lines and an extra key do not matter" {
    const counters = try parse("future_key: 9\nwrite_bytes: 7\ncancelled_write_bytes: 1\nread_bytes: 2\nsyscw: 3\nsyscr: 4\nwchar: 5\nrchar: 6\n");
    try std.testing.expectEqual(@as(u64, 6), counters.rchar_bytes);
    try std.testing.expectEqual(@as(u64, 7), counters.write_bytes);
}

test "an io file missing a key, cut short, or with a bad number is an error" {
    try std.testing.expectError(error.MissingField, parse("rchar: 1\nwchar: 2\n"));
    try std.testing.expectError(error.Truncated, parse(sample[0 .. sample.len - 1]));
    try std.testing.expectError(error.Truncated, parse(""));
    try std.testing.expectError(error.Malformed, parse("rchar: -1\n"));
    try std.testing.expectError(error.Malformed, parse("rchar: 1x\n"));
    try std.testing.expectError(error.Malformed, parse("rchar 1\n"));
    try std.testing.expectError(error.Malformed, parse("rchar:\n"));
}

test "a counter of the largest u64 is kept and one more is malformed" {
    const top = "rchar: 18446744073709551615\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n";
    try std.testing.expectEqual(std.math.maxInt(u64), (try parse(top)).rchar_bytes);
    const over = "rchar: 18446744073709551616\nwchar: 0\nsyscr: 0\nsyscw: 0\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n";
    try std.testing.expectError(error.Malformed, parse(over));
}

test "fuzz: arbitrary io bytes never panic" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [192]u8 = undefined;
            const length = smith.slice(&input);
            _ = parse(input[0..length]) catch return;
        }
    }.run, .{});
}
