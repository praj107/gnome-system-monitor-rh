// SPDX-License-Identifier: GPL-2.0-or-later
//! Pressure stall information, Documentation/accounting/psi.rst. One file
//! (`/proc/pressure/{cpu,memory,io}` or a cgroup's `{cpu,memory,io}.pressure`)
//! holds a `some` line and, usually, a `full` line:
//!
//!     some avg10=0.12 avg60=0.36 avg300=0.32 total=74156353
//!     full avg10=0.00 avg60=0.00 avg300=0.00 total=0
//!
//! `some` is the share of time at least one task was stalled, `full` the
//! share when every non-idle task was. `avgN` are the kernel's moving
//! averages in percent; `total` is the cumulative stall in microseconds.
//! The share over a window of the caller's choosing comes from two `total`s,
//! not from the averages (see `stallPercent`).
//!
//! `full` is absent from the CPU file on kernels before 5.13 (and is all
//! zero at system level on later ones): absent is `null`, never zero.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

pub const Line = struct {
    avg10_percent: f32,
    avg60_percent: f32,
    avg300_percent: f32,
    /// Cumulative stall time since boot (or cgroup creation).
    total_us: u64,
};

pub const Pressure = struct {
    some: Line,
    /// Null when the file has no `full` line.
    full: ?Line,
};

pub fn parse(bytes: []const u8) Error!Pressure {
    try numbers.requireComplete(bytes);
    var some: ?Line = null;
    var full: ?Line = null;
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |line| {
        const space = std.mem.indexOfScalar(u8, line, ' ') orelse return error.Malformed;
        const kind = line[0..space];
        const slot = if (std.mem.eql(u8, kind, "some"))
            &some
        else if (std.mem.eql(u8, kind, "full"))
            &full
        else
            return error.Malformed;
        if (slot.* != null) return error.Malformed;
        slot.* = try parseLine(line[space + 1 ..]);
    }
    return .{ .some = some orelse return error.MissingField, .full = full };
}

/// "avg10=0.12 avg60=0.36 avg300=0.32 total=74156353", each key once, any order.
fn parseLine(text: []const u8) Error!Line {
    var avg10: ?f32 = null;
    var avg60: ?f32 = null;
    var avg300: ?f32 = null;
    var total: ?u64 = null;
    var pieces = std.mem.splitScalar(u8, text, ' ');
    while (pieces.next()) |piece| {
        const equals = std.mem.indexOfScalar(u8, piece, '=') orelse return error.Malformed;
        const key = piece[0..equals];
        const value = piece[equals + 1 ..];
        if (std.mem.eql(u8, key, "avg10")) {
            if (avg10 != null) return error.Malformed;
            avg10 = try percent(value);
        } else if (std.mem.eql(u8, key, "avg60")) {
            if (avg60 != null) return error.Malformed;
            avg60 = try percent(value);
        } else if (std.mem.eql(u8, key, "avg300")) {
            if (avg300 != null) return error.Malformed;
            avg300 = try percent(value);
        } else if (std.mem.eql(u8, key, "total")) {
            if (total != null) return error.Malformed;
            total = try numbers.unsigned(u64, value);
        } else return error.Malformed;
    }
    return .{
        .avg10_percent = avg10 orelse return error.MissingField,
        .avg60_percent = avg60 orelse return error.MissingField,
        .avg300_percent = avg300 orelse return error.MissingField,
        .total_us = total orelse return error.MissingField,
    };
}

/// The kernel prints averages as "%lu.%02lu"; digits with an optional
/// fractional part are accepted, nothing else (no sign, exponent, "nan").
fn percent(text: []const u8) Error!f32 {
    const dot = std.mem.indexOfScalar(u8, text, '.');
    const whole = text[0 .. dot orelse text.len];
    const fraction = if (dot) |at| text[at + 1 ..] else "0";
    if (whole.len == 0 or whole.len > 10 or fraction.len == 0 or fraction.len > 10) return error.Malformed;
    for (whole) |digit| if (digit < '0' or digit > '9') return error.Malformed;
    for (fraction) |digit| if (digit < '0' or digit > '9') return error.Malformed;
    return std.fmt.parseFloat(f32, text) catch error.Malformed;
}

/// The share of a window during which tasks were stalled, in percent:
/// `Δtotal_us / Δwall_us × 100`. Null (unknown) when the window has no
/// length or the counter went backwards (a cgroup recreated under the same
/// path, a reboot). The result can exceed 100 slightly because the two
/// reads and the wall clock are not simultaneous; it is not clamped.
pub fn stallPercent(previous: Line, current: Line, wall_us: u64) ?f64 {
    if (wall_us == 0 or current.total_us < previous.total_us) return null;
    const stalled_us: f64 = @floatFromInt(current.total_us - previous.total_us);
    return stalled_us / @as(f64, @floatFromInt(wall_us)) * 100.0;
}

// ---- tests ----------------------------------------------------------

const testing = std.testing;

fn fixture(name: []const u8, buffer: []u8) ![]u8 {
    return std.Io.Dir.cwd().readFile(testing.io, name, buffer);
}

test "a memory pressure file gives some and full with averages and totals" {
    var buffer: [512]u8 = undefined;
    const p = try parse(try fixture("testdata/system/pressure-memory-b.txt", &buffer));
    try testing.expectEqual(@as(u64, 63114656), p.some.total_us);
    try testing.expectEqual(@as(u64, 58973392), p.full.?.total_us);
    try testing.expectEqual(@as(f32, 0.0), p.some.avg10_percent);
}

test "a CPU pressure file shows the kernel's averages" {
    var buffer: [512]u8 = undefined;
    const p = try parse(try fixture("testdata/system/pressure-cpu-a.txt", &buffer));
    try testing.expectEqual(@as(f32, 0.12), p.some.avg10_percent);
    try testing.expectEqual(@as(f32, 0.36), p.some.avg60_percent);
    try testing.expectEqual(@as(f32, 0.32), p.some.avg300_percent);
    try testing.expectEqual(@as(u64, 74156353), p.some.total_us);
    try testing.expectEqual(@as(u64, 0), p.full.?.total_us);
}

test "a CPU pressure file from an older kernel has no full line, which is null and not zero" {
    var buffer: [512]u8 = undefined;
    const p = try parse(try fixture("testdata/system/pressure-cpu-no-full.txt", &buffer));
    try testing.expectEqual(@as(?Line, null), p.full);
    try testing.expectEqual(@as(u64, 74156353), p.some.total_us);
}

test "the stall share over a window comes from the totals, not the averages" {
    var buffer_a: [512]u8 = undefined;
    var buffer_b: [512]u8 = undefined;
    const a = try parse(try fixture("testdata/system/pressure-io-a.txt", &buffer_a));
    const b = try parse(try fixture("testdata/system/pressure-io-b.txt", &buffer_b));
    // some: 10940280 - 10939815 = 465 us over 2 s of wall time
    const some = stallPercent(a.some, b.some, 2_000_000).?;
    try testing.expectApproxEqAbs(@as(f64, 0.02325), some, 1e-9);
    // full: 9557034 - 9556574 = 460 us
    try testing.expectApproxEqAbs(@as(f64, 0.023), stallPercent(a.full.?, b.full.?, 2_000_000).?, 1e-9);
}

test "a half-stalled window is fifty percent" {
    const before: Line = .{ .avg10_percent = 0, .avg60_percent = 0, .avg300_percent = 0, .total_us = 1_000 };
    const after: Line = .{ .avg10_percent = 0, .avg60_percent = 0, .avg300_percent = 0, .total_us = 501_000 };
    try testing.expectEqual(@as(?f64, 50.0), stallPercent(before, after, 1_000_000));
}

test "an empty window or a counter that went backwards is unknown, not zero" {
    const before: Line = .{ .avg10_percent = 0, .avg60_percent = 0, .avg300_percent = 0, .total_us = 500 };
    var after = before;
    try testing.expectEqual(@as(?f64, null), stallPercent(before, after, 0));
    after.total_us = 100;
    try testing.expectEqual(@as(?f64, null), stallPercent(before, after, 1_000_000));
    after.total_us = 500;
    try testing.expectEqual(@as(?f64, 0.0), stallPercent(before, after, 1_000_000));
}

test "malformed pressure text is an error value" {
    try testing.expectError(error.Truncated, parse(""));
    try testing.expectError(error.Truncated, parse("some avg10=0.00 avg60=0.00 avg300=0.00 total=1"));
    try testing.expectError(error.MissingField, parse("full avg10=0.00 avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.MissingField, parse("some avg10=0.00 avg60=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=0.00 avg60=0.00 avg300=0.00 total=-1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=nan avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=1e3 avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=-0.5 avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=0.00 avg10=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("most avg10=0.00 avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some avg10=0.00 avg60=0.00 avg300=0.00 total=1\nsome avg10=0.00 avg60=0.00 avg300=0.00 total=1\n"));
    try testing.expectError(error.Malformed, parse("some\n"));
    try testing.expectError(error.Malformed, parse("some avg10=0.00 avg60=0.00 avg300=0.00 total=18446744073709551616\n"));
    try testing.expectError(error.Malformed, parse("\n"));
}

test "keys may come in any order" {
    const p = try parse("some total=7 avg300=3.00 avg60=2.50 avg10=1.25\n");
    try testing.expectEqual(@as(u64, 7), p.some.total_us);
    try testing.expectEqual(@as(f32, 1.25), p.some.avg10_percent);
}

test "fuzz: arbitrary pressure bytes never panic" {
    try testing.fuzz({}, struct {
        fn run(_: void, smith: *testing.Smith) anyerror!void {
            var input: [160]u8 = undefined;
            const length = smith.slice(&input);
            const p = parse(input[0..length]) catch return;
            _ = stallPercent(p.some, p.some, 1);
        }
    }.run, .{});
}
