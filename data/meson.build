################################################################################
# Generate files

desktop_files = []

desktop_files += i18n.merge_file(
        input: configure_file(
                  input: 'org.gnome.SystemMonitor.desktop.in.in',
                 output: '@0@.desktop.in'.format(app_id),
          configuration: dataconf,
        ),
       output: '@0@.desktop'.format(app_id),
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

desktop_files += i18n.merge_file(
        input: configure_file(
                  input: 'gnome-system-monitor-kde.desktop.in.in',
                 output: 'gnome-system-monitor-kde.desktop.in',
          configuration: dataconf,
        ),
       output: 'gnome-system-monitor-kde.desktop',
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

metainfo_file = i18n.merge_file(
  input : configure_file(
  configuration: dataconf,
  input : 'org.gnome.SystemMonitor.metainfo.xml.in.in',
  output: '@0@.metainfo.xml.in'.format(app_id),
  ),
  output: '@0@.metainfo.xml'.format(app_id),
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)

i18n.merge_file(
  input : configure_file(
  configuration: dataconf,
  input : 'org.gnome.gnome-system-monitor.policy.in.in',
  output: 'org.gnome.gnome-system-monitor.policy.in'
  ),
  output: 'org.gnome.gnome-system-monitor.policy',
  po_dir: '../po',
  data_dirs: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'polkit-1', 'actions'),
)

configure_file(
  input: 'org.gnome.SystemMonitor.service.in',
  output: '@0@.service'.format(app_id),
  configuration: dataconf,
  install: true,
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services'),
)

# The wear sampler's systemd user unit. Installed but NOT enabled: the user
# turns it on from the Health tab, which is also where they are told what it
# records.
#
# systemd only searches lib/systemd/user under /usr (see systemd.unit(5),
# "Unit File Load Path"); a user prefix such as ~/.local is not on that path,
# so there the unit goes to share/systemd/user for the user to link or copy.
if get_option('prefix') in ['/usr', '/usr/local']
  sampler_unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
else
  sampler_unit_dir = join_paths(get_option('prefix'), 'share', 'systemd', 'user')
endif

configure_file(
  input: 'gnome-system-monitor-health-sampler.service.in',
  output: 'gnome-system-monitor-health-sampler.service',
  configuration: dataconf,
  install: true,
  install_dir: sampler_unit_dir,
)

# The SMART reader for USB-attached drives: a hardened oneshot service run
# every 10 minutes by a timer and by a udev rule when a USB disk appears.
# Unlike the wear sampler above, which stays opt-in and links nothing, this
# one is enabled when the package is installed (dh_installsystemd links the
# timer and starts it). It records nothing that persists (its file is in
# /run, a RAM file system), takes no input and has no network.
#
# systemd reads system units from lib/systemd/system and udev reads rules from
# lib/udev/rules.d, under /usr only. Under /usr the directories come from the
# systemd and udev pkg-config files; under any other prefix they stay inside
# the prefix so a developer install never writes to the system's directories.
if prefix == '/usr'
  systemd_dep = dependency('systemd', required: false)
  udev_dep = dependency('udev', required: false)
  smart_unit_dir = systemd_dep.found() ? systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir') : '/usr/lib/systemd/system'
  smart_rules_dir = udev_dep.found() ? udev_dep.get_variable(pkgconfig: 'udevdir') / 'rules.d' : '/usr/lib/udev/rules.d'
elif prefix == '/usr/local'
  smart_unit_dir = join_paths(prefix, 'lib', 'systemd', 'system')
  smart_rules_dir = join_paths(prefix, 'lib', 'udev', 'rules.d')
else
  smart_unit_dir = join_paths(prefix, 'share', 'systemd', 'system')
  smart_rules_dir = join_paths(prefix, 'share', 'udev', 'rules.d')
endif

configure_file(
  input: 'gsm-smart-reader.service.in',
  output: 'gsm-smart-reader.service',
  configuration: dataconf,
  install: true,
  install_dir: smart_unit_dir,
)

install_data('gsm-smart-reader.timer', install_dir: smart_unit_dir)
install_data('70-gnome-system-monitor-rh-smart.rules', install_dir: smart_rules_dir)

################################################################################
# Tests

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate_metainfo',
    appstreamcli,
    args: [ 'validate', '--no-net', '--explain', metainfo_file ],
  )
endif

desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('validate_desktop',
    desktop_validate,
    args: [ desktop_files ],
  )
endif


subdir('icons')
