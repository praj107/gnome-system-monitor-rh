// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/<pid>/status`, proc_pid_status(5) and Documentation/filesystems/
//! proc.rst (Table 1-2): `Key:\tvalue` lines. Kept: `RssAnon`, `RssFile`,
//! `RssShmem`, `VmSwap`, `VmSize` (kB in the file, bytes here) and the real uid. A key
//! the kernel does not print (a kernel thread has no mm, so no Rss lines)
//! stays null: unknown, never zero (a guess shown as a measurement is worse
//! than no figure).
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

pub const Status = struct {
    /// Private (anonymous) resident memory.
    rss_anon_bytes: ?u64 = null,
    /// Resident file-backed pages, possibly shared.
    rss_file_bytes: ?u64 = null,
    /// Resident shared-memory pages.
    rss_shmem_bytes: ?u64 = null,
    vm_swap_bytes: ?u64 = null,
    /// Virtual size: address space, not memory use. Null for a kernel thread.
    vm_size_bytes: ?u64 = null,
    /// First of the four ids on the `Uid:` line.
    uid_real: ?u32 = null,
};

const Key = enum { RssAnon, RssFile, RssShmem, VmSwap, VmSize, Uid };

const bytes_per_kb = 1024;

pub fn parse(bytes: []const u8) Error!Status {
    try numbers.requireComplete(bytes);
    var result: Status = .{};
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |text| {
        const colon = std.mem.indexOfScalar(u8, text, ':') orelse continue;
        const key = std.meta.stringToEnum(Key, text[0..colon]) orelse continue;
        const value = std.mem.trim(u8, text[colon + 1 ..], " \t");
        switch (key) {
            .RssAnon => result.rss_anon_bytes = try kilobytes(value),
            .RssFile => result.rss_file_bytes = try kilobytes(value),
            .RssShmem => result.rss_shmem_bytes = try kilobytes(value),
            .VmSwap => result.vm_swap_bytes = try kilobytes(value),
            .VmSize => result.vm_size_bytes = try kilobytes(value),
            .Uid => {
                var ids = std.mem.tokenizeAny(u8, value, " \t");
                result.uid_real = try numbers.unsigned(u32, ids.next() orelse return error.MissingField);
            },
        }
    }
    return result;
}

/// "112 kB" to bytes. The unit is required: a bare number is a cut line.
fn kilobytes(value: []const u8) Error!u64 {
    var pieces = std.mem.tokenizeAny(u8, value, " \t");
    const amount = try numbers.unsigned(u64, pieces.next() orelse return error.MissingField);
    const unit = pieces.next() orelse return error.Malformed;
    if (!std.mem.eql(u8, unit, "kB") or pieces.next() != null) return error.Malformed;
    return std.math.mul(u64, amount, bytes_per_kb) catch error.Malformed;
}

const sample =
    "Name:\tcat\nUmask:\t0002\nState:\tR (running)\nTgid:\t333354\nUid:\t1000\t1001\t1002\t1003\n" ++
    "VmSize:\t    8444 kB\nVmRSS:\t    1940 kB\nRssAnon:\t     112 kB\nRssFile:\t    1828 kB\n" ++
    "RssShmem:\t       3 kB\nVmData:\t     360 kB\nVmSwap:\t      16 kB\nHugetlbPages:\t       0 kB\n";

test "a status file gives the four memory figures in bytes and the real uid" {
    const s = try parse(sample);
    try std.testing.expectEqual(@as(?u64, 112 * 1024), s.rss_anon_bytes);
    try std.testing.expectEqual(@as(?u64, 1828 * 1024), s.rss_file_bytes);
    try std.testing.expectEqual(@as(?u64, 3 * 1024), s.rss_shmem_bytes);
    try std.testing.expectEqual(@as(?u64, 16 * 1024), s.vm_swap_bytes);
    try std.testing.expectEqual(@as(?u64, 8444 * 1024), s.vm_size_bytes);
    try std.testing.expectEqual(@as(?u32, 1000), s.uid_real);
}

test "a kernel thread without Rss lines has unknown memory, not zero" {
    const s = try parse("Name:\tkthreadd\nState:\tS (sleeping)\nUid:\t0\t0\t0\t0\nKthread:\t1\n");
    try std.testing.expectEqual(@as(?u64, null), s.rss_anon_bytes);
    try std.testing.expectEqual(@as(?u64, null), s.vm_swap_bytes);
    try std.testing.expectEqual(@as(?u64, null), s.vm_size_bytes);
    try std.testing.expectEqual(@as(?u32, 0), s.uid_real);
}

test "a process that really has zero swap reports zero swap" {
    const s = try parse("Uid:\t1\t1\t1\t1\nVmSwap:\t       0 kB\n");
    try std.testing.expectEqual(@as(?u64, 0), s.vm_swap_bytes);
}

test "a key that only contains a known key's name is ignored" {
    const s = try parse("RssAnonX:\t5 kB\nxRssAnon:\t5 kB\nUid:\t3\t3\t3\t3\n");
    try std.testing.expectEqual(@as(?u64, null), s.rss_anon_bytes);
}

test "a status cut inside a memory line is an error, not a smaller figure" {
    try std.testing.expectError(error.Truncated, parse("Uid:\t1\t1\t1\t1\nRssAnon:\t     11"));
    try std.testing.expectError(error.Malformed, parse("RssAnon:\t     11\n"));
    try std.testing.expectError(error.Malformed, parse("RssAnon:\t     11 MB\n"));
    try std.testing.expectError(error.Malformed, parse("RssAnon:\t     11 kB extra\n"));
    try std.testing.expectError(error.MissingField, parse("RssAnon:\t\n"));
    try std.testing.expectError(error.MissingField, parse("Uid:\t\n"));
    try std.testing.expectError(error.Truncated, parse(""));
}

test "a huge kB figure that overflows bytes is malformed and the largest that fits is kept" {
    const max_kb = std.math.maxInt(u64) / 1024;
    var buffer: [64]u8 = undefined;
    const fits = try std.fmt.bufPrint(&buffer, "RssAnon:\t{d} kB\n", .{max_kb});
    try std.testing.expectEqual(@as(?u64, max_kb * 1024), (try parse(fits)).rss_anon_bytes);
    const over = try std.fmt.bufPrint(&buffer, "RssAnon:\t{d} kB\n", .{max_kb + 1});
    try std.testing.expectError(error.Malformed, parse(over));
    try std.testing.expectError(error.Malformed, parse("RssAnon:\t99999999999999999999 kB\n"));
}

test "fuzz: arbitrary status bytes never panic and sizes are whole kilobytes" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [256]u8 = undefined;
            const length = smith.slice(&input);
            const s = parse(input[0..length]) catch return;
            if (s.rss_anon_bytes) |anon| try std.testing.expectEqual(@as(u64, 0), anon % bytes_per_kb);
            if (s.vm_swap_bytes) |swap| try std.testing.expectEqual(@as(u64, 0), swap % bytes_per_kb);
        }
    }.run, .{});
}
