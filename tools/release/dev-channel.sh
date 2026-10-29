#!/usr/bin/env bash
# dev-channel.sh — the user-prefix development channel: versioned installs
# side by side, an atomic switch between them, and a one-step rollback.
#
#   dev-channel.sh [--root DIR] [--export DIR] COMMAND
#
#     build [--rev REV] [--test]  release-profile build of REV (default HEAD),
#                                 installed to <root>/<version>
#     switch VERSION              make VERSION current; export its desktop,
#                                 D-Bus and schema files; refused while
#                                 the dpkg package is installed (the export
#                                 would shadow it) when the export is the
#                                 real user data directory
#     rollback                    switch to the previous version
#     status                      what is installed, current, exported
#     prune [VERSION|DIR...]      remove versions that are neither current
#                                 nor previous (or just the ones named)
#     plan                        print what a real install would create,
#                                 replace and keep; writes nothing
#
#   --root DIR    where versions live       (default ~/.local/opt/gsm-rh)
#   --export DIR  the XDG data dir whose applications/, dbus-1/services/ and
#                 glib-2.0/schemas/ point at <root>/current
#                                           (default ~/.local/share)
#
# Layout:  <root>/<version>/      a complete install (meson --prefix)
#          <root>/current         symlink to a version; the exported files
#                                 name this path, so a switch is one rename
#          <root>/previous        symlink to the version current replaced
#          <root>/original-export copy of the export files as they were
#                                 before the first switch (to restore by hand)
#
# Versions are <release>~dev.<commit day>.g<short sha>, the scheme of
# build-deb.sh --dev, so a dev build sorts below the release it leads to.
#
# Unprivileged by design. The Health tab's privileged helpers and polkit
# policy cannot work from a user prefix (polkit reads actions only from
# /usr/share/polkit-1/actions, and pkexec matches the helper's exact path), so
# this channel tests the UI and the unprivileged data path; the signed .deb is
# the only way to test those. Nothing here installs system-wide or raises an
# authentication prompt. A dev build is never a release: dev_ipc stays off,
# and the installed binaries are checked for it before they become switchable.
#
# Optional helpers, as in build-deb.sh: tools/scratch.sh makes and removes the
# build's scratch directory in RAM (fallback: a marked directory under
# ${XDG_RUNTIME_DIR:-/tmp}, removed by a checked function here); agent-run caps
# memory and queues on the machine's heavy-job slots (fallback: none).
#
# Environment: GSM_RH_GATE_SELFTEST=dev_ipc  force dev_ipc=true to prove the gate fails
#              GSM_RH_LTO=false  quicker build without link-time optimisation
#              GSM_RH_MEM=6G     memory cap for agent-run
#              GSM_RH_WAIT=540   seconds agent-run waits for a heavy slot

set -euo pipefail

die() { printf 'dev-channel.sh: %s\n' "$*" >&2; exit 2; }
note() { printf 'dev-channel.sh: %s\n' "$*" >&2; }

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
[[ -n "${HOME:-}" && "$HOME" == /* ]] || die "HOME is not set"

readonly VERSION_MARKER=".gsm-rh-dev-channel"   # content: the directory's own name
readonly INSTALLED_FILE=".installed"            # written last: only then is a version switchable
readonly BUILD_MARKER=".gsm-rh-dev-build"
readonly BACKUP_NAME="original-export"

# Files exported from <root>/current/share into the export dir: kind, then
# the path below share/. "desktop" and "service" files are rewritten to name
# <root>/current; "plain" files are copied as they are. The compiled schema
# cache is generated in the export dir, never copied.
readonly EXPORTS=(
    "desktop applications/org.gnome.SystemMonitor.desktop"
    "desktop applications/gnome-system-monitor-kde.desktop"
    "service dbus-1/services/org.gnome.SystemMonitor.service"
    "plain glib-2.0/schemas/org.gnome.gnome-system-monitor.enums.xml"
    "plain glib-2.0/schemas/org.gnome.gnome-system-monitor.gschema.xml"
)
readonly GENERATED_EXPORT="glib-2.0/schemas/gschemas.compiled"

root="$HOME/.local/opt/gsm-rh"
export_dir="$HOME/.local/share"
rev=HEAD
run_tests=false
positional=()
while (($#)); do
    case "$1" in
        --root) root="${2:?--root needs a directory}"; shift 2 ;;
        --export) export_dir="${2:?--export needs a directory}"; shift 2 ;;
        --rev) rev="${2:?--rev needs a revision}"; shift 2 ;;
        --test) run_tests=true; shift ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) positional+=("$1"); shift ;;
    esac
done
((${#positional[@]})) || die "usage: dev-channel.sh [--root DIR] [--export DIR] build|switch|rollback|status|prune|plan (see --help)"
command="${positional[0]}"
args=("${positional[@]:1}")

# Both directories end up inside Exec= lines and symlinks: keep them plain.
for dir in "$root" "$export_dir"; do
    [[ "$dir" == /* && "$dir" =~ ^/[A-Za-z0-9_./+-]+$ && "$dir" != *..* ]] \
        || die "directories must be absolute paths of letters, digits and _./+- only: $dir"
done
root="${root%/}"
export_dir="${export_dir%/}"

# ---------------------------------------------------------------- versions

# <release>~dev.<UTC commit day>.g<short sha> for REV. The release number is
# the newest entry of debian/changelog at REV, else of the working tree's.
dev_version() {
    local release day sha line
    line="$(git -C "$repo" show "$1:debian/changelog" 2>/dev/null | sed -n '1p' || true)"
    if [[ -z "$line" ]]; then line="$(sed -n '1p' "$repo/debian/changelog")"; fi
    release="$(printf '%s\n' "$line" | sed -n 's/^[^(]*(\([^)]*\)).*/\1/p')"
    [[ -n "$release" && "$release" != *'~'* ]] || die "cannot read a release version from debian/changelog"
    day="$(TZ=UTC git -C "$repo" log -1 --format=%cd --date=format-local:%Y%m%d "$1")"
    sha="$(git -C "$repo" rev-parse --short=7 "$1^{commit}")"
    printf '%s~dev.%s.g%s' "$release" "$day" "$sha"
}

valid_version() { [[ "$1" =~ ^[0-9A-Za-z.+~_-]+$ && "$1" != . && "$1" != .. ]]; }

link_target() { # name of the version a link points at, or nothing
    local link="$root/$1"
    if [[ -L "$link" ]]; then basename -- "$(readlink -- "$link")"; fi
}

installed() { [[ -f "$root/$1/$VERSION_MARKER" && -f "$root/$1/$INSTALLED_FILE" ]]; }

set_link() { # name target: one rename, so readers see the old or the new, never neither
    ln -s -- "$2" "$root/.$1.new.$$"
    mv -T -- "$root/.$1.new.$$" "$root/$1"
}

# --------------------------------------------------------- checked removal

# Remove a directory only if it is one this script made: its real path is
# directly under ROOT (never ROOT itself) and its marker file names it.
# Prints the refusal and returns 1 otherwise.
remove_checked() { # root dir marker
    local real real_root name
    real="$(realpath -e -- "$2" 2>/dev/null)" || { note "refusing: no such path: $2"; return 1; }
    real_root="$(realpath -e -- "$1" 2>/dev/null)" || { note "refusing: no such root: $1"; return 1; }
    if [[ "$real" == "$real_root" ]]; then note "refusing: $real is the root itself"; return 1; fi
    if [[ "$(dirname -- "$real")" != "$real_root" ]]; then note "refusing: $real is not directly under $real_root"; return 1; fi
    name="$(basename -- "$real")"
    if [[ ! -f "$real/$3" || -L "$real/$3" ]]; then note "refusing: $real has no $3 marker"; return 1; fi
    if [[ "$(cat -- "$real/$3")" != "$name" ]]; then note "refusing: the marker in $real does not name it"; return 1; fi
    rm -rf -- "$real"
}

scratch=""
scratch_fallback_root="${XDG_RUNTIME_DIR:-/tmp}/gsm-rh-dev-build"
make_scratch() {
    if [[ -x "$repo/tools/scratch.sh" ]]; then
        scratch="$("$repo/tools/scratch.sh" new ram devchan)"
    else
        mkdir -p -- "$scratch_fallback_root"
        scratch="$(mktemp -d -- "$scratch_fallback_root/build.XXXXXX")"
        basename -- "$scratch" > "$scratch/$BUILD_MARKER"
    fi
}
drop_scratch() {
    [[ -n "$scratch" && -d "$scratch" ]] || return 0
    if [[ -x "$repo/tools/scratch.sh" ]]; then "$repo/tools/scratch.sh" rm "$scratch" >/dev/null
    else remove_checked "$scratch_fallback_root" "$scratch" "$BUILD_MARKER"; fi
}

# ------------------------------------------------------------------- gates

# meson was configured as a release build without dev IPC (same three
# options as debian/gate-check.sh).
gate_configured() {
    local builddir="$1" options pair name want got
    options="$(meson introspect --buildoptions "$builddir")"
    for pair in dev_ipc=false development=false buildtype=release; do
        name="${pair%%=*}"; want="${pair#*=}"
        got="$(printf '%s' "$options" | python3 -I -c '
import json, sys
for option in json.load(sys.stdin):
    if option["name"] == sys.argv[1]:
        print(str(option["value"]).lower())
        break
' "$name")"
        [[ "$got" == "$want" ]] || die "GATE FAILED ($name): configured as '$got', a release profile needs '$want'"
    done
    note "dev_ipc gate passed (dev_ipc=false, development=false, buildtype=release)"
}

# No ELF file in the install defines a dev_ipc symbol or carries the
# GSM_DEV_IPC run-time switch: the developer control socket must never ship.
gate_symbols() {
    local prefix="$1" file checked=0
    for file in "$prefix"/bin/* "$prefix"/libexec/*/*; do
        [[ -f "$file" && ! -L "$file" ]] || continue
        [[ "$(head -c 4 -- "$file" | od -An -c | tr -d ' ')" == '177ELF' ]] || continue
        checked=$((checked + 1))
        if { nm -D --defined-only "$file"; nm "$file" 2>/dev/null || true; } | grep -qi 'dev_ipc'; then
            die "GATE FAILED (dev_ipc): $file defines a dev_ipc symbol"
        fi
        if grep -aq 'GSM_DEV_IPC' "$file"; then
            die "GATE FAILED (dev_ipc): $file contains the GSM_DEV_IPC switch"
        fi
    done
    ((checked > 0)) || die "GATE FAILED (dev_ipc): no ELF files under $prefix, nothing was checked"
    note "dev_ipc symbol gate passed ($checked ELF files)"
}

# ------------------------------------------------------------------- build

build_prefix=""
build_cleanup() {
    # A version that never became switchable is removed again; its marker is
    # written first, so the checked removal accepts it.
    if [[ -n "$build_prefix" && -d "$build_prefix" && ! -f "$build_prefix/$INSTALLED_FILE" ]]; then
        remove_checked "$root" "$build_prefix" "$VERSION_MARKER" || note "could not remove the unfinished $build_prefix"
    fi
    drop_scratch || note "could not remove $scratch"
}

cmd_build() {
    git -C "$repo" rev-parse --verify --quiet "$rev^{commit}" >/dev/null || die "unknown revision: $rev"
    local version prefix devflag src runner=()
    version="$(dev_version "$rev")"
    valid_version "$version" || die "unusable version: $version"
    prefix="$root/$version"
    if [[ -e "$prefix" ]]; then
        installed "$version" || die "$prefix exists but is not a finished install; run: dev-channel.sh --root $root prune $version"
        note "$version is already built; nothing to do"
        return 0
    fi
    mkdir -p -- "$root"

    devflag=false
    if [[ "${GSM_RH_GATE_SELFTEST:-}" == dev_ipc ]]; then devflag=true; fi
    if command -v agent-run >/dev/null; then
        runner=(agent-run -m "${GSM_RH_MEM:-6G}" --wait "${GSM_RH_WAIT:-540}" --)
    else
        note "agent-run not found; building without a memory cap"
    fi

    make_scratch
    trap build_cleanup EXIT
    src="$scratch/src"
    mkdir -p -- "$src"
    git -C "$repo" archive "$rev" | tar -x -C "$src"

    meson setup "$scratch/b" "$src" --prefix="$prefix" --buildtype=release \
        -Ddev_ipc="$devflag" -Ddevelopment=false -Db_lto="${GSM_RH_LTO:-true}" >"$scratch/setup.log" 2>&1 \
        || { tail -n 30 "$scratch/setup.log" >&2; die "meson setup failed"; }
    gate_configured "$scratch/b"

    "${runner[@]}" meson compile -C "$scratch/b" || die "build failed"
    if "$run_tests"; then
        "${runner[@]}" meson test -C "$scratch/b" --no-suite formatting --print-errorlogs || die "tests failed"
    fi

    build_prefix="$prefix"
    mkdir -- "$prefix"
    printf '%s\n' "$version" > "$prefix/$VERSION_MARKER"
    meson install -C "$scratch/b" --no-rebuild --strip >"$scratch/install.log" 2>&1 \
        || { tail -n 30 "$scratch/install.log" >&2; die "meson install into $prefix failed"; }
    gate_symbols "$prefix"
    printf 'version %s\nrevision %s\n' "$version" "$(git -C "$repo" rev-parse "$rev^{commit}")" > "$prefix/$INSTALLED_FILE"
    note "built $version at $prefix"
    printf '%s\n' "$version"
}

# ------------------------------------------------------------------ export

# <prefix> with its version replaced by "current", and the bare Exec= of the
# desktop files made absolute: the exported files then follow the symlink.
rewrite_for_current() { # version-prefix
    local line
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line//"$1/"/"$root/current/"}"
        if [[ "$line" == Exec=gnome-system-monitor-rh* ]]; then line="Exec=$root/current/bin/${line#Exec=}"; fi
        printf '%s\n' "$line"
    done
}

backup_original_export() {
    local backup="$root/$BACKUP_NAME" entry rel
    [[ ! -e "$backup" ]] || return 0
    mkdir -p -- "$backup.new"
    for entry in "${EXPORTS[@]}" "plain $GENERATED_EXPORT"; do
        rel="${entry#* }"
        if [[ -e "$export_dir/$rel" ]]; then
            mkdir -p -- "$backup.new/$(dirname -- "$rel")"
            cp -a -- "$export_dir/$rel" "$backup.new/$rel"
        else
            printf 'absent before the first switch: %s\n' "$rel" >> "$backup.new/MANIFEST"
        fi
    done
    mv -T -- "$backup.new" "$backup"
    note "saved the export files as they were to $backup"
}

export_current() {
    local version entry kind rel src dest
    version="$(link_target current)"
    [[ -n "$version" ]] || die "no current version to export"
    backup_original_export
    for entry in "${EXPORTS[@]}"; do
        kind="${entry%% *}"; rel="${entry#* }"
        src="$root/current/share/$rel"
        dest="$export_dir/$rel"
        [[ -f "$src" ]] || die "version $version has no share/$rel to export"
        mkdir -p -- "$(dirname -- "$dest")"
        if [[ "$kind" == plain ]]; then cp -- "$src" "$dest.gsm-rh.new"
        else rewrite_for_current "$root/$version" < "$src" > "$dest.gsm-rh.new"; fi
        mv -T -- "$dest.gsm-rh.new" "$dest"
    done
    glib-compile-schemas "$export_dir/$(dirname -- "$GENERATED_EXPORT")"
    note "exported $version into $export_dir (desktop, D-Bus and schema files now follow $root/current)"
}

# ---------------------------------------------------------------- commands

# The channel exports desktop, D-Bus and schema files into the user's data
# directory, and those take precedence over the installed package's
# (desktop-entry and basedir specs; dbus-daemon(1) for session services). The
# schema id is the same, so a stale user schema also aborts the installed app
# on a missing key. So while the dpkg package is installed, a switch into the
# real user data directory is refused. A scratch --export is not.
# Prints the reason and returns 0 when a switch must be refused.
package_shadow_reason() {
    local pkg=gnome-system-monitor-rh data real_export version
    data="$(realpath -m -- "${XDG_DATA_HOME:-$HOME/.local/share}")"
    real_export="$(realpath -m -- "$export_dir")"
    [[ "$real_export" == "$data" ]] || return 1
    version="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null)" || return 1
    [[ -n "$version" ]] || return 1
    printf '%s %s is installed by dpkg; exporting this channel into %s would hide it behind user files (schemas, D-Bus service, desktop files). Remove the package first, or use a scratch --export.' "$pkg" "$version" "$export_dir"
}

cmd_switch() {
    local target="${1:-}" now reason
    if reason="$(package_shadow_reason)"; then die "switch refused: $reason"; fi
    [[ -n "$target" ]] || die "usage: dev-channel.sh switch VERSION"
    valid_version "$target" || die "unusable version: $target"
    installed "$target" || die "$target is not an installed version under $root (see: status)"
    now="$(link_target current)"
    if [[ "$now" == "$target" ]]; then
        note "$target is already current; exporting again"
    else
        if [[ -n "$now" ]]; then set_link previous "$now"; fi
        set_link current "$target"
        note "current is now $target${now:+ (previous: $now)}"
    fi
    export_current
    note "a running instance is not restarted: quit it (gapplication action org.gnome.SystemMonitor quit), then launch again"
}

cmd_rollback() {
    local back
    back="$(link_target previous)"
    [[ -n "$back" ]] || die "there is no previous version to roll back to"
    cmd_switch "$back"
}

cmd_status() {
    local current previous entry rel file version marker
    current="$(link_target current)"; previous="$(link_target previous)"
    printf 'root:     %s\nexport:   %s\ncurrent:  %s\nprevious: %s\nversions:\n' \
        "$root" "$export_dir" "${current:-(none)}" "${previous:-(none)}"
    for file in "$root"/*; do
        [[ -d "$file" && ! -L "$file" && -f "$file/$VERSION_MARKER" ]] || continue
        version="$(basename -- "$file")"; marker="  "
        if [[ "$version" == "$current" ]]; then marker="* "; elif [[ "$version" == "$previous" ]]; then marker="p "; fi
        if installed "$version"; then printf '  %s%s\n' "$marker" "$version"; else printf '  %s%s (unfinished: prune removes it)\n' "$marker" "$version"; fi
    done
    printf 'exported files:\n'
    for entry in "${EXPORTS[@]}"; do
        rel="${entry#* }"
        if [[ ! -e "$export_dir/$rel" ]]; then printf '  absent      share/%s\n' "$rel"
        elif grep -qF -- "$root/current/" "$export_dir/$rel"; then printf '  via current share/%s\n' "$rel"
        elif [[ "${entry%% *}" == plain ]]; then printf '  present     share/%s\n' "$rel"
        else printf '  other       share/%s (not this channel)\n' "$rel"; fi
    done
    printf 'limits:\n  The Health tab'"'"'s privileged probes (SMART, per-process I/O) and the kill/renice\n'
    printf '  polkit actions cannot work from a user prefix: polkit reads actions only from\n'
    printf '  /usr/share/polkit-1/actions and pkexec matches the helper'"'"'s exact path. They\n'
    printf '  show "unknown" here with a reason; install the signed .deb to test them.\n'
}

# The checks specific to prune (in use, not a version dir), then the shared
# checked removal.
remove_checked_guard() {
    local dir="$1" real name
    real="$(realpath -e -- "$dir" 2>/dev/null)" || { note "refusing: no such path: $dir"; return 1; }
    name="$(basename -- "$real")"
    if [[ "$name" == "$(link_target current)" || "$name" == "$(link_target previous)" ]] \
        && [[ "$(dirname -- "$real")" == "$(realpath -e -- "$root")" ]]; then
        note "refusing: $real is the current or previous version"
        return 1
    fi
    remove_checked "$root" "$real" "$VERSION_MARKER" && note "removed $real"
}

cmd_prune() {
    [[ -d "$root" ]] || die "no such root: $root"
    local failed=0 candidate arg
    if ((${#args[@]})); then
        for arg in "${args[@]}"; do
            if [[ "$arg" == /* ]]; then candidate="$arg"; else candidate="$root/$arg"; fi
            remove_checked_guard "$candidate" || failed=1
        done
    else
        for candidate in "$root"/*; do
            [[ -d "$candidate" && ! -L "$candidate" ]] || continue
            if [[ ! -f "$candidate/$VERSION_MARKER" ]]; then note "skipping $candidate (no marker: not a version)"; continue; fi
            case "$(basename -- "$candidate")" in
                "$(link_target current)"|"$(link_target previous)") continue ;;
            esac
            remove_checked_guard "$candidate" || failed=1
        done
    fi
    return "$failed"
}

cmd_plan() {
    local version entry rel verdict reason
    version="$(dev_version "$rev")"
    printf 'PLAN — nothing is written by this command.\n'
    printf 'root: %s\nexport: %s\n\n' "$root" "$export_dir"
    printf 'CREATE  %s/%s/        a complete release-profile install of %s (bin, libexec, share)\n' "$root" "$version" "$rev"
    printf 'CREATE  %s/current    symlink to the version switched to (and previous, once there are two)\n' "$root"
    printf 'CREATE  %s/%s/   the export files as they are now, kept for a manual restore\n\n' "$root" "$BACKUP_NAME"
    printf 'EXPORT  into %s, after a switch:\n' "$export_dir"
    for entry in "${EXPORTS[@]}" "generated $GENERATED_EXPORT"; do
        rel="${entry#* }"
        if [[ -e "$export_dir/$rel" ]]; then verdict="REPLACE"; else verdict="CREATE "; fi
        printf '  %s share/%s\n' "$verdict" "$rel"
    done
    printf '\nKEEP    untouched (the existing user-prefix install, if any):\n'
    for rel in "$(dirname -- "$export_dir")/bin/gnome-system-monitor-rh" "$(dirname -- "$export_dir")"/libexec/gnome-system-monitor/*; do
        if [[ -e "$rel" ]]; then printf '  %s\n' "$rel"; fi
    done
    printf '  The menu entry, D-Bus activation and schemas would stop using it; rollback or the\n'
    printf '  saved copy in %s/%s brings the old files back.\n\n' "$root" "$BACKUP_NAME"
    printf 'NEVER   /usr, /etc, apt, polkit, systemd units, a running instance.\n'
    if reason="$(package_shadow_reason)"; then printf '\nREFUSED switch (and rollback) is refused: %s\n' "$reason"; fi
}

case "$command" in
    build) cmd_build ;;
    switch) cmd_switch "${args[0]:-}" ;;
    rollback) cmd_rollback ;;
    status) cmd_status ;;
    prune) cmd_prune ;;
    plan) cmd_plan ;;
    *) die "unknown command: $command" ;;
esac
