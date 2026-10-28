// SPDX-License-Identifier: GPL-2.0-or-later
//! One small kernel file, read with a single pread into a caller's buffer.
//! procfs and cgroupfs files are produced whole by one read; a file that
//! fills the buffer was cut off and is refused rather than parsed as if whole.
//! Shared by the system and cgroup readers; the process walk keeps its own
//! reader because a vanished process is a different outcome there.
const std = @import("std");
const Io = std.Io;

pub const Outcome = union(enum) {
    /// The file's bytes (possibly none: the parser decides what empty means).
    bytes: []const u8,
    /// The file does not exist: a controller not enabled, a kernel without
    /// the feature, or a cgroup that was removed. The caller tells which.
    absent,
    /// It exists but could not be read (EACCES, EIO, ENODEV, an unsupported
    /// PSI file...).
    unreadable,
    /// The content filled the whole buffer.
    too_long,
};

pub fn read(io: Io, dir: Io.Dir, sub_path: []const u8, buffer: []u8) Outcome {
    const handle = dir.openFile(io, sub_path, .{}) catch |err| switch (err) {
        error.FileNotFound => return .absent,
        else => return .unreadable,
    };
    defer handle.close(io);

    const length = handle.readPositional(io, &.{buffer}, 0) catch return .unreadable;
    if (length == buffer.len) return .too_long;
    return .{ .bytes = buffer[0..length] };
}

const testing = std.testing;

test "a file is read whole, a missing one is absent, a directory is unreadable, a full buffer is too long" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(testing.io, .{ .sub_path = "small", .data = "12\n" });
    try tmp.dir.writeFile(testing.io, .{ .sub_path = "big", .data = "0123456789\n" });
    try tmp.dir.createDirPath(testing.io, "dir");

    var buffer: [8]u8 = undefined;
    try testing.expectEqualStrings("12\n", read(testing.io, tmp.dir, "small", &buffer).bytes);
    try testing.expect(read(testing.io, tmp.dir, "nothing", &buffer) == .absent);
    try testing.expect(read(testing.io, tmp.dir, "dir", &buffer) == .unreadable);
    try testing.expect(read(testing.io, tmp.dir, "big", &buffer) == .too_long);
}
