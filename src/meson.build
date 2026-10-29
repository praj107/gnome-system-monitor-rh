subdir('action-impls')
subdir('actions')
subdir('legacy')
subdir('systemd')
subdir('selinux')
subdir('cgroups')
subdir('health')

system_monitor_sources = [
  'application.cpp',
  'column-view-persister.c',
  'dev-ipc.c',
  'dev-ipc-rows.cpp',
  'disk.c',
  'disks.c',
  'gsm-graph.c',
  'interface.cpp',
  'load-graph.cpp',
  'lsof.cpp',
  'mem-map.c',
  'meminfo.c',
  'cputopology.c',
  'cpurate.c',
  'diskstats.c',
  'graphscale.c',
  'netif.c',
  'nvml.c',
  'procstat.c',
  'psi.c',
  'memmaps.cpp',
  'open-file.cpp',
  'openfiles.cpp',
  'prefsdialog.cpp',
  'prettytable.cpp',
  'procactions.cpp',
  'procdialogs.cpp',
  'procinfo.cpp',
  'proclist.cpp',
  'procproperties.cpp',
  'procsource.cpp',
  'proctable.cpp',
  'setaffinity.cpp',
  'smooth_refresh.cpp',
  'update_interval.cpp',
  'util.cpp',
]

system_monitor_headers = [
  'legacy/e_date.h',
  'legacy/gsm_color_button.h',
  'legacy/treeview.h',
  'application.h',
  'column-view-persister.h',
  'defaulttable.h',
  'dev-ipc.h',
  'dev-ipc-rows.h',
  'disk.h',
  'disks.h',
  'gsm-graph.h',
  'interface.h',
  'load-graph.h',
  'lsof.h',
  'mem-map.h',
  'memmaps.h',
  'open-file.h',
  'openfiles.h',
  'prefsdialog.h',
  'prettytable.h',
  'procactions.h',
  'procdialogs.h',
  'procinfo.h',
  'proclist.h',
  'procproperties.h',
  'proctable.h',
  'setaffinity.h',
  'settings-keys.h',
  'smooth_refresh.h',
  'update_interval.h',
  'util.h',
]

gresourceconf = configuration_data()
gresourceconf.set('metainfo_file', '@0@.metainfo.xml'.format(app_id))

gresource_file = configure_file(
  input: 'gsm.gresource.xml.in',
  output: 'gsm.gresource.xml',
  configuration: gresourceconf,
)

gsm_resource = gnome.compile_resources(
  'gsm-resources',
  gresource_file,
  c_name: 'gsm',
  dependencies: metainfo_file,
  source_dir: meson.project_source_root(),
)

gsm_schemas = configure_file(
  input : 'org.gnome.gnome-system-monitor.gschema.xml.in',
  output: 'org.gnome.gnome-system-monitor.gschema.xml',
  configuration: dataconf,
  install: true,
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

gnome.compile_schemas(depend_files: gsm_schemas)

gsm_gsettings = gnome.mkenums('org.gnome.gnome-system-monitor.enums.xml',
  sources: system_monitor_headers,
  comments: '<!-- @comment@ -->',
  fhead:    '<schemalist>',
  vhead:    '  <@type@ id=\'org.gnome.gnome-system-monitor.@EnumName@\'>',
  vprod:    '    <value nick=\'@valuenick@\' value=\'@valuenum@\'/>',
  vtail:    '  </@type@>',
  ftail:    '</schemalist>',
  install_header: true,
  install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas'),
)

# dlopen of the NVIDIA management library (a separate library before glibc 2.34)
dl_dep = meson.get_compiler('c').find_library('dl', required: false)
m_dep = meson.get_compiler('c').find_library('m', required: false)

system_monitor_dependencies = [
  glibmm,
  giomm,
  gio_unix,
  gmodule,
  gtk,
  gtkmm,
  libgtop,
  libadwaita,
  libgsm_actions_dep,
  libgsm_systemd_dep,
  libgsm_selinux_dep,
  libgsm_cgroups_dep,
  libgsm_health_dep,
  dl_dep,
]
# The collector is a prebuilt archive: the static libgsm only needs its
# header; whatever links libgsm_dep gets the archive itself.
libgsm = static_library(
  'libgsm',
  system_monitor_sources + [gsm_resource],
  include_directories: rootInclude,
  dependencies: system_monitor_dependencies + [libgsmcollect_dep.partial_dependency(includes: true)],
  link_with: libgsm_legacy,
)
libgsm_dep = declare_dependency(
  dependencies: system_monitor_dependencies + [libgsmcollect_dep],
  include_directories: rootInclude,
  link_with: libgsm,
)

test(
  'meminfo',
  executable(
    'test-meminfo',
    ['meminfo.c', 'tests/test-meminfo.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

test(
  'cputopology',
  executable(
    'test-cputopology',
    ['cputopology.c', 'tests/test-cputopology.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

test(
  'diskstats',
  executable(
    'test-diskstats',
    ['diskstats.c', 'tests/test-diskstats.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

test(
  'procstat',
  executable(
    'test-procstat',
    ['procstat.c', 'tests/test-procstat.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

test(
  'graphscale',
  executable(
    'test-graphscale',
    ['graphscale.c', 'tests/test-graphscale.c'],
    include_directories: rootInclude,
    dependencies: [glib, m_dep],
  ),
  protocol: 'tap',
)

test(
  'netif',
  executable(
    'test-netif',
    ['netif.c', 'tests/test-netif.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

test(
  'cpurate',
  executable(
    'test-cpurate',
    ['cpurate.c', 'tests/test-cpurate.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  protocol: 'tap',
)

test(
  'procsource',
  executable(
    'test-procsource',
    ['procsource.cpp', 'tests/test-procsource.cpp'],
    include_directories: rootInclude,
    dependencies: [glib, libgtop, libgsmcollect_dep],
  ),
  args: [meson.project_source_root() / 'src' / 'collector' / 'testdata' / 'proc'],
  protocol: 'tap',
)

test(
  'psi',
  executable(
    'test-psi',
    ['psi.c', 'tests/test-psi.c'],
    include_directories: rootInclude,
    dependencies: [glib],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures'],
  protocol: 'tap',
)

fake_nvml = shared_library(
  'fake-nvml',
  'tests/fake-nvml.c',
)

test(
  'nvml',
  executable(
    'test-nvml',
    ['nvml.c', 'tests/test-nvml.c'],
    include_directories: rootInclude,
    dependencies: [glib, dl_dep],
  ),
  args: [meson.current_source_dir() / 'tests' / 'fixtures', fake_nvml.full_path()],
  depends: fake_nvml,
  protocol: 'tap',
)

# The health sampler's write budget: a simulated hour through a counting
# SQLite VFS. Lives in src/ because it links the health library.
test(
  'health-budget',
  executable(
    'test-health-budget',
    ['tests/test-health-budget.c'],
    include_directories: rootInclude,
    dependencies: [libgsm_health_dep, glib, sqlite],
  ),
  protocol: 'tap',
  timeout: 120,
)

# Write attribution over fake cgroup trees (the tracker's root-path seam) and
# the blame table's schema migration.
test(
  'health-blame',
  executable(
    'test-health-blame',
    ['tests/test-health-blame.c'],
    include_directories: rootInclude,
    dependencies: [libgsm_health_dep, glib, sqlite],
  ),
  protocol: 'tap',
  timeout: 120,
)

# The endurance table: every row, the models that must say "not published",
# this workstation's two drives, and the wear range.
test(
  'drive-spec',
  executable(
    'test-drive-spec',
    ['tests/test-drive-spec.c'],
    include_directories: rootInclude,
    dependencies: [libgsm_health_dep, glib],
  ),
  protocol: 'tap',
  timeout: 60,
)

gsm_app = executable('gnome-system-monitor-rh',
  ['main.cpp'],
  dependencies: libgsm_dep,
  install: true,
)

# Runs the application in a private headless Weston, so it only exists in a
# developer build (the control socket it drives is compiled out otherwise) and
# skips itself when Weston is missing.
if get_option('dev_ipc')
  python = find_program('python3', required: false)
  glib_compile_schemas = find_program('glib-compile-schemas', required: false)
  if python.found() and glib_compile_schemas.found()
    test(
      'headless-crashes',
      python,
      args: [
        meson.current_source_dir() / 'tests' / 'test-headless-crashes.py',
        gsm_app.full_path(),
        meson.project_source_root() / 'tools' / 'gsm-dev-ctl',
        meson.current_build_dir(),
      ],
      depends: [gsm_app, gsm_gsettings],
      timeout: 180,
    )
  endif
endif

gnome.post_install(
  glib_compile_schemas: true,
)
