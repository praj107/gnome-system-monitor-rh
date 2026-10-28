// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/<pid>/cgroup`, Documentation/admin-guide/cgroup-v2.rst "Processes":
//! lines of `hierarchy-ID:controller-list:path`. The v2 (unified) line is
//! `0::/path`; v1 lines (a non-zero ID or a controller list) are ignored.
//! A removed cgroup's path carries a " (deleted)" suffix, kept verbatim.
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

/// The v2 path, borrowed from `bytes`, or null when the file has no v2 line
/// (a pure v1 system). Null is "unknown", never a root cgroup.
pub fn parse(bytes: []const u8) Error!?[]const u8 {
    try numbers.requireComplete(bytes);
    var found: ?[]const u8 = null;
    var lines = std.mem.splitScalar(u8, bytes[0 .. bytes.len - 1], '\n');
    while (lines.next()) |text| {
        const first = std.mem.indexOfScalar(u8, text, ':') orelse return error.Malformed;
        const second = std.mem.indexOfScalarPos(u8, text, first + 1, ':') orelse return error.Malformed;
        const path = text[second + 1 ..];
        if (path.len == 0 or path[0] != '/') return error.Malformed;
        const is_unified = std.mem.eql(u8, text[0..first], "0") and second == first + 1;
        if (is_unified) found = path;
    }
    return found;
}

test "the v2 line gives the cgroup path" {
    const path = (try parse("0::/user.slice/user-1000.slice/session-3.scope\n")).?;
    try std.testing.expectEqualStrings("/user.slice/user-1000.slice/session-3.scope", path);
}

test "v1 lines are ignored and the v2 line is found among them" {
    const text = "12:cpu,cpuacct:/old\n1:name=systemd:/old/sys\n0::/init.scope\n";
    try std.testing.expectEqualStrings("/init.scope", (try parse(text)).?);
}

test "a pure v1 file has no v2 path, which is unknown and not the root" {
    try std.testing.expectEqual(@as(?[]const u8, null), try parse("4:cpu:/a\n1:name=systemd:/a\n"));
}

test "the root cgroup is a path of one slash" {
    try std.testing.expectEqualStrings("/", (try parse("0::/\n")).?);
}

test "a path may hold colons, spaces and a deleted suffix" {
    const path = (try parse("0::/a:b/c d (deleted)\n")).?;
    try std.testing.expectEqualStrings("/a:b/c d (deleted)", path);
}

test "a cgroup file that is cut short or shaped wrongly is an error" {
    try std.testing.expectError(error.Truncated, parse("0::/a"));
    try std.testing.expectError(error.Truncated, parse(""));
    try std.testing.expectError(error.Malformed, parse("0:/a\n"));
    try std.testing.expectError(error.Malformed, parse("0::a\n"));
    try std.testing.expectError(error.Malformed, parse("\n"));
}

test "fuzz: arbitrary cgroup bytes never panic and the path lies inside the input" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [160]u8 = undefined;
            const length = smith.slice(&input);
            const path = (parse(input[0..length]) catch return) orelse return;
            try std.testing.expect(path.len >= 1 and path[0] == '/');
            try std.testing.expect(@intFromPtr(path.ptr) >= @intFromPtr(&input));
            try std.testing.expect(@intFromPtr(path.ptr) + path.len <= @intFromPtr(&input) + length);
        }
    }.run, .{});
}
