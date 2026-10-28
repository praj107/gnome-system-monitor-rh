# The collector: a Zig library that reads /proc and cgroupfs, with a C
# interface (include/gsmcollect.h). Meson builds the static library with
# `zig build` and runs the Zig tests and a C test that links the library.
# Nothing here is installed.

zig = find_program('zig', required: false)
if not zig.found()
  error('The collector needs Zig 0.16.0 on PATH (https://ziglang.org/download/); "zig" was not found.')
endif

collector_zig_version = run_command(zig, 'version', check: false).stdout().strip()
if collector_zig_version != '0.16.0'
  error('The collector needs Zig exactly 0.16.0, but "zig version" prints "' + collector_zig_version + '". Put Zig 0.16.0 first on PATH.')
endif

collector_dir = meson.current_source_dir()
collector_build_file = collector_dir / 'build.zig'

# Keep Zig's caches in the build directory, or wherever the caller already
# pointed them (a RAM disk), rather than next to the sources or in ~/.cache.
collector_cache_args = []
if run_command('sh', '-c', 'printf %s "$ZIG_LOCAL_CACHE_DIR"', check: false).stdout().strip() == ''
  collector_cache_args += ['--cache-dir', meson.current_build_dir() / 'zig-cache']
endif
if run_command('sh', '-c', 'printf %s "$ZIG_GLOBAL_CACHE_DIR"', check: false).stdout().strip() == ''
  collector_cache_args += ['--global-cache-dir', meson.current_build_dir() / 'zig-global-cache']
endif

# Every file the library is made of, so a change rebuilds it.
collector_sources = files(
  'build.zig',
  'build.zig.zon',
  'include/gsmcollect.h',
  'src/c_api.zig',
  'src/cgroup.zig',
  'src/cgroup_files.zig',
  'src/cgroup_tree.zig',
  'src/meminfo.zig',
  'src/numbers.zig',
  'src/pressure.zig',
  'src/readfile.zig',
  'src/root.zig',
  'src/schedstat.zig',
  'src/stat.zig',
  'src/status.zig',
  'src/walk.zig',
)

# A packaged binary must run on any x86-64 machine the distribution supports,
# so the library targets the baseline CPU, not the build machine's.
collector_lib = custom_target(
  'libgsmcollect',
  input: collector_sources,
  output: 'libgsmcollect.a',
  command: [
    zig, 'build', 'library',
    '--build-file', collector_build_file,
    '--prefix', '@OUTDIR@',
    '--prefix-lib-dir', '.',
    '-j4',
    '-Dtarget=x86_64-linux-gnu',
    '-Dcpu=baseline',
    '-Doptimize=ReleaseSafe',
    collector_cache_args,
  ],
)

collector_include = include_directories('include')

# For the GUI and the agent daemon to link later; nothing links it yet.
libgsmcollect_dep = declare_dependency(
  include_directories: collector_include,
  link_with: collector_lib,
  sources: collector_lib,
)

test(
  'collector-zig',
  zig,
  args: [
    'build', 'test',
    '--build-file', collector_build_file,
    '-j4',
    collector_cache_args,
  ],
  suite: 'collector-zig',
  timeout: 600,
  is_parallel: false,
)

test_collector = executable(
  'test-collector',
  'tests/c/test_collector.c',
  include_directories: collector_include,
  link_with: collector_lib,
  override_options: ['c_std=c11', 'b_lto=false'],
)

test(
  'collector-c',
  test_collector,
  args: [
    collector_dir / 'tests/c/fixture/proc',
    collector_dir / 'testdata/cgroup',
  ],
  suite: 'collector-c',
)
