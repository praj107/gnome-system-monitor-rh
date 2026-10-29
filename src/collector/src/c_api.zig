// SPDX-License-Identifier: GPL-2.0-or-later
//! The C interface of the collector (include/gsmcollect.h, layout 3): a
//! collector handle that reuses its buffers, and immutable snapshots made of
//! plain structs. The types and the `known` bits come from the header itself
//! (translated at build time), so the Zig side and the C side cannot drift.
//!
//! Rules kept here:
//!  - memory is the library's: allocations use `std.heap.smp_allocator`, never
//!    the consumer's allocator and not libc's;
//!  - a field the kernel did not give is a clear `known` bit and a zero value;
//!  - a process or cgroup that vanished mid-read has no record and is counted;
//!  - `GSM_COLLECT_API_VERSION` counts record layouts and a layout only appends
//!    to the one before; a consumer that opens with an older `api_version`
//!    gets records at that layout's size and stride, holding that layout's
//!    fields only (the full record is built, its first `size` bytes handed over);
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
const classify = @import("classify.zig");
const unit_path = @import("unit_path.zig");

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

const api_version: u32 = c.GSM_COLLECT_API_VERSION;
const max_root_bytes = 4095;
// A meminfo is about 1.5 KiB; a read that fills the buffer is refused.
const scratch_bytes = 8192;

/// The `known` bits layout 3 added to a process record.
const process_layout3_bits: u64 = c.GSM_PROCESS_HAS_CLASS | c.GSM_PROCESS_HAS_UNIT | c.GSM_PROCESS_HAS_USER_UNIT |
    c.GSM_PROCESS_HAS_SESSION | c.GSM_PROCESS_HAS_OWNER_UID | c.GSM_PROCESS_HAS_AGENT;

/// The sizes of the records that differ between layouts, and the `known`
/// bits a layout has. A layout only appends, so the size of layout N is the
/// offset of the first field layout N+1 appended (read from the header, so it
/// cannot drift); memory and pressure records are the same in every layout and
/// use `@sizeOf`.
const Layout = struct {
    /// Where the last field of the layout ends, and the record size: that end
    /// rounded up to the record's alignment (layout 1 ends at 212 and is 216
    /// bytes, the last 4 being padding).
    process_fields_bytes: u32,
    process_bytes: u32,
    /// A cgroup record's layout-1 and layout-2 size is where layout 3's first
    /// field starts: the previous record ended on an 8-byte boundary.
    cgroup_bytes: u32,
    stats_bytes: u32,
    process_known_mask: u64,
    cgroup_known_mask: u64,
};

fn makeLayout(
    comptime process_fields_bytes: u32,
    comptime cgroup_bytes: u32,
    comptime stats_bytes: u32,
    comptime process_mask: u64,
    comptime cgroup_mask: u64,
) Layout {
    std.debug.assert(cgroup_bytes % @alignOf(c.gsm_cgroup) == 0);
    return .{
        .process_fields_bytes = process_fields_bytes,
        .process_bytes = std.mem.alignForward(u32, process_fields_bytes, @alignOf(c.gsm_process)),
        .cgroup_bytes = cgroup_bytes,
        .stats_bytes = stats_bytes,
        .process_known_mask = process_mask,
        .cgroup_known_mask = cgroup_mask,
    };
}

const layouts = [api_version]Layout{
    makeLayout(
        @offsetOf(c.gsm_process, "priority"),
        @offsetOf(c.gsm_cgroup, "workload_class"),
        @offsetOf(c.gsm_sample_stats, "process_io_unreadable"),
        ~(@as(u64, c.GSM_PROCESS_HAS_IO) | process_layout3_bits),
        ~@as(u64, c.GSM_CGROUP_HAS_CLASS),
    ),
    makeLayout(
        @offsetOf(c.gsm_process, "workload_class"),
        @offsetOf(c.gsm_cgroup, "workload_class"),
        @sizeOf(c.gsm_sample_stats),
        ~process_layout3_bits,
        ~@as(u64, c.GSM_CGROUP_HAS_CLASS),
    ),
    makeLayout(@sizeOf(c.gsm_process), @sizeOf(c.gsm_cgroup), @sizeOf(c.gsm_sample_stats), ~@as(u64, 0), ~@as(u64, 0)),
};

fn layoutOf(requested_version: u32) Layout {
    return layouts[requested_version - 1];
}

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

const io_rows = [_]Row{
    row("io_rchar_bytes", "rchar_bytes", c.GSM_PROCESS_HAS_IO),
    row("io_wchar_bytes", "wchar_bytes", c.GSM_PROCESS_HAS_IO),
    row("io_syscr", "syscr", c.GSM_PROCESS_HAS_IO),
    row("io_syscw", "syscw", c.GSM_PROCESS_HAS_IO),
    row("io_read_bytes", "read_bytes", c.GSM_PROCESS_HAS_IO),
    row("io_write_bytes", "write_bytes", c.GSM_PROCESS_HAS_IO),
    row("io_cancelled_write_bytes", "cancelled_write_bytes", c.GSM_PROCESS_HAS_IO),
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
    /// Process records at the consumer's layout: `processes_count` records
    /// of `process_stride` bytes each.
    processes: []align(@alignOf(c.gsm_process)) u8 = &.{},
    processes_count: usize = 0,
    /// Cgroup records at the consumer's layout, like `processes`.
    cgroups: []align(@alignOf(c.gsm_cgroup)) u8 = &.{},
    cgroups_count: usize = 0,
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
    /// The record layout the consumer asked for (1 to `api_version`).
    layout: u32,
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
    /// Reused by every sample: the class of each process of the walk, in order.
    classes: std.ArrayList(classify.Classification) = .empty,

    fn io(self: *Collector) Io {
        return self.threaded.io();
    }

    fn destroy(self: *Collector) void {
        self.processes.deinit();
        self.tree.deinit();
        self.interned.deinit(gpa);
        self.classes.deinit(gpa);
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
    var layout: u32 = api_version;
    var procfs_root: []const u8 = "/proc";
    var cgroupfs_root: []const u8 = "/sys/fs/cgroup";
    if (config) |given| {
        if (given.struct_size < @sizeOf(c.gsm_collect_config)) return errnoValue(.INVAL);
        if (given.api_version > api_version) return errnoValue(.INVAL);
        if (given.api_version != 0) layout = given.api_version;
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
        .layout = layout,
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
    snapshot.stats.struct_size = layoutOf(collector.layout).stats_bytes;
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
    const layout = layoutOf(collector.layout);
    const stride: usize = layout.cgroup_bytes;
    const bytes = try gpa.alignedAlloc(u8, .of(c.gsm_cgroup), groups.len * stride);
    snapshot.cgroups = bytes;
    snapshot.cgroups_count = groups.len;
    for (groups, 0..) |group, index| {
        // As for processes: the full record is built, its first `stride` bytes kept.
        var full = zeroed(c.gsm_cgroup);
        const record = &full;
        defer @memcpy(bytes[index * stride ..][0..stride], std.mem.asBytes(&full)[0..stride]);
        record.struct_size = layout.cgroup_bytes;
        const path = try intern(collector, table, group.path);
        record.path_offset = path.offset;
        record.path_len = path.len;
        if (group.parent) |parent| {
            record.parent = parent;
            record.known |= c.GSM_CGROUP_HAS_PARENT;
        }
        fillCgroup(record, group);
        if (collector.layout >= 3) fillCgroupClass(record, group.path, classify.classifyPath(group.path));
        record.known &= layout.cgroup_known_mask;
        @memset(std.mem.asBytes(&full)[layout.cgroup_bytes..stride], 0);
    }
    return 0;
}

/// Where `part` sits in the string table, given that `whole` was interned at
/// `whole_offset`: an (offset, length) pair, or null when `part` is not a
/// slice of `whole` (a key that is a process's comm, say). An empty `part` is
/// the empty string (0, 0).
const Located = struct { offset: u32, length: u32 };

fn locate(whole: []const u8, whole_offset: u32, part: []const u8) ?Located {
    if (part.len == 0) return .{ .offset = 0, .length = 0 };
    const start = @intFromPtr(part.ptr);
    const base = @intFromPtr(whole.ptr);
    if (start < base or start - base + part.len > whole.len) return null;
    return .{
        .offset = whole_offset + @as(u32, @intCast(start - base)),
        .length = @intCast(part.len),
    };
}

fn fillCgroupClass(record: *c.gsm_cgroup, path: []const u8, class: classify.Classification) void {
    record.workload_class = if (class.mixed) c.GSM_CLASS_MIXED else @intFromEnum(class.class);
    record.class_rule = class.rule;
    record.class_confidence = @intFromEnum(class.confidence);
    record.known |= c.GSM_CGROUP_HAS_CLASS;
    if (locate(path, record.path_offset, class.key)) |key| {
        record.class_key_offset = key.offset;
        record.class_key_length = key.length;
    }
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
    const layout = layoutOf(collector.layout);
    snapshot.stats.processes_exited = counts.exited;
    // A refused or absent `io` is the normal case for other users' processes:
    // it has its own counter (layout 2) and is not "something unexpected".
    for (counts.unreadable, 0..) |n, file| {
        if (file == @intFromEnum(walk.File.io)) {
            if (collector.layout >= 2) snapshot.stats.process_io_unreadable = n;
            continue;
        }
        snapshot.stats.process_files_unreadable +|= n;
    }
    for (counts.malformed) |n| snapshot.stats.process_files_malformed +|= n;

    const listed = collector.processes.processes.items;
    // Classification is pure over what the walk read: no syscall. Only a
    // layout-3 consumer has the fields, so older consumers do not pay for it.
    if (collector.layout >= 3) {
        try collector.classes.resize(gpa, listed.len);
        classify.classifyProcesses(gpa, listed, collector.classes.items) catch |err| switch (err) {
            error.OutOfMemory => return error.OutOfMemory,
            error.LengthMismatch => return errnoValue(.IO),
        };
    }
    const stride: usize = layout.process_bytes;
    const bytes = try gpa.alignedAlloc(u8, .of(c.gsm_process), listed.len * stride);
    snapshot.processes = bytes;
    snapshot.processes_count = listed.len;
    for (listed, 0..) |process, index| {
        // The full record is built, then its first `stride` bytes are kept:
        // an older layout is a prefix of a newer one.
        var full = zeroed(c.gsm_process);
        const record = &full;
        defer @memcpy(bytes[index * stride ..][0..stride], std.mem.asBytes(&full)[0..stride]);
        record.struct_size = layout.process_bytes;
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
        record.priority = process.priority;
        record.nice = process.nice;
        record.state = process.state;
        record.comm_len = process.comm_len;
        // comm_bytes is zero-filled past comm_len, so the buffer is a C string.
        @memcpy(std.mem.asBytes(&record.comm)[0..process.comm_len], process.comm());
        if (process.status) |status| {
            fill(record, status, &status_rows);
            if (lacksAddressSpace(&process)) record.known |= address_space_bits;
        }
        if (process.schedstat) |schedstat| copyKnown(record, schedstat, &schedstat_rows);
        if (process.io) |counters| copyKnown(record, counters, &io_rows);
        if (process.cgroup_path) |path| {
            const interned = try intern(collector, table, path);
            record.cgroup_offset = interned.offset;
            record.cgroup_len = interned.len;
            record.known |= c.GSM_PROCESS_HAS_CGROUP;
        }
        if (collector.layout >= 3) fillProcessClass(record, &process, collector.classes.items[index]);
        record.known &= layout.process_known_mask;
        // Bytes past the layout's last field are padding: hand them over zero.
        @memset(std.mem.asBytes(&full)[layout.process_fields_bytes..stride], 0);
    }
    return 0;
}

/// The five memory figures of a status file; absent for a task with no mm.
const address_space_bits: u64 = c.GSM_PROCESS_HAS_RSS_ANON | c.GSM_PROCESS_HAS_RSS_FILE | c.GSM_PROCESS_HAS_RSS_SHMEM |
    c.GSM_PROCESS_HAS_VM_SWAP | c.GSM_PROCESS_HAS_VM_SIZE;

/// A kernel thread, or a task that has exited and is waiting to be reaped
/// (state Z) or is dead (X), has no user address space: the kernel prints no
/// Vm* or Rss* line for it (proc_pid_status(5)), so its resident memory is
/// really zero. Whether the file simply lacked the lines for another reason
/// is decided by the caller, which asks only after a successful parse.
fn lacksAddressSpace(process: *const walk.Process) bool {
    return process.isKernelThread() or process.state == 'Z' or process.state == 'X';
}

fn fillProcessClass(record: *c.gsm_process, process: *const walk.Process, class: classify.Classification) void {
    record.workload_class = @intFromEnum(class.class);
    record.class_rule = class.rule;
    record.class_confidence = @intFromEnum(class.confidence);
    record.known |= c.GSM_PROCESS_HAS_CLASS;
    if (class.key_kind == .agent_process) {
        record.agent_pid = class.agent_pid;
        record.agent_start_ticks = class.agent_start_ticks;
        record.known |= c.GSM_PROCESS_HAS_AGENT;
    }
    // Everything else is a slice of the cgroup path already in the table.
    const path = process.cgroup_path orelse return;
    const path_offset = record.cgroup_offset;
    if (locate(path, path_offset, class.key)) |key| {
        record.class_key_offset = key.offset;
        record.class_key_length = key.length;
    }
    if (unit_path.unit(path)) |name| if (locate(path, path_offset, name)) |at| {
        record.unit_offset = at.offset;
        record.unit_length = at.length;
        record.known |= c.GSM_PROCESS_HAS_UNIT;
    };
    if (unit_path.userUnit(path)) |name| if (locate(path, path_offset, name)) |at| {
        record.user_unit_offset = at.offset;
        record.user_unit_length = at.length;
        record.known |= c.GSM_PROCESS_HAS_USER_UNIT;
    };
    if (unit_path.session(path)) |name| if (locate(path, path_offset, name)) |at| {
        record.login_session_offset = at.offset;
        record.login_session_length = at.length;
        record.known |= c.GSM_PROCESS_HAS_SESSION;
    };
    if (unit_path.ownerUid(path)) |uid| {
        record.owner_uid = uid;
        record.known |= c.GSM_PROCESS_HAS_OWNER_UID;
    }
}

// ---- names, built from the classifier's tables ----

/// NUL-terminated copies of `texts`, made at compile time, for the C side.
fn terminated(comptime texts: []const []const u8) [texts.len][*:0]const u8 {
    comptime {
        var out: [texts.len][*:0]const u8 = undefined;
        for (texts, 0..) |text, index| out[index] = (text ++ "\x00")[0..text.len :0];
        return out;
    }
}

fn ruleTexts() [classify.rules.len][]const u8 {
    comptime {
        var out: [classify.rules.len][]const u8 = undefined;
        for (classify.rules, 0..) |rule, index| out[index] = rule.name;
        return out;
    }
}

const class_name_table = terminated(&classify.class_labels);
const confidence_name_table = terminated(&classify.confidence_names);
const rule_name_table = terminated(&ruleTexts());

// The header's constants and the classifier's tables must agree, or the
// build stops: a class added on one side only cannot ship.
comptime {
    const Class = classify.Class;
    const class_pairs = .{
        .{ c.GSM_CLASS_KERNEL, Class.kernel },
        .{ c.GSM_CLASS_INIT, Class.init },
        .{ c.GSM_CLASS_SYSTEM_SERVICES, Class.system_services },
        .{ c.GSM_CLASS_SESSION_SERVICES, Class.session_services },
        .{ c.GSM_CLASS_APPLICATIONS, Class.applications },
        .{ c.GSM_CLASS_LOGIN_SESSION, Class.login_session },
        .{ c.GSM_CLASS_AGENT_SESSIONS, Class.agent_sessions },
        .{ c.GSM_CLASS_CONTAINERS_AND_VMS, Class.containers_and_vms },
        .{ c.GSM_CLASS_UNCLASSIFIED, Class.unclassified },
    };
    for (class_pairs) |pair| {
        if (pair[0] != @intFromEnum(pair[1])) @compileError("a GSM_CLASS_* value differs from classify.Class");
    }
    if (c.GSM_CLASS_COUNT != class_pairs.len or c.GSM_CLASS_COUNT != @typeInfo(Class).@"enum".fields.len)
        @compileError("GSM_CLASS_COUNT differs from classify.Class");
    if (c.GSM_CLASS_MIXED != std.math.maxInt(u8)) @compileError("GSM_CLASS_MIXED must be 255");
    const Confidence = classify.Confidence;
    if (c.GSM_CONFIDENCE_NONE != @intFromEnum(Confidence.none) or c.GSM_CONFIDENCE_LOW != @intFromEnum(Confidence.low) or
        c.GSM_CONFIDENCE_MEDIUM != @intFromEnum(Confidence.medium) or c.GSM_CONFIDENCE_HIGH != @intFromEnum(Confidence.high) or
        c.GSM_CONFIDENCE_COUNT != @typeInfo(Confidence).@"enum".fields.len)
        @compileError("a GSM_CONFIDENCE_* value differs from classify.Confidence");
    if (c.GSM_CLASS_RULE_COUNT != classify.rules.len) @compileError("GSM_CLASS_RULE_COUNT differs from classify.rules");
}

export fn gsm_class_name(workload_class: u32) [*:0]const u8 {
    if (workload_class == c.GSM_CLASS_MIXED) return "Mixed";
    if (workload_class >= class_name_table.len) return "unknown";
    return class_name_table[workload_class];
}

export fn gsm_confidence_name(confidence: u32) [*:0]const u8 {
    if (confidence >= confidence_name_table.len) return "unknown";
    return confidence_name_table[confidence];
}

export fn gsm_class_rule_count() u32 {
    return rule_name_table.len;
}

export fn gsm_class_rule_name(rule: u32) [*:0]const u8 {
    if (rule >= rule_name_table.len) return "unknown";
    return rule_name_table[rule];
}

// ---- accessors ----

export fn gsm_snapshot_processes(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_process {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.processes_count;
    return if (snapshot.processes_count == 0) null else @ptrCast(snapshot.processes.ptr);
}

export fn gsm_snapshot_cgroups(handle: *const c.gsm_snapshot, count: *usize) ?[*]const c.gsm_cgroup {
    const snapshot = snapshotOf(handle);
    count.* = snapshot.cgroups_count;
    return if (snapshot.cgroups_count == 0) null else @ptrCast(snapshot.cgroups.ptr);
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

test "a kernel thread has no address space: its resident memory is a known zero" {
    const collector = fixtureCollector(c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    // Its status file has no Vm* or Rss* lines because there is no mm to
    // print, so the figures are really zero (proc_pid_status(5)).
    const memory_bits = c.GSM_PROCESS_HAS_RSS_ANON | c.GSM_PROCESS_HAS_RSS_FILE | c.GSM_PROCESS_HAS_RSS_SHMEM |
        c.GSM_PROCESS_HAS_VM_SWAP | c.GSM_PROCESS_HAS_VM_SIZE;
    for ([_]u32{ 2, 103116 }) |pid| {
        const record = try processFor(snapshot.?, pid);
        try testing.expect(record.flags & c.GSM_PF_KTHREAD != 0);
        try testing.expectEqual(@as(u64, memory_bits), record.known & memory_bits);
        try testing.expectEqual(@as(u64, 0), record.rss_anon_bytes + record.rss_file_bytes + record.rss_shmem_bytes + record.vm_swap_bytes + record.vm_size_bytes);
        try testing.expect(record.known & c.GSM_PROCESS_HAS_CGROUP != 0);
    }
    // An ordinary process keeps what its status says.
    const sleeper = try processFor(snapshot.?, 344260);
    try testing.expectEqual(@as(u64, 112 * 1024), sleeper.rss_anon_bytes);
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

fn layoutCollector(layout: u32, readers: u32) ?*c.gsm_collector {
    var config = zeroed(c.gsm_collect_config);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.api_version = layout;
    config.readers = readers;
    config.procfs_root = fixture_proc;
    config.cgroupfs_root = fixture_cgroup;
    return gsm_collect_new(&config);
}

/// Record `index` of a snapshot read at `stride` bytes a record, copied into a
/// zeroed full record: only the first `stride` bytes are the consumer's.
fn recordAt(snapshot: *c.gsm_snapshot, stride: usize, index: usize) c.gsm_process {
    var count: usize = 0;
    const base: [*]const u8 = @ptrCast(gsm_snapshot_processes(snapshot, &count).?);
    std.debug.assert(index < count);
    var record = zeroed(c.gsm_process);
    @memcpy(std.mem.asBytes(&record)[0..stride], base[index * stride ..][0..stride]);
    return record;
}

/// The record of process `pid` in a snapshot read at `stride` bytes a record.
fn recordWithPid(snapshot: *c.gsm_snapshot, stride: usize, pid: u32) c.gsm_process {
    var count: usize = 0;
    _ = gsm_snapshot_processes(snapshot, &count);
    for (0..count) |index| {
        const record = recordAt(snapshot, stride, index);
        if (record.pid == pid) return record;
    }
    std.debug.panic("no process {d} in the snapshot", .{pid});
}

test "the layouts' sizes are the header's: 216 and 64 bytes for layout 1, 280 for layout 2, no hidden padding in layout 3" {
    try testing.expectEqual(@as(u32, 216), layoutOf(1).process_bytes);
    try testing.expectEqual(@as(u32, 64), layoutOf(1).stats_bytes);
    try testing.expectEqual(@as(u32, 280), layoutOf(2).process_bytes);
    try testing.expectEqual(@as(u32, 72), layoutOf(2).stats_bytes);
    try testing.expectEqual(@as(u32, 336), layoutOf(3).process_bytes);
    try testing.expectEqual(@as(u32, 72), layoutOf(3).stats_bytes);
    try testing.expectEqual(@as(usize, 336), @sizeOf(c.gsm_process));
    try testing.expectEqual(@sizeOf(c.gsm_process), @offsetOf(c.gsm_process, "agent_start_ticks") + 8);
    try testing.expectEqual(@as(usize, 280), @offsetOf(c.gsm_process, "workload_class"));
    try testing.expectEqual(@sizeOf(c.gsm_sample_stats), @offsetOf(c.gsm_sample_stats, "reserved3") + 4);
    // cgroups: layouts 1 and 2 end where layout 3 starts, layout 3 adds 16 bytes
    try testing.expectEqual(@as(u32, @offsetOf(c.gsm_cgroup, "workload_class")), layoutOf(1).cgroup_bytes);
    try testing.expectEqual(layoutOf(1).cgroup_bytes, layoutOf(2).cgroup_bytes);
    try testing.expectEqual(layoutOf(1).cgroup_bytes + 16, layoutOf(3).cgroup_bytes);
    try testing.expectEqual(@sizeOf(c.gsm_cgroup), @offsetOf(c.gsm_cgroup, "reserved3b") + 4);
    try testing.expectEqual(@as(u32, 3), gsm_collect_api_version());
}

test "a layout-1 consumer sees the same processes at layout 1's size and stride" {
    const old = layoutCollector(1, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(old);
    const new = layoutCollector(2, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(new);
    var old_snapshot: ?*c.gsm_snapshot = null;
    var new_snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(old, &old_snapshot));
    defer gsm_snapshot_free(old_snapshot);
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(new, &new_snapshot));
    defer gsm_snapshot_free(new_snapshot);

    var old_count: usize = 0;
    var new_count: usize = 0;
    _ = gsm_snapshot_processes(old_snapshot.?, &old_count);
    _ = gsm_snapshot_processes(new_snapshot.?, &new_count);
    try testing.expectEqual(@as(usize, 5), old_count);
    try testing.expectEqual(new_count, old_count);

    for (0..old_count) |index| {
        const before = recordAt(old_snapshot.?, 216, index);
        const after = recordAt(new_snapshot.?, 280, index);
        try testing.expectEqual(@as(u32, 216), before.struct_size);
        try testing.expectEqual(@as(u32, 280), after.struct_size);
        try testing.expectEqual(after.pid, before.pid);
        try testing.expectEqual(after.start_ticks, before.start_ticks);
        try testing.expectEqualStrings(std.mem.sliceTo(&after.comm, 0), std.mem.sliceTo(&before.comm, 0));
        try testing.expectEqual(after.rss_anon_bytes, before.rss_anon_bytes);
        try testing.expectEqual(after.run_ns, before.run_ns);
        try testing.expectEqualStrings(
            pathOf(new_snapshot.?, after.cgroup_offset, after.cgroup_len),
            pathOf(old_snapshot.?, before.cgroup_offset, before.cgroup_len),
        );
        // layout 1 has no io bit, even where the file was read
        try testing.expectEqual(@as(u64, 0), before.known & c.GSM_PROCESS_HAS_IO);
        try testing.expectEqual(after.known & ~@as(u64, c.GSM_PROCESS_HAS_IO), before.known);
    }
    try testing.expectEqual(@as(u32, 64), gsm_snapshot_stats(old_snapshot.?).struct_size);
    try testing.expectEqual(@as(u32, 72), gsm_snapshot_stats(new_snapshot.?).struct_size);
}

test "layout 2 carries nice, priority and io: known for the processes that have the file, unknown for the rest" {
    const collector = layoutCollector(2, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    _ = gsm_snapshot_processes(snapshot.?, &count);
    try testing.expectEqual(@as(usize, 5), count);
    const shell_record = recordWithPid(snapshot.?, 280, 344261);
    const shell = &shell_record;
    try testing.expectEqual(@as(i32, 20), shell.priority);
    try testing.expectEqual(@as(i32, 0), shell.nice);
    try testing.expect(shell.known & c.GSM_PROCESS_HAS_IO != 0);
    try testing.expectEqual(@as(u64, 1048576), shell.io_rchar_bytes);
    try testing.expectEqual(@as(u64, 524288), shell.io_wchar_bytes);
    try testing.expectEqual(@as(u64, 812), shell.io_syscr);
    try testing.expectEqual(@as(u64, 301), shell.io_syscw);
    try testing.expectEqual(@as(u64, 262144), shell.io_read_bytes);
    try testing.expectEqual(@as(u64, 131072), shell.io_write_bytes);
    try testing.expectEqual(@as(u64, 4096), shell.io_cancelled_write_bytes);

    // pid 1, pid 2 and the kworker have no io file in the capture
    for ([_]u32{ 1, 2, 103116 }) |pid| {
        const record = recordWithPid(snapshot.?, 280, pid);
        try testing.expectEqual(@as(u64, 0), record.known & c.GSM_PROCESS_HAS_IO);
        try testing.expectEqual(@as(u64, 0), record.io_write_bytes);
        try testing.expectEqual(@as(i32, 20), record.priority); // stat is always there
    }
    const stats = gsm_snapshot_stats(snapshot.?);
    try testing.expectEqual(@as(u32, 3), stats.process_io_unreadable);
    try testing.expectEqual(@as(u32, 0), stats.process_files_unreadable);
    try testing.expectEqual(@as(u32, 0), stats.processes_exited);
}

test "an api_version of 0 asks for the newest layout" {
    const collector = layoutCollector(0, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);
    try testing.expectEqual(@as(u32, 336), (try processFor(snapshot.?, 1)).struct_size);
}

const fixture_agent_proc = "tests/c/fixture-agent/proc";

fn classCollector(proc_root: []const u8, layout: u32, readers: u32) ?*c.gsm_collector {
    var config = zeroed(c.gsm_collect_config);
    config.struct_size = @sizeOf(c.gsm_collect_config);
    config.api_version = layout;
    config.readers = readers;
    config.procfs_root = proc_root.ptr;
    config.cgroupfs_root = fixture_cgroup;
    return gsm_collect_new(&config);
}

fn cgroupAt(snapshot: *c.gsm_snapshot, stride: usize, index: usize) c.gsm_cgroup {
    var count: usize = 0;
    const base: [*]const u8 = @ptrCast(gsm_snapshot_cgroups(snapshot, &count).?);
    std.debug.assert(index < count);
    var record = zeroed(c.gsm_cgroup);
    @memcpy(std.mem.asBytes(&record)[0..stride], base[index * stride ..][0..stride]);
    return record;
}

fn cgroupNamed(snapshot: *c.gsm_snapshot, path: []const u8) !c.gsm_cgroup {
    var count: usize = 0;
    _ = gsm_snapshot_cgroups(snapshot, &count);
    for (0..count) |index| {
        const record = cgroupAt(snapshot, @sizeOf(c.gsm_cgroup), index);
        if (std.mem.eql(u8, pathOf(snapshot, record.path_offset, record.path_len), path)) return record;
    }
    return error.TestExpectedEqual;
}

fn expectClass(snapshot: *c.gsm_snapshot, record: *const c.gsm_process, class: u8, rule: u8, confidence: u8, key: []const u8) !void {
    try testing.expect(record.known & c.GSM_PROCESS_HAS_CLASS != 0);
    try testing.expectEqual(class, record.workload_class);
    try testing.expectEqualStrings(classify.rules[rule].name, classify.rules[record.class_rule].name);
    try testing.expectEqual(confidence, record.class_confidence);
    try testing.expectEqualStrings(key, pathOf(snapshot, record.class_key_offset, record.class_key_length));
}

test "layout 3 gives every fixture process a class, with the unit its cgroup path names" {
    const collector = fixtureCollector(0).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    const records = gsm_snapshot_processes(snapshot.?, &count).?[0..count];
    for (records) |*record| {
        try testing.expect(record.known & c.GSM_PROCESS_HAS_CLASS != 0);
        try testing.expect(record.workload_class < c.GSM_CLASS_COUNT);
        try testing.expect(record.class_rule < c.GSM_CLASS_RULE_COUNT);
        try testing.expect(record.workload_class != c.GSM_CLASS_MIXED);
    }

    const sleeper = try processFor(snapshot.?, 344260);
    try expectClass(snapshot.?, sleeper, c.GSM_CLASS_APPLICATIONS, classify.rule_application, c.GSM_CONFIDENCE_HIGH, "com.visualstudio.CodeOSS");
    const scope = "app-com.visualstudio.CodeOSS-4247.scope";
    try testing.expect(sleeper.known & (c.GSM_PROCESS_HAS_UNIT | c.GSM_PROCESS_HAS_USER_UNIT | c.GSM_PROCESS_HAS_OWNER_UID) == (c.GSM_PROCESS_HAS_UNIT | c.GSM_PROCESS_HAS_USER_UNIT | c.GSM_PROCESS_HAS_OWNER_UID));
    // the unit is the user manager's, the user unit the scope inside it
    try testing.expectEqualStrings("user@1000.service", pathOf(snapshot.?, sleeper.unit_offset, sleeper.unit_length));
    try testing.expectEqualStrings(scope, pathOf(snapshot.?, sleeper.user_unit_offset, sleeper.user_unit_length));
    try testing.expectEqual(@as(u32, 1000), sleeper.owner_uid);
    try testing.expectEqual(@as(u64, 0), sleeper.known & (c.GSM_PROCESS_HAS_SESSION | c.GSM_PROCESS_HAS_AGENT));
    try testing.expectEqual(@as(u32, 0), sleeper.login_session_length);

    const init_process = try processFor(snapshot.?, 1);
    try expectClass(snapshot.?, init_process, c.GSM_CLASS_INIT, classify.rule_init, c.GSM_CONFIDENCE_HIGH, "");
    // a kernel thread sitting in the root group, and one the capture put in init.scope
    for ([_]u32{ 2, 103116 }) |pid| {
        const record = try processFor(snapshot.?, pid);
        try expectClass(snapshot.?, record, c.GSM_CLASS_KERNEL, classify.rule_kernel_thread, c.GSM_CONFIDENCE_HIGH, "");
    }
    // no cgroup path names no unit
    try testing.expectEqual(@as(u64, 0), (try processFor(snapshot.?, 2)).known & c.GSM_PROCESS_HAS_UNIT);
}

test "layout 3 gives every cgroup a class, and a node that holds several classes is mixed" {
    const collector = fixtureCollector(c.GSM_READ_CGROUPS).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    var count: usize = 0;
    const groups = gsm_snapshot_cgroups(snapshot.?, &count).?[0..count];
    try testing.expectEqual(@as(usize, 11), count);
    for (groups) |group| try testing.expect(group.known & c.GSM_CGROUP_HAS_CLASS != 0);

    const user_manager_path = "/user.slice/user-1000.slice/user@1000.service";
    for ([_][]const u8{ "/", "/user.slice", "/user.slice/user-1000.slice", user_manager_path }) |path| {
        const group = try cgroupNamed(snapshot.?, path);
        try testing.expectEqual(@as(u8, c.GSM_CLASS_MIXED), group.workload_class);
        try testing.expectEqual(@as(u8, classify.rule_mixed), group.class_rule);
        try testing.expectEqual(@as(u8, c.GSM_CONFIDENCE_NONE), group.class_confidence);
        try testing.expectEqual(@as(u32, 0), group.class_key_length);
    }

    const udev = try cgroupNamed(snapshot.?, "/system.slice/systemd-udevd.service/udev");
    try testing.expectEqual(@as(u8, c.GSM_CLASS_SYSTEM_SERVICES), udev.workload_class);
    try testing.expectEqual(@as(u8, classify.rule_system_service), udev.class_rule);
    try testing.expectEqualStrings("systemd-udevd.service", pathOf(snapshot.?, udev.class_key_offset, udev.class_key_length));

    const job = try cgroupNamed(snapshot.?, user_manager_path ++ "/agents.slice/agents-alpha.slice/job-alpha-386902.scope");
    try testing.expectEqual(@as(u8, c.GSM_CLASS_AGENT_SESSIONS), job.workload_class);
    try testing.expectEqual(@as(u8, classify.rule_agent_job_scope), job.class_rule);
    try testing.expectEqualStrings("job-alpha-386902.scope", pathOf(snapshot.?, job.class_key_offset, job.class_key_length));

    const beta = try cgroupNamed(snapshot.?, user_manager_path ++ "/agents.slice/agents-beta.slice");
    try testing.expectEqual(@as(u8, c.GSM_CLASS_AGENT_SESSIONS), beta.workload_class);
    try testing.expectEqualStrings("beta", pathOf(snapshot.?, beta.class_key_offset, beta.class_key_length));
}

test "layout 1 and 2 consumers get cgroup records at their own size, without class fields" {
    var records: [3]?*c.gsm_snapshot = undefined;
    const sizes = [3]usize{ layoutOf(1).cgroup_bytes, layoutOf(2).cgroup_bytes, layoutOf(3).cgroup_bytes };
    for (&records, 1..) |*slot, layout| {
        const collector = layoutCollector(@intCast(layout), c.GSM_READ_CGROUPS).?;
        defer gsm_collect_free(collector);
        slot.* = null;
        try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, slot));
    }
    defer for (records) |snapshot| gsm_snapshot_free(snapshot);

    var count: usize = 0;
    _ = gsm_snapshot_cgroups(records[2].?, &count);
    try testing.expectEqual(@as(usize, 11), count);
    for (0..count) |index| {
        const newest = cgroupAt(records[2].?, sizes[2], index);
        try testing.expectEqual(@as(u32, @intCast(sizes[2])), newest.struct_size);
        try testing.expect(newest.known & c.GSM_CGROUP_HAS_CLASS != 0);
        for (records[0..2], sizes[0..2]) |snapshot, size| {
            const older = cgroupAt(snapshot.?, size, index);
            try testing.expectEqual(@as(u32, @intCast(size)), older.struct_size);
            try testing.expectEqual(newest.known & ~@as(u64, c.GSM_CGROUP_HAS_CLASS), older.known);
            try testing.expectEqual(newest.memory_current_bytes, older.memory_current_bytes);
            try testing.expectEqual(newest.parent, older.parent);
            try testing.expectEqualStrings(
                pathOf(records[2].?, newest.path_offset, newest.path_len),
                pathOf(snapshot.?, older.path_offset, older.path_len),
            );
            // nothing of layout 3 leaks into the bytes past the older record
            try testing.expectEqual(@as(u8, 0), older.workload_class);
        }
    }
    _ = gsm_snapshot_cgroups(records[0].?, &count);
    try testing.expectEqual(@as(usize, 11), count);
}

test "layout 1 and 2 process records carry no class bits, and layout 3 only adds them" {
    var snapshots: [3]?*c.gsm_snapshot = undefined;
    const strides = [3]usize{ layoutOf(1).process_bytes, layoutOf(2).process_bytes, layoutOf(3).process_bytes };
    for (&snapshots, 1..) |*slot, layout| {
        const collector = layoutCollector(@intCast(layout), c.GSM_READ_PROCESSES).?;
        defer gsm_collect_free(collector);
        slot.* = null;
        try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, slot));
    }
    defer for (snapshots) |snapshot| gsm_snapshot_free(snapshot);

    for (0..5) |index| {
        const newest = recordAt(snapshots[2].?, strides[2], index);
        try testing.expect(newest.known & c.GSM_PROCESS_HAS_CLASS != 0);
        const second = recordAt(snapshots[1].?, strides[1], index);
        try testing.expectEqual(newest.known & ~process_layout3_bits, second.known);
        try testing.expectEqual(@as(u32, 0), second.owner_uid);
        const first_layout = recordAt(snapshots[0].?, strides[0], index);
        try testing.expectEqual(second.known & ~@as(u64, c.GSM_PROCESS_HAS_IO), first_layout.known);
        // the memory rule applies in every layout
        try testing.expectEqual(newest.rss_anon_bytes, first_layout.rss_anon_bytes);
        try testing.expectEqual(newest.vm_size_bytes, second.vm_size_bytes);
    }
}

test "an agent's descendants in a shared scope are agent sessions, with the agent's identity" {
    const collector = classCollector(fixture_agent_proc, c.GSM_COLLECT_API_VERSION, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    // 100 is the agent (comm "claude"), 101 its shell, 102 the shell's child: one scope
    for ([_]u32{ 100, 101, 102 }) |pid| {
        const record = try processFor(snapshot.?, pid);
        try expectClass(snapshot.?, record, c.GSM_CLASS_AGENT_SESSIONS, classify.rule_agent_ancestry, c.GSM_CONFIDENCE_MEDIUM, "");
        try testing.expect(record.known & c.GSM_PROCESS_HAS_AGENT != 0);
        try testing.expectEqual(@as(u32, 100), record.agent_pid);
        try testing.expectEqual(@as(u64, 1000), record.agent_start_ticks);
        // the path still names the application's unit
        try testing.expectEqualStrings("app-com.visualstudio.CodeOSS-4247.scope", pathOf(snapshot.?, record.user_unit_offset, record.user_unit_length));
    }
    // 103 shares the scope but is not below the agent: it stays an application
    const editor = try processFor(snapshot.?, 103);
    try expectClass(snapshot.?, editor, c.GSM_CLASS_APPLICATIONS, classify.rule_application, c.GSM_CONFIDENCE_HIGH, "com.visualstudio.CodeOSS");
    try testing.expectEqual(@as(u64, 0), editor.known & c.GSM_PROCESS_HAS_AGENT);
    try testing.expectEqual(@as(u32, 0), editor.agent_pid);
}

test "a zombie has no address space: known zero when its status parsed, unknown when there was none" {
    const collector = classCollector(fixture_agent_proc, 1, c.GSM_READ_PROCESSES).?;
    defer gsm_collect_free(collector);
    var snapshot: ?*c.gsm_snapshot = null;
    try testing.expectEqual(@as(c_int, 0), gsm_collect_sample(collector, &snapshot));
    defer gsm_snapshot_free(snapshot);

    const memory_bits = c.GSM_PROCESS_HAS_RSS_ANON | c.GSM_PROCESS_HAS_RSS_FILE | c.GSM_PROCESS_HAS_RSS_SHMEM |
        c.GSM_PROCESS_HAS_VM_SWAP | c.GSM_PROCESS_HAS_VM_SIZE;
    // layout 1, the oldest consumer, gets the corrected meaning too
    const zombie = recordWithPid(snapshot.?, layoutOf(1).process_bytes, 104);
    try testing.expectEqual(@as(u8, 'Z'), zombie.state);
    try testing.expectEqual(@as(u64, memory_bits), zombie.known & memory_bits);
    try testing.expectEqual(@as(u64, 0), zombie.rss_anon_bytes + zombie.vm_size_bytes);
    // the same task whose status file could not be read: nothing is known about it
    const unseen = recordWithPid(snapshot.?, layoutOf(1).process_bytes, 105);
    try testing.expectEqual(@as(u64, 0), unseen.known & (memory_bits | c.GSM_PROCESS_HAS_UID));
    // a live process whose status has the lines keeps its figures
    const shell = recordWithPid(snapshot.?, layoutOf(1).process_bytes, 101);
    try testing.expectEqual(@as(u64, 112 * 1024), shell.rss_anon_bytes);
}

test "the class, confidence and rule names come from the classifier's tables" {
    try testing.expectEqualStrings("Kernel", std.mem.span(gsm_class_name(c.GSM_CLASS_KERNEL)));
    try testing.expectEqualStrings("Agent sessions", std.mem.span(gsm_class_name(c.GSM_CLASS_AGENT_SESSIONS)));
    try testing.expectEqualStrings("Unclassified", std.mem.span(gsm_class_name(c.GSM_CLASS_UNCLASSIFIED)));
    try testing.expectEqualStrings("Mixed", std.mem.span(gsm_class_name(c.GSM_CLASS_MIXED)));
    try testing.expectEqualStrings("unknown", std.mem.span(gsm_class_name(c.GSM_CLASS_COUNT)));
    for (0..c.GSM_CLASS_COUNT) |index| {
        try testing.expectEqualStrings(classify.class_labels[index], std.mem.span(gsm_class_name(@intCast(index))));
    }
    try testing.expectEqualStrings("none", std.mem.span(gsm_confidence_name(c.GSM_CONFIDENCE_NONE)));
    try testing.expectEqualStrings("high", std.mem.span(gsm_confidence_name(c.GSM_CONFIDENCE_HIGH)));
    try testing.expectEqualStrings("unknown", std.mem.span(gsm_confidence_name(c.GSM_CONFIDENCE_COUNT)));
    try testing.expectEqual(@as(u32, c.GSM_CLASS_RULE_COUNT), gsm_class_rule_count());
    for (classify.rules, 0..) |rule, index| {
        try testing.expectEqualStrings(rule.name, std.mem.span(gsm_class_rule_name(@intCast(index))));
    }
    try testing.expectEqualStrings("agent ancestry", std.mem.span(gsm_class_rule_name(classify.rule_agent_ancestry)));
    try testing.expectEqualStrings("unknown", std.mem.span(gsm_class_rule_name(gsm_class_rule_count())));
    try testing.expectEqualStrings("unknown", std.mem.span(gsm_class_rule_name(0xFFFF_FFFF)));
}

test "the header's escape decoder agrees with the classifier's" {
    const cases = [_][]const u8{
        "plain.service",
        "gnome\\x2dtweaks",
        "a\\x2fb\\x2Fc",
        "bad\\xZZescape\\x4",
        "trailing\\x",
        "\\x41\\x42",
        "",
    };
    for (cases) |name| {
        var from_c: [64]u8 = undefined;
        var from_zig: [64]u8 = undefined;
        const length = c.gsm_unescape_name(name.ptr, name.len, &from_c, from_c.len);
        const decoded = unit_path.unescape(&from_zig, name).?;
        try testing.expectEqualStrings(decoded, from_c[0..@intCast(length)]);
        try testing.expectEqual(@as(u8, 0), from_c[@intCast(length)]);
    }
    // too small a buffer for the result and its NUL is refused, not truncated
    var small: [4]u8 = undefined;
    try testing.expectEqual(@as(c_long, -1), c.gsm_unescape_name("abcd", 4, &small, small.len));
    try testing.expectEqual(@as(c_long, 3), c.gsm_unescape_name("abc", 3, &small, small.len));
    try testing.expectEqual(@as(c_long, -1), c.gsm_unescape_name("a", 1, &small, 0));
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
