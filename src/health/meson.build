################################################################################
# Health: storage wear, process blame, component lifespan.

sqlite = dependency('sqlite3', version: '>=3.24')

libgsm_health_sources = [
  'gsm-battery.c',
  'gsm-blame.c',
  'gsm-drive.c',
  'gsm-drive-spec.c',
  'gsm-health-db.c',
  'gsm-history-chart.c',
  'gsm-procio.c',
  'gsm-udisks.c',
  'health.c',
]

libgsm_health_headers = [
  'gsm-battery.h',
  'gsm-blame.h',
  'gsm-drive.h',
  'gsm-drive-spec.h',
  'gsm-health-db.h',
  'gsm-history-chart.h',
  'gsm-procio.h',
  'gsm-udisks.h',
  'health.h',
]

libgsm_health_dependencies = [
  glib,
  gio_unix,
  gtk,
  libadwaita,
  sqlite,
]

libgsm_health = static_library(
  'libgsm_health',
  libgsm_health_sources + libgsm_health_headers,
  include_directories: [rootInclude, include_directories('..')],
  dependencies: libgsm_health_dependencies,
)

libgsm_health_dep = declare_dependency(
  dependencies: libgsm_health_dependencies,
  include_directories: include_directories('.'),
  link_with: libgsm_health,
)

# The privileged SMART reader. Runs as root via pkexec, reads one device, and
# exits. Talks to the hardware directly (NVMe admin ioctl, SG_IO ATA
# passthrough) rather than shelling out to smartctl, so it adds no runtime
# dependency and no shell.
executable('gsm-smart',
  ['gsm-smart-helper.c'],
  install: true,
  install_dir: get_option('libexecdir') / meson.project_name(),
)

# One-shot privileged helper that enables per-app IO accounting. Takes no
# arguments; writes two systemd drop-ins and reloads. The only thing in the
# project that touches system configuration, and only when the user asks.
executable('gsm-io-accounting',
  ['gsm-io-accounting-helper.c'],
  install: true,
  install_dir: get_option('libexecdir') / meson.project_name(),
)

# The background sampler. Strictly unprivileged; see the unit file.
executable('gsm-health-sampler',
  ['gsm-sampler.c'],
  include_directories: [rootInclude, include_directories('..')],
  dependencies: [libgsm_health_dep, glib, gio_unix, sqlite],
  install: true,
  install_dir: get_option('libexecdir') / meson.project_name(),
)
