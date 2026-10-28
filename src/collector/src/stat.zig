// SPDX-License-Identifier: GPL-2.0-or-later
//! `/proc/<pid>/stat`, field numbers and meanings from proc_pid_stat(5)
//! (man7.org/linux/man-pages/man5/proc_pid_stat.5.html). `comm` sits in
//! parentheses and may itself hold spaces and ')': the name ends at the LAST
//! ')' of the file and everything after it is space-separated. `PF_KTHREAD`
//! is 0x00200000 (include/linux/sched.h).
const std = @import("std");
const numbers = @import("numbers.zig");

pub const Error = numbers.Error;

/// fs/proc/array.c prints the name through a 64-byte buffer (63 bytes): a
/// plain task name is at most 15, but a kworker's carries its work
/// description ("kworker/4:2-events"), so 15 would reject real processes.
pub const comm_max = 63;
pub const pf_kthread: u32 = 0x0020_0000;

pub const Stat = struct {
    pid: u32,
    /// Borrowed from the parsed bytes.
    comm: []const u8,
    /// Field 3: R, S, D, Z, T, t, X, ...
    state: u8,
    ppid: u32,
    pgrp: u32,
    session: u32,
    tty_nr: i32,
    flags: u32,
    utime_ticks: u64,
    stime_ticks: u64,
    num_threads: u32,
    /// Field 22: clock ticks after boot at which the process started. With
    /// `pid` it is the process's identity: a reused pid gets a new value.
    start_ticks: u64,
};

// Space-separated tokens after the ')', counted from the state (field 3).
const token_state = 0;
const token_ppid = 1;
const token_pgrp = 2;
const token_session = 3;
const token_tty_nr = 4;
const token_flags = 6;
const token_utime = 11;
const token_stime = 12;
const token_num_threads = 17;
const token_starttime = 19;
// One token past starttime (vsize) must exist: it proves the starttime
// token ended on a space and was not cut mid-number.
const tokens_needed = token_starttime + 2;

pub fn parse(bytes: []const u8) Error!Stat {
    try numbers.requireComplete(bytes);
    const open = std.mem.indexOfScalar(u8, bytes, '(') orelse return error.Malformed;
    const close = std.mem.lastIndexOfScalar(u8, bytes, ')') orelse return error.Malformed;
    if (close < open) return error.Malformed;
    if (open < 2 or bytes[open - 1] != ' ') return error.Malformed;
    const pid = try numbers.unsigned(u32, bytes[0 .. open - 1]);
    const comm = bytes[open + 1 .. close];
    if (comm.len > comm_max) return error.Malformed;
    if (close + 1 >= bytes.len or bytes[close + 1] != ' ') return error.MissingField;

    var tokens: [tokens_needed][]const u8 = undefined;
    var count: usize = 0;
    var pieces = std.mem.tokenizeAny(u8, bytes[close + 1 ..], " \n");
    while (count < tokens_needed) : (count += 1) {
        tokens[count] = pieces.next() orelse return error.MissingField;
    }

    if (tokens[token_state].len != 1) return error.Malformed;
    return .{
        .pid = pid,
        .comm = comm,
        .state = tokens[token_state][0],
        .ppid = try numbers.unsigned(u32, tokens[token_ppid]),
        .pgrp = try numbers.unsigned(u32, tokens[token_pgrp]),
        .session = try numbers.unsigned(u32, tokens[token_session]),
        .tty_nr = try numbers.signed(i32, tokens[token_tty_nr]),
        .flags = try numbers.unsigned(u32, tokens[token_flags]),
        .utime_ticks = try numbers.unsigned(u64, tokens[token_utime]),
        .stime_ticks = try numbers.unsigned(u64, tokens[token_stime]),
        .num_threads = try numbers.unsigned(u32, tokens[token_num_threads]),
        .start_ticks = try numbers.unsigned(u64, tokens[token_starttime]),
    };
}

pub fn isKernelThread(flags: u32) bool {
    return flags & pf_kthread != 0;
}

fn expectParsed(bytes: []const u8) !Stat {
    return parse(bytes);
}

// A synthetic line: fields 4..52 are plausible values, vsize is 8192.
fn line(comptime head: []const u8) []const u8 {
    return head ++ " S 1 1 1 0 -1 4194560 100 0 2 0 7 3 0 0 20 0 4 0 987654 8192 55 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
}

test "a stat line gives identity, state, tree ids, cpu ticks and start" {
    const st = try expectParsed(line("4242 (worker)"));
    try std.testing.expectEqual(@as(u32, 4242), st.pid);
    try std.testing.expectEqualStrings("worker", st.comm);
    try std.testing.expectEqual(@as(u8, 'S'), st.state);
    try std.testing.expectEqual(@as(u32, 1), st.ppid);
    try std.testing.expectEqual(@as(i32, 0), st.tty_nr);
    try std.testing.expectEqual(@as(u32, 4194560), st.flags);
    try std.testing.expectEqual(@as(u64, 7), st.utime_ticks);
    try std.testing.expectEqual(@as(u64, 3), st.stime_ticks);
    try std.testing.expectEqual(@as(u32, 4), st.num_threads);
    try std.testing.expectEqual(@as(u64, 987654), st.start_ticks);
    try std.testing.expect(!isKernelThread(st.flags));
}

test "a comm holding spaces and parentheses is parsed from the last close parenthesis" {
    const st = try expectParsed(line("77 (a) (b c) d)"));
    try std.testing.expectEqual(@as(u32, 77), st.pid);
    try std.testing.expectEqualStrings("a) (b c) d", st.comm);
    try std.testing.expectEqual(@as(u64, 987654), st.start_ticks);
}

test "a kworker's long comm is kept whole" {
    const st = try expectParsed(line("103116 (kworker/4:2-events)"));
    try std.testing.expectEqualStrings("kworker/4:2-events", st.comm);
}

test "an empty comm is a valid name" {
    const st = try expectParsed(line("5 ()"));
    try std.testing.expectEqualStrings("", st.comm);
}

test "a kernel thread is recognised by PF_KTHREAD" {
    try std.testing.expect(isKernelThread(0x00208040));
    try std.testing.expect(!isKernelThread(0x00400100));
}

test "a stat file cut short is an error, never a smaller number" {
    const whole = line("9 (x)");
    try std.testing.expectError(error.Truncated, parse(whole[0 .. whole.len - 1]));
    // cut inside the starttime field, newline restored: vsize is gone
    const cut = "9 (x) S 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 20 0 4 0 98\n";
    try std.testing.expectError(error.MissingField, parse(cut));
    try std.testing.expectError(error.MissingField, parse("9 (x) S 1\n"));
    try std.testing.expectError(error.Truncated, parse(""));
}

test "a stat line that is not shaped like one is malformed" {
    try std.testing.expectError(error.Malformed, parse("garbage\n"));
    try std.testing.expectError(error.Malformed, parse("x (a) S\n"));
    try std.testing.expectError(error.Malformed, parse("1(a) S\n"));
    try std.testing.expectError(error.Malformed, parse(") (\n"));
    try std.testing.expectError(error.Malformed, parse("1 (" ++ "n" ** 64 ++ ") S 1\n"));
    // a state that is not one character
    try std.testing.expectError(error.Malformed, parse("1 (a) SS 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 20 0 4 0 9 8\n"));
}

test "a huge start time is an error and the largest u64 is kept" {
    const big = "1 (a) S 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 20 0 4 0 18446744073709551615 8\n";
    try std.testing.expectEqual(std.math.maxInt(u64), (try parse(big)).start_ticks);
    const over = "1 (a) S 1 1 1 0 -1 0 0 0 0 0 7 3 0 0 20 0 4 0 18446744073709551616 8\n";
    try std.testing.expectError(error.Malformed, parse(over));
}

test "fuzz: arbitrary stat bytes never panic and keep comm inside the input" {
    try std.testing.fuzz({}, struct {
        fn run(_: void, smith: *std.testing.Smith) anyerror!void {
            var input: [256]u8 = undefined;
            const length = smith.slice(&input);
            const st = parse(input[0..length]) catch return;
            try std.testing.expect(st.comm.len <= comm_max);
            try std.testing.expect(@intFromPtr(st.comm.ptr) >= @intFromPtr(&input));
        }
    }.run, .{});
}
