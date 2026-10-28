// SPDX-License-Identifier: GPL-2.0-or-later
//! Strict number and line helpers shared by the procfs parsers. Kernel input
//! is untrusted: a field is all digits or it is an error, never
//! a partial number, and a file that does not end in a newline was cut short.
const std = @import("std");

pub const Error = error{
    /// A field is not a number, a unit is wrong, or a value does not fit.
    Malformed,
    /// A field the parser needs is absent.
    MissingField,
    /// The text does not end in a newline: the read was cut short.
    Truncated,
};

/// Every procfs file this collector reads is newline-terminated; text that
/// is not was cut short, and a cut-short number would still parse as a
/// smaller number, so it is refused before any field is read.
pub fn requireComplete(bytes: []const u8) Error!void {
    if (bytes.len == 0 or bytes[bytes.len - 1] != '\n') return error.Truncated;
}

pub fn unsigned(comptime T: type, text: []const u8) Error!T {
    if (text.len == 0) return error.Malformed;
    for (text) |digit| if (digit < '0' or digit > '9') return error.Malformed;
    return std.fmt.parseUnsigned(T, text, 10) catch error.Malformed;
}

/// Like `unsigned`, with an optional leading '-'.
pub fn signed(comptime T: type, text: []const u8) Error!T {
    const digits = if (text.len > 0 and text[0] == '-') text[1..] else text;
    if (digits.len == 0) return error.Malformed;
    for (digits) |digit| if (digit < '0' or digit > '9') return error.Malformed;
    return std.fmt.parseInt(T, text, 10) catch error.Malformed;
}

/// A /proc directory name that is a pid: digits only, no leading zero.
pub fn pidFromName(name: []const u8) ?u32 {
    if (name.len == 0 or (name.len > 1 and name[0] == '0')) return null;
    return unsigned(u32, name) catch null;
}

test "an unsigned field is all digits or an error" {
    try std.testing.expectEqual(@as(u64, 42), try unsigned(u64, "42"));
    try std.testing.expectError(error.Malformed, unsigned(u64, ""));
    try std.testing.expectError(error.Malformed, unsigned(u64, "+4"));
    try std.testing.expectError(error.Malformed, unsigned(u64, "-4"));
    try std.testing.expectError(error.Malformed, unsigned(u64, "4_0"));
    try std.testing.expectError(error.Malformed, unsigned(u8, "256"));
    try std.testing.expectError(error.Malformed, unsigned(u64, "18446744073709551616"));
    try std.testing.expectEqual(@as(u64, std.math.maxInt(u64)), try unsigned(u64, "18446744073709551615"));
}

test "a signed field takes one leading minus and nothing else" {
    try std.testing.expectEqual(@as(i32, -1), try signed(i32, "-1"));
    try std.testing.expectEqual(@as(i32, 34816), try signed(i32, "34816"));
    try std.testing.expectError(error.Malformed, signed(i32, "-"));
    try std.testing.expectError(error.Malformed, signed(i32, "--1"));
    try std.testing.expectError(error.Malformed, signed(i32, "+1"));
}

test "a pid name has digits only and no leading zero" {
    try std.testing.expectEqual(@as(?u32, 1), pidFromName("1"));
    try std.testing.expectEqual(@as(?u32, 4194304), pidFromName("4194304"));
    try std.testing.expectEqual(@as(?u32, null), pidFromName("0123"));
    try std.testing.expectEqual(@as(?u32, null), pidFromName("self"));
    try std.testing.expectEqual(@as(?u32, null), pidFromName("1a"));
    try std.testing.expectEqual(@as(?u32, null), pidFromName(""));
}

test "text without a final newline is truncated" {
    try requireComplete("1 2 3\n");
    try std.testing.expectError(error.Truncated, requireComplete("1 2 3"));
    try std.testing.expectError(error.Truncated, requireComplete(""));
}
