// SPDX-License-Identifier: GPL-2.0-or-later
//! The unit, user unit, login session and owner uid that a cgroup path names,
//! derived from the path alone (no syscall), the way systemd derives them.
//!
//! Semantics follow src/basic/cgroup-util.c at systemd v255 (cg_path_get_unit,
//! cg_path_get_user_unit, cg_path_get_session, cg_path_get_owner_uid,
//! cg_path_get_slice, cg_unescape and the static skip_slices / skip_user_manager
//! / skip_session helpers) and the unit-name rules of src/basic/unit-name.c
//! (unit_name_is_valid, VALID_CHARS_WITH_AT), re-implemented here; systemd's
//! files are LGPL-2.1-or-later, which is GPL-compatible, and no code is copied.
//! Where this file differs, it says so:
//!  - an owner uid is plain decimal digits (systemd also takes whatever
//!    strtoul accepts); 65535 and 4294967295 are refused as systemd does;
//!  - an escape that is malformed is kept as it is when decoding, where
//!    systemd's unit_name_unescape fails.
//!
//! Every returned name is a slice of the input path; nothing is allocated and
//! nothing is decoded. A name is still escaped as the cgroup directory holds
//! it (`postgresql@16\x2dmain.service` stays so); `unescape` decodes `\xNN`
//! into a caller's buffer. A cgroup directory name that starts with `_` had
//! one underscore added by systemd to avoid kernel names; the returned names
//! have it removed (cg_unescape), so they are slices starting after it.
const std = @import("std");

/// UNIT_NAME_MAX of systemd: a unit name must be shorter than this.
pub const unit_name_max = 256;

const unit_suffixes = [_][]const u8{
    "service", "socket", "target", "device", "mount", "automount", "swap", "timer", "path", "slice", "scope",
};

/// Removes the one underscore systemd prefixes to a cgroup name that would
/// clash with a kernel name (cg_unescape).
pub fn cgUnescape(component: []const u8) []const u8 {
    if (component.len > 0 and component[0] == '_') return component[1..];
    return component;
}

fn isNameChar(byte: u8, allow_at: bool) bool {
    return std.ascii.isAlphanumeric(byte) or switch (byte) {
        ':', '-', '_', '.', '\\' => true,
        '@' => allow_at,
        else => false,
    };
}

/// unit_name_is_valid for plain (`ssh.service`) and instance
/// (`getty@tty1.service`) names; a template (`getty@.service`) is not valid.
pub fn isUnitName(name: []const u8) bool {
    if (name.len == 0 or name.len >= unit_name_max) return false;
    const dot = std.mem.lastIndexOfScalar(u8, name, '.') orelse return false;
    if (dot == 0) return false;
    const known = for (unit_suffixes) |suffix| {
        if (std.mem.eql(u8, name[dot + 1 ..], suffix)) break true;
    } else false;
    if (!known) return false;

    var at: ?usize = null;
    for (name[0..dot], 0..) |byte, i| {
        if (byte == '@' and at == null) at = i;
        if (!isNameChar(byte, true)) return false;
    }
    const at_index = at orelse return true;
    if (at_index == 0) return false;
    return dot > at_index + 1;
}

fn endsWith(text: []const u8, suffix: []const u8) bool {
    return std.mem.endsWith(u8, text, suffix);
}

/// valid_slice_name: at least `x.slice`, ending in `.slice`, a valid plain
/// unit name once unescaped (a `@` makes it an instance, which is refused).
pub fn isSliceName(component: []const u8) bool {
    if (component.len < "x.slice".len) return false;
    if (!endsWith(component, ".slice")) return false;
    const name = cgUnescape(component);
    return isUnitName(name) and std.mem.indexOfScalar(u8, name, '@') == null;
}

/// The part of `path` that starts at the first component that is not a
/// slice (skip_slices): `/user.slice/user-1000.slice/session-c2.scope/x`
/// gives `session-c2.scope/x`. Empty when the path is slices only.
pub fn skipSlices(path: []const u8) []const u8 {
    var rest = path;
    while (true) {
        rest = std.mem.trimStart(u8, rest, "/");
        const end = std.mem.indexOfScalar(u8, rest, '/') orelse rest.len;
        if (!isSliceName(rest[0..end])) return rest;
        rest = rest[end..];
    }
}

fn firstComponent(path: []const u8) []const u8 {
    const trimmed = std.mem.trimStart(u8, path, "/");
    return trimmed[0 .. std.mem.indexOfScalar(u8, trimmed, '/') orelse trimmed.len];
}

/// cg_path_decode_unit: the first component, as a valid unit name, or null.
fn decodeUnit(path: []const u8) ?[]const u8 {
    const component = firstComponent(path);
    if (component.len < 3) return null;
    const name = cgUnescape(component);
    if (!isUnitName(name)) return null;
    return name;
}

/// cg_path_get_unit: the unit that owns the path, the first component after
/// the slices. `/system.slice/system-postgresql.slice/postgresql@16-main.service/x`
/// gives `postgresql@16-main.service`; a user's processes give
/// `user@1000.service` or `session-3.scope`. Null for the root, a slice, or
/// a name that is no unit.
pub fn unit(path: []const u8) ?[]const u8 {
    const name = decodeUnit(skipSlices(path)) orelse return null;
    if (endsWith(name, ".slice")) return null;
    return name;
}

/// skip_user_manager: after the slices, a `user@UID.service` component, and
/// what follows it (empty when the path ends there). Null when the path has
/// no user manager.
pub fn userManagerRest(path: []const u8) ?[]const u8 {
    const rest = std.mem.trimStart(u8, skipSlices(path), "/");
    const component = firstComponent(rest);
    if (component.len < "user@x.service".len) return null;
    if (!std.mem.startsWith(u8, component, "user@") or !endsWith(component, ".service")) return null;
    _ = parseUid(component["user@".len .. component.len - ".service".len]) orelse return null;
    return std.mem.trimStart(u8, rest[component.len..], "/");
}

/// session_id_valid: letters and digits only, not empty.
fn isSessionId(id: []const u8) bool {
    if (id.len == 0) return false;
    for (id) |byte| if (!std.ascii.isAlphanumeric(byte)) return false;
    return true;
}

/// skip_session: after the slices, a `session-ID.scope` component, and what
/// follows it.
fn sessionRest(path: []const u8) ?[]const u8 {
    const rest = std.mem.trimStart(u8, skipSlices(path), "/");
    const component = firstComponent(rest);
    if (component.len < "session-x.scope".len) return null;
    if (!std.mem.startsWith(u8, component, "session-") or !endsWith(component, ".scope")) return null;
    if (!isSessionId(component["session-".len .. component.len - ".scope".len])) return null;
    return std.mem.trimStart(u8, rest[component.len..], "/");
}

/// cg_path_get_user_unit: the unit inside a user manager or a login session,
/// the one `systemctl --user` would show. `…/user@1000.service/app.slice/app-x-1.scope`
/// gives `app-x-1.scope`. Null outside a user manager or session.
pub fn userUnit(path: []const u8) ?[]const u8 {
    const after = userManagerRest(path) orelse sessionRest(path) orelse return null;
    return unit(after);
}

/// cg_path_get_session: the login session id of `…/session-ID.scope`.
pub fn session(path: []const u8) ?[]const u8 {
    const name = unit(path) orelse return null;
    if (!std.mem.startsWith(u8, name, "session-") or !endsWith(name, ".scope")) return null;
    const id = name["session-".len .. name.len - ".scope".len];
    if (!isSessionId(id)) return null;
    return id;
}

/// parse_uid: decimal digits that fit a uid, and are not the two values
/// Linux reserves for "none" (65535 and 4294967295).
pub fn parseUid(text: []const u8) ?u32 {
    if (text.len == 0) return null;
    for (text) |byte| if (!std.ascii.isDigit(byte)) return null;
    const uid = std.fmt.parseInt(u32, text, 10) catch return null;
    if (uid == 0xFFFF or uid == std.math.maxInt(u32)) return null;
    return uid;
}

/// cg_path_get_slice: the right-most slice among the leading slices, or null
/// when the path begins with a non-slice (systemd answers `-.slice`).
pub fn innermostSlice(path: []const u8) ?[]const u8 {
    var found: ?[]const u8 = null;
    var rest = path;
    while (true) {
        rest = std.mem.trimStart(u8, rest, "/");
        const end = std.mem.indexOfScalar(u8, rest, '/') orelse rest.len;
        if (!isSliceName(rest[0..end])) return if (found) |slice| cgUnescape(slice) else null;
        found = rest[0..end];
        rest = rest[end..];
    }
}

/// cg_path_get_owner_uid: the uid of the innermost `user-UID.slice`.
pub fn ownerUid(path: []const u8) ?u32 {
    const slice = innermostSlice(path) orelse return null;
    if (!std.mem.startsWith(u8, slice, "user-") or !endsWith(slice, ".slice")) return null;
    return parseUid(slice["user-".len .. slice.len - ".slice".len]);
}

/// Decodes the `\xNN` escapes of a unit name into `buffer` and returns the
/// decoded bytes, or null when `buffer` is too small. (`-` stays a hyphen:
/// it separates words in a unit name; it is only a path separator in the
/// name of a slice.)
pub fn unescape(buffer: []u8, name: []const u8) ?[]const u8 {
    var out: usize = 0;
    var i: usize = 0;
    while (i < name.len) : (out += 1) {
        if (out == buffer.len) return null;
        if (name[i] == '\\' and i + 3 < name.len and name[i + 1] == 'x') {
            const high = std.fmt.charToDigit(name[i + 2], 16) catch null;
            const low = std.fmt.charToDigit(name[i + 3], 16) catch null;
            if (high != null and low != null) {
                buffer[out] = high.? * 16 + low.?;
                i += 4;
                continue;
            }
        }
        buffer[out] = name[i];
        i += 1;
    }
    return buffer[0..out];
}

const testing = std.testing;

fn expectUnit(expected: ?[]const u8, path: []const u8) !void {
    const got = unit(path);
    if (expected) |text| {
        try testing.expectEqualStrings(text, got orelse return error.TestUnexpectedResult);
    } else {
        try testing.expectEqual(@as(?[]const u8, null), got);
    }
}

test "the unit of a path is its first component after the slices" {
    try expectUnit("ssh.service", "/system.slice/ssh.service");
    try expectUnit("postgresql@16-main.service", "/system.slice/system-postgresql.slice/postgresql@16-main.service");
    try expectUnit("systemd-udevd.service", "/system.slice/systemd-udevd.service/udev");
    try expectUnit("user@1000.service", "/user.slice/user-1000.slice/user@1000.service/app.slice/app-x-1.scope");
    try expectUnit("session-c2.scope", "/user.slice/user-1000.slice/session-c2.scope");
    try expectUnit("init.scope", "/init.scope");
    try expectUnit("sys-kernel-debug.mount", "/sys-kernel-debug.mount");
}

test "escaped names stay escaped and are valid units" {
    try expectUnit("dev-disk-by\\x2duuid-1.device", "/system.slice/dev-disk-by\\x2duuid-1.device");
    try expectUnit(null, "/system.slice/system-systemd\\x2dfsck.slice");
}

test "the root, a slice and a name that is no unit have no unit" {
    try expectUnit(null, "/");
    try expectUnit(null, "");
    try expectUnit(null, "/system.slice");
    try expectUnit(null, "/user.slice/user-1000.slice");
    try expectUnit(null, "/system.slice/ab");
    try expectUnit(null, "/system.slice/x.unknown");
    try expectUnit(null, "/system.slice/@bad.service");
    try expectUnit(null, "/system.slice/bad name.service");
    try expectUnit(null, "/system.slice/getty@.service");
    try expectUnit(null, "/system.slice/foo@bar.slice");
    try expectUnit(null, "/openrc");
}

test "a leading underscore that systemd adds to a cgroup name is removed" {
    try expectUnit("memory.service", "/system.slice/_memory.service");
    try expectUnit("cpu.slice.service", "/_cpu.slice.service");
}

test "repeated and trailing slashes do not change the answer" {
    try expectUnit("ssh.service", "//system.slice///ssh.service//");
}

fn expectUserUnit(expected: ?[]const u8, path: []const u8) !void {
    const got = userUnit(path);
    if (expected) |text| {
        try testing.expectEqualStrings(text, got orelse return error.TestUnexpectedResult);
    } else {
        try testing.expectEqual(@as(?[]const u8, null), got);
    }
}

test "the user unit is the unit inside the user manager or the login session" {
    const manager = "/user.slice/user-1000.slice/user@1000.service";
    try expectUserUnit("app-x-1.scope", manager ++ "/app.slice/app-x-1.scope");
    try expectUserUnit("dbus.service", manager ++ "/session.slice/dbus.service/child");
    try expectUserUnit("vte-spawn-abc.scope", manager ++ "/app.slice/app-org.gnome.Terminal.slice/vte-spawn-abc.scope");
    try expectUserUnit("foo.service", "/user.slice/user-1000.slice/session-c2.scope/foo.service");
}

test "there is no user unit outside a user manager or session, or when none is named" {
    try expectUserUnit(null, "/system.slice/ssh.service");
    try expectUserUnit(null, "/user.slice/user-1000.slice/user@1000.service");
    try expectUserUnit(null, "/user.slice/user-1000.slice/user@1000.service/app.slice");
    try expectUserUnit(null, "/user.slice/user-1000.slice/session-c2.scope");
    try expectUserUnit(null, "/user.slice/user-1000.slice/user@abc.service/app.slice/x.service");
}

test "the login session id is read from session-ID.scope" {
    try testing.expectEqualStrings("c2", session("/user.slice/user-1000.slice/session-c2.scope").?);
    try testing.expectEqualStrings("3", session("/user.slice/user-1000.slice/session-3.scope/leaf").?);
    try testing.expectEqual(@as(?[]const u8, null), session("/user.slice/user-1000.slice/session-.scope"));
    try testing.expectEqual(@as(?[]const u8, null), session("/user.slice/user-1000.slice/session-a_b.scope"));
    try testing.expectEqual(@as(?[]const u8, null), session("/user.slice/user-1000.slice/user@1000.service"));
    try testing.expectEqual(@as(?[]const u8, null), session("/system.slice/ssh.service"));
}

test "the owner uid is that of the innermost user slice" {
    try testing.expectEqual(@as(?u32, 1000), ownerUid("/user.slice/user-1000.slice/user@1000.service/app.slice/x.scope"));
    try testing.expectEqual(@as(?u32, 0), ownerUid("/user.slice/user-0.slice/session-1.scope"));
    try testing.expectEqual(@as(?u32, 1000), ownerUid("/user.slice/user-1000.slice"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/system.slice/ssh.service"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/user.slice/user-65535.slice/x.scope"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/user.slice/user-4294967295.slice/x.scope"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/user.slice/user-99999999999.slice/x.scope"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/user.slice/user--1.slice/x.scope"));
    try testing.expectEqual(@as(?u32, null), ownerUid("/user.slice/user-abc.slice/x.scope"));
}

test "a user manager is found only when its uid is a number" {
    const rest = userManagerRest("/user.slice/user-1000.slice/user@1000.service/app.slice/x.scope").?;
    try testing.expectEqualStrings("app.slice/x.scope", rest);
    try testing.expectEqualStrings("", userManagerRest("/user.slice/user-1000.slice/user@1000.service").?);
    try testing.expectEqual(@as(?[]const u8, null), userManagerRest("/user.slice/user-1000.slice/user@x.service/a"));
    try testing.expectEqual(@as(?[]const u8, null), userManagerRest("/system.slice/ssh.service"));
}

test "\\xNN escapes are decoded into the caller's buffer" {
    var buffer: [64]u8 = undefined;
    try testing.expectEqualStrings("system-systemd-fsck.slice", unescape(&buffer, "system-systemd\\x2dfsck.slice").?);
    try testing.expectEqualStrings("machine-qemu-1-vm1.scope", unescape(&buffer, "machine-qemu\\x2d1\\x2dvm1.scope").?);
    try testing.expectEqualStrings("plain.service", unescape(&buffer, "plain.service").?);
}

test "a malformed escape is kept as written and a small buffer is refused" {
    var buffer: [64]u8 = undefined;
    try testing.expectEqualStrings("a\\xZZb\\x2", unescape(&buffer, "a\\xZZb\\x2").?);
    var small: [4]u8 = undefined;
    try testing.expectEqual(@as(?[]const u8, null), unescape(&small, "longer-than-four"));
    try testing.expectEqualStrings("", unescape(&small, "").?);
}

fn insideInput(input: []const u8, part: []const u8) bool {
    const start = @intFromPtr(part.ptr);
    const base = @intFromPtr(input.ptr);
    return start >= base and start + part.len <= base + input.len;
}

test "fuzz: arbitrary path bytes never panic and every name lies inside the path" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [160]u8 = undefined;
            const length = smith.slice(&input);
            const path = input[0..length];
            if (unit(path)) |name| {
                try testing.expect(insideInput(path, name));
                try testing.expect(isUnitName(name));
                try testing.expect(!endsWith(name, ".slice"));
            }
            if (userUnit(path)) |name| try testing.expect(insideInput(path, name));
            if (session(path)) |id| try testing.expect(insideInput(path, id));
            if (userManagerRest(path)) |rest| try testing.expect(insideInput(path, rest));
            _ = ownerUid(path);
            var decoded: [64]u8 = undefined;
            _ = unescape(&decoded, path);
        }
    }.run, .{});
}
