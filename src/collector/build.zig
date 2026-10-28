// SPDX-License-Identifier: GPL-2.0-or-later
//
// Build graph of the collector: the library that turns procfs into records,
// its tests and fuzz tests, and the gsmcollect-probe measuring tool.
//
//   zig build                       gsmcollect-probe into zig-out/bin
//   zig build test                  every test, over the fixture tree in testdata/
//   zig build test --fuzz           the same, with Zig's coverage-guided fuzzer
//   zig build run -- --once         run the probe (args after --)
//
// Optionally keep the build cache in RAM rather than on the SSD:
//   ZIG_LOCAL_CACHE_DIR=/dev/shm/gsmcollect-cache zig build test
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const collector = b.addModule("gsmcollect", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const probe = b.addExecutable(.{
        .name = "gsmcollect-probe",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/probe.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "gsmcollect", .module = collector }},
        }),
    });
    b.installArtifact(probe);

    const run_probe = b.addRunArtifact(probe);
    if (b.args) |args| run_probe.addArgs(args);
    b.step("run", "Run gsmcollect-probe (args after --)").dependOn(&run_probe.step);

    // -Dtest-filter=SUBSTRING runs only the tests whose name contains it; with
    // `--fuzz` it picks the fuzz test to run.
    const filters = b.option([]const []const u8, "test-filter", "Run only tests whose name contains this") orelse &.{};
    // Zig 0.16.0's default test runner does not compile under --fuzz; this
    // flag swaps in a copy with the one-line fix (tools/fuzz_test_runner.zig).
    const fuzz_runner = b.option(bool, "fuzz-runner", "Use tools/fuzz_test_runner.zig (needed for --fuzz on Zig 0.16.0)") orelse false;
    const tests = b.addTest(.{
        .root_module = collector,
        .filters = filters,
        .test_runner = if (fuzz_runner) .{ .path = b.path("tools/fuzz_test_runner.zig"), .mode = .server } else null,
    });
    const run_tests = b.addRunArtifact(tests);
    // The tests open testdata/ relative to the package root.
    run_tests.setCwd(b.path("."));
    b.step("test", "Run every test").dependOn(&run_tests.step);

    // Optional: when the ztest runner is present at vendor/ztest (it is not
    // part of this repository), `zig build ztest` runs the same tests under it,
    // adding filtering, isolation, timeouts and reports. The tests are plain
    // `test` blocks either way, so `zig build test` is always the full suite.
    const ztest_runner = "vendor/ztest/src/runner.zig";
    if (fileExists(b, ztest_runner)) {
        const ztests = b.addTest(.{
            .name = "gsmcollect-ztest",
            .root_module = collector,
            .filters = filters,
            .test_runner = .{ .path = b.path(ztest_runner), .mode = .simple },
        });
        const run_ztests = b.addRunArtifact(ztests);
        run_ztests.setCwd(b.path("."));
        // With a custom runner the build system does not check the exit code
        // itself, so inherit stdio: a failing suite then fails the step.
        run_ztests.stdio = .inherit;
        if (b.args) |args| run_ztests.addArgs(args);
        b.step("ztest", "Run every test under the ztest runner in vendor/ztest").dependOn(&run_ztests.step);
    }
}

fn fileExists(b: *std.Build, sub_path: []const u8) bool {
    b.build_root.handle.access(b.graph.io, sub_path, .{}) catch return false;
    return true;
}
