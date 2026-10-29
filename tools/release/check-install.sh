#!/usr/bin/env bash
# check-install.sh — is the installed package the app that runs, and is it
# installed the way it was designed to be? Read-only, needs no sudo.
#
#   check-install.sh [--version VERSION] [--sysroot DIR] [--home DIR]
#
# Checks, each printed as "ok:" or "FAILED:"; the exit status is 1 when any
# failed:
#   - the package is installed (at VERSION, when given) and the
#     distribution's gnome-system-monitor is not;
#   - gnome-system-monitor resolves to /usr/bin;
#   - polkit's actions name the helpers in /usr/libexec/gnome-system-monitor;
#   - the system D-Bus service file runs /usr/bin;
#   - the health sampler's user unit is not enabled for every user (no link to
#     it anywhere under /etc/systemd/user: it is opt-in, enabled per user from
#     the Health tab);
#   - the SMART reader for USB drives is installed as designed: the reader
#     under the helpers' directory, its service and timer, the udev rule, the
#     timer enabled (a link in timers.target.wants; live, also running), the
#     service static (no [Install]), and nothing of the retired pkexec SMART
#     helper (its binary or its polkit action);
#   - nothing in the user's own data directory shadows the package: no
#     glib-2.0/schemas/gschemas.compiled (a stale one aborts the app on a
#     missing key), no D-Bus service file, no desktop file of the app;
#   - /usr/bin/gnome-system-monitor-rh --help runs.
#
# --sysroot DIR looks for files below DIR instead of /; the checks that can
# only be made against the running system (dpkg's database, PATH, pkaction,
# running the binary) are made when DIR has a dpkg database or are skipped
# with a note. --home DIR is the home whose data directory is checked.
#
# Settings: release.conf.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
sysroot=""; home_dir="${HOME:-}"; version=""
while (($#)); do
    case "$1" in
        --version) version="${2:?--version needs a version}"; shift 2 ;;
        --sysroot) sysroot="${2:?--sysroot needs a directory}"; shift 2 ;;
        --home) home_dir="${2:?--home needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'check-install.sh: unknown argument: %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
# shellcheck source=release.conf
source "$here/release.conf"
[[ -z "$sysroot" || ( "$sysroot" == /* && -d "$sysroot" ) ]] || die "--sysroot must be an existing absolute directory"
[[ "$home_dir" == /* && -d "$home_dir" ]] || die "--home must be an existing absolute directory"

fail=0
ok()   { say "    ok: $*"; }
bad()  { say "    FAILED: $*"; fail=1; }
note() { say "    skipped: $*"; }

unit=gnome-system-monitor-health-sampler.service
dpkg_query=(dpkg-query)
have_db=true
if [[ -n "$sysroot" ]]; then
    if [[ -d "$sysroot/var/lib/dpkg" ]]; then dpkg_query=(dpkg-query --admindir "$sysroot/var/lib/dpkg"); else have_db=false; fi
fi

# --- packages
if "$have_db"; then
    installed="$("${dpkg_query[@]}" -W -f='${Version}' "$GSM_RH_PACKAGE" 2>/dev/null || true)"
    if [[ -z "$installed" ]]; then bad "$GSM_RH_PACKAGE is not installed"
    elif [[ -n "$version" && "$installed" != "$version" ]]; then bad "$GSM_RH_PACKAGE $installed is installed, not $version"
    else ok "$GSM_RH_PACKAGE $installed installed"; fi
    if "${dpkg_query[@]}" -W -f='${Status}' "$GSM_RH_DISTRO_PACKAGE" 2>/dev/null | grep -q 'ok installed'; then
        bad "$GSM_RH_DISTRO_PACKAGE is still installed"
    else ok "$GSM_RH_DISTRO_PACKAGE is gone"; fi
else
    note "no dpkg database under $sysroot: package state not checked"
fi

# --- what runs
if [[ -z "$sysroot" ]]; then
    hash -r
    found="$(command -v gnome-system-monitor || true)"
    if [[ "$found" == /usr/bin/gnome-system-monitor ]]; then ok "gnome-system-monitor resolves to $found"
    else bad "gnome-system-monitor resolves to '${found:-nothing}', not /usr/bin"; fi
else
    note "PATH lookup is not made under --sysroot"
fi

policy="$sysroot/usr/share/polkit-1/actions/org.gnome.gnome-system-monitor.policy"
if [[ ! -f "$policy" ]]; then
    bad "no polkit policy at $policy"
else
    while read -r action; do
        if [[ -z "$sysroot" ]]; then
            exec_path="$(pkaction --verbose --action-id "$action" 2>/dev/null | sed -n 's/.*exec\.path -> //p')"
        else
            exec_path="$(sed -n "/<action id=\"$action\"/,/<\/action>/ s|.*exec\.path\">\(.*\)</annotate>.*|\1|p" "$policy" | head -n 1)"
        fi
        if [[ "$exec_path" == /usr/libexec/gnome-system-monitor/* ]]; then ok "polkit $action -> $exec_path"
        else bad "polkit $action -> '${exec_path:-unknown}'"; fi
    done < <(sed -n 's/.*<action id="\([^"]*\)".*/\1/p' "$policy")
fi

service="$sysroot/usr/share/dbus-1/services/org.gnome.SystemMonitor.service"
if grep -q '^Exec=/usr/bin/gnome-system-monitor' "$service" 2>/dev/null; then ok "the system D-Bus service file runs /usr/bin"
else bad "the system D-Bus service file does not run /usr/bin ($service)"; fi

# --- the sampler is opt-in
links="$(find "$sysroot/etc/systemd/user" -name "$unit" -type l 2>/dev/null || true)"
if [[ -z "$links" ]]; then ok "the health sampler is not enabled system-wide (no link under /etc/systemd/user)"
else bad "the health sampler is enabled system-wide, so the Health tab's off switch cannot keep it off: $(echo $links)"; fi

# --- the SMART reader service
libexec="$sysroot/usr/libexec/gnome-system-monitor"
units_dir="$sysroot/usr/lib/systemd/system"
reader_service="$units_dir/gsm-smart-reader.service"
reader_timer="$units_dir/gsm-smart-reader.timer"
reader_rule="$sysroot/usr/lib/udev/rules.d/70-gnome-system-monitor-rh-smart.rules"
if [[ -x "$libexec/gsm-smart-reader" ]]; then ok "the SMART reader is installed ($libexec/gsm-smart-reader)"
else bad "no executable SMART reader at $libexec/gsm-smart-reader"; fi
if [[ -f "$reader_service" ]] && grep -qx 'ExecStart=/usr/libexec/gnome-system-monitor/gsm-smart-reader' "$reader_service"; then
    ok "the SMART reader service runs the installed reader"
else bad "no SMART reader service at $reader_service that runs /usr/libexec/gnome-system-monitor/gsm-smart-reader"; fi
if [[ -f "$reader_service" ]] && ! grep -q '^\[Install\]' "$reader_service"; then ok "the SMART reader service is static (started by its timer and udev only)"
else bad "the SMART reader service has an [Install] section or is missing"; fi
if [[ -f "$reader_timer" ]] && grep -qx 'WantedBy=timers.target' "$reader_timer" && grep -qx 'OnUnitActiveSec=10min' "$reader_timer"; then
    ok "the SMART reader timer is installed (every 10 minutes)"
else bad "no SMART reader timer at $reader_timer (WantedBy=timers.target, OnUnitActiveSec=10min)"; fi
if grep -q 'SYSTEMD_WANTS}+="gsm-smart-reader.service"' "$reader_rule" 2>/dev/null; then ok "the udev rule starts the SMART reader for a USB disk"
else bad "no udev rule that starts gsm-smart-reader.service at $reader_rule"; fi
# The timer is enabled by the package's install script (a link in
# timers.target.wants). An administrator may have disabled it on purpose;
# that is reported, not hidden.
timer_link="$sysroot/etc/systemd/system/timers.target.wants/gsm-smart-reader.timer"
if [[ -L "$timer_link" ]]; then ok "the SMART reader timer is enabled ($timer_link)"
else bad "the SMART reader timer is not enabled: no link at $timer_link (the package enables it on install; systemctl disable --now gsm-smart-reader.timer removes it)"; fi
if [[ -z "$sysroot" ]]; then
    if systemctl is-active --quiet gsm-smart-reader.timer 2>/dev/null; then ok "the SMART reader timer is running"
    else bad "the SMART reader timer is not running"; fi
else
    note "whether the timer is running is not checked under --sysroot"
fi
# The pkexec SMART helper is gone, with its polkit action.
if [[ -e "$libexec/gsm-smart" ]]; then bad "the retired pkexec SMART helper is still installed: $libexec/gsm-smart"
else ok "no pkexec SMART helper (gsm-smart)"; fi
if grep -q 'org.gnome.gnome-system-monitor.smart"' "$policy" 2>/dev/null; then bad "polkit still has the retired SMART action org.gnome.gnome-system-monitor.smart"
else ok "polkit has no SMART action"; fi

# --- nothing shadows the package
data="$home_dir/.local/share"
shadow=0
for f in "$data/glib-2.0/schemas/gschemas.compiled" "$data/dbus-1/services/org.gnome.SystemMonitor.service" \
        "$data/applications/org.gnome.SystemMonitor.desktop" "$data/applications/gnome-system-monitor-kde.desktop"; do
    if [[ -e "$f" || -L "$f" ]]; then bad "$f shadows the package"; shadow=1; fi
done
((shadow)) || ok "nothing under $data shadows the package (schemas, D-Bus service, desktop files)"

# --- it runs
if [[ -z "$sysroot" ]]; then
    if timeout 20 /usr/bin/gnome-system-monitor-rh --help >/dev/null 2>&1; then ok "/usr/bin/gnome-system-monitor-rh --help runs"
    else bad "/usr/bin/gnome-system-monitor-rh --help"; fi
else
    note "the binary is not run under --sysroot"
fi

if ((fail)); then say ""; say "Some checks failed."; exit 1; fi
say ""; say "All checks passed."
