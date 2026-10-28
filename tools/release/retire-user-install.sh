#!/usr/bin/env bash
# retire-user-install.sh — move a user-prefix (~/.local) install of the app out
# of the way, reversibly, so the packaged app under /usr is the one that runs.
#
#   retire-user-install.sh [--home DIR] [--manifest FILE] [--dry-run]
#                          [--force-not-running-check]
#   retire-user-install.sh [--home DIR] [--manifest FILE] [--dry-run]
#                          --restore STAMP|latest
#
# Why: files in the user's own directories win over the system's. With the
# package installed, ~/.local/bin precedes /usr/bin in PATH, a user D-Bus
# service file under ~/.local/share/dbus-1/services is preferred over
# /usr/share/dbus-1/services (dbus-daemon(1), session bus search order), user
# .desktop files override system ones (desktop-entry-spec and
# basedir-spec: XDG_DATA_HOME comes first), and a user glib-2.0/schemas
# directory with its own gschemas.compiled shadows the system schemas.
#
# What: every file of the install that is present is MOVED, never deleted, to
# <home>/.local/share/gsm-rh-retired-<STAMP>/files/<same path below .local>,
# and recorded in <that dir>/MANIFEST (or --manifest FILE). The install is
# described by the table RULES below (a meson `--prefix ~/.local` install of
# this app: binary, helpers, desktop files, D-Bus service file, schemas, icons,
# metainfo, the polkit policy that polkit ignores in a user directory, the
# sampler's user unit, help pages and translations). The app's own data
# (history database, settings) is not in the table and stays put.
#
# Schemas: if other schemas remain in the user schema directory, its
# gschemas.compiled is copied into the backup and recompiled with
# glib-compile-schemas; if none remain, the compiled file is moved too.
#
# --restore moves every recorded file back to where it was. It refuses, and
# moves nothing, if any destination exists again. A restored backup is marked
# (RESTORED) and is not picked by `latest` again. Parent directories that
# became empty are left in place: nothing is removed, so there is nothing to
# recreate and nothing that can be removed by mistake.
#
# The app must not be running (the binary is being moved from under it):
# the script refuses unless --force-not-running-check, which tests use with a
# fake --home. --dry-run prints every step and changes nothing.

set -euo pipefail

die() { printf 'retire-user-install.sh: %s\n' "$*" >&2; exit 2; }
say() { printf 'retire-user-install.sh: %s\n' "$*"; }

# kind, then a glob below <home>/.local. "file" matches files and links, "dir"
# whole directories (moved as one entry).
readonly RULES=(
    "file bin/gnome-system-monitor-rh"
    "file lib/systemd/user/gnome-system-monitor-health-sampler.service"
    "file libexec/gnome-system-monitor/gsm-*"
    "file share/applications/gnome-system-monitor-kde.desktop"
    "file share/applications/org.gnome.SystemMonitor.desktop"
    "file share/dbus-1/services/org.gnome.SystemMonitor.service"
    "file share/glib-2.0/schemas/org.gnome.gnome-system-monitor.enums.xml"
    "file share/glib-2.0/schemas/org.gnome.gnome-system-monitor.gschema.xml"
    "file share/icons/hicolor/scalable/apps/org.gnome.SystemMonitor.svg"
    "file share/icons/hicolor/scalable/apps/org.gnome.SystemMonitor.Devel.svg"
    "file share/icons/hicolor/symbolic/apps/org.gnome.SystemMonitor-symbolic.svg"
    "file share/icons/hicolor/symbolic/apps/processes-symbolic.svg"
    "file share/icons/hicolor/symbolic/apps/resources-symbolic.svg"
    "file share/metainfo/org.gnome.SystemMonitor.metainfo.xml"
    "file share/polkit-1/actions/org.gnome.gnome-system-monitor.policy"
    "dir share/help/*/gnome-system-monitor"
    "file share/locale/*/LC_MESSAGES/gnome-system-monitor.mo"
)
readonly SCHEMA_DIR="share/glib-2.0/schemas"
readonly SCHEMA_FILE_PREFIX="$SCHEMA_DIR/org.gnome.gnome-system-monitor."
readonly COMPILED="$SCHEMA_DIR/gschemas.compiled"
readonly BACKUP_PREFIX="gsm-rh-retired-"

home_dir="${HOME:-}"; manifest_override=""; restore=""; dry=false; skip_running=false
while (($#)); do
    case "$1" in
        --home) home_dir="${2:?--home needs a directory}"; shift 2 ;;
        --manifest) manifest_override="${2:?--manifest needs a file}"; shift 2 ;;
        --restore) restore="${2:?--restore needs a stamp or 'latest'}"; shift 2 ;;
        --dry-run) dry=true; shift ;;
        --force-not-running-check) skip_running=true; shift ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done
[[ "$home_dir" == /* && -d "$home_dir" ]] || die "--home must be an existing absolute directory: $home_dir"
local_dir="$home_dir/.local"
[[ -d "$local_dir" ]] || die "no $local_dir: nothing to retire"

# Run a mutating command, or only say so under --dry-run.
act() {
    if "$dry"; then say "would: $*"; else "$@"; fi
}

# ------------------------------------------------------------------ restore

latest_backup() {
    local d found=""
    for d in "$local_dir/share/$BACKUP_PREFIX"*; do
        [[ -d "$d" && -f "$d/MANIFEST" && ! -e "$d/RESTORED" ]] || continue
        found="$d"   # names carry a sortable stamp; the glob is sorted
    done
    [[ -n "$found" ]] || die "no un-restored backup under $local_dir/share"
    printf '%s' "$found"
}

do_restore() {
    local backup manifest line kind rel src dest conflicts=()
    if [[ "$restore" == latest ]]; then backup="$(latest_backup)"; else backup="$local_dir/share/$BACKUP_PREFIX$restore"; fi
    [[ -d "$backup" ]] || die "no such backup: $backup"
    [[ ! -e "$backup/RESTORED" ]] || die "$backup was already restored"
    manifest="${manifest_override:-$backup/MANIFEST}"
    [[ -f "$manifest" ]] || die "no manifest: $manifest"
    say "restoring from $backup"

    # Check every destination first; move nothing if any is taken.
    while IFS=$'\t' read -r kind rel; do
        [[ -n "$kind" && "$kind" != '#'* ]] || continue
        [[ "$rel" != /* && "$rel" != *..* ]] || die "bad path in manifest: $rel"
        [[ -e "$backup/files/$rel" || -L "$backup/files/$rel" ]] || continue
        if [[ "$kind" == move && ( -e "$local_dir/$rel" || -L "$local_dir/$rel" ) ]]; then conflicts+=("$rel"); fi
    done < "$manifest"
    if ((${#conflicts[@]})); then
        printf 'retire-user-install.sh: already exists, nothing restored:\n' >&2
        for rel in "${conflicts[@]}"; do printf '  %s/%s\n' "$local_dir" "$rel" >&2; done
        die "${#conflicts[@]} destination(s) exist again (first: $local_dir/${conflicts[0]})"
    fi

    while IFS=$'\t' read -r kind rel; do
        [[ -n "$kind" && "$kind" != '#'* ]] || continue
        src="$backup/files/$rel"; dest="$local_dir/$rel"
        if [[ ! -e "$src" && ! -L "$src" ]]; then say "not in the backup, skipped: $rel"; continue; fi
        say "$kind back: $rel"
        act mkdir -p -- "$(dirname -- "$dest")"
        case "$kind" in
            move) act mv -T -n -- "$src" "$dest" ;;
            replace) act mv -T -f -- "$src" "$dest" ;;
            *) die "unknown kind in manifest: $kind" ;;
        esac
    done < "$manifest"
    if ! "$dry"; then : > "$backup/RESTORED"; fi
    say "restored; log in $backup"
}

# ------------------------------------------------------------------- retire

app_running() {
    command -v pgrep >/dev/null || die "needs pgrep to check that the app is not running (or --force-not-running-check in tests)"
    # The kernel truncates a process name to 15 characters, so `pgrep -x` can
    # never match "gnome-system-monitor-rh" (proc(5), comm); match the command
    # line's program name instead.
    pgrep -u "$(id -u)" -f -- '^([^ ]*/)?gnome-system-monitor(-rh)?( |$)' >/dev/null
}

stamp="${GSM_RH_RETIRE_STAMP:-$(date +%Y%m%dT%H%M%S)}"
backup="$local_dir/share/$BACKUP_PREFIX$stamp"
manifest="${manifest_override:-$backup/MANIFEST}"
moved=0

record_and_move() { # kind rel
    local kind="$1" rel="$2"
    say "$kind: $rel"
    if "$dry"; then moved=$((moved + 1)); return 0; fi
    # The backup directory and the manifest appear with the first entry, so a
    # run that finds nothing leaves nothing behind.
    if [[ ! -f "$manifest" ]]; then
        mkdir -p -- "$backup/files" "$(dirname -- "$manifest")"
        printf '# gsm-rh retire manifest v1\tstamp=%s\thome=%s\n' "$stamp" "$home_dir" > "$manifest"
    fi
    printf '%s\t%s\n' "$kind" "$rel" >> "$manifest"
    mkdir -p -- "$backup/files/$(dirname -- "$rel")"
    if [[ "$kind" == replace ]]; then
        cp -p -- "$local_dir/$rel" "$backup/files/$rel"
    else
        mv -T -n -- "$local_dir/$rel" "$backup/files/$rel"
    fi
    moved=$((moved + 1))
}

do_retire() {
    local rule kind glob path rel schema_touched=false other_schemas=false f
    if ! "$skip_running" && app_running; then
        if "$dry"; then say "note: the app is running; a real run would refuse until it is quit"; else
            die "the app is running; quit it first (gapplication action org.gnome.SystemMonitor quit)"
        fi
    fi
    [[ ! -e "$backup" ]] || die "$backup exists already; wait a second or restore it"
    if [[ -d "$local_dir/opt/gsm-rh/current" ]]; then
        say "note: the development channel in $local_dir/opt/gsm-rh exports files of the same names; they are retired too"
    fi

    if ! "$dry"; then command -v glib-compile-schemas >/dev/null || die "needs glib-compile-schemas"; fi

    shopt -s nullglob
    for rule in "${RULES[@]}"; do
        kind="${rule%% *}"; glob="${rule#* }"
        for path in "$local_dir"/$glob; do
            if [[ "$kind" == file ]]; then [[ -f "$path" || -L "$path" ]] || continue; else [[ -d "$path" && ! -L "$path" ]] || continue; fi
            rel="${path#"$local_dir"/}"
            record_and_move move "$rel"
            if [[ "$rel" == "$SCHEMA_FILE_PREFIX"* ]]; then schema_touched=true; fi
        done
    done

    if "$schema_touched" && [[ -d "$local_dir/$SCHEMA_DIR" ]]; then
        for f in "$local_dir/$SCHEMA_DIR"/*.xml "$local_dir/$SCHEMA_DIR"/*.gschema.override; do
            [[ -e "$f" ]] || continue
            # Ours do not count (in a dry run they are still in place).
            [[ "${f#"$local_dir"/}" != "$SCHEMA_FILE_PREFIX"* ]] || continue
            other_schemas=true
            break
        done
        if [[ -e "$local_dir/$COMPILED" ]]; then
            if "$other_schemas"; then
                record_and_move replace "$COMPILED"
                say "recompiling the remaining user schemas"
                act glib-compile-schemas -- "$local_dir/$SCHEMA_DIR" \
                    || die "glib-compile-schemas failed; undo with --restore $stamp"
            else
                record_and_move move "$COMPILED"
            fi
        fi
    fi
    shopt -u nullglob

    if ((moved == 0)); then
        say "nothing to retire under $local_dir"
        return 0
    fi
    if "$dry"; then say "$moved entries would be retired"; else
        say "$moved entries retired; undo with: $0 --home $home_dir --restore $stamp"
    fi
}

if [[ -n "$restore" ]]; then do_restore; else do_retire; fi
