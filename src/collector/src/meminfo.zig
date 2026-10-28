// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/meminfo` (Documentation/filesystems/proc.rst) and the partition of
//! MemTotal into rows that say what each byte is: application private
//! (AnonPages), RAM-backed files (Shmem: tmpfs and shared memory, which
//! cannot be dropped without swap), file cache (Cached - Shmem + Buffers),
//! kernel reclaimable, kernel fixed, free, and an explicit unaccounted
//! residue. The same definitions as the GUI's C implementation (src/meminfo.c),
//! so both give the same rows for the same file.
//!
//! A key that is absent is `null`, never zero: kernels add and drop fields.
//! A known key with a value that is not "<digits> kB" is an error.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

/// Every value in bytes (the file's kB times 1024), or null when the key is
/// not in the file.
pub const Meminfo = struct {
    mem_total_bytes: ?u64 = null,
    mem_free_bytes: ?u64 = null,
    mem_available_bytes: ?u64 = null,
    buffers_bytes: ?u64 = null,
    cached_bytes: ?u64 = null,
    shmem_bytes: ?u64 = null,
    anon_pages_bytes: ?u64 = null,
    s_reclaimable_bytes: ?u64 = null,
    s_unreclaim_bytes: ?u64 = null,
    k_reclaimable_bytes: ?u64 = null,
    kernel_stack_bytes: ?u64 = null,
    page_tables_bytes: ?u64 = null,
    sec_page_tables_bytes: ?u64 = null,
    percpu_bytes: ?u64 = null,
    vmalloc_used_bytes: ?u64 = null,
    dirty_bytes: ?u64 = null,
    writeback_bytes: ?u64 = null,
};

const KeySpec = struct { key: []const u8, field: []const u8 };

// Data, not an if chain: one row per meminfo key the partition uses.
const keys = [_]KeySpec{
    .{ .key = "MemTotal", .field = "mem_total_bytes" },
    .{ .key = "MemFree", .field = "mem_free_bytes" },
    .{ .key = "MemAvailable", .field = "mem_available_bytes" },
    .{ .key = "Buffers", .field = "buffers_bytes" },
    .{ .key = "Cached", .field = "cached_bytes" },
    .{ .key = "Shmem", .field = "shmem_bytes" },
    .{ .key = "AnonPages", .field = "anon_pages_bytes" },
    .{ .key = "SReclaimable", .field = "s_reclaimable_bytes" },
    .{ .key = "SUnreclaim", .field = "s_unreclaim_bytes" },
    .{ .key = "KReclaimable", .field = "k_reclaimable_bytes" },
    .{ .key = "KernelStack", .field = "kernel_stack_bytes" },
    .{ .key = "PageTables", .field = "page_tables_bytes" },
    .{ .key = "SecPageTables", .field = "sec_page_tables_bytes" },
    .{ .key = "Percpu", .field = "percpu_bytes" },
    .{ .key = "VmallocUsed", .field = "vmalloc_used_bytes" },
    .{ .key = "Dirty", .field = "dirty_bytes" },
    .{ .key = "Writeback", .field = "writeback_bytes" },
};

pub fn parse(bytes: []const u8) Error!Meminfo {
    try numbers.requireComplete(bytes);
    var info: Meminfo = .{};
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |line| {
        if (line.len == 0) continue;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return error.Malformed;
        const key = line[0..colon];
        inline for (keys) |spec| {
            if (std.mem.eql(u8, key, spec.key)) {
                @field(info, spec.field) = try kibToBytes(line[colon + 1 ..]);
            }
        }
    }
    return info;
}

/// "   1234 kB" to bytes; a missing unit, another unit or an overflow is an error.
fn kibToBytes(value: []const u8) Error!u64 {
    const trimmed = std.mem.trimStart(u8, value, " \t");
    const space = std.mem.indexOfScalar(u8, trimmed, ' ') orelse return error.Malformed;
    if (!std.mem.eql(u8, trimmed[space..], " kB")) return error.Malformed;
    const kib = try numbers.unsigned(u64, trimmed[0..space]);
    return std.math.mul(u64, kib, 1024) catch error.Malformed;
}

/// MemTotal - MemAvailable, the "Used" figure; null when either is missing
/// or the kernel's estimate exceeds the total.
pub fn usedBytes(info: Meminfo) ?u64 {
    const total = info.mem_total_bytes orelse return null;
    const available = info.mem_available_bytes orelse return null;
    if (available > total) return null;
    return total - available;
}

/// The rows that partition MemTotal.
pub const Partition = struct {
    total_bytes: u64,
    /// AnonPages
    application_private_bytes: u64,
    /// Shmem: not reclaimable without swap
    ram_backed_files_bytes: u64,
    /// Cached - Shmem + Buffers
    file_cache_bytes: u64,
    /// max(SReclaimable, KReclaimable)
    kernel_reclaimable_bytes: u64,
    /// SUnreclaim + KernelStack + PageTables + SecPageTables + Percpu +
    /// VmallocUsed (approximate: VmallocUsed may overlap the others)
    kernel_fixed_bytes: u64,
    /// MemFree
    free_bytes: u64,
    /// total - sum of the rows when positive, else 0
    unaccounted_bytes: u64,
    /// sum of the rows - total when positive, else 0
    overlap_bytes: u64,
};

/// Null when a field the rows need is missing (or Cached < Shmem, which the
/// kernel never reports): the caller shows "unknown", not a row of zeros.
/// Fields newer kernels add or older ones drop count as 0 in the sums.
pub fn partition(info: Meminfo) ?Partition {
    const total = info.mem_total_bytes orelse return null;
    const free = info.mem_free_bytes orelse return null;
    const buffers = info.buffers_bytes orelse return null;
    const cached = info.cached_bytes orelse return null;
    const shmem = info.shmem_bytes orelse return null;
    const anon = info.anon_pages_bytes orelse return null;
    const slab_reclaimable = info.s_reclaimable_bytes orelse return null;
    const slab_unreclaimable = info.s_unreclaim_bytes orelse return null;
    const kernel_stack = info.kernel_stack_bytes orelse return null;
    const page_tables = info.page_tables_bytes orelse return null;
    if (cached < shmem) return null;

    const file_cache = cached - shmem +| buffers;
    const kernel_reclaimable = @max(slab_reclaimable, info.k_reclaimable_bytes orelse 0);
    const kernel_fixed = slab_unreclaimable +| kernel_stack +| page_tables +|
        (info.sec_page_tables_bytes orelse 0) +| (info.percpu_bytes orelse 0) +|
        (info.vmalloc_used_bytes orelse 0);
    const accounted = anon +| shmem +| file_cache +| kernel_reclaimable +| kernel_fixed +| free;

    return .{
        .total_bytes = total,
        .application_private_bytes = anon,
        .ram_backed_files_bytes = shmem,
        .file_cache_bytes = file_cache,
        .kernel_reclaimable_bytes = kernel_reclaimable,
        .kernel_fixed_bytes = kernel_fixed,
        .free_bytes = free,
        .unaccounted_bytes = if (accounted < total) total - accounted else 0,
        .overlap_bytes = if (accounted > total) accounted - total else 0,
    };
}

// ---- tests ----------------------------------------------------------

const testing = std.testing;

fn rowsSum(p: Partition) u64 {
    return p.application_private_bytes + p.ram_backed_files_bytes + p.file_cache_bytes +
        p.kernel_reclaimable_bytes + p.kernel_fixed_bytes + p.free_bytes + p.unaccounted_bytes;
}

fn readFixture(name: []const u8, buffer: []u8) ![]u8 {
    return std.Io.Dir.cwd().readFile(testing.io, name, buffer);
}

fn checkFixture(name: []const u8, expected: Partition) !void {
    var buffer: [8192]u8 = undefined;
    const info = try parse(try readFixture(name, &buffer));
    const p = partition(info) orelse return error.TestUnexpectedResult;

    // The same assertions as the C suite (src/tests/test-meminfo.c).
    try testing.expectEqual(info.mem_total_bytes.?, p.total_bytes);
    try testing.expectEqual(info.anon_pages_bytes.?, p.application_private_bytes);
    try testing.expectEqual(info.shmem_bytes.?, p.ram_backed_files_bytes);
    try testing.expectEqual(info.cached_bytes.? - info.shmem_bytes.? + info.buffers_bytes.?, p.file_cache_bytes);
    try testing.expect(p.kernel_reclaimable_bytes >= info.s_reclaimable_bytes.?);
    try testing.expectEqual(info.mem_free_bytes.?, p.free_bytes);
    try testing.expectEqual(@as(u64, 0), p.overlap_bytes);
    try testing.expectEqual(p.total_bytes, rowsSum(p));

    // Rows worked out by hand from the captured file (kB times 1024).
    try testing.expectEqual(expected, p);
}

test "fixture a: the rows sum to MemTotal and match the rows worked out by hand" {
    try checkFixture("testdata/system/meminfo-a.txt", .{
        .total_bytes = 32557452 * 1024,
        .application_private_bytes = 8083944 * 1024,
        .ram_backed_files_bytes = 4942948 * 1024,
        .file_cache_bytes = 5961964 * 1024, // 10422472 - 4942948 + 482440
        .kernel_reclaimable_bytes = 412436 * 1024,
        .kernel_fixed_bytes = 646392 * 1024, // 301912 + 35756 + 115036 + 5816 + 17536 + 170336
        .free_bytes = 11733512 * 1024,
        .unaccounted_bytes = 776256 * 1024,
        .overlap_bytes = 0,
    });
}

test "fixture b: the rows sum to MemTotal and match the rows worked out by hand" {
    try checkFixture("testdata/system/meminfo-b.txt", .{
        .total_bytes = 32557452 * 1024,
        .application_private_bytes = 8145856 * 1024,
        .ram_backed_files_bytes = 5010740 * 1024,
        .file_cache_bytes = 5961996 * 1024,
        .kernel_reclaimable_bytes = 412548 * 1024,
        .kernel_fixed_bytes = 646812 * 1024,
        .free_bytes = 11719544 * 1024,
        .unaccounted_bytes = 659956 * 1024,
        .overlap_bytes = 0,
    });
}

test "fixture a: used is MemTotal minus MemAvailable" {
    var buffer: [8192]u8 = undefined;
    const info = try parse(try readFixture("testdata/system/meminfo-a.txt", &buffer));
    try testing.expectEqual(@as(?u64, (32557452 - 17634120) * 1024), usedBytes(info));
}

test "values are bytes, not kB" {
    const info = try parse("MemTotal:       1000 kB\nMemAvailable:    250 kB\n");
    try testing.expectEqual(@as(?u64, 1000 * 1024), info.mem_total_bytes);
    try testing.expectEqual(@as(?u64, 250 * 1024), info.mem_available_bytes);
    try testing.expectEqual(@as(?u64, 750 * 1024), usedBytes(info));
}

test "a missing field is null, not zero, and the partition is unknown" {
    const info = try parse("MemTotal: 1000 kB\nMemFree: 0 kB\n");
    try testing.expectEqual(@as(?u64, 0), info.mem_free_bytes);
    try testing.expectEqual(@as(?u64, null), info.mem_available_bytes);
    try testing.expectEqual(@as(?u64, null), info.shmem_bytes);
    try testing.expectEqual(@as(?Partition, null), partition(info));
    try testing.expectEqual(@as(?u64, null), usedBytes(info));
}

test "overlapping rows are reported as overlap, never as a negative residue" {
    // The rows add up to more than MemTotal (VmallocUsed overlap).
    const info = try parse(
        "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 0 kB\nCached: 100 kB\nShmem: 50 kB\n" ++
            "AnonPages: 400 kB\nSReclaimable: 100 kB\nSUnreclaim: 100 kB\nKernelStack: 10 kB\n" ++
            "PageTables: 10 kB\nVmallocUsed: 300 kB\n",
    );
    const p = partition(info).?;
    try testing.expectEqual(@as(u64, 0), p.unaccounted_bytes);
    // 400 + 50 + 50 + 100 + 420 + 100 = 1120 kB against 1000 kB
    try testing.expectEqual(@as(u64, 120 * 1024), p.overlap_bytes);
}

test "KReclaimable wins when it is larger than SReclaimable" {
    const info = try parse(
        "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 0 kB\nCached: 100 kB\nShmem: 50 kB\n" ++
            "AnonPages: 100 kB\nSReclaimable: 20 kB\nKReclaimable: 80 kB\nSUnreclaim: 10 kB\n" ++
            "KernelStack: 1 kB\nPageTables: 1 kB\n",
    );
    try testing.expectEqual(@as(u64, 80 * 1024), partition(info).?.kernel_reclaimable_bytes);
}

test "Cached below Shmem is refused instead of wrapping around" {
    const info = try parse(
        "MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 0 kB\nCached: 10 kB\nShmem: 50 kB\n" ++
            "AnonPages: 100 kB\nSReclaimable: 20 kB\nSUnreclaim: 10 kB\nKernelStack: 1 kB\nPageTables: 1 kB\n",
    );
    try testing.expectEqual(@as(?Partition, null), partition(info));
}

test "a malformed known value, a wrong unit, an overflow or a cut-short file is an error" {
    try testing.expectError(error.Malformed, parse("MemTotal: notanumber kB\n"));
    try testing.expectError(error.Malformed, parse("MemTotal: 12 MB\n"));
    try testing.expectError(error.Malformed, parse("MemTotal: 12\n"));
    try testing.expectError(error.Malformed, parse("MemTotal: -12 kB\n"));
    try testing.expectError(error.Malformed, parse("MemTotal: 18446744073709551615 kB\n"));
    try testing.expectError(error.Malformed, parse("garbage without a colon\n"));
    try testing.expectError(error.Truncated, parse("MemTotal: 12 kB"));
    try testing.expectError(error.Truncated, parse(""));
}

test "keys the partition does not use may carry any value" {
    const info = try parse("HugePages_Total:       0\nHardwareCorrupted: weird\nMemTotal: 4 kB\n");
    try testing.expectEqual(@as(?u64, 4096), info.mem_total_bytes);
}

test "fuzz: arbitrary meminfo bytes never panic" {
    try testing.fuzz({}, struct {
        fn run(_: void, smith: *testing.Smith) anyerror!void {
            var input: [256]u8 = undefined;
            const length = smith.slice(&input);
            const info = parse(input[0..length]) catch return;
            _ = partition(info);
            _ = usedBytes(info);
        }
    }.run, .{});
}
