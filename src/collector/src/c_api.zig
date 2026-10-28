// SPDX-License-Identifier: GPL-2.0-or-later
//! The C interface of the collector (include/gsmcollect.h, version 1): a
//! collector handle that reuses its buffers, and immutable snapshots made of
//! plain structs. The types and the `known` bits come from the header itself
//! (translated at build time), so the Zig side and the C side cannot drift.
//!
//! Rules kept here:
//!  - memory is the library's: allocations use `std.heap.smp_allocator`, never
//!    the consumer's allocator and not libc's;
//!  - a field the kernel did not give is a clear `known` bit and a zero value;
//!  - a process or cgroup that vanished mid-read has no record and is counted;
//!  - errors are negative errno values, and one reader failing while another
//!    works is not an error of the sample (it is in `reader_errno`).
const std = @import("std");
const builtin = @import("builtin");
const Io = std.Io;
const linux = std.os.linux;
const c = @import("gsm_c");
const walk = @import("walk.zig");
const cgroup_tree = @import("cgroup_tree.zig");
const meminfo = @import("meminfo.zig");
const pressure_file = @import("pressure.zig");
const readfile = @import("readfile.zig");

const gpa = std.heap.smp_allocator;

// Stack probes. The readers keep large frames (a 16 KiB cgroup read buffer, a
// block sort's cache), and a worker thread's guard is one page, so a frame
// must touch each page on the way down or it can step over the guard into
// other memory. Zig calls `__zig_probe_stack` for that; it lives in Zig's
// compiler-rt, which the static library does not bundle (its weak symbols
// would show up as exports). So the library carries its own copy as a local
// symbol of its one object. The routine is Zig's lib/compiler_rt/stack_probe.zig
// (MIT licence), x86_64 only, which is the only target the library builds for.
// Zig's licence for that routine:
//
// The MIT License (Expat)
//
// Copyright (c) Zig contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
comptime {
    if (builtin.output_mode == .Lib and builtin.cpu.arch == .x86_64) {
        // Module-level assembly with a `.local` symbol: it stays in the
        // object, is found by the calls the compiler emits, and is not
        // exported.
        asm (
            \\        .text
            \\        .local  __zig_probe_stack
            \\        .type   __zig_probe_stack,@function
            \\__zig_probe_stack:
            \\        push   %rcx
            \\        mov    %rax, %rcx
            \\        cmp    $0x1000,%rcx
            \\        jb     2f
            \\ 1:
            \\        sub    $0x1000,%rsp
            \\        orl    $0,16(%rsp)
            \\        sub    $0x1000,%rcx
            \\        cmp    $0x1000,%rcx
            \\        ja     1b
            \\ 2:
            \\        sub    %rcx, %rsp
            \\        orl    $0,16(%rsp)
            \\        add    %rax,%rsp
            \\        pop    %rcx
            \\        ret
            \\        .size  __zig_probe_stack, .-__zig_probe_stack
        );
    }
}

const api_version: u32 = 1;
const max_root_bytes = 4095;
// A meminfo is about 1.5 KiB; a read that fills the buffer is refused.
const scratch_bytes = 8192;

const reader_processes = 0;
const reader_memory = 1;
const reader_pressure = 2;
const reader_cgroups = 3;

fn errnoValue(code: linux.E) i32 {
    return -@as(i32, @intCast(@intFromEnum(code)));
}

// ---- tables: one row per field, so a metric is data, not an if chain ----

const Row = struct { c_field: []const u8, zig_field: []const u8, bit: u64 };

fn row(comptime c_field: []const u8, comptime zig_field: []const u8, comptime bit: anytype) Row {
    return .{ .c_field = c_field, .zig_field = zig_field, .bit = @as(u64, bit) };
}

const status_rows = [_]Row{
    row("rss_anon_bytes", "rss_anon_bytes", c.GSM_PROCESS_HAS_RSS_ANON),
    row("rss_file_bytes", "rss_file_bytes", c.GSM_PROCESS_HAS_RSS_FILE),
    row("rss_shmem_bytes", "rss_shmem_bytes", c.GSM_PROCESS_HAS_RSS_SHMEM),
    row("vm_swap_bytes", "vm_swap_bytes", c.GSM_PROCESS_HAS_VM_SWAP),
    row("vm_size_bytes", "vm_size_bytes", c.GSM_PROCESS_HAS_VM_SIZE),
    row("uid_real", "uid_real", c.GSM_PROCESS_HAS_UID),
};

const schedstat_rows = [_]Row{
    row("run_ns", "run_ns", c.GSM_PROCESS_HAS_RUN_NS),
    row("run_delay_ns", "run_delay_ns", c.GSM_PROCESS_HAS_RUN_DELAY_NS),
    row("timeslices", "timeslices", c.GSM_PROCESS_HAS_TIMESLICES),
};

const meminfo_rows = [_]Row{
    row("mem_total_bytes", "mem_total_bytes", c.GSM_MEMORY_HAS_MEM_TOTAL),
    row("mem_free_bytes", "mem_free_bytes", c.GSM_MEMORY_HAS_MEM_FREE),
    row("mem_available_bytes", "mem_available_bytes", c.GSM_MEMORY_HAS_MEM_AVAILABLE),
    row("buffers_bytes", "buffers_bytes", c.GSM_MEMORY_HAS_BUFFERS),
    row("cached_bytes", "cached_bytes", c.GSM_MEMORY_HAS_CACHED),
    row("shmem_bytes", "shmem_bytes", c.GSM_MEMORY_HAS_SHMEM),
    row("anon_pages_bytes", "anon_pages_bytes", c.GSM_MEMORY_HAS_ANON_PAGES),
    row("s_reclaimable_bytes", "s_reclaimable_bytes", c.GSM_MEMORY_HAS_S_RECLAIMABLE),
    row("s_unreclaim_bytes", "s_unreclaim_bytes", c.GSM_MEMORY_HAS_S_UNRECLAIM),
    row("k_reclaimable_bytes", "k_reclaimable_bytes", c.GSM_MEMORY_HAS_K_RECLAIMABLE),
    row("kernel_stack_bytes", "kernel_stack_bytes", c.GSM_MEMORY_HAS_KERNEL_STACK),
    row("page_tables_bytes", "page_tables_bytes", c.GSM_MEMORY_HAS_PAGE_TABLES),
    row("sec_page_tables_bytes", "sec_page_tables_bytes", c.GSM_MEMORY_HAS_SEC_PAGE_TABLES),
    row("percpu_bytes", "percpu_bytes", c.GSM_MEMORY_HAS_PERCPU),
    row("vmalloc_used_bytes", "vmalloc_used_bytes", c.GSM_MEMORY_HAS_VMALLOC_USED),
    row("dirty_bytes", "dirty_bytes", c.GSM_MEMORY_HAS_DIRTY),
    row("writeback_bytes", "writeback_bytes", c.GSM_MEMORY_HAS_WRITEBACK),
};

const partition_rows = [_]Row{
    row("partition_total_bytes", "total_bytes", 0),
    row("partition_application_private_bytes", "application_private_bytes", 0),
    row("partition_ram_backed_files_bytes", "ram_backed_files_bytes", 0),
    row("partition_file_cache_bytes", "file_cache_bytes", 0),
    row("partition_kernel_reclaimable_bytes", "kernel_reclaimable_bytes", 0),
    row("partition_kernel_fixed_bytes", "kernel_fixed_bytes", 0),
    row("partition_free_bytes", "free_bytes", 0),
    row("partition_unaccounted_bytes", "unaccounted_bytes", 0),
    row("partition_overlap_bytes", "overlap_bytes", 0),
};

const memory_stat_rows = [_]Row{
    row("anon_bytes", "anon_bytes", c.GSM_CGROUP_HAS_ANON),
    row("file_bytes", "file_bytes", c.GSM_CGROUP_HAS_FILE),
    row("shmem_bytes", "shmem_bytes", c.GSM_CGROUP_HAS_SHMEM),
    row("kernel_bytes", "kernel_bytes", c.GSM_CGROUP_HAS_KERNEL),
    row("sock_bytes", "sock_bytes", c.GSM_CGROUP_HAS_SOCK),
    row("slab_reclaimable_bytes", "slab_reclaimable_bytes", c.GSM_CGROUP_HAS_SLAB_RECLAIMABLE),
    row("slab_unreclaimable_bytes", "slab_unreclaimable_bytes", c.GSM_CGROUP_HAS_SLAB_UNRECLAIMABLE),
    row("file_mapped_bytes", "file_mapped_bytes", c.GSM_CGROUP_HAS_FILE_MAPPED),
    row("file_dirty_bytes", "file_dirty_bytes", c.GSM_CGROUP_HAS_FILE_DIRTY),
    row("workingset_refault_anon", "workingset_refault_anon", c.GSM_CGROUP_HAS_WORKINGSET_REFAULT_ANON),
    row("workingset_refault_file", "workingset_refault_file", c.GSM_CGROUP_HAS_WORKINGSET_REFAULT_FILE),
    row("pgmajfault", "pgmajfault", c.GSM_CGROUP_HAS_PGMAJFAULT),
};

const memory_events_rows = [_]Row{
    row("events_high", "high", c.GSM_CGROUP_HAS_EVENTS_HIGH),
    row("events_max", "max", c.GSM_CGROUP_HAS_EVENTS_MAX),
    row("events_oom", "oom", c.GSM_CGROUP_HAS_EVENTS_OOM),
    row("events_oom_kill", "oom_kill", c.GSM_CGROUP_HAS_EVENTS_OOM_KILL),
};

const cpu_stat_rows = [_]Row{
    row("cpu_usage_usec", "usage_usec", c.GSM_CGROUP_HAS_CPU_USAGE_USEC),
    row("cpu_user_usec", "user_usec", c.GSM_CGROUP_HAS_CPU_USER_USEC),
    row("cpu_system_usec", "system_usec", c.GSM_CGROUP_HAS_CPU_SYSTEM_USEC),
    row("cpu_nr_throttled", "nr_throttled", c.GSM_CGROUP_HAS_CPU_NR_THROTTLED),
    row("cpu_throttled_usec", "throttled_usec", c.GSM_CGROUP_HAS_CPU_THROTTLED_USEC),
};

/// Copies every optional field of `source` named in `rows` into `out`,
/// setting its bit in `out.known`; a null field leaves its bit clear and its
/// value 0.
fn fill(out: anytype, source: anytype, comptime rows: []const Row) void {
    inline for (rows) |r| {
        if (@field(source, r.zig_field)) |value| {
            @field(out, r.c_field) = value;
            out.known |= r.bit;
        }
    }
}

/// The same for a source whose fields are not optional (all known together),
/// setting each row's bit.
fn copyKnown(out: anytype, source: anytype, comptime rows: []const Row) void {
    inline for (rows) |r| {
        @field(out, r.c_field) = @field(source, r.zig_field);
        out.known |= r.bit;
    }
}

/// The same without bits, for a record whose one bit covers every row.
fn copyAll(out: anytype, source: anytype, comptime rows: []const Row) void {
    inline for (rows) |r| @field(out, r.c_field) = @field(source, r.zig_field);
}

fn zeroed(comptime T: type) T {
    return std.mem.zeroes(T);
}

// ---- the snapshot ----

const Snapshot = struct {
    processes: []c.gsm_process = &.{},
    cgroups: []c.gsm_cgroup = &.{},
    memory: [1]c.gsm_memory = undefined,
    memory_count: usize = 0,
    pressure: [c.GSM_PRESSURE_RESOURCE_COUNT]c.gsm_pressure = undefined,
    pressure_count: usize = 0,
    strings: []u8 = &.{},
    stats: c.gsm_sample_stats = undefined,

    fn destroy(self: *Snapshot) void {
        gpa.free(self.processes);
        gpa.free(self.cgroups);
        gpa.free(self.strings);
        gpa.destroy(self);
    }
};

fn snapshotOf(pointer: *const c.gsm_snapshot) *const Snapshot {
    return @ptrCast(@alignCast(pointer));
}

// ---- the collector ----

const Collector = struct {
    threaded: Io.Threaded,
    readers: u32,
    procfs_root: []u8,
    cgroupfs_root: []u8,
    /// Read buffer for the files this file reads itself (meminfo, pressure),
    /// allocated once so no frame here holds a large array.
    scratch: []u8,
    /// Reused by every sample, so a steady process count allocates nothing.
    processes: walk.Snapshot,
    tree: cgroup_tree.Tree,
    /// Reused string interning: path -> offset in the sample's string table.
    interned: std.StringHashMapUnmanaged(u32) = .empty,

    fn io(self: *Collector) Io {
        return self.threaded.io();
    }

    fn destroy(self: *Collector) void {
        self.processes.deinit();
        self.tree.deinit();
        self.interned.deinit(gpa);
        gpa.free(self.procfs_root);
        gpa.free(self.cgroupfs_root);
        gpa.free(self.scratch);
        gpa.destroy(self);
    }
};

export fn gsm_collect_api_version() u32 {
    return api_version;
}

export fn gsm_collect_open(config: ?*const c.gsm_collect_config, out: ?*?*c.gsm_collector) c_int {
    const out_pointer = out orelse return errnoValue(.INVAL);
    out_pointer.* = null;

    var readers: u32 = c.GSM_READ_ALL;
    var procfs_root: []const u8 = "/proc";
    var cgroupfs_root: []const u8 = "/sys/fs/cgroup";
    if (config) |given| {
        if (given.struct_size < @sizeOf(c.gsm_collect_config)) return errnoValue(.INVAL);
        if (given.api_version > api_version) return errnoValue(.INVAL);
        if (given.readers & ~@as(u32, c.GSM_READ_ALL) != 0) return errnoValue(.INVAL);
        if (given.readers != 0) readers = given.readers;
        if (given.procfs_root) |text| procfs_root = std.mem.span(text);
        if (given.cgroupfs_root) |text| cgroupfs_root = std.mem.span(text);
    }
    if (procfs_root.len == 0 or procfs_root.len > max_root_bytes) return errnoValue(.INVAL);
    if (cgroupfs_root.len == 0 or cgroupfs_root.len > max_root_bytes) return errnoValue(.INVAL);

    const collector = gpa.create(Collector) catch return errnoValue(.NOMEM);
    const procfs_copy = gpa.dupe(u8, procfs_root) catch {
        gpa.destroy(collector);
        return errnoValue(.NOMEM);
    };
    const cgroupfs_copy = gpa.dupe(u8, cgroupfs_root) catch {
        gpa.free(procfs_copy);
        gpa.destroy(collector);
        return errnoValue(.NOMEM);
    };
    const scratch = gpa.alloc(u8, scratch_bytes) catch {
        gpa.free(cgroupfs_copy);
        gpa.free(procfs_copy);
        gpa.destroy(collector);
        return errnoValue(.NOMEM);
    };
    collector.* = .{
        .threaded = .init_single_threaded,
        .readers = readers,
        .procfs_root = procfs_copy,
        .cgroupfs_root = cgroupfs_copy,
        .scratch = scratch,
        .processes = .init(gpa),
        .tree = .init(gpa),
    };
    out_pointer.* = @ptrCast(collector);
    return 0;
}

export fn gsm_collect_new(config: ?*const c.gsm_collect_config) ?*c.gsm_collector {
    var collector: ?*c.gsm_collector = null;
    if (gsm_collect_open(config, &collector) != 0) return null;
    return collector;
}

export fn gsm_collect_free(handle: ?*c.gsm_collector) void {
    const collector: *Collector = @ptrCast(@alignCast(handle orelse return));
    collector.destroy();
}

export fn gsm_snapshot_free(handle: ?*c.gsm_snapshot) void {
    const snapshot: *Snapshot = @ptrCast(@alignCast(handle orelse return));
    snapshot.destroy();
}

export fn gsm_collect_strerror(err: c_int) [*:0]const u8 {
    const code: u32 = @intCast(@abs(err));
    const known = [_]struct { code: linux.E, text: [*:0]const u8 }{
        .{ .code = .NOMEM, .text = "out of memory" },
        .{ .code = .IO, .text = "the kernel's file was unreadable or malformed" },
        .{ .code = .NOENT, .text = "the directory or file does not exist" },
        .{ .code = .ACCES, .text = "permission denied" },
        .{ .code = .INVAL, .text = "invalid argument" },
        .{ .code = .RANGE, .text = "a value did not fit the interface" },
    };
    if (code == 0) return "success";
    for (known) |entry| if (@intFromEnum(entry.code) == code) return entry.text;
    return "unknown error";
}

fn nowNs() u64 {
    var ts: linux.timespec = undefined;
    if (linux.errno(linux.clock_gettime(.MONOTONIC, &ts)) != .SUCCESS) return 0;
    return @as(u64, @intCast(ts.sec)) * std.time.ns_per_s + @as(u64, @intCast(ts.nsec));
}

fn errnoOf(err: anyerror) i32 {
    return switch (err) {
        error.OutOfMemory => errnoValue(.NOMEM),
        error.FileNotFound, error.GroupNotFound => errnoValue(.NOENT),
        error.AccessDenied, error.PermissionDenied, error.CannotOpen => errnoValue(.ACCES),
        error.InvalidPath => errnoValue(.INVAL),
        else => errnoValue(.IO),
    };
}

fn wants(readers: u32, index: usize) bool {
    return readers & (@as(u32, 1) << @intCast(index)) != 0;
}

export fn gsm_collect_sample(handle: ?*c.gsm_collector, out: ?*?*c.gsm_snapshot) c_int {
    const collector: *Collector = @ptrCast(@alignCast(handle orelse return errnoValue(.INVAL)));
    const out_pointer = out orelse return errnoValue(.INVAL);
    out_pointer.* = null;

    const snapshot = gpa.create(Snapshot) catch return errnoValue(.NOMEM);
    snapshot.* = .{};
    const status = sampleInto(collector, snapshot) catch errnoValue(.NOMEM);
    if (status != 0) {
        snapshot.destroy();
        return status;
    }
    out_pointer.* = @ptrCast(snapshot);
    return 0;
}

/// Fills `snapshot`; returns 0, or the negative errno when every requested
/// reader failed. The caller destroys the snapshot on anything but 0.
fn sampleInto(collector: *Collector, snapshot: *Snapshot) error{OutOfMemory}!i32 {
    snapshot.stats = zeroed(c.gsm_sample_stats);
    snapshot.stats.struct_size = @sizeOf(c.gsm_sample_stats);
    snapshot.stats.readers_run = collector.readers;
    snapshot.stats.sampled_at_ns = nowNs();

    const io = collector.io();
    var failures: [c.GSM_READER_COUNT]i32 = @splat(0);
    var table: std.ArrayList(u8) = .empty;
    errdefer table.deinit(gpa);
    collector.interned.clearRetainingCapacity();

    if (wants(collector.readers, reader_memory)) failures[reader_memory] = readMemory(collector, io, snapshot);
    if (wants(collector.readers, reader_pressure)) failures[reader_pressure] = readPressure(collector, io, snapshot);
    // Cgroups before processes: a process's cgroup path is nearly always one
    // of the tree's paths, so interning finds it already in the table.
    if (wants(collector.readers, reader_cgroups)) failures[reader_cgroups] = try readCgroups(collector, io, snapshot, &table);
    if (wants(collector.readers, reader_processes)) failures[reader_processes] = try readProcesses(collector, io, snapshot, &table);
    snapshot.strings = try table.toOwnedSlice(gpa);
    snapshot.stats.reader_errno = failures;

    var requested: u32 = 0;
    var failed: u32 = 0;
    var first_failure: i32 = 0;
    for (failures, 0..) |failure, index| {
        if (!wants(collector.readers, index)) continue;
        requested += 1;
        if (failure == 0) continue;
        failed += 1;
        if (first_failure == 0) first_failure = failure;
    }
    // Every requested reader failing is an error; a partial sample is not.
    if (failed == requested and failed > 0) return first_failure;
    return 0;
}

// ---- readers: each returns 0 or the negative errno that stopped it ----

fn readMemory(collector: *Collector, io: Io, snapshot: *Snapshot) i32 {
    var dir = Io.Dir.cwd().openDir(io, collector.procfs_root, .{}) catch |err| return errnoOf(err);
    defer dir.close(io);

    const bytes = switch (readfile.read(io, dir, "meminfo", collector.scratch)) {
        .bytes => |text| text,
        .absent => return errnoValue(.NOENT),
        .unreadable => return errnoValue(.ACCES),
        .too_long => return errnoValue(.IO),
    };
    const info = meminfo.parse(bytes) catch return errnoValue(.IO);

    var record = zeroed(c.gsm_memory);
    record.struct_size = @sizeOf(c.gsm_memory);
    fill(&record, info, &meminfo_rows);
    if (meminfo.usedBytes(info)) |used| {
        record.used_bytes = used;
        record.known |= c.GSM_MEMORY_HAS_USED;
    }
    if (meminfo.partition(info)) |partition| {
        copyAll(&record, partition, &partition_rows);
        record.known |= c.GSM_MEMORY_HAS_PARTITION;
    }
    snapshot.memory[0] = record;
    snapshot.memory_count = 1;
    return 0;
}

const pressure_names = [c.GSM_PRESSURE_RESOURCE_COUNT][]const u8{ "cpu", "memory", "io" };

fn copyLine(line: pressure_file.Line) c.gsm_pressure_line {
    var out = zeroed(c.gsm_pressure_line);
    out.avg10_percent = line.avg10_percent;
    out.avg60_percent = line.avg60_percent;
    out.avg300_percent = line.avg300_percent;
    out.total_us = line.total_us;
    return out;
}

fn readPressure(collector: *Collector, io: Io, snapshot: *Snapshot) i32 {
    var dir = Io.Dir.cwd().openDir(io, collector.procfs_root, .{}) catch |err| return errnoOf(err);
    defer dir.close(io);

    var first_failure: i32 = 0;
    var failed: usize = 0;
    inline for (pressure_names, 0..) |name, resource| {
        var record = zeroed(c.gsm_pressure);
        record.struct_size = @sizeOf(c.gsm_pressure);
        record.resource = resource;
        const failure: i32 = blk: {
            const bytes = switch (readfile.read(io, dir, "pressure/" ++ name, collector.scratch)) {
                .bytes => |text| text,
                .absent => break :blk errnoValue(.NOENT),
                .unreadable => break :blk errnoValue(.ACCES),
                .too_long => break :blk errnoValue(.IO),
            };
            const parsed = pressure_file.parse(bytes) catch break :blk errnoValue(.IO);
            record.some = copyLine(parsed.some);
            record.known |= c.GSM_PRESSURE_HAS_SOME;
            if (parsed.full) |full| {
                record.full = copyLine(full);
                record.known |= c.GSM_PRESSURE_HAS_FULL;
            }
            break :blk 0;
        };
        if (failure != 0) {
            failed += 1;
            if (first_failure == 0) first_failure = failure;
        }
        snapshot.pressure[resource] = record;
    }
    snapshot.pressure_count = c.GSM_PRESSURE_RESOURCE_COUNT;
    // A kernel without PSI has none of the three files: the reader failed.
    // One missing file (cpu on a kernel before 5.13 has no `full`, but does
    // have the file) is just a record with known == 0.
    return if (failed == c.GSM_PRESSURE_RESOURCE_COUNT) first_failure else 0;
}

/// Offset of `text` in the string table, adding it (and a NUL) when new.
fn intern(collector: *Collector, table: *std.ArrayList(u8), text: []const u8) error{OutOfMemory}!struct { offset: u32, len: u32 } {
    const len: u32 = std.math.cast(u32, text.len) orelse return error.OutOfMemory;
    if (collector.interned.get(text)) |offset| return .{ .offset = offset, .len = len };
    const offset = std.math.cast(u32, table.items.len) orelse return error.OutOfMemory;
    try table.ensureUnusedCapacity(gpa, text.len + 1);
    table.appendSliceAssumeCapacity(text);
    table.appendAssumeCapacity(0);
    // The key is the walk's own copy of the text, alive until the next sample.
    try collector.interned.put(gpa, text, offset);
    return .{ .offset = offset, .len = len };
}

fn readCgroups(collector: *Collector, io: Io, snapshot: *Snapshot, table: *std.ArrayList(u8)) error{OutOfMemory}!i32 {
    var base = Io.Dir.cwd().openDir(io, collector.cgroupfs_root, .{}) catch |err| return errnoOf(err);
    defer base.close(io);

    collector.tree.walk(io, base, "/") catch |err| return errnoOf(err);
    const counts = collector.tree.counts;
    snapshot.stats.cgroups_vanished = counts.vanished;
    for (counts.unreadable) |n| snapshot.stats.cgroup_files_unreadable +|= n;
    for (counts.malformed) |n| snapshot.stats.cgroup_files_malformed +|= n;

    const groups = collector.tree.groups.items;
    const records = try gpa.alloc(c.gsm_cgroup, groups.len);
    snapshot.cgroups = records;
    for (groups, records) |group, *record| {
        record.* = zeroed(c.gsm_cgroup);
        record.struct_size = @sizeOf(c.gsm_cgroup);
        const path = try intern(collector, table, group.path);
        record.path_offset = path.offset;
        record.path_len = path.len;
        if (group.parent) |parent| {
            record.parent = parent;
            record.known |= c.GSM_CGROUP_HAS_PARENT;
        }
        fillCgroup(record, group);
    }
    return 0;
}

fn fillLimit(record: *c.gsm_cgroup, limit: ?@import("cgroup_files.zig").Limit, comptime field: []const u8, finite_bit: u64, unlimited_bit: u64) void {
    const value = limit orelse return;
    switch (value) {
        .unlimited => record.known |= unlimited_bit,
        .bytes => |bytes| {
            @field(record, field) = bytes;
            record.known |= finite_bit;
        },
    }
}

fn fillPressureLines(record: *c.gsm_cgroup, parsed: ?pressure_file.Pressure, comptime some: []const u8, comptime full: []const u8, some_bit: u64, full_bit: u64) void {
    const value = parsed orelse return;
    @field(record, some) = copyLine(value.some);
    record.known |= some_bit;
    if (value.full) |line| {
        @field(record, full) = copyLine(line);
        record.known |= full_bit;
    }
}

fn fillCgroup(record: *c.gsm_cgroup, group: cgroup_tree.Group) void {
    if (group.memory_current_bytes) |value| {
        record.memory_current_bytes = value;
        record.known |= c.GSM_CGROUP_HAS_MEMORY_CURRENT;
    }
    if (group.memory_peak_bytes) |value| {
        record.memory_peak_bytes = value;
        record.known |= c.GSM_CGROUP_HAS_MEMORY_PEAK;
    }
    fillLimit(record, group.memory_max, "memory_max_bytes", c.GSM_CGROUP_HAS_MEMORY_MAX, c.GSM_CGROUP_MEMORY_MAX_UNLIMITED);
    fillLimit(record, group.memory_high, "memory_high_bytes", c.GSM_CGROUP_HAS_MEMORY_HIGH, c.GSM_CGROUP_MEMORY_HIGH_UNLIMITED);
    if (group.memory_stat) |stat| fill(record, stat, &memory_stat_rows);
    if (group.memory_events) |events| fill(record, events, &memory_events_rows);
    if (group.cpu_stat) |stat| fill(record, stat, &cpu_stat_rows);
    if (group.pids_current) |value| {
        record.pids_current = value;
        record.known |= c.GSM_CGROUP_HAS_PIDS_CURRENT;
    }
    fillPressureLines(record, group.memory_pressure, "memory_pressure_some", "memory_pressure_full", c.GSM_CGROUP_HAS_MEMORY_PRESSURE_SOME, c.GSM_CGROUP_HAS_MEMORY_PRESSURE_FULL);
    fillPressureLines(record, group.cpu_pressure, "cpu_pressure_some", "cpu_pressure_full", c.GSM_CGROUP_HAS_CPU_PRESSURE_SOME, c.GSM_CGROUP_HAS_CPU_PRESSURE_FULL);
    fillPressureLines(record, group.io_pressure, "io_pressure_some", "io_pressure_full", c.GSM_CGROUP_HAS_IO_PRESSURE_SOME, c.GSM_CGROUP_HAS_IO_PRESSURE_FULL);
}

fn readProcesses(collector: *Collector, io: Io, snapshot: *Snapshot, table: *std.ArrayList(u8)) error{OutOfMemory}!i32 {
    var root = Io.Dir.cwd().openDir(io, collector.procfs_root, .{ .iterate = true }) catch |err| return errnoOf(err);
    defer root.close(io);

    collector.processes.walk(io, root) catch |err| return errnoOf(err);
    const counts = collector.processes.counts;
    snapshot.stats.processes_exited = counts.exited;
    for (counts.unreadable) |n| snapshot.stats.process_files_unreadable +|= n;
    for (counts.malformed) |n| snapshot.stats.process_files_malformed +|= n;

    const listed = collector.processes.processes.items;
    const records = try gpa.alloc(c.gsm_process, listed.len);
    snapshot.processes = records;
    for (listed, records) |process, *record| {
        record.* = zeroed(c.gsm_process);
        record.struct_size = @sizeOf(c.gsm_process);
        record.pid = process.pid;
        record.start_ticks = process.start_ticks;
        record.ppid = process.ppid;
        record.pgrp = process.pgrp;
        record.session = process.session;
        record.tty_nr = process.tty_nr;
        record.flags = process.flags;
        record.num_threads = process.num_threads;
        record.utime_ticks = process.utime_ticks;
        record.stime_ticks = process.stime_ticks;
        record.state = process.state;
        record.comm_len = process.comm_len;
        // comm_bytes is zero-filled past comm_len, so the buffer is a C string.
        @memcpy(std.mem.asBytes(&record.comm)[0..process.comm_len], process.comm());
        if (process.status) |status| fill(record, status, &status_rows);
        if (process.schedstat) |schedstat| copyKnown(record, schedstat, &schedstat_rows);
        if (process.cgroup_path) |path| {
            const interned = try intern(collector, table, path);
            record.cgroup_offset = interned.offset;
            record.cgroup_len = interned.len;
            record.known |= c.GSM_PROCESS_HAS_CGROUP;
        }
    }
    return 0;
}

// ---- accessors ----

export fn gsm_snapshot_processes(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_process {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.processes.len;
    return if (snapshot.processes.len == 0) null else snapshot.processes.ptr;
}

export fn gsm_snapshot_cgroups(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_cgroup {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.cgroups.len;
    return if (snapshot.cgroups.len == 0) null else snapshot.cgroups.ptr;
}

export fn gsm_snapshot_memory(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_memory {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.memory_count;
    return if (snapshot.memory_count == 0) null else &snapshot.memory;
}

export fn gsm_snapshot_pressure(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_pressure {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.pressure_count;
    return if (snapshot.pressure_count == 0) null else &snapshot.pressure;
}

export fn gsm_snapshot_strings(handle: *const c.gsm_snapshot, length: *usize) [*]const u8 {
    const snapshot = snapshotOf(handle);
    length.* = snapshot.strings.len;
    return snapshot.strings.ptr;
}

export fn gsm_snapshot_stats(handle: *const c.gsm_snapshot) *const c.gsm_sample_stats {
    return &snapshotOf(handle).stats;
}

// ---- tests, over the fixture trees ----

const testing = std.testing;
const fixture_proc = "tests/c/fixture/proc";
const fixture_cgroup = "testdata/cgroup";

fn fixtureCollector(readers: u32) ?*c.gsm_collector {
    var config = zeroed(c.gsm_collect_config);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.api_version = c.GSM_COLLECT_API_VERSION;
    config.readers = readers;
    config.procfs_root = fixture_proc;
    config.cgroupfs_root = fixture_cgroup;
    return gsm_collect_new(&config);
}

fn processFor(snapshot: *c.gsm_snapshot, pid: u32) !*const c.gsm_process {
    var count: usize = 0;
    const records = gsm_snapshot_processes(snapshot, &count).?[0..count];
    for (records) |*record| if (record.pid == pid) return record;
    return error.TestExpectedEqual;
}

fn pathOf(snapshot: *c.gsm_snapshot, offset: u32, len: u32) []const u8 {
    var length: usize = 0;
    const base = gsm_snapshot_strings(snapshot, &length);
    return base[offset..][0..len];
}

test "a snapshot over the fixtures lists every process, with a known process field for field" {
    const collector = fixtureCollector(0).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    _ = gsm_snapshot_processes(snapshot.?, &count);
    try testing.expectEqual(@as(usize, 5), count);

    const sleeper = try processFor(snapshot.?, 344260);
    try testing.expectEqual(@as(u32, @sizeOf(c.gsm_process)), sleeper.struct_size);
    try testing.expectEqualStrings("sleep", std.mem.sliceTo(&sleeper.comm, 0));
    try testing.expectEqual(@as(u64, 484977), sleeper.start_ticks);
    try testing.expectEqual(@as(u64, 112 * 1024), sleeper.rss_anon_bytes);
    try testing.expectEqual(@as(u32, 1000), sleeper.uid_real);
    try testing.expectEqual(@as(u64, 482125), sleeper.run_ns);
    const all_known = c.GSM_PROCESS_HAS_RSS_ANON | c.GSM_PROCESS_HAS_UID | c.GSM_PROCESS_HAS_RUN_NS | c.GSM_PROCESS_HAS_CGROUP;
    try testing.expectEqual(@as(u64, all_known), sleeper.known & all_known);
    try testing.expectEqualStrings(
        "/user.slice/user-1000.slice/user@1000.service/app.slice/app-com.visualstudio.CodeOSS-4247.scope",
        pathOf(snapshot.?, sleeper.cgroup_offset, sleeper.cgroup_len),
    );
}

test "a kernel thread's resident memory is unknown, not zero" {
    const collector = fixtureCollector(c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    const kthreadd = try processFor(snapshot.?, 2);
    try testing.expect(kthreadd.flags & c.GSM_PF_KTHREAD != 0);
    try testing.expectEqual(@as(u64, 0), kthreadd.known & c.GSM_PROCESS_HAS_RSS_ANON);
    try testing.expectEqual(@as(u64, 0), kthreadd.rss_anon_bytes);
    try testing.expect(kthreadd.known & c.GSM_PROCESS_HAS_CGROUP != 0);
}

test "memory, pressure and the cgroup tree come through with their unknowns clear" {
    const collector = fixtureCollector(0).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    const memory = gsm_snapshot_memory(snapshot.?, &count).?[0];
    try testing.expectEqual(@as(usize, 1), count);
    try testing.expectEqual(@as(u64, 32557452 * 1024), memory.mem_total_bytes);
    try testing.expect(memory.known & c.GSM_MEMORY_HAS_PARTITION != 0);
    try testing.expectEqual(memory.mem_total_bytes - memory.mem_available_bytes, memory.used_bytes);

    const pressure = gsm_snapshot_pressure(snapshot.?, &count).?[0..3];
    try testing.expectEqual(@as(usize, 3), count);
    try testing.expectEqual(@as(u64, 74156353), pressure[c.GSM_PRESSURE_CPU].some.total_us);
    try testing.expectEqual(@as(u64, 63114656), pressure[c.GSM_PRESSURE_MEMORY].some.total_us);

    const groups = gsm_snapshot_cgroups(snapshot.?, &count).?[0..count];
    try testing.expectEqual(@as(usize, 11), count);
    try testing.expectEqualStrings("/", pathOf(snapshot.?, groups[0].path_offset, groups[0].path_len));
    try testing.expectEqual(@as(u64, 0), groups[0].known & c.GSM_CGROUP_HAS_PARENT);
    // The root has no memory.current: unknown, not zero.
    try testing.expectEqual(@as(u64, 0), groups[0].known & c.GSM_CGROUP_HAS_MEMORY_CURRENT);
    for (groups[1..]) |group| try testing.expect(group.known & c.GSM_CGROUP_HAS_PARENT != 0);
}

test "a process cgroup path that is also a cgroup shares its string" {
    const collector = fixtureCollector(0).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    const groups = gsm_snapshot_cgroups(snapshot.?, &count).?[0..count];
    const init_process = try processFor(snapshot.?, 2);
    // pid 2 sits in "/", the root group: same offset, one string.
    try testing.expectEqual(groups[0].path_offset, init_process.cgroup_offset);
}

test "a missing root fails the reader, and a sample of only that reader is an error" {
    var config = zeroed(c.gsm_collect_config);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.readers = c.GSM_READ_PROCESSES;
    config.procfs_root = "tests/c/fixture/no-such-root";
    const collector = gsm_collect_new(&config).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, -@as(c_int, @intFromEnum(linux.E.NOENT))), gsm_collect_sample(collector, &snapshot));
    try testing.expect(snapshot == null);
}

test "one reader failing leaves the others' records and names its errno" {
    var config = zeroed(c.gsm_collect_config);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.procfs_root = fixture_proc;
    config.cgroupfs_root = "testdata/no-such-cgroupfs";
    const collector = gsm_collect_new(&config).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    _ = gsm_snapshot_cgroups(snapshot.?, &count);
    try testing.expectEqual(@as(usize, 0), count);
    const stats = gsm_snapshot_stats(snapshot.?);
    try testing.expectEqual(-@as(i32, @intFromEnum(linux.E.NOENT)), stats.reader_errno[reader_cgroups]);
    try testing.expectEqual(@as(i32, 0), stats.reader_errno[reader_processes]);
}

test "an invalid configuration gives no collector" {
    var config = zeroed(c.gsm_collect_config);
    try testing.expect(gsm_collect_new(&config) == null); // struct_size 0
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.api_version = c.GSM_COLLECT_API_VERSION + 1;
    try testing.expect(gsm_collect_new(&config) == null);
    config.api_version = 1;
    config.readers = 0x10;
    try testing.expect(gsm_collect_new(&config) == null);
    config.readers = 0;
    const defaults = gsm_collect_new(null);
    try testing.expect(defaults != null);
    gsm_collect_free(defaults);
}

test "strerror names the errno values the library returns" {
    try testing.expectEqualStrings("out of memory", std.mem.span(gsm_collect_strerror(-@as(c_int, @intFromEnum(linux.E.NOMEM)))));
    try testing.expectEqualStrings("success", std.mem.span(gsm_collect_strerror(0)));
    try testing.expectEqualStrings("unknown error", std.mem.span(gsm_collect_strerror(-9999)));
}

test "the rate and share helpers say unknown for an empty or backwards window" {
    var value: f64 = -1;
    try testing.expectEqual(@as(c_int, 1), c.gsm_rate_per_s(100, 300, 1_000_000_000, 3_000_000_000, &value));
    try testing.expectEqual(@as(f64, 100.0), value);
    try testing.expectEqual(@as(c_int, 0), c.gsm_rate_per_s(300, 100, 1, 2_000_000_000, &value));
    try testing.expectEqual(@as(c_int, 0), c.gsm_rate_per_s(1, 2, 5, 5, &value));
    // 500 ms stalled in a 2 s window is 25 %, the same arithmetic as pressure.stallPercent.
    try testing.expectEqual(@as(c_int, 1), c.gsm_pressure_share(0, 500_000, 0, 2_000_000_000, &value));
    try testing.expectEqual(@as(f64, 25.0), value);
    const line: pressure_file.Line = .{ .avg10_percent = 0, .avg60_percent = 0, .avg300_percent = 0, .total_us = 0 };
    var later = line;
    later.total_us = 500_000;
    try testing.expectEqual(pressure_file.stallPercent(line, later, 2_000_000).?, value);
    try testing.expectEqual(@as(c_int, 0), c.gsm_pressure_share(9, 3, 0, 1, &value));
}

test "two samples from one collector reuse its buffers and keep identities" {
    const collector = fixtureCollector(c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var first: ?*c.gsm_snapshot = null;
    var second: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &first));
    defer gsm_snapshot_free(first);
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &second));
    defer gsm_snapshot_free(second);
    const a = try processFor(first.?, 344260);
    const b = try processFor(second.?, 344260);
    try testing.expectEqual(a.start_ticks, b.start_ticks);
    try testing.expect(gsm_snapshot_stats(second.?).sampled_at_ns >= gsm_snapshot_stats(first.?).sampled_at_ns);
}

test "gsm_collect_open says why it failed, and gsm_collect_new is the same without the reason" {
    var collector: ?*c.gsm_collector = undefined;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_open(null, &collector));
    try testing.expect(collector != null);
    gsm_collect_free(collector);

    var config = zeroed(c.gsm_collect_config);
    try testing.expectEqual(errnoValue(.INVAL), gsm_collect_open(&config, &collector)); // struct_size 0
    try testing.expect(collector == null);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.api_version = c.GSM_COLLECT_API_VERSION + 1;
    try testing.expectEqual(errnoValue(.INVAL), gsm_collect_open(&config, &collector));
    try testing.expectEqual(errnoValue(.INVAL), gsm_collect_open(null, null));
    try testing.expect(gsm_collect_new(&config) == null);
}

test "no function here keeps a large array on the stack" {
    // Frames over a page can step over a thread's one-page guard when stack
    // probes are off (build.zig), so buffers live in the collector.
    const source = @embedFile("c_api.zig");
    const production = source[0..std.mem.indexOf(u8, source, "// ---- tests").?];
    var rest: []const u8 = production;
    while (std.mem.indexOfScalar(u8, rest, '[')) |open| {
        rest = rest[open + 1 ..];
        const close = std.mem.indexOfScalar(u8, rest, ']') orelse break;
        const size = std.fmt.parseInt(usize, rest[0..close], 10) catch continue;
        if (!std.mem.startsWith(u8, rest[close + 1 ..], "u8")) continue;
        try testing.expect(size <= 1024);
    }
}
