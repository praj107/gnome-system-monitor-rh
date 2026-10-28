// SPDX-License-Identifier: GPL-2.0-or-later
//! The collector's first slice: one record per live process from procfs.
//! See README.md for what is read and what is guaranteed.
pub const numbers = @import("numbers.zig");
pub const stat = @import("stat.zig");
pub const status = @import("status.zig");
pub const schedstat = @import("schedstat.zig");
pub const cgroup = @import("cgroup.zig");
pub const walk = @import("walk.zig");

pub const Snapshot = walk.Snapshot;
pub const Process = walk.Process;
pub const Counts = walk.Counts;
pub const File = walk.File;

test {
    @import("std").testing.refAllDecls(@This());
}
