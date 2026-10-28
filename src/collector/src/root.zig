// SPDX-License-Identifier: GPL-2.0-or-later
//! The collector: one record per live process from procfs, the system memory
//! partition, pressure, and the cgroup v2 tree.
//! See README.md for what is read and what is guaranteed.
pub const numbers = @import("numbers.zig");
pub const stat = @import("stat.zig");
pub const status = @import("status.zig");
pub const schedstat = @import("schedstat.zig");
pub const cgroup = @import("cgroup.zig");
pub const walk = @import("walk.zig");
pub const readfile = @import("readfile.zig");
pub const meminfo = @import("meminfo.zig");
pub const pressure = @import("pressure.zig");
pub const cgroup_files = @import("cgroup_files.zig");
pub const cgroup_tree = @import("cgroup_tree.zig");

pub const Snapshot = walk.Snapshot;
pub const Process = walk.Process;
pub const Counts = walk.Counts;
pub const File = walk.File;

test {
    @import("std").testing.refAllDecls(@This());
}
