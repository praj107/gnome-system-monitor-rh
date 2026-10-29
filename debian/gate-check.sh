#!/bin/sh
# Release gates for debian/rules. Fails the build, naming the gate.
#   gate-check.sh configured BUILDDIR   meson was configured as a release build without dev IPC,
#                                       and its project version is the changelog's
#   gate-check.sh symbols ROOT          nothing installed under ROOT carries dev IPC
#   gate-check.sh units ROOT            the SMART reader's units and udev rule under ROOT are
#                                       valid, run an installed executable, and its service scores
#                                       at most 2.0 in systemd-analyze security (a hardening gate)
set -eu

fail() { echo "gate-check: GATE FAILED ($1): $2" >&2; exit 1; }

configured() {
    builddir=$1
    options=$(meson introspect --buildoptions "$builddir")
    for pair in dev_ipc=false development=false buildtype=release; do
        name=${pair%%=*}
        want=${pair#*=}
        got=$(printf '%s' "$options" | python3 -I -c '
import json, sys
name = sys.argv[1]
for option in json.load(sys.stdin):
    if option["name"] == name:
        print(str(option["value"]).lower())
        break
' "$name")
        [ "$got" = "$want" ] || fail "$name" "configured as '$got', a release needs '$want'"
    done
    echo "gate-check: dev_ipc gate passed (dev_ipc=false, development=false, buildtype=release)"

    # The binary reports meson's project version (--version, About); the
    # package is named by debian/changelog. They must say the same release,
    # a development build's ~dev suffix aside.
    package=$(dpkg-parsechangelog -S Version)
    project=$(meson introspect --projectinfo "$builddir" | python3 -I -c '
import json, sys
print(json.load(sys.stdin)["version"])
')
    [ "$project" = "${package%%~*}" ] || fail version "meson.build says '$project', debian/changelog says '$package'"
    echo "gate-check: version gate passed ($project)"
}

symbols() {
    root=$1
    checked=0
    for file in "$root"/usr/bin/* "$root"/usr/libexec/*/*; do
        [ -f "$file" ] && [ ! -L "$file" ] || continue
        case $(head -c 4 "$file" | od -An -c | tr -d ' ') in
            '177ELF') ;;
            *) continue ;;
        esac
        checked=$((checked + 1))
        # Both tables: -D is the dynamic table, the plain one catches local symbols.
        if { nm -D --defined-only "$file"; nm "$file" 2>/dev/null || true; } | grep -qi 'dev_ipc'; then
            fail dev_ipc "$file defines a dev_ipc symbol"
        fi
        if grep -aq 'GSM_DEV_IPC' "$file"; then
            fail dev_ipc "$file contains the GSM_DEV_IPC switch"
        fi
    done
    [ "$checked" -gt 0 ] || fail dev_ipc "no ELF files found under $root, nothing was checked"
    echo "gate-check: dev_ipc symbol gate passed ($checked ELF files)"
}

# The SMART reader service runs as root, so its hardening is a release gate:
# systemd's own exposure score (0 to 10, lower is tighter) must stay at or
# below 2.0. The reader's draft scored 1.1; the margin is for a directive added
# later, not for dropping one.
smart_max_exposure_tenths=20

units() {
    root=$1
    dir=$root/usr/lib/systemd/system
    service=$dir/gsm-smart-reader.service
    timer=$dir/gsm-smart-reader.timer
    rules=$root/usr/lib/udev/rules.d/70-gnome-system-monitor-rh-smart.rules
    for file in "$service" "$timer" "$rules"; do
        [ -f "$file" ] || fail units "missing $file"
    done

    # The reader must be an installed executable under ROOT. systemd-analyze
    # looks for it on this machine's file system, not ROOT's, so that one
    # message is expected when the build tree is not the root; every other
    # message is a fault.
    exec_path=$(sed -n 's/^ExecStart=//p' "$service")
    case $exec_path in /usr/libexec/*/gsm-smart-reader) ;; *) fail units "ExecStart is '$exec_path'" ;; esac
    [ -x "$root$exec_path" ] || fail units "ExecStart $exec_path is not an executable under $root"
    messages=$(systemd-analyze verify "$service" "$timer" 2>&1 || true)
    messages=$(printf '%s\n' "$messages" | grep -v "Command $exec_path is not executable: No such file or directory" || true)
    [ -z "$messages" ] || fail units "systemd-analyze verify: $messages"

    # The timer is enabled by the package's install script, which needs an
    # [Install] section on the timer and none on the service.
    grep -q '^WantedBy=timers.target' "$timer" || fail units "the timer has no [Install] WantedBy=timers.target"
    if grep -q '^\[Install\]' "$service"; then fail units "the service has an [Install] section; it is started by the timer and udev only"; fi

    score=$(systemd-analyze security --offline=true "$service" 2>&1 | sed -n 's/.*Overall exposure level for [^:]*: \([0-9.]*\) .*/\1/p')
    [ -n "$score" ] || fail units "no exposure score from systemd-analyze security"
    tenths=$(printf '%s' "$score" | awk '{ printf "%d", $1 * 10 + 0.5 }')
    [ "$tenths" -le "$smart_max_exposure_tenths" ] || fail units "exposure level $score is above 2.0"

    if udevadm verify --help >/dev/null 2>&1; then
        udevadm verify "$rules" >/dev/null 2>&1 || fail units "udevadm verify rejects $rules"
    fi
    echo "gate-check: SMART reader units gate passed (verify clean, exposure level $score, limit 2.0)"
}

case ${1:-} in
    configured) configured "${2:?builddir}" ;;
    symbols) symbols "${2:?root}" ;;
    units) units "${2:?root}" ;;
    *) echo "usage: gate-check.sh configured BUILDDIR | symbols ROOT | units ROOT" >&2; exit 2 ;;
esac
