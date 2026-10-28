// SPDX-License-Identifier: GPL-2.0-or-later
//
// Build graph of the collector: the library that turns procfs into records,
// its tests and fuzz tests, and the gsmcollect-probe measuring tool.
//
//   zig build library               only libgsmcollect.a and gsmcollect.h
//   zig build                       gsmcollect-probe into zig-out/bin, the static
//                                   library into zig-out/lib and the C header
//                                   into zig-out/include
//   zig build test                  every test, over the fixture tree in testdata/
//   zig build test --fuzz           the same, with Zig's coverage-guided fuzzer
//   zig build budget                the process walk's CPU time on the live /proc, ReleaseSafe
//   zig build run -- --once         run the probe (args after --)
//
// Optionally keep the build cache in RAM rather than on the SSD:
//   ZIG_LOCAL_CACHE_DIR=/dev/shm/gsmcollect-cache zig build test
const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The C header is the contract; the Zig side takes its types and `known`
    // bits from it (translated here), so the two cannot drift apart. The
    // translation only supplies declarations: the library itself does not link libc.
    const header = b.addTranslateC(.{
        .root_source_file = b.path("include/gsmcollect.h"),
        .target = target,
        .optimize = optimize,
        .link_libc = false,
    });

    const collector = b.addModule("gsmcollect", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        // The static library is linked into position-independent programs.
        .pic = true,
        // No libc inside the library: it talks to the kernel directly and
        // allocates with Zig's own allocator. Only memcpy/memset/getauxval
        // and TLS are left for the final link to resolve from the host's libc.
        .link_libc = false,
        .imports = &.{.{ .name = "gsm_c", .module = header.createModule() }},
    });

    const library = b.addLibrary(.{
        .name = "gsmcollect",
        .linkage = .static,
        .root_module = collector,
    });
    // The archive holds only the collector's own object, so `nm` shows gsm_*
    // and nothing else. Stack probes use a local copy of Zig's probe (see
    // c_api.zig), since compiler-rt is not bundled; the helpers it calls
    // (__divti3) come from
    // the final link's libgcc or compiler-rt.
    const install_library = b.addInstallArtifact(library, .{});
    const install_header = b.addInstallFile(b.path("include/gsmcollect.h"), "include/gsmcollect.h");
    b.getInstallStep().dependOn(&install_library.step);
    b.getInstallStep().dependOn(&install_header.step);
    // `zig build library`: just the static library and the header, which is
    // what the Meson build asks for (the probe is not needed there).
    const library_step = b.step("library", "Build libgsmcollect.a and install it with gsmcollect.h");
    library_step.dependOn(&install_library.step);
    library_step.dependOn(&install_header.step);

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

    // `zig build budget`: the overhead budgets measured on the live machine.
    // Always ReleaseSafe, which is how Meson builds the library, whatever
    // -Doptimize says; it is its own step because `test` runs in Debug over
    // fixtures. has_side_effects keeps the build system from caching a result
    // that depends on the machine's load.
    const budget_tests = b.addTest(.{
        .name = "gsmcollect-budget",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/budget.zig"),
            .target = target,
            .optimize = .ReleaseSafe,
        }),
        // The walk's own tests come along with its import; only the budget runs here.
        .filters = &.{"budget: "},
    });
    const run_budget = b.addRunArtifact(budget_tests);
    run_budget.has_side_effects = true;
    b.step("budget", "Assert the process walk's CPU time on the live /proc (ReleaseSafe)").dependOn(&run_budget.step);

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
