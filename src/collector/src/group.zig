// SPDX-License-Identifier: GPL-2.0-or-later
//! The workload group table: for one snapshot, a row per workload class
//! (all nine, in `classify.Class` order, present even when empty so the table
//! keeps its shape) and under each class one row per group (a unit, an
//! application, a login session, an agent session, a machine), with the
//! memory, tasks and CPU over the interval since the previous sample that the
//! row's members account for, and a residue that makes the class rows add up
//! to the kernel's user-space memory total.
//!
//! A pure function of what the walks already read (processes, cgroups, their
//! classes, `/proc/meminfo`) plus the previous sample's member counters; it
//! makes no syscall.
//!
//! Members. A row's members are cgroups or processes:
//!  - a cgroup is a member of a class row when it is a topmost cgroup of that
//!    class (its parent is a node that holds several classes, or has another
//!    class), and of a group row when it is a topmost cgroup of that group's
//!    (class, key) (its parent is another group's). cgroup counters are
//!    hierarchical and keep what exited processes used, so a member cgroup is
//!    the honest total for everything below it (cgroup-v2.rst: `memory.stat`,
//!    `cpu.stat` `usage_usec`, `pids.current`);
//!  - a process is a *summed* member of the rows of its own classification
//!    when no cgroup of its row's identity already holds its figures: kernel
//!    threads (no cgroup), a process classified by a process rule (no-cgroup
//!    rules, agent ancestry), a process in a node that holds several classes.
//!    Memory is `RssAnon`, `RssFile` and `RssShmem` of `status` (proc_pid_status(5)),
//!    tasks is `num_threads`, CPU is utime + stime of `stat`;
//!  - an *overlay* is a member (a summed process, or a cgroup that is itself a
//!    member of a row) that lies inside a member cgroup of another row (a coding agent's processes found by ancestry
//!    inside the editor's scope). Its figures are in its own row (summed) and,
//!    by construction, inside the host's cgroup counters: the row is marked
//!    overlay with the host row named, the host row carries "of which in
//!    overlay rows" sums, and the residue counts the bytes once.
//!
//! Process CPU. `/proc/PID/schedstat` is the CPU time of the process's first
//! thread only (the PID directory stands for that thread; sched-stats.rst),
//! so the sum of a multi-threaded program (a JVM, a browser) would be far too
//! small. `stat` utime + stime is the whole thread group's, including threads
//! that have exited (proc_pid_stat(5)), at the granularity of a clock tick
//! (USER_HZ, 100 a second). That is the figure used for summed members.
//!
//! Memory of a row, in three parts that do not overlap: `anon` (private
//! memory), `file` (page cache and mapped files without shared memory) and
//! `shmem` (tmpfs and shared anonymous memory). For cgroups `file` is
//! `memory.stat` `file - shmem` (the kernel's `file` includes shmem; clamped at
//! 0). For summed processes `file` and `shmem` are resident pages that other
//! processes may map too: the row is marked `file_shmem_double_counted`. A
//! summed process whose cgroup could not be read may also be inside a member
//! cgroup of another row, anon memory included: `anon_possibly_double_counted`.
//!
//! CPU over the interval is member-wise. For a member present in both samples:
//! the difference of its counter. For a cgroup not seen before: its whole
//! counter (it was created in the interval). For a process absent before:
//! its whole counter when it started after the previous sample, else nothing
//! (it joined the row although it ran before; its earlier counter is not
//! known). A member that left, a counter that went backwards (a cgroup
//! recreated: the new instance's whole counter counts), a member whose
//! counter could not be read, or a process that joined although older makes the figure a
//! *lower bound* (`cpu_lower_bound`): a true statement, never negative.
//! Two more rules keep it a bound. A cgroup that is new after a previous walk
//! that was incomplete (a directory could not be listed, or the tree was cut
//! off) may only have been missed, not created, so it counts nothing and marks
//! the lower bound. A row whose CPU over the interval exceeds what the online
//! CPUs could have given (interval times CPU count) counts nothing and marks
//! it too. And every row with summed members is a lower bound by construction:
//! a cgroup's counters keep what its exited processes used, a sum over live
//! processes cannot see a process that started and exited between two samples.
//! Members are remembered by identity, not by position: a cgroup by its path
//! and its directory inode, a process by (pid, start_ticks).
//!
//! A figure that cannot be told is unknown (its `known` bit clear), never
//! zero: the Kernel row has no memory by definition (kernel memory is in the
//! system partition, not in any task), a row none of whose members could be
//! measured has none, and a row where only some could be shows the sum with
//! a `*_lower_bound` mark.
//!
//! The residue. The kernel's user-space total is `AnonPages + Cached +
//! Buffers` of `/proc/meminfo` (proc_meminfo(5); `Cached` includes shmem). The class rows'
//! anon + file + shmem, each overlay counted once (in its host), is
//! subtracted: what is left is `unaccounted_bytes` (page cache charged to no
//! class, memory of nodes that hold several classes), and when the rows
//! exceed the total, `overlap_bytes` (pages that summed members share). So
//! `classes_bytes + unaccounted_bytes - overlap_bytes == user_space_bytes`
//! exactly.
//!
//! Row keys are slices of the inputs' strings (cgroup paths, an agent's
//! comm): the inputs must outlive the table. Nothing of the inputs is kept in
//! the returned `State`.
const std = @import("std");
const Allocator = std.mem.Allocator;
const classify = @import("classify.zig");
const cgroup_tree = @import("cgroup_tree.zig");
const meminfo = @import("meminfo.zig");
const walk = @import("walk.zig");

pub const Class = classify.Class;
pub const KeyKind = classify.KeyKind;
pub const class_count = classify.class_labels.len;

/// USER_HZ: the unit of `stat`'s times and start time (proc_pid_stat(5)),
/// fixed at 100 for the procfs ABI whatever the kernel's tick rate.
pub const ticks_per_second = 100;
const ns_per_tick = std.time.ns_per_s / ticks_per_second;
const ns_per_us = std.time.ns_per_us;

pub const Level = enum(u8) {
    /// One of the nine classes.
    class,
    /// A unit, application, session, machine or agent under a class.
    group,
};

/// A bit per figure that is known; a clear bit is "unknown", never zero.
pub const Known = packed struct(u8) {
    memory_anon: bool = false,
    memory_file: bool = false,
    memory_shmem: bool = false,
    /// `memory.current`: only for rows whose members are all cgroups.
    memory_current: bool = false,
    tasks: bool = false,
    /// `cpu_delta_ns` over the interval (a previous sample existed).
    cpu_delta: bool = false,
    _padding: u2 = 0,
};

/// What a reader must be told about how a row's figures were made.
pub const Marks = packed struct(u16) {
    /// Has cgroup members (their counters are exact totals).
    cgroup_backed: bool = false,
    /// Has summed members (figures are sums over processes).
    summed: bool = false,
    /// Some of the row's figures are overlay members', also counted inside a
    /// host row; `host_row` names it.
    overlay: bool = false,
    /// The row's cgroups hold other rows' overlay members; `hosted` has their sums.
    hosts_overlays: bool = false,
    /// With `overlay`: the overlay members have more than one host.
    overlay_hosts_differ: bool = false,
    /// `cpu_delta_ns` is at least that much (see the header).
    cpu_lower_bound: bool = false,
    /// A member's memory could not be read: the sum is at least that much.
    memory_lower_bound: bool = false,
    tasks_lower_bound: bool = false,
    /// `file` and `shmem` of summed members may count shared pages twice.
    file_shmem_double_counted: bool = false,
    /// A summed member's cgroup could not be read, so its anon memory too may
    /// also be inside another row's cgroup counters.
    anon_possibly_double_counted: bool = false,
    _padding: u6 = 0,
};

/// Figures that add up over members.
pub const Figures = struct {
    anon_bytes: u64 = 0,
    /// Page cache and mapped files, shmem excluded.
    file_bytes: u64 = 0,
    shmem_bytes: u64 = 0,
    /// Threads: `pids.current` of cgroups, `num_threads` of summed processes.
    tasks: u64 = 0,
    /// Processes classified here (a count of the process table, every member
    /// kind). In `hosted` and `overlay_part`: the overlay processes.
    processes: u32 = 0,
    /// CPU time used over the interval, ns. See `Marks.cpu_lower_bound`.
    cpu_delta_ns: u64 = 0,

    pub fn memoryBytes(self: Figures) u64 {
        return self.anon_bytes +| self.file_bytes +| self.shmem_bytes;
    }
};

pub const Row = struct {
    level: Level,
    class: Class,
    /// With the next three: the row's identity across samples. A class row has
    /// `.none` and no key.
    key_kind: KeyKind,
    key: []const u8,
    /// With `key_kind == .agent_process`: the agent program the row is for.
    agent_pid: u32 = 0,
    agent_start_ticks: u64 = 0,
    /// Index in `Table.rows` of this row's class row (its own for a class row).
    class_row: u32,
    /// With `marks.overlay`: the row whose cgroups hold the overlay members
    /// (a group row when the host has one, else the host's class row).
    host_row: ?u32 = null,
    known: Known = .{},
    marks: Marks = .{},
    cgroup_members: u32 = 0,
    summed_members: u32 = 0,
    figures: Figures = .{},
    /// `memory.current`, which also holds the kernel memory charged to the
    /// group; only with `known.memory_current`.
    memory_current_bytes: u64 = 0,
    /// The part of `figures` that is overlay members' (counted again in a host).
    overlay_part: Figures = .{},
    /// With `marks.hosts_overlays`: the part of `figures` that belongs to other
    /// rows' overlay members.
    hosted: Figures = .{},
};

pub const Residue = struct {
    /// False when meminfo lacks `AnonPages`, `Cached` or `Buffers`: the
    /// figures below are then zero and mean nothing.
    known: bool,
    /// `AnonPages + Cached + Buffers`.
    user_space_bytes: u64 = 0,
    /// The class rows' anon + file + shmem, each overlay once.
    classes_bytes: u64 = 0,
    /// The overlay members' share that was left out of `classes_bytes`.
    overlay_bytes: u64 = 0,
    /// `user_space_bytes - classes_bytes` when positive.
    unaccounted_bytes: u64 = 0,
    /// `classes_bytes - user_space_bytes` when positive: shared pages counted twice.
    overlap_bytes: u64 = 0,
    /// Some class row's memory is unknown or a lower bound (the Kernel row,
    /// whose memory is not in this total, does not count).
    classes_incomplete: bool = false,
};

pub const Table = struct {
    /// Class rows in `Class` order, each followed by its group rows.
    rows: []Row,
    /// Index in `rows` of each class's row.
    class_rows: [class_count]u32,
    residue: Residue,
    /// Time between the previous sample and this one; 0 when there was no
    /// usable previous sample (every `known.cpu_delta` is then clear).
    interval_ns: u64,

    pub fn deinit(self: *Table, gpa: Allocator) void {
        gpa.free(self.rows);
        self.* = undefined;
    }

    /// CPU use of the row over the interval in percent of one CPU, or null
    /// when unknown. With `marks.cpu_lower_bound` it is "at least".
    pub fn cpuPercent(self: *const Table, row: *const Row) ?f64 {
        if (!row.known.cpu_delta or self.interval_ns == 0) return null;
        return 100.0 * @as(f64, @floatFromInt(row.figures.cpu_delta_ns)) / @as(f64, @floatFromInt(self.interval_ns));
    }
};

const MemberKey = struct { row: u64, member: u64 };

/// What the next sample needs of this one. Holds no strings.
pub const State = struct {
    taken_ns: u64,
    taken_boot_ticks: u64,
    /// The cgroup walk lost nothing: a cgroup new in the next sample was then
    /// created in between rather than missed.
    cgroups_complete: bool = true,
    /// Per (row, member): the member's cumulative CPU counter in ns, null
    /// when it could not be read.
    members: std.AutoHashMapUnmanaged(MemberKey, ?u64) = .empty,
    /// Per row: how many members it had.
    counts: std.AutoHashMapUnmanaged(u64, u32) = .empty,

    pub fn deinit(self: *State, gpa: Allocator) void {
        self.members.deinit(gpa);
        self.counts.deinit(gpa);
        self.* = undefined;
    }
};

pub const Input = struct {
    processes: []const walk.Process,
    /// One per process, as `classify.classifyProcesses` fills them.
    process_classes: []const classify.Classification,
    /// Parents before children, as `cgroup_tree.Tree` lists them.
    groups: []const cgroup_tree.Group,
    /// One per group: `classify.classifyPath` of its path.
    group_classes: []const classify.Classification,
    meminfo: ?meminfo.Meminfo = null,
    /// The cgroup walk listed every directory (no listing failed, nothing was
    /// cut off by depth).
    cgroups_complete: bool = true,
    /// Online CPUs (`countCpus` of /sys/devices/system/cpu/online); 0 when
    /// unknown, which turns the CPU bound of a row off.
    online_cpus: u32 = 0,
    /// A monotonic clock reading for the interval's length.
    now_ns: u64,
    /// CLOCK_BOOTTIME in clock ticks (ns / 10 ms), the clock `start_ticks` is
    /// on (proc_pid_stat(5)). Read just before the walks: a process that
    /// started after this reading is "born in the next interval" if the walk
    /// missed it, and one the walk caught is known by identity anyway.
    now_boot_ticks: u64,
};

pub const Result = struct {
    table: Table,
    next: State,
};

pub const Error = Allocator.Error || error{LengthMismatch};

// ---- identities ----

const RowKey = struct {
    class: Class,
    key_kind: KeyKind,
    key: []const u8,
    agent_pid: u32,
    agent_start_ticks: u64,

    fn of(c: classify.Classification) RowKey {
        return .{ .class = c.class, .key_kind = c.key_kind, .key = c.key, .agent_pid = c.agent_pid, .agent_start_ticks = c.agent_start_ticks };
    }

    fn classRow(class: Class) RowKey {
        return .{ .class = class, .key_kind = .none, .key = "", .agent_pid = 0, .agent_start_ticks = 0 };
    }

    fn id(self: RowKey) u64 {
        var hasher = std.hash.Wyhash.init(0x67726f7570);
        hasher.update(&.{ @intFromEnum(self.class), @intFromEnum(self.key_kind) });
        hasher.update(std.mem.asBytes(&self.agent_pid));
        hasher.update(std.mem.asBytes(&self.agent_start_ticks));
        hasher.update(self.key);
        return hasher.final();
    }

    fn eql(a: RowKey, b: RowKey) bool {
        return a.class == b.class and a.key_kind == b.key_kind and a.agent_pid == b.agent_pid and
            a.agent_start_ticks == b.agent_start_ticks and std.mem.eql(u8, a.key, b.key);
    }

    fn lessThan(_: void, a: RowKey, b: RowKey) bool {
        if (a.class != b.class) return @intFromEnum(a.class) < @intFromEnum(b.class);
        if (a.key_kind != b.key_kind) return @intFromEnum(a.key_kind) < @intFromEnum(b.key_kind);
        switch (std.mem.order(u8, a.key, b.key)) {
            .lt => return true,
            .gt => return false,
            .eq => {},
        }
        if (a.agent_pid != b.agent_pid) return a.agent_pid < b.agent_pid;
        return a.agent_start_ticks < b.agent_start_ticks;
    }
};

const RowContext = struct {
    pub fn hash(_: RowContext, key: RowKey) u64 {
        return key.id();
    }
    pub fn eql(_: RowContext, a: RowKey, b: RowKey) bool {
        return a.eql(b);
    }
};
const RowMap = std.HashMapUnmanaged(RowKey, u32, RowContext, std.hash_map.default_max_load_percentage);

/// Which classifications get a group row: those whose key names something.
/// A project slice is a grouping node (its scopes are the groups); no key
/// names nothing.
fn hasGroupRow(c: classify.Classification) bool {
    if (c.mixed or c.key.len == 0) return false;
    return switch (c.key_kind) {
        .unit, .app_id, .session, .scope, .agent_process => true,
        .none, .project => false,
    };
}

fn isClassTop(groups: []const cgroup_tree.Group, classes: []const classify.Classification, index: usize) bool {
    const c = classes[index];
    if (c.mixed) return false;
    const parent = groups[index].parent orelse return true;
    const parent_class = classes[parent];
    return parent_class.mixed or parent_class.class != c.class;
}

fn isGroupTop(groups: []const cgroup_tree.Group, classes: []const classify.Classification, index: usize) bool {
    const c = classes[index];
    if (!hasGroupRow(c)) return false;
    const parent = groups[index].parent orelse return true;
    const parent_class = classes[parent];
    return parent_class.mixed or !RowKey.of(parent_class).eql(RowKey.of(c));
}

// ---- member figures ----

const Memory = struct { anon: u64, file: u64, shmem: u64 };

/// What one member contributes; null where it could not be read.
const Measure = struct {
    memory: ?Memory = null,
    current: ?u64 = null,
    tasks: ?u64 = null,
    /// Cumulative CPU time, ns.
    cpu_ns: ?u64 = null,
};

fn measureCgroup(group: cgroup_tree.Group) Measure {
    var measure: Measure = .{ .current = group.memory_current_bytes, .tasks = group.pids_current };
    if (group.memory_stat) |stat| {
        if (stat.anon_bytes != null and stat.file_bytes != null and stat.shmem_bytes != null) {
            // `file` includes shmem (cgroup-v2.rst, memory.stat).
            measure.memory = .{ .anon = stat.anon_bytes.?, .file = stat.file_bytes.? -| stat.shmem_bytes.?, .shmem = stat.shmem_bytes.? };
        }
    }
    if (group.cpu_stat) |stat| {
        if (stat.usage_usec) |usec| measure.cpu_ns = usec *| ns_per_us;
    }
    return measure;
}

fn measureProcess(process: *const walk.Process) Measure {
    var measure: Measure = .{ .tasks = process.num_threads };
    measure.cpu_ns = (process.utime_ticks +| process.stime_ticks) *| ns_per_tick;
    if (process.status) |status| {
        if (status.rss_anon_bytes != null and status.rss_file_bytes != null and status.rss_shmem_bytes != null) {
            measure.memory = .{ .anon = status.rss_anon_bytes.?, .file = status.rss_file_bytes.?, .shmem = status.rss_shmem_bytes.? };
        }
    }
    return measure;
}

// ---- the builder ----

const Tally = struct {
    memory_measured: u32 = 0,
    memory_missing: u32 = 0,
    tasks_measured: u32 = 0,
    tasks_missing: u32 = 0,
    current_missing: u32 = 0,
};

const MemberKind = enum { cgroup, process };

const Member = struct {
    kind: MemberKind,
    id: u64,
    measure: Measure,
    /// Processes: it started after the previous sample.
    born_after: bool = false,
};

const Builder = struct {
    gpa: Allocator,
    rows: []Row,
    row_ids: []u64,
    tallies: []Tally,
    /// Per row: members found in the previous sample.
    matched: []u32,
    /// Null when there is no usable previous sample.
    previous: ?*const State,
    /// The most CPU time a row can have used over the interval (interval times
    /// online CPUs); null when it cannot be told.
    cpu_limit_ns: ?u64 = null,
    next_members: std.AutoHashMapUnmanaged(MemberKey, ?u64) = .empty,
    next_counts: std.AutoHashMapUnmanaged(u64, u32) = .empty,

    fn deinit(self: *Builder) void {
        self.next_members.deinit(self.gpa);
        self.next_counts.deinit(self.gpa);
    }

    /// Adds one member to a row; returns the member's own figures (for the
    /// overlay sums).
    fn addMember(self: *Builder, row_index: u32, member: Member) Allocator.Error!Figures {
        const row = &self.rows[row_index];
        const tally = &self.tallies[row_index];
        const measure = member.measure;
        var added: Figures = .{};

        switch (member.kind) {
            .cgroup => row.cgroup_members += 1,
            .process => {
                row.summed_members += 1;
                added.processes = 1;
            },
        }

        if (measure.memory) |memory| {
            row.figures.anon_bytes +|= memory.anon;
            row.figures.file_bytes +|= memory.file;
            row.figures.shmem_bytes +|= memory.shmem;
            added.anon_bytes = memory.anon;
            added.file_bytes = memory.file;
            added.shmem_bytes = memory.shmem;
            tally.memory_measured += 1;
        } else tally.memory_missing += 1;

        if (measure.tasks) |tasks| {
            row.figures.tasks +|= tasks;
            added.tasks = tasks;
            tally.tasks_measured += 1;
        } else tally.tasks_missing += 1;

        if (member.kind == .cgroup) {
            if (measure.current) |current| row.memory_current_bytes +|= current else tally.current_missing += 1;
        }

        const key: MemberKey = .{ .row = self.row_ids[row_index], .member = member.id };
        try self.next_members.put(self.gpa, key, measure.cpu_ns);
        added.cpu_delta_ns = self.cpuDelta(row_index, key, member);
        row.figures.cpu_delta_ns +|= added.cpu_delta_ns;
        return added;
    }

    /// The member's CPU over the interval, marking the row when that is only
    /// a lower bound.
    fn cpuDelta(self: *Builder, row_index: u32, key: MemberKey, member: Member) u64 {
        const previous = self.previous orelse return 0;
        const row = &self.rows[row_index];
        const before = previous.members.get(key);
        if (before != null) self.matched[row_index] += 1;

        const now = member.measure.cpu_ns orelse {
            row.marks.cpu_lower_bound = true;
            return 0;
        };
        if (before) |counter| {
            const was = counter orelse {
                row.marks.cpu_lower_bound = true;
                return 0;
            };
            if (now >= was) return now - was;
            // The counter went backwards: the same identity was recreated.
            row.marks.cpu_lower_bound = true;
            return now;
        }
        switch (member.kind) {
            .cgroup => {
                // Not seen before: created in the interval, so everything it
                // has used is the interval's. After a walk that lost
                // directories it may only have been missed.
                if (previous.cgroups_complete) return now;
                row.marks.cpu_lower_bound = true;
                return 0;
            },
            .process => {
                if (member.born_after) return now;
                row.marks.cpu_lower_bound = true;
                return 0;
            },
        }
    }
};

fn processMemberId(process: *const walk.Process) u64 {
    return std.hash.Wyhash.hash(process.start_ticks, std.mem.asBytes(&process.pid));
}

fn groupMemberId(group: cgroup_tree.Group) u64 {
    return std.hash.Wyhash.hash(group.inode orelse 0, group.path);
}

/// The deepest cgroup in the tree at or above `path`: a process whose own
/// group vanished or is not listed still sits inside its parent's.
fn nearestGroup(index: *const std.StringHashMapUnmanaged(u32), path: []const u8) ?u32 {
    var current = path;
    while (true) {
        if (index.get(current)) |found| return found;
        if (current.len <= 1) return null;
        const slash = std.mem.lastIndexOfScalar(u8, current, '/') orelse return null;
        current = if (slash == 0) "/" else current[0..slash];
    }
}

const Hosts = struct {
    /// The nearest cgroup above of another class that is not a mixed node.
    class_host: ?u32 = null,
    /// The nearest cgroup above that is another group's.
    group_host: ?u32 = null,
};

/// The cgroups whose counters already hold this cgroup's, when it is a member
/// of a row of its own (a container's scope below `system.slice`).
fn hostsOf(groups: []const cgroup_tree.Group, classes: []const classify.Classification, index: usize) Hosts {
    const own = classes[index];
    var hosts: Hosts = .{};
    var cursor = groups[index].parent;
    while (cursor) |at| : (cursor = groups[at].parent) {
        const above = classes[at];
        if (above.mixed) continue;
        if (hosts.class_host == null and above.class != own.class) hosts.class_host = at;
        if (hosts.group_host == null and hasGroupRow(above) and !RowKey.of(above).eql(RowKey.of(own))) hosts.group_host = at;
        if (hosts.class_host != null and hosts.group_host != null) break;
    }
    return hosts;
}

fn addToRowFigures(target: *Figures, added: Figures) void {
    target.anon_bytes +|= added.anon_bytes;
    target.file_bytes +|= added.file_bytes;
    target.shmem_bytes +|= added.shmem_bytes;
    target.tasks +|= added.tasks;
    target.processes +|= added.processes;
    target.cpu_delta_ns +|= added.cpu_delta_ns;
}

/// Builds the table for one sample. `previous` is the `State` the previous
/// call returned (null on the first sample, or after a pause that makes it
/// stale); the returned `State` is for the next call and does not point into
/// the inputs.
pub fn build(gpa: Allocator, input: Input, previous: ?*const State) Error!Result {
    if (input.process_classes.len != input.processes.len or input.group_classes.len != input.groups.len)
        return error.LengthMismatch;
    const processes = input.processes;
    const groups = input.groups;

    var scratch: std.heap.ArenaAllocator = .init(gpa);
    defer scratch.deinit();
    const temp = scratch.allocator();

    // A previous sample from the future (or the same instant) is no interval.
    const usable_previous: ?*const State = if (previous) |state| (if (state.taken_ns < input.now_ns) state else null) else null;

    // cgroups by path.
    var path_index: std.StringHashMapUnmanaged(u32) = .empty;
    try path_index.ensureTotalCapacity(temp, @intCast(groups.len));
    for (groups, 0..) |group, at| path_index.putAssumeCapacity(group.path, @intCast(at));

    // The group rows: from topmost member cgroups and from processes.
    var key_set: RowMap = .empty;
    var keys: std.ArrayList(RowKey) = .empty;
    const class_top = try temp.alloc(bool, groups.len);
    const group_top = try temp.alloc(bool, groups.len);
    for (groups, 0..) |_, at| {
        class_top[at] = isClassTop(groups, input.group_classes, at);
        group_top[at] = isGroupTop(groups, input.group_classes, at);
        if (group_top[at]) try addKey(temp, &key_set, &keys, RowKey.of(input.group_classes[at]));
    }
    for (input.process_classes) |c| {
        if (hasGroupRow(c)) try addKey(temp, &key_set, &keys, RowKey.of(c));
    }
    std.mem.sort(RowKey, keys.items, {}, RowKey.lessThan);

    // The layout: each class row, then its group rows (sorted keys are grouped by class).
    const rows = try gpa.alloc(Row, class_count + keys.items.len);
    errdefer gpa.free(rows);
    const row_ids = try temp.alloc(u64, rows.len);
    var class_rows: [class_count]u32 = undefined;
    {
        var next: u32 = 0;
        var at: usize = 0;
        inline for (0..class_count) |class_number| {
            const class: Class = @enumFromInt(class_number);
            class_rows[class_number] = next;
            rows[next] = .{ .level = .class, .class = class, .key_kind = .none, .key = "", .class_row = next };
            row_ids[next] = RowKey.classRow(class).id();
            next += 1;
            while (at < keys.items.len and keys.items[at].class == class) : (at += 1) {
                const key = keys.items[at];
                rows[next] = .{
                    .level = .group,
                    .class = class,
                    .key_kind = key.key_kind,
                    .key = key.key,
                    .agent_pid = key.agent_pid,
                    .agent_start_ticks = key.agent_start_ticks,
                    .class_row = class_rows[class_number],
                };
                row_ids[next] = key.id();
                key_set.getPtr(key).?.* = next;
                next += 1;
            }
        }
    }

    var builder: Builder = .{
        .gpa = gpa,
        .rows = rows,
        .row_ids = row_ids,
        .tallies = try temp.alloc(Tally, rows.len),
        .matched = try temp.alloc(u32, rows.len),
        .previous = usable_previous,
        .cpu_limit_ns = if (usable_previous != null and input.online_cpus > 0)
            (input.now_ns - usable_previous.?.taken_ns) *| input.online_cpus
        else
            null,
    };
    defer builder.deinit();
    @memset(builder.tallies, .{});
    @memset(builder.matched, 0);
    const expected_members = groups.len * 2 + processes.len;
    try builder.next_members.ensureTotalCapacity(gpa, @intCast(expected_members));
    try builder.next_counts.ensureTotalCapacity(gpa, @intCast(rows.len));

    // Member cgroups.
    for (groups, 0..) |group, at| {
        const class = input.group_classes[at];
        const member: Member = .{ .kind = .cgroup, .id = groupMemberId(group), .measure = measureCgroup(group) };
        if (!class_top[at] and !group_top[at]) continue;
        const hosts = hostsOf(groups, input.group_classes, at);
        if (class_top[at]) {
            const class_row = class_rows[@intFromEnum(class.class)];
            const added = try builder.addMember(class_row, member);
            // Inside another class's member cgroup, whose counters hold this one's.
            if (hosts.class_host) |above| {
                const host = class_rows[@intFromEnum(input.group_classes[above].class)];
                markOverlay(&builder.rows[class_row], &builder.rows[host], host, added);
            }
        }
        if (group_top[at]) {
            const group_row = key_set.get(RowKey.of(class)).?;
            const added = try builder.addMember(group_row, member);
            if (hosts.group_host) |above| {
                const host = key_set.get(RowKey.of(input.group_classes[above])).?;
                markOverlay(&builder.rows[group_row], &builder.rows[host], host, added);
            } else if (hosts.class_host) |above| {
                // The host has no group row; its class row carries the sums.
                markOverlayRow(&builder.rows[group_row], class_rows[@intFromEnum(input.group_classes[above].class)], added);
            }
        }
    }

    // Processes.
    const born_after_ticks: u64 = if (usable_previous) |state| state.taken_boot_ticks else 0;
    for (processes, input.process_classes) |*process, c| {
        const class_row = class_rows[@intFromEnum(c.class)];
        const group_row: ?u32 = if (hasGroupRow(c)) key_set.get(RowKey.of(c)) else null;
        builder.rows[class_row].figures.processes += 1;
        if (group_row) |at| builder.rows[at].figures.processes += 1;

        const holder: ?classify.Classification = blk: {
            const path = process.cgroup_path orelse break :blk null;
            const at = nearestGroup(&path_index, path) orelse break :blk null;
            const held = input.group_classes[at];
            break :blk if (held.mixed) null else held;
        };
        const class_covered = if (holder) |held| held.class == c.class else false;
        const group_covered = if (holder) |held| group_row != null and RowKey.of(held).eql(RowKey.of(c)) else false;

        var member: Member = .{
            .kind = .process,
            .id = processMemberId(process),
            .measure = measureProcess(process),
            .born_after = process.start_ticks > born_after_ticks,
        };
        // A kernel thread has no address space of its own: its memory is not
        // a figure (kernel memory is in the system partition), whatever its
        // status file shows (pid 2 may carry Rss lines in a container).
        if (c.class == .kernel) member.measure.memory = null;

        // A process whose cgroup is unknown is summed, and may be inside a
        // cgroup that another row's counters hold.
        const cgroup_unknown = process.cgroup_path == null and c.class != .kernel;

        if (!class_covered) {
            const added = try builder.addMember(class_row, member);
            if (cgroup_unknown) builder.rows[class_row].marks.anon_possibly_double_counted = true;
            if (holder) |held| {
                // The process lies inside another class's cgroup: an overlay.
                const host = class_rows[@intFromEnum(held.class)];
                markOverlay(&builder.rows[class_row], &builder.rows[host], host, added);
            }
        }
        if (group_row) |at| {
            if (!group_covered) {
                const added = try builder.addMember(at, member);
                if (cgroup_unknown) builder.rows[at].marks.anon_possibly_double_counted = true;
                if (holder) |held| {
                    const host_group: ?u32 = if (hasGroupRow(held)) key_set.get(RowKey.of(held)) else null;
                    if (host_group) |host| {
                        markOverlay(&builder.rows[at], &builder.rows[host], host, added);
                    } else {
                        // The host has no group row; the class row carries the sums.
                        const host = class_rows[@intFromEnum(held.class)];
                        markOverlayRow(&builder.rows[at], host, added);
                    }
                }
            }
        }
    }

    finish(&builder);

    const residue = residueOf(builder.rows, &class_rows, input.meminfo);
    const interval_ns: u64 = if (usable_previous) |state| input.now_ns - state.taken_ns else 0;

    const next: State = .{
        .taken_ns = input.now_ns,
        .taken_boot_ticks = input.now_boot_ticks,
        .cgroups_complete = input.cgroups_complete,
        .members = builder.next_members,
        .counts = builder.next_counts,
    };
    // The builder's maps now belong to the state.
    builder.next_members = .empty;
    builder.next_counts = .empty;
    return .{
        .table = .{ .rows = rows, .class_rows = class_rows, .residue = residue, .interval_ns = interval_ns },
        .next = next,
    };
}

fn addKey(temp: Allocator, set: *RowMap, keys: *std.ArrayList(RowKey), key: RowKey) Allocator.Error!void {
    const slot = try set.getOrPut(temp, key);
    if (slot.found_existing) return;
    slot.value_ptr.* = 0;
    try keys.append(temp, key);
}

/// An overlay member joined `row`; `host_row` holds it in its cgroup.
fn markOverlayRow(row: *Row, host_index: u32, added: Figures) void {
    row.marks.overlay = true;
    if (row.host_row) |existing| {
        if (existing != host_index) row.marks.overlay_hosts_differ = true;
    } else row.host_row = host_index;
    addToRowFigures(&row.overlay_part, added);
}

fn markOverlay(row: *Row, host: *Row, host_index: u32, added: Figures) void {
    markOverlayRow(row, host_index, added);
    host.marks.hosts_overlays = true;
    addToRowFigures(&host.hosted, added);
}

/// Known bits, marks and the next state's member counts, once every member is in.
fn finish(builder: *Builder) void {
    for (builder.rows, builder.tallies, builder.matched, builder.row_ids) |*row, tally, matched, row_id| {
        const members = row.cgroup_members + row.summed_members;
        // Capacity for every row was reserved up front.
        if (members > 0) builder.next_counts.putAssumeCapacity(row_id, members);

        row.marks.cgroup_backed = row.cgroup_members > 0;
        row.marks.summed = row.summed_members > 0;
        // The kernel's own memory is not in any task: unknown by definition.
        const memory_known = row.class != .kernel and (members == 0 or tally.memory_measured > 0);
        row.known.memory_anon = memory_known;
        row.known.memory_file = memory_known;
        row.known.memory_shmem = memory_known;
        row.marks.memory_lower_bound = memory_known and tally.memory_missing > 0;
        row.known.tasks = members == 0 or tally.tasks_measured > 0;
        row.marks.tasks_lower_bound = row.known.tasks and tally.tasks_missing > 0;
        row.known.memory_current = row.cgroup_members > 0 and row.summed_members == 0 and tally.current_missing == 0;
        row.marks.file_shmem_double_counted = row.summed_members > 0 and memory_known;

        if (builder.previous) |previous| {
            row.known.cpu_delta = true;
            const was = previous.counts.get(row_id) orelse 0;
            if (was > matched) row.marks.cpu_lower_bound = true;
            // A process sum cannot see what started and exited between samples.
            if (row.summed_members > 0) row.marks.cpu_lower_bound = true;
            if (builder.cpu_limit_ns) |limit| {
                // More than every online CPU could have given: a counter is
                // wrong (or a member moved), so claim nothing.
                if (boundCpu(&row.figures, limit)) row.marks.cpu_lower_bound = true;
                if (boundCpu(&row.overlay_part, limit)) row.marks.cpu_lower_bound = true;
                if (boundCpu(&row.hosted, limit)) row.marks.cpu_lower_bound = true;
            }
        } else {
            row.figures.cpu_delta_ns = 0;
            row.overlay_part.cpu_delta_ns = 0;
            row.hosted.cpu_delta_ns = 0;
        }
    }
}

/// Zeroes `figures`' CPU when it is above `limit_ns`; true when it did.
fn boundCpu(figures: *Figures, limit_ns: u64) bool {
    if (figures.cpu_delta_ns <= limit_ns) return false;
    figures.cpu_delta_ns = 0;
    return true;
}

/// The number of CPUs in a kernel CPU list ("0-15", "0,2-3"; cpumask_parselist
/// syntax, as /sys/devices/system/cpu/online prints it), or null when the text
/// is not one or names none.
pub fn countCpus(list: []const u8) ?u32 {
    const text = std.mem.trim(u8, list, " \n");
    if (text.len == 0) return null;
    var total: u32 = 0;
    var parts = std.mem.splitScalar(u8, text, ',');
    while (parts.next()) |part| {
        const dash = std.mem.indexOfScalar(u8, part, '-');
        const low = std.fmt.parseInt(u32, part[0 .. dash orelse part.len], 10) catch return null;
        const high = if (dash) |at| (std.fmt.parseInt(u32, part[at + 1 ..], 10) catch return null) else low;
        if (high < low) return null;
        total = std.math.add(u32, total, high - low + 1) catch return null;
    }
    return total;
}

fn residueOf(rows: []const Row, class_rows: *const [class_count]u32, info: ?meminfo.Meminfo) Residue {
    const available = info orelse return .{ .known = false };
    const anon_pages = available.anon_pages_bytes orelse return .{ .known = false };
    const cached = available.cached_bytes orelse return .{ .known = false };
    const buffers = available.buffers_bytes orelse return .{ .known = false };
    var residue: Residue = .{ .known = true, .user_space_bytes = anon_pages +| cached +| buffers };

    for (class_rows) |at| {
        const row = rows[at];
        if (!row.known.memory_anon) {
            if (row.class != .kernel) residue.classes_incomplete = true;
            continue;
        }
        if (row.marks.memory_lower_bound) residue.classes_incomplete = true;
        const total = row.figures.memoryBytes();
        const overlay = @min(row.overlay_part.memoryBytes(), total);
        residue.classes_bytes +|= total - overlay;
        residue.overlay_bytes +|= overlay;
    }
    if (residue.user_space_bytes >= residue.classes_bytes) {
        residue.unaccounted_bytes = residue.user_space_bytes - residue.classes_bytes;
    } else {
        residue.overlap_bytes = residue.classes_bytes - residue.user_space_bytes;
    }
    return residue;
}

// ---- tests ----

const testing = std.testing;

const user_manager = "/user.slice/user-1000.slice/user@1000.service";
const editor_scope = user_manager ++ "/app.slice/app-com.visualstudio.CodeOSS-4247.scope";
const alpha_slice = user_manager ++ "/agents.slice/agents-alpha.slice";

const GroupSpec = struct {
    path: []const u8,
    anon: u64 = 0,
    file: u64 = 0,
    shmem: u64 = 0,
    cpu_us: u64 = 0,
    pids: u64 = 1,
    inode: u64 = 0,
};

/// A world of fixed inputs: groups (with their classes) and processes.
const World = struct {
    groups: []cgroup_tree.Group,
    group_classes: []classify.Classification,
    processes: []walk.Process,
    process_classes: []classify.Classification,

    fn make(arena: Allocator, specs: []const GroupSpec, procs: []const walk.Process) !World {
        const groups = try arena.alloc(cgroup_tree.Group, specs.len);
        const group_classes = try arena.alloc(classify.Classification, specs.len);
        for (specs, 0..) |spec, at| {
            var parent: ?u32 = null;
            var longest: usize = 0;
            for (specs[0..at], 0..) |other, other_at| {
                const prefix = if (other.path.len == 1) "" else other.path;
                const is_ancestor = std.mem.startsWith(u8, spec.path, prefix) and
                    spec.path.len > prefix.len and spec.path[prefix.len] == '/';
                if (is_ancestor and other.path.len >= longest) {
                    parent = @intCast(other_at);
                    longest = other.path.len;
                }
            }
            groups[at] = .{
                .path = spec.path,
                .parent = parent,
                .inode = if (at == 0) null else spec.inode,
                .memory_current_bytes = spec.anon + spec.file,
                .memory_stat = .{ .anon_bytes = spec.anon, .file_bytes = spec.file + spec.shmem, .shmem_bytes = spec.shmem },
                .cpu_stat = .{ .usage_usec = spec.cpu_us },
                .pids_current = spec.pids,
            };
            group_classes[at] = classify.classifyPath(spec.path);
        }
        const process_classes = try arena.alloc(classify.Classification, procs.len);
        try classify.classifyProcesses(arena, procs, process_classes);
        return .{ .groups = groups, .group_classes = group_classes, .processes = try arena.dupe(walk.Process, procs), .process_classes = process_classes };
    }

    fn input(self: World, now_ns: u64, now_boot_ticks: u64, info: ?meminfo.Meminfo) Input {
        return .{
            .processes = self.processes,
            .process_classes = self.process_classes,
            .groups = self.groups,
            .group_classes = self.group_classes,
            .meminfo = info,
            .now_ns = now_ns,
            .now_boot_ticks = now_boot_ticks,
        };
    }
};

const Proc = struct {
    pid: u32,
    start_ticks: u64 = 1,
    ppid: u32 = 1,
    comm: []const u8 = "prog",
    cgroup: ?[]const u8 = null,
    cpu_ticks: u64 = 0,
    anon: u64 = 0,
    file: u64 = 0,
    shmem: u64 = 0,
    threads: u32 = 1,
    kthread: bool = false,
    uid: ?u32 = null,
    with_status: bool = true,

    fn process(self: Proc) walk.Process {
        var p: walk.Process = .{
            .pid = self.pid,
            .start_ticks = self.start_ticks,
            .comm_bytes = @splat(0),
            .comm_len = @intCast(self.comm.len),
            .state = 'S',
            .ppid = self.ppid,
            .pgrp = self.pid,
            .session = self.pid,
            .tty_nr = 0,
            .flags = if (self.kthread) 0x0020_0000 else 0,
            .utime_ticks = self.cpu_ticks,
            .stime_ticks = 0,
            .num_threads = self.threads,
            .priority = 20,
            .nice = 0,
            .status = null,
            .schedstat = null,
            .io = null,
            .cgroup_path = self.cgroup,
        };
        @memcpy(p.comm_bytes[0..self.comm.len], self.comm);
        if (self.with_status and !self.kthread) {
            p.status = .{ .rss_anon_bytes = self.anon, .rss_file_bytes = self.file, .rss_shmem_bytes = self.shmem, .uid_real = self.uid };
        } else if (self.uid != null) {
            p.status = .{ .uid_real = self.uid };
        }
        return p;
    }
};

fn processesOf(arena: Allocator, procs: []const Proc) ![]walk.Process {
    const out = try arena.alloc(walk.Process, procs.len);
    for (procs, out) |spec, *slot| slot.* = spec.process();
    return out;
}

fn rowOf(table: *const Table, class: Class) *const Row {
    return &table.rows[table.class_rows[@intFromEnum(class)]];
}

fn groupRowOf(table: *const Table, class: Class, key: []const u8) !*const Row {
    for (table.rows) |*row| {
        if (row.level == .group and row.class == class and std.mem.eql(u8, row.key, key)) return row;
    }
    return error.TestUnexpectedResult;
}

fn memoryInfo(user_space_anon: u64, cached: u64, buffers: u64) meminfo.Meminfo {
    return .{ .anon_pages_bytes = user_space_anon, .cached_bytes = cached, .buffers_bytes = buffers };
}

const system_tree = [_]GroupSpec{
    .{ .path = "/", .anon = 0 },
    .{ .path = "/system.slice", .anon = 5000, .file = 2000, .shmem = 100, .cpu_us = 10_000, .pids = 4 },
    .{ .path = "/system.slice/ssh.service", .anon = 1000, .file = 300, .cpu_us = 4000, .pids = 1, .inode = 11 },
    .{ .path = "/system.slice/cron.service", .anon = 500, .file = 100, .cpu_us = 1000, .pids = 2, .inode = 12 },
};

test "a single-class slice backs its class row whole, its units back the group rows" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const procs = try processesOf(a, &.{
        .{ .pid = 10, .cgroup = "/system.slice/ssh.service", .anon = 900, .file = 250 },
        .{ .pid = 11, .cgroup = "/system.slice/cron.service", .anon = 400 },
        .{ .pid = 12, .cgroup = "/system.slice/cron.service", .anon = 50 },
    });
    const world = try World.make(a, &system_tree, procs);
    var result = try build(testing.allocator, world.input(1_000_000_000, 500, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const services = rowOf(table, .system_services);
    // system.slice alone: its counters, not the sum of the units nor of the processes.
    try testing.expectEqual(@as(u64, 5000), services.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 2000), services.figures.file_bytes);
    try testing.expectEqual(@as(u64, 100), services.figures.shmem_bytes);
    try testing.expectEqual(@as(u64, 4), services.figures.tasks);
    try testing.expectEqual(@as(u32, 3), services.figures.processes);
    try testing.expectEqual(@as(u32, 1), services.cgroup_members);
    try testing.expectEqual(@as(u32, 0), services.summed_members);
    try testing.expect(services.marks.cgroup_backed and !services.marks.summed and !services.marks.overlay);
    try testing.expect(services.known.memory_anon and services.known.memory_current and services.known.tasks);
    try testing.expectEqual(@as(u64, 7000), services.memory_current_bytes);

    const ssh = try groupRowOf(table, .system_services, "ssh.service");
    try testing.expectEqual(@as(u64, 1000), ssh.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 300), ssh.figures.file_bytes);
    try testing.expectEqual(services.class_row, ssh.class_row);
    try testing.expectEqual(@as(u32, 1), ssh.figures.processes);
    const cron = try groupRowOf(table, .system_services, "cron.service");
    try testing.expectEqual(@as(u32, 2), cron.figures.processes);
    // No previous sample: no CPU figure, not zero.
    try testing.expect(!services.known.cpu_delta);
    try testing.expectEqual(@as(?f64, null), table.cpuPercent(services));
}

test "the table has all nine class rows in order, each group row right after its class row" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const world = try World.make(a, &system_tree, &.{});
    var result = try build(testing.allocator, world.input(1, 1, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    try testing.expectEqual(@as(usize, class_count + 2), table.rows.len);
    for (0..class_count) |number| {
        const row = table.rows[table.class_rows[number]];
        try testing.expectEqual(Level.class, row.level);
        try testing.expectEqual(@as(usize, number), @intFromEnum(row.class));
        try testing.expectEqual(table.class_rows[number], row.class_row);
    }
    // Empty classes are present, with known zeros except the Kernel's memory.
    const empty = rowOf(table, .containers_and_vms);
    try testing.expect(empty.known.memory_anon);
    try testing.expectEqual(@as(u64, 0), empty.figures.memoryBytes());
    for (table.rows, 0..) |row, at| {
        if (row.level == .group) {
            try testing.expect(row.class_row < at);
            try testing.expectEqual(row.class, table.rows[row.class_row].class);
        }
    }
}

fn twoSamples(first: World, second: World, ticks: [2]u64, info: ?meminfo.Meminfo) !Result {
    var one = try build(testing.allocator, first.input(1_000_000_000, ticks[0], info), null);
    one.table.deinit(testing.allocator);
    defer one.next.deinit(testing.allocator);
    return build(testing.allocator, second.input(3_000_000_000, ticks[1], info), &one.next);
}

const apps_tree = [_]GroupSpec{
    .{ .path = "/", .anon = 0 },
    .{ .path = "/user.slice" },
    .{ .path = "/user.slice/user-1000.slice" },
    .{ .path = user_manager },
    .{ .path = user_manager ++ "/app.slice" },
};

fn withApps(comptime extra: []const GroupSpec) [apps_tree.len + extra.len]GroupSpec {
    var out: [apps_tree.len + extra.len]GroupSpec = undefined;
    for (apps_tree, 0..) |spec, at| out[at] = spec;
    for (extra, 0..) |spec, at| out[apps_tree.len + at] = spec;
    return out;
}

test "a scope that exits between two samples makes its row a lower bound and never negative" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const scope_a = user_manager ++ "/app.slice/app-org.example.A-100.scope";
    const scope_b = user_manager ++ "/app.slice/app-org.example.B-200.scope";
    const before = withApps(&.{
        .{ .path = scope_a, .cpu_us = 1000, .inode = 21 },
        .{ .path = scope_b, .cpu_us = 2000, .inode = 22 },
    });
    // B used 9 s and exited; A used 0.5 s more.
    const after = withApps(&.{
        .{ .path = scope_a, .cpu_us = 1500, .inode = 21 },
    });
    var result = try twoSamples(try World.make(a, &before, &.{}), try World.make(a, &after, &.{}), .{ 100, 300 }, null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    try testing.expectEqual(@as(u64, 2_000_000_000), table.interval_ns);
    const applications = rowOf(table, .applications);
    try testing.expect(applications.known.cpu_delta);
    try testing.expectEqual(@as(u64, 500_000), applications.figures.cpu_delta_ns);
    try testing.expect(applications.marks.cpu_lower_bound);
    // A, which stayed, is exact.
    const survivor = try groupRowOf(table, .applications, "org.example.A");
    try testing.expectEqual(@as(u64, 500_000), survivor.figures.cpu_delta_ns);
    try testing.expect(!survivor.marks.cpu_lower_bound);
    try testing.expectApproxEqAbs(@as(f64, 0.025), table.cpuPercent(survivor).?, 1e-9);
}

test "a service whose cgroup is recreated counts the new instance whole and is a lower bound" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const old = [_]GroupSpec{
        .{ .path = "/" },
        .{ .path = "/system.slice" },
        .{ .path = "/system.slice/web.service", .cpu_us = 90_000, .inode = 31 },
    };
    // Same path, another directory: the new instance has used 0.8 s.
    var recreated = old;
    recreated[2] = .{ .path = "/system.slice/web.service", .cpu_us = 800_000, .inode = 77 };
    var result = try twoSamples(try World.make(a, &old, &.{}), try World.make(a, &recreated, &.{}), .{ 100, 300 }, null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const web = try groupRowOf(&result.table, .system_services, "web.service");
    try testing.expectEqual(@as(u64, 800_000_000), web.figures.cpu_delta_ns);
    try testing.expect(web.marks.cpu_lower_bound);
    // system.slice itself was there all along: exact, whatever its unit did.
    try testing.expect(!rowOf(&result.table, .system_services).marks.cpu_lower_bound);
}

test "a counter that went backwards for the same identity counts whole and is a lower bound" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const old = [_]GroupSpec{
        .{ .path = "/" },
        .{ .path = "/system.slice" },
        .{ .path = "/system.slice/web.service", .cpu_us = 90_000, .inode = 31 },
    };
    var restarted = old;
    restarted[2].cpu_us = 5000;
    var result = try twoSamples(try World.make(a, &old, &.{}), try World.make(a, &restarted, &.{}), .{ 100, 300 }, null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const web = try groupRowOf(&result.table, .system_services, "web.service");
    try testing.expectEqual(@as(u64, 5_000_000), web.figures.cpu_delta_ns);
    try testing.expect(web.marks.cpu_lower_bound);
}

test "a process born in the interval counts its whole CPU, one that joined although older counts nothing" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    // Daemons with no cgroup information are System services, summed (uid below 1000).
    const tree = [_]GroupSpec{.{ .path = "/" }};
    const before = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .cgroup = null, .uid = 0, .cpu_ticks = 100 },
    }));
    const after = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .cgroup = null, .uid = 0, .cpu_ticks = 130 },
        // Born after the first sample (taken at tick 100): all 40 ticks are the interval's.
        .{ .pid = 51, .start_ticks = 150, .cgroup = null, .uid = 0, .cpu_ticks = 40 },
    }));
    var result = try twoSamples(before, after, .{ 100, 300 }, null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const services = rowOf(&result.table, .system_services);
    // 30 ticks of pid 50 and 40 of pid 51, at 10 ms a tick.
    try testing.expectEqual(@as(u64, 700_000_000), services.figures.cpu_delta_ns);
    // A row of summed processes is always a lower bound: a process that started and exited in between is invisible.
    try testing.expect(services.marks.cpu_lower_bound);
    try testing.expect(services.marks.summed and !services.marks.cgroup_backed);

    // A process that started long before but was not in the row before: nothing of it is counted.
    const later = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .cgroup = null, .uid = 0, .cpu_ticks = 130 },
        .{ .pid = 52, .start_ticks = 20, .cgroup = null, .uid = 0, .cpu_ticks = 5000 },
    }));
    var second = try twoSamples(before, later, .{ 100, 300 }, null);
    defer second.table.deinit(testing.allocator);
    defer second.next.deinit(testing.allocator);
    const row = rowOf(&second.table, .system_services);
    try testing.expectEqual(@as(u64, 300_000_000), row.figures.cpu_delta_ns);
    try testing.expect(row.marks.cpu_lower_bound);
}

test "an exited summed process makes the row a lower bound" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{.{ .path = "/" }};
    const before = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .uid = 0, .cpu_ticks = 100 },
        .{ .pid = 51, .start_ticks = 11, .uid = 0, .cpu_ticks = 100 },
    }));
    const after = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .uid = 0, .cpu_ticks = 110 },
    }));
    var result = try twoSamples(before, after, .{ 100, 300 }, null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const row = rowOf(&result.table, .system_services);
    try testing.expectEqual(@as(u64, 100_000_000), row.figures.cpu_delta_ns);
    try testing.expect(row.marks.cpu_lower_bound);
}

test "kernel threads give CPU and tasks and an unknown memory, never a zero" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{.{ .path = "/" }};
    const before = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 2, .ppid = 0, .kthread = true, .cgroup = "/", .cpu_ticks = 10 },
        .{ .pid = 30, .kthread = true, .cgroup = "/", .cpu_ticks = 20 },
    }));
    const after = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 2, .ppid = 0, .kthread = true, .cgroup = "/", .cpu_ticks = 15 },
        .{ .pid = 30, .kthread = true, .cgroup = "/", .cpu_ticks = 60 },
    }));
    var result = try twoSamples(before, after, .{ 100, 300 }, memoryInfo(1000, 500, 100));
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const kernel = rowOf(&result.table, .kernel);
    try testing.expect(!kernel.known.memory_anon and !kernel.known.memory_file and !kernel.known.memory_shmem);
    try testing.expectEqual(@as(u64, 0), kernel.figures.memoryBytes());
    try testing.expect(kernel.known.tasks);
    try testing.expectEqual(@as(u64, 2), kernel.figures.tasks);
    try testing.expectEqual(@as(u32, 2), kernel.figures.processes);
    try testing.expect(kernel.known.cpu_delta);
    try testing.expectEqual(@as(u64, 450_000_000), kernel.figures.cpu_delta_ns);
    try testing.expect(kernel.marks.summed and kernel.marks.cpu_lower_bound);
    // Kernel memory is in no task: it is not in the user-space residue either.
    try testing.expect(!result.table.residue.classes_incomplete);
}

test "a kernel thread that shows Rss lines still gives no memory, in its row or as an overlay" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = withApps(&.{
        .{ .path = system_tree[1].path, .anon = 100 },
    });
    const procs = try processesOf(a, &.{
        .{ .pid = 2, .ppid = 0, .cgroup = system_tree[1].path, .anon = 900, .file = 90, .shmem = 9 },
    });
    var result = try build(testing.allocator, (try World.make(a, &tree, procs)).input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const kernel = rowOf(&result.table, .kernel);
    try testing.expect(!kernel.known.memory_anon);
    try testing.expectEqual(@as(u64, 0), kernel.figures.memoryBytes());
    try testing.expectEqual(@as(u64, 0), kernel.overlay_part.memoryBytes());
    try testing.expectEqual(@as(u64, 0), rowOf(&result.table, .system_services).hosted.memoryBytes());
}

test "an agent found by ancestry inside an editor scope is an overlay counted once" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = withApps(&.{
        .{ .path = editor_scope, .anon = 10_000, .file = 2000, .cpu_us = 8000, .pids = 6, .inode = 41 },
    });
    const procs = try processesOf(a, &.{
        .{ .pid = 400, .start_ticks = 400, .ppid = 1, .comm = "codium", .cgroup = editor_scope, .anon = 4000, .cpu_ticks = 50 },
        .{ .pid = 411, .start_ticks = 500, .ppid = 400, .comm = "claude", .cgroup = editor_scope, .anon = 3000, .file = 200, .cpu_ticks = 20, .threads = 9 },
        .{ .pid = 412, .start_ticks = 501, .ppid = 411, .comm = "node", .cgroup = editor_scope, .anon = 500, .cpu_ticks = 5 },
    });
    const world = try World.make(a, &tree, procs);
    var result = try build(testing.allocator, world.input(5, 600, memoryInfo(12_000, 2000, 500)), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const agents = rowOf(table, .agent_sessions);
    try testing.expectEqual(@as(u64, 3500), agents.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 200), agents.figures.file_bytes);
    try testing.expect(agents.marks.summed and agents.marks.overlay and !agents.marks.cgroup_backed);
    try testing.expectEqual(@as(u32, 2), agents.figures.processes);
    try testing.expectEqual(@as(u64, 10), agents.figures.tasks);
    try testing.expectEqual(@as(u64, 3500), agents.overlay_part.anon_bytes);
    try testing.expect(agents.marks.file_shmem_double_counted);

    const editor_class = rowOf(table, .applications);
    try testing.expect(editor_class.marks.hosts_overlays);
    try testing.expectEqual(table.class_rows[@intFromEnum(Class.applications)], agents.host_row.?);
    // The editor's cgroup counters hold the agent's memory: they are the host's figure, not reduced.
    try testing.expectEqual(@as(u64, 10_000), editor_class.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 3500), editor_class.hosted.anon_bytes);
    try testing.expectEqual(@as(u32, 2), editor_class.hosted.processes);

    const editor = try groupRowOf(table, .applications, "com.visualstudio.CodeOSS");
    try testing.expect(editor.marks.hosts_overlays and editor.marks.cgroup_backed);
    try testing.expectEqual(@as(u64, 3500), editor.hosted.anon_bytes);
    // Only the editor's own two processes are classified in its row.
    try testing.expectEqual(@as(u32, 1), editor.figures.processes);

    // One agent instance, named by its program and identity, hosted by the editor's group row.
    const agent = try groupRowOf(table, .agent_sessions, "claude");
    try testing.expectEqual(KeyKind.agent_process, agent.key_kind);
    try testing.expectEqual(@as(u32, 411), agent.agent_pid);
    try testing.expectEqual(@as(u64, 500), agent.agent_start_ticks);
    try testing.expect(agent.marks.overlay);
    const editor_index: u32 = @intCast((@intFromPtr(editor) - @intFromPtr(table.rows.ptr)) / @sizeOf(Row));
    try testing.expectEqual(editor_index, agent.host_row.?);

    // The residue counts the agent's bytes once: the editor's cgroup alone is 12000.
    const residue = table.residue;
    try testing.expect(residue.known);
    try testing.expectEqual(@as(u64, 14_500), residue.user_space_bytes);
    try testing.expectEqual(@as(u64, 12_000), residue.classes_bytes);
    try testing.expectEqual(@as(u64, 3700), residue.overlay_bytes);
    try testing.expectEqual(@as(u64, 2500), residue.unaccounted_bytes);
}

test "an agent in its own scope is cgroup-backed, not an overlay" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const scope = alpha_slice ++ "/claude-alpha-100.scope";
    const tree = withApps(&.{
        .{ .path = user_manager ++ "/agents.slice", .anon = 900, .inode = 51 },
        .{ .path = alpha_slice, .anon = 900, .inode = 52 },
        .{ .path = scope, .anon = 900, .cpu_us = 100, .inode = 53 },
    });
    const procs = try processesOf(a, &.{
        .{ .pid = 100, .comm = "claude", .cgroup = scope, .anon = 800 },
    });
    var result = try build(testing.allocator, (try World.make(a, &tree, procs)).input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const agents = rowOf(&result.table, .agent_sessions);
    try testing.expectEqual(@as(u64, 900), agents.figures.anon_bytes);
    try testing.expect(agents.marks.cgroup_backed and !agents.marks.summed and !agents.marks.overlay);
    // The group row is the scope; the project slice is a grouping node, not a row.
    const session = try groupRowOf(&result.table, .agent_sessions, "claude-alpha-100.scope");
    try testing.expectEqual(@as(u64, 900), session.figures.anon_bytes);
    try testing.expectError(error.TestUnexpectedResult, groupRowOf(&result.table, .agent_sessions, "alpha"));
}

test "memory that could not be read is unknown or a lower bound, never a zero" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{.{ .path = "/" }};
    const procs = try processesOf(a, &.{
        .{ .pid = 50, .uid = 0, .anon = 700, .file = 10 },
        .{ .pid = 51, .uid = 0, .with_status = false },
    });
    var world = try World.make(a, &tree, procs);
    var result = try build(testing.allocator, world.input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const services = rowOf(&result.table, .system_services);
    try testing.expect(services.known.memory_anon);
    try testing.expect(services.marks.memory_lower_bound);
    try testing.expectEqual(@as(u64, 700), services.figures.anon_bytes);

    world.processes[0].status = null;
    var blind = try build(testing.allocator, world.input(5, 600, null), null);
    defer blind.table.deinit(testing.allocator);
    defer blind.next.deinit(testing.allocator);
    try testing.expect(!rowOf(&blind.table, .system_services).known.memory_anon);
}

test "the residue identity holds exactly, with unaccounted memory and with an overlap" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const world = try World.make(a, &system_tree, &.{});
    // System services hold 7100 bytes of user-space memory.
    const cases = [_]meminfo.Meminfo{
        memoryInfo(6000, 3000, 500), // 9500: 2400 unaccounted
        memoryInfo(3000, 3000, 500), // 6500: 600 overlap
        memoryInfo(5000, 2100, 0), // 7100: exact
    };
    for (cases) |info| {
        var result = try build(testing.allocator, world.input(5, 600, info), null);
        defer result.table.deinit(testing.allocator);
        defer result.next.deinit(testing.allocator);
        const residue = result.table.residue;
        try testing.expect(residue.known);
        try testing.expectEqual(@as(u64, 7100), residue.classes_bytes);
        try testing.expectEqual(residue.user_space_bytes + residue.overlap_bytes, residue.classes_bytes + residue.unaccounted_bytes);
        try testing.expect(residue.unaccounted_bytes == 0 or residue.overlap_bytes == 0);
    }
    var result = try build(testing.allocator, world.input(5, 600, memoryInfo(3000, 3000, 500)), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    try testing.expectEqual(@as(u64, 600), result.table.residue.overlap_bytes);

    // Without meminfo there is no residue to state.
    var unknown = try build(testing.allocator, world.input(5, 600, null), null);
    defer unknown.table.deinit(testing.allocator);
    defer unknown.next.deinit(testing.allocator);
    try testing.expect(!unknown.table.residue.known);
}

test "a process inside a node that holds several classes is unclassified and summed" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = withApps(&.{});
    const procs = try processesOf(a, &.{
        .{ .pid = 60, .cgroup = user_manager ++ "/app.slice", .anon = 123 },
    });
    var result = try build(testing.allocator, (try World.make(a, &tree, procs)).input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const row = rowOf(&result.table, .unclassified);
    try testing.expectEqual(@as(u64, 123), row.figures.anon_bytes);
    try testing.expect(row.marks.summed and !row.marks.overlay);
}

test "a previous sample from the same instant or later is no interval" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const world = try World.make(a, &system_tree, &.{});
    var first = try build(testing.allocator, world.input(900, 1, null), null);
    first.table.deinit(testing.allocator);
    defer first.next.deinit(testing.allocator);
    var again = try build(testing.allocator, world.input(900, 1, null), &first.next);
    defer again.table.deinit(testing.allocator);
    defer again.next.deinit(testing.allocator);
    try testing.expectEqual(@as(u64, 0), again.table.interval_ns);
    try testing.expect(!rowOf(&again.table, .system_services).known.cpu_delta);
}

test "mismatched input lengths are refused" {
    const classes: [1]classify.Classification = undefined;
    try testing.expectError(error.LengthMismatch, build(testing.allocator, .{
        .processes = &.{},
        .process_classes = &classes,
        .groups = &.{},
        .group_classes = &.{},
        .now_ns = 1,
        .now_boot_ticks = 1,
    }, null));
}

fn twoInputs(first: Input, second: Input) !Result {
    var one = try build(testing.allocator, first, null);
    one.table.deinit(testing.allocator);
    defer one.next.deinit(testing.allocator);
    return build(testing.allocator, second, &one.next);
}

fn indexIn(table: *const Table, row: *const Row) u32 {
    return @intCast((@intFromPtr(row) - @intFromPtr(table.rows.ptr)) / @sizeOf(Row));
}

const nested_tree = [_]GroupSpec{
    .{ .path = "/" },
    .{ .path = "/system.slice", .anon = 5000, .file = 2000, .shmem = 100, .cpu_us = 10_000, .pids = 4, .inode = 2 },
    .{ .path = "/system.slice/docker-abc.scope", .anon = 800, .file = 200, .cpu_us = 3000, .pids = 3, .inode = 3 },
};

test "a container's scope inside system.slice is an overlay: its row keeps its figures, system.slice hosts them, the residue counts them once" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    var later = nested_tree;
    later[1].cpu_us = 12_500;
    later[2].cpu_us = 5000;
    // 7100 bytes in System services (which already hold the scope's 1000), 400 charged to no class.
    const info = memoryInfo(5500, 2000, 0);
    var result = try twoInputs(
        (try World.make(a, &nested_tree, &.{})).input(1_000_000_000, 100, info),
        (try World.make(a, &later, &.{})).input(3_000_000_000, 300, info),
    );
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const containers = rowOf(table, .containers_and_vms);
    try testing.expectEqual(@as(u64, 800), containers.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 200), containers.figures.file_bytes);
    try testing.expectEqual(@as(u64, 2_000_000), containers.figures.cpu_delta_ns);
    try testing.expect(containers.marks.overlay and containers.marks.cgroup_backed and !containers.marks.cpu_lower_bound);
    try testing.expectEqual(table.class_rows[@intFromEnum(Class.system_services)], containers.host_row.?);
    try testing.expectEqual(@as(u64, 1000), containers.overlay_part.memoryBytes());
    try testing.expectEqual(@as(u64, 2_000_000), containers.overlay_part.cpu_delta_ns);

    // System services' own figures are system.slice's counters, untouched; the overlay is "of which".
    const services = rowOf(table, .system_services);
    try testing.expectEqual(@as(u64, 5000), services.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 2_500_000), services.figures.cpu_delta_ns);
    try testing.expect(services.marks.hosts_overlays);
    try testing.expectEqual(@as(u64, 800), services.hosted.anon_bytes);
    try testing.expectEqual(@as(u64, 200), services.hosted.file_bytes);
    try testing.expectEqual(@as(u64, 2_000_000), services.hosted.cpu_delta_ns);
    try testing.expectEqual(@as(u64, 3), services.hosted.tasks);

    // The container's own group row points at the class row that hosts it (system.slice has no group row).
    const scope = try groupRowOf(table, .containers_and_vms, "docker-abc.scope");
    try testing.expect(scope.marks.overlay);
    try testing.expectEqual(table.class_rows[@intFromEnum(Class.system_services)], scope.host_row.?);

    const residue = table.residue;
    try testing.expectEqual(@as(u64, 7100), residue.classes_bytes);
    try testing.expectEqual(@as(u64, 1000), residue.overlay_bytes);
    try testing.expectEqual(@as(u64, 7500), residue.user_space_bytes);
    try testing.expectEqual(@as(u64, 400), residue.unaccounted_bytes);
    try testing.expectEqual(@as(u64, 0), residue.overlap_bytes);
}

test "a group nested in another group's member cgroup is hosted by that group's row" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{
        .{ .path = "/" },
        .{ .path = "/system.slice", .anon = 5000 },
        .{ .path = "/system.slice/foo.service", .anon = 1000 },
        .{ .path = "/system.slice/foo.service/docker-x.scope", .anon = 300 },
    };
    var result = try build(testing.allocator, (try World.make(a, &tree, &.{})).input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const foo = try groupRowOf(table, .system_services, "foo.service");
    const scope = try groupRowOf(table, .containers_and_vms, "docker-x.scope");
    try testing.expectEqual(indexIn(table, foo), scope.host_row.?);
    try testing.expect(scope.marks.overlay and foo.marks.hosts_overlays);
    try testing.expectEqual(@as(u64, 300), foo.hosted.anon_bytes);
    // And at class level it is inside System services.
    try testing.expectEqual(@as(u64, 300), rowOf(table, .system_services).hosted.anon_bytes);
    try testing.expectEqual(@as(u64, 5000), rowOf(table, .system_services).figures.anon_bytes);
}

test "a cgroup that is new after an incomplete walk counts nothing and is a lower bound" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    var with_new = system_tree ++ [_]GroupSpec{.{ .path = "/system.slice/new.service", .cpu_us = 40_000, .inode = 99 }};
    const before = try World.make(a, &system_tree, &.{});
    const after = try World.make(a, &with_new, &.{});

    var complete_first = before.input(1_000_000_000, 100, null);
    var result = try twoInputs(complete_first, after.input(3_000_000_000, 300, null));
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const created = try groupRowOf(&result.table, .system_services, "new.service");
    try testing.expectEqual(@as(u64, 40_000_000), created.figures.cpu_delta_ns);
    try testing.expect(!created.marks.cpu_lower_bound);

    complete_first.cgroups_complete = false;
    var missed = try twoInputs(complete_first, after.input(3_000_000_000, 300, null));
    defer missed.table.deinit(testing.allocator);
    defer missed.next.deinit(testing.allocator);
    const returned = try groupRowOf(&missed.table, .system_services, "new.service");
    try testing.expectEqual(@as(u64, 0), returned.figures.cpu_delta_ns);
    try testing.expect(returned.marks.cpu_lower_bound);
    // system.slice was in both walks: still exact.
    try testing.expect(!rowOf(&missed.table, .system_services).marks.cpu_lower_bound);
}

test "a row's CPU above what the online CPUs could give counts nothing and is a lower bound" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const old = [_]GroupSpec{
        .{ .path = "/" },
        .{ .path = "/system.slice" },
        .{ .path = "/system.slice/web.service", .cpu_us = 1000, .inode = 31 },
    };
    var busy = old;
    busy[2].cpu_us = 1000 + 5_000_000; // 5 s of CPU in a 2 s interval
    var fits = old;
    fits[2].cpu_us = 1000 + 3_000_000; // 3 s: possible on 2 CPUs
    const first = (try World.make(a, &old, &.{})).input(1_000_000_000, 100, null);

    var second = (try World.make(a, &busy, &.{})).input(3_000_000_000, 300, null);
    second.online_cpus = 2;
    var over = try twoInputs(first, second);
    defer over.table.deinit(testing.allocator);
    defer over.next.deinit(testing.allocator);
    const web = try groupRowOf(&over.table, .system_services, "web.service");
    try testing.expectEqual(@as(u64, 0), web.figures.cpu_delta_ns);
    try testing.expect(web.marks.cpu_lower_bound);

    second = (try World.make(a, &fits, &.{})).input(3_000_000_000, 300, null);
    second.online_cpus = 2;
    var within = try twoInputs(first, second);
    defer within.table.deinit(testing.allocator);
    defer within.next.deinit(testing.allocator);
    const kept = try groupRowOf(&within.table, .system_services, "web.service");
    try testing.expectEqual(@as(u64, 3_000_000_000), kept.figures.cpu_delta_ns);
    try testing.expect(!kept.marks.cpu_lower_bound);

    // With the CPU count unknown nothing is bounded.
    second = (try World.make(a, &busy, &.{})).input(3_000_000_000, 300, null);
    var unbounded = try twoInputs(first, second);
    defer unbounded.table.deinit(testing.allocator);
    defer unbounded.next.deinit(testing.allocator);
    try testing.expectEqual(@as(u64, 5_000_000_000), (try groupRowOf(&unbounded.table, .system_services, "web.service")).figures.cpu_delta_ns);
}

test "CPU counts in kernel CPU lists" {
    try testing.expectEqual(@as(?u32, 16), countCpus("0-15\n"));
    try testing.expectEqual(@as(?u32, 4), countCpus("0,2-3,9"));
    try testing.expectEqual(@as(?u32, 1), countCpus("0"));
    try testing.expectEqual(@as(?u32, null), countCpus(""));
    try testing.expectEqual(@as(?u32, null), countCpus("3-1"));
    try testing.expectEqual(@as(?u32, null), countCpus("a-b"));
}

test "an overlay's CPU over two samples is in its row and in the host's hosted sums" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    var tree = withApps(&.{
        .{ .path = editor_scope, .anon = 10_000, .cpu_us = 8000, .pids = 6, .inode = 41 },
    });
    const before = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 400, .start_ticks = 400, .ppid = 1, .comm = "codium", .cgroup = editor_scope, .cpu_ticks = 50 },
        .{ .pid = 411, .start_ticks = 500, .ppid = 400, .comm = "claude", .cgroup = editor_scope, .cpu_ticks = 20 },
        .{ .pid = 412, .start_ticks = 501, .ppid = 411, .comm = "node", .cgroup = editor_scope, .cpu_ticks = 5 },
    }));
    tree[tree.len - 1].cpu_us = 1_008_000;
    const after = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 400, .start_ticks = 400, .ppid = 1, .comm = "codium", .cgroup = editor_scope, .cpu_ticks = 90 },
        .{ .pid = 411, .start_ticks = 500, .ppid = 400, .comm = "claude", .cgroup = editor_scope, .cpu_ticks = 50 },
        .{ .pid = 412, .start_ticks = 501, .ppid = 411, .comm = "node", .cgroup = editor_scope, .cpu_ticks = 15 },
    }));
    var result = try twoInputs(before.input(1_000_000_000, 600, null), after.input(3_000_000_000, 800, null));
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    // 30 + 10 ticks of the agent's two processes, 10 ms a tick.
    const agents = rowOf(table, .agent_sessions);
    try testing.expectEqual(@as(u64, 400_000_000), agents.figures.cpu_delta_ns);
    try testing.expectEqual(@as(u64, 400_000_000), agents.overlay_part.cpu_delta_ns);
    try testing.expect(agents.marks.cpu_lower_bound);
    const applications = rowOf(table, .applications);
    try testing.expectEqual(@as(u64, 400_000_000), applications.hosted.cpu_delta_ns);
    // The editor's cgroup counter is the host's own figure: 1 s.
    try testing.expectEqual(@as(u64, 1_000_000_000), applications.figures.cpu_delta_ns);
    try testing.expect(!applications.marks.cpu_lower_bound);
    const editor = try groupRowOf(table, .applications, "com.visualstudio.CodeOSS");
    try testing.expectEqual(@as(u64, 400_000_000), editor.hosted.cpu_delta_ns);
}

test "a process whose class matches its cgroup's but whose key does not is summed in its group row only" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const procs = try processesOf(a, &.{
        .{ .pid = 10, .cgroup = "/system.slice/ssh.service", .anon = 77, .file = 5 },
    });
    var world = try World.make(a, &system_tree, procs);
    world.process_classes[0] = .{
        .class = .system_services,
        .rule = classify.rule_system_service,
        .confidence = .high,
        .key_kind = .unit,
        .key = "other.service",
    };
    var result = try build(testing.allocator, world.input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const services = rowOf(table, .system_services);
    try testing.expectEqual(@as(u32, 0), services.summed_members);
    try testing.expect(!services.marks.overlay and !services.marks.hosts_overlays);
    const other = try groupRowOf(table, .system_services, "other.service");
    try testing.expectEqual(@as(u32, 1), other.summed_members);
    try testing.expectEqual(@as(u64, 77), other.figures.anon_bytes);
    const ssh = try groupRowOf(table, .system_services, "ssh.service");
    try testing.expect(other.marks.overlay and ssh.marks.hosts_overlays);
    try testing.expectEqual(indexIn(table, ssh), other.host_row.?);
    try testing.expectEqual(@as(u64, 77), ssh.hosted.anon_bytes);
    // Counted once in the residue: the class row is system.slice alone.
    try testing.expectEqual(@as(u64, 77), other.overlay_part.anon_bytes);
}

test "cgroup members that could not be measured make memory and tasks lower bounds, or unknown for a row of one" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const scope_a = user_manager ++ "/app.slice/app-org.example.A-100.scope";
    const scope_b = user_manager ++ "/app.slice/app-org.example.B-200.scope";
    const tree = withApps(&.{
        .{ .path = scope_a, .anon = 100, .file = 10, .pids = 3, .inode = 21 },
        .{ .path = scope_b, .anon = 900, .file = 90, .pids = 5, .inode = 22 },
    });
    var world = try World.make(a, &tree, &.{});
    const blind = &world.groups[world.groups.len - 1];
    blind.memory_stat = null;
    blind.pids_current = null;
    blind.memory_current_bytes = null;
    var result = try build(testing.allocator, world.input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    const table = &result.table;

    const applications = rowOf(table, .applications);
    try testing.expect(applications.known.memory_anon and applications.known.tasks);
    try testing.expect(applications.marks.memory_lower_bound and applications.marks.tasks_lower_bound);
    try testing.expectEqual(@as(u64, 100), applications.figures.anon_bytes);
    try testing.expectEqual(@as(u64, 3), applications.figures.tasks);
    try testing.expect(!applications.known.memory_current);

    const only = try groupRowOf(table, .applications, "org.example.B");
    try testing.expect(!only.known.memory_anon and !only.known.tasks);
    try testing.expect(!only.marks.memory_lower_bound);
    const measured = try groupRowOf(table, .applications, "org.example.A");
    try testing.expect(measured.known.memory_anon and measured.known.memory_current and !measured.marks.memory_lower_bound);
}

test "a process that started in the very tick of the previous sample, and is new to the row, counts nothing" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{.{ .path = "/" }};
    const before = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .uid = 0, .cpu_ticks = 100 },
    }));
    const after = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .start_ticks = 10, .uid = 0, .cpu_ticks = 100 },
        .{ .pid = 51, .start_ticks = 100, .uid = 0, .cpu_ticks = 25 }, // same tick as the first sample
        .{ .pid = 52, .start_ticks = 101, .uid = 0, .cpu_ticks = 7 }, // the tick after
    }));
    var result = try twoInputs(before.input(1_000_000_000, 100, null), after.input(3_000_000_000, 300, null));
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    // Only pid 52 is known to have started after: 7 ticks.
    try testing.expectEqual(@as(u64, 70_000_000), rowOf(&result.table, .system_services).figures.cpu_delta_ns);
}

test "a summed process whose cgroup could not be read may be double counted, one in a mixed node is not" {
    var arena: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const tree = [_]GroupSpec{ .{ .path = "/" }, .{ .path = "/user.slice" } };
    const unreadable = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 50, .uid = 0, .anon = 10 },
    }));
    var result = try build(testing.allocator, unreadable.input(5, 600, null), null);
    defer result.table.deinit(testing.allocator);
    defer result.next.deinit(testing.allocator);
    try testing.expect(rowOf(&result.table, .system_services).marks.anon_possibly_double_counted);

    const mixed = try World.make(a, &tree, try processesOf(a, &.{
        .{ .pid = 60, .cgroup = "/user.slice", .anon = 10 },
    }));
    var other = try build(testing.allocator, mixed.input(5, 600, null), null);
    defer other.table.deinit(testing.allocator);
    defer other.next.deinit(testing.allocator);
    const row = rowOf(&other.table, .unclassified);
    try testing.expect(row.marks.summed and !row.marks.anon_possibly_double_counted);
}

// ---- fuzz ----

test "fuzz: random trees and process sets never panic, nothing goes negative, the residue identity holds" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            const paths = [_][]const u8{
                "/system.slice",
                "/system.slice/ssh.service",
                "/system.slice/cron.service",
                "/system.slice/docker-abc.scope",
                "/system.slice/ssh.service/docker-x.scope",
                "/user.slice",
                "/user.slice/user-1000.slice",
                user_manager,
                user_manager ++ "/app.slice",
                editor_scope,
                user_manager ++ "/app.slice/app-org.example.B-9.scope",
                user_manager ++ "/session.slice",
                user_manager ++ "/session.slice/pipewire.service",
                user_manager ++ "/agents.slice",
                alpha_slice,
                alpha_slice ++ "/claude-alpha-100.scope",
                "/user.slice/user-1000.slice/session-c2.scope",
                "/machine.slice",
                "/machine.slice/machine-vm.scope",
                "/init.scope",
                "/unknown.slice",
            };
            const comms = [_][]const u8{ "claude", "opencode", "pi", "bash", "node", "codium" };
            const big: u64 = 1 << 30;

            var arena: std.heap.ArenaAllocator = .init(testing.allocator);
            defer arena.deinit();
            const a = arena.allocator();

            // Two samples of one randomly chosen world; the second is built over the first's state.
            var state: ?State = null;
            defer if (state) |*held| held.deinit(testing.allocator);
            var round: u32 = 0;
            while (round < 2) : (round += 1) {
                var specs: std.ArrayList(GroupSpec) = .empty;
                try specs.append(a, .{ .path = "/" });
                for (paths) |path| {
                    if (!smith.value(bool)) continue;
                    try specs.append(a, .{
                        .path = path,
                        .anon = smith.valueRangeAtMost(u64, 0, big),
                        .file = smith.valueRangeAtMost(u64, 0, big),
                        .shmem = smith.valueRangeAtMost(u64, 0, big),
                        .cpu_us = smith.valueRangeAtMost(u64, 0, big),
                        .pids = smith.valueRangeAtMost(u64, 0, 100),
                        .inode = smith.valueRangeAtMost(u64, 1, 3),
                    });
                }
                var procs: std.ArrayList(Proc) = .empty;
                const count = smith.valueRangeAtMost(u8, 0, 12);
                var pid: u32 = 0;
                while (pid < count) : (pid += 1) {
                    const comm = comms[smith.valueRangeAtMost(u8, 0, comms.len - 1)];
                    const cgroup: ?[]const u8 = switch (smith.valueRangeAtMost(u8, 0, paths.len + 1)) {
                        paths.len => null,
                        paths.len + 1 => "/no/such/group",
                        else => |choice| paths[choice],
                    };
                    try procs.append(a, .{
                        .pid = pid + 2,
                        .start_ticks = smith.valueRangeAtMost(u64, 1, 400),
                        .ppid = smith.valueRangeAtMost(u32, 0, 14),
                        .comm = comm,
                        .cgroup = cgroup,
                        .cpu_ticks = smith.valueRangeAtMost(u64, 0, 100_000),
                        .anon = smith.valueRangeAtMost(u64, 0, big),
                        .file = smith.valueRangeAtMost(u64, 0, big),
                        .shmem = smith.valueRangeAtMost(u64, 0, big),
                        .threads = smith.valueRangeAtMost(u32, 1, 50),
                        .kthread = smith.valueRangeAtMost(u8, 0, 5) == 0,
                        .uid = if (smith.value(bool)) smith.valueRangeAtMost(u32, 0, 2000) else null,
                        .with_status = smith.value(bool),
                    });
                }
                const world = try World.make(a, specs.items, try processesOf(a, procs.items));
                const info = memoryInfo(smith.valueRangeAtMost(u64, 0, 4 * big), smith.valueRangeAtMost(u64, 0, 4 * big), smith.valueRangeAtMost(u64, 0, big));
                const now_ns = 1_000_000 + @as(u64, round) * smith.valueRangeAtMost(u64, 0, 5_000_000);
                var input = world.input(now_ns, 200 + round * 100, info);
                input.online_cpus = smith.valueRangeAtMost(u32, 0, 4);
                input.cgroups_complete = smith.value(bool);
                var result = try build(testing.allocator, input, if (state) |*held| held else null);
                defer result.table.deinit(testing.allocator);
                if (state) |*held| held.deinit(testing.allocator);
                state = result.next;

                try checkTable(&result.table, world.processes.len, if (input.online_cpus > 0) result.table.interval_ns * input.online_cpus else null);
            }
        }
    }.run, .{});
}

fn checkTable(table: *const Table, process_count: usize, cpu_limit_ns: ?u64) !void {
    var classified: u64 = 0;
    for (table.class_rows, 0..) |at, number| {
        const row = table.rows[at];
        try testing.expectEqual(Level.class, row.level);
        try testing.expectEqual(number, @intFromEnum(row.class));
        classified += row.figures.processes;
    }
    // Every process is in exactly one class row.
    try testing.expectEqual(@as(u64, process_count), classified);

    for (table.rows) |row| {
        try testing.expect(row.class_row < table.rows.len);
        try testing.expectEqual(row.class, table.rows[row.class_row].class);
        // The overlay part is a part.
        try testing.expect(row.overlay_part.anon_bytes <= row.figures.anon_bytes);
        try testing.expect(row.overlay_part.file_bytes <= row.figures.file_bytes);
        try testing.expect(row.overlay_part.shmem_bytes <= row.figures.shmem_bytes);
        try testing.expect(row.overlay_part.processes <= row.figures.processes);
        if (row.host_row) |host| {
            try testing.expect(host < table.rows.len);
            try testing.expect(row.marks.overlay);
        }
        if (!row.known.cpu_delta) try testing.expectEqual(@as(u64, 0), row.figures.cpu_delta_ns);
        if (row.known.cpu_delta and row.summed_members > 0) try testing.expect(row.marks.cpu_lower_bound);
        if (row.known.cpu_delta) if (cpu_limit_ns) |limit| try testing.expect(row.figures.cpu_delta_ns <= limit);
        if (!row.known.memory_anon) try testing.expectEqual(@as(u64, 0), row.figures.memoryBytes());
        if (row.class == .kernel) try testing.expect(!row.known.memory_anon);
    }

    const residue = table.residue;
    if (!residue.known) return;
    // The identity, exactly, with at most one of the two remainders set.
    try testing.expectEqual(residue.user_space_bytes + residue.overlap_bytes, residue.classes_bytes + residue.unaccounted_bytes);
    try testing.expect(residue.unaccounted_bytes == 0 or residue.overlap_bytes == 0);
    // Overlays are left out, never added: the classes cannot exceed the plain sum of the rows.
    var plain_sum: u64 = 0;
    for (table.class_rows) |at| {
        if (table.rows[at].known.memory_anon) plain_sum += table.rows[at].figures.memoryBytes();
    }
    try testing.expectEqual(plain_sum, residue.classes_bytes + residue.overlay_bytes);
}
