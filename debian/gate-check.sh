#!/bin/sh
# Release gates for debian/rules. Fails the build, naming the gate.
#   gate-check.sh configured BUILDDIR   meson was configured as a release build without dev IPC
#   gate-check.sh symbols ROOT          nothing installed under ROOT carries dev IPC
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

case ${1:-} in
    configured) configured "${2:?builddir}" ;;
    symbols) symbols "${2:?root}" ;;
    *) echo "usage: gate-check.sh configured BUILDDIR | symbols ROOT" >&2; exit 2 ;;
esac
