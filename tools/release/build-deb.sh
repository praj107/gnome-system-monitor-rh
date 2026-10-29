#!/usr/bin/env bash
# build-deb.sh — build the gated gnome-system-monitor-rh .deb from a git revision.
#
#   build-deb.sh [--rev REV] [--dev] [--out DIR] [--distribution NAME] [--overlay PATH...]
#
# Exports REV (default HEAD) with `git archive` into a scratch directory, copies
# the given working-tree paths over it, runs dpkg-buildpackage there, and copies
# the .deb and .buildinfo files to <repo>/build-release/. The working tree is
# never built in place. --dev stamps a development version
# (<release>~dev.<commit date>.g<short sha>, which sorts below the release)
# into the export's changelog; the tracked changelog is never touched.
# --out DIR puts the .deb and .buildinfo files in DIR instead of
# <repo>/build-release/ (a test package must not land beside the installed
# release's). --distribution NAME replaces UNRELEASED in the export's newest
# changelog entry, so a package that is still unreleased in git can be
# published to a test repository. Nothing is installed: the install command is left
# for the owner, since installing needs root and a build script must never
# raise an authentication prompt. Every temporary file, logs included, lives
# inside the scratch directory and goes away with it.
#
# Optional helpers, used when present and replaced by a plain fallback when not:
#   tools/scratch.sh   makes and removes the scratch directory (in RAM).
#                      Fallback: a directory under ${XDG_RUNTIME_DIR:-/tmp}
#                      holding a marker file this script writes, removed by
#                      this script's own checked function (real path, parent
#                      must be the root chosen here, marker must name the
#                      directory, the root itself is refused).
#   agent-run          caps the build's memory and queues it on machine-wide
#                      heavy-job slots. Fallback: dpkg-buildpackage is run
#                      directly, without a memory cap.
#
# Environment:
#   GSM_RH_GATE_SELFTEST=dev_ipc   force a dev_ipc build to prove the gate fails.
#   GSM_RH_LTO=false               quicker build without link-time optimisation
#                                  (the project's release profile has LTO on).
#   GSM_RH_MEM=6G                  memory cap for agent-run.
#   GSM_RH_WAIT=540                seconds agent-run waits for a heavy slot.

set -euo pipefail

die() { printf 'build-deb.sh: %s\n' "$*" >&2; exit 2; }

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
rev=HEAD
overlay=()
dev=false
out=""
distribution=""

while (($#)); do
    case "$1" in
        --rev) rev="${2:?--rev needs a revision}"; shift 2 ;;
        --dev) dev=true; shift ;;
        --out) out="${2:?--out needs a directory}"; shift 2 ;;
        --distribution) distribution="${2:?--distribution needs a name}"; shift 2 ;;
        --overlay)
            shift
            while (($#)) && [[ "$1" != --* ]]; do overlay+=("$1"); shift; done
            ((${#overlay[@]})) || die "--overlay needs at least one path"
            ;;
        *) die "usage: build-deb.sh [--rev REV] [--dev] [--out DIR] [--distribution NAME] [--overlay PATH...]" ;;
    esac
done

git -C "$repo" rev-parse --verify --quiet "$rev^{commit}" >/dev/null || die "unknown revision: $rev"

readonly MARKER=".gsm-rh-build"
scratch_helper="$repo/tools/scratch.sh"
fallback_root="${XDG_RUNTIME_DIR:-/tmp}/gsm-rh-build"

# Fallback removal: only a directory this script made, one level below the
# root it chose, carrying a marker that names it. Never the root itself.
remove_checked() {
    local dir real root
    dir="${1:-}"
    [[ -n "$dir" ]] || die "remove_checked: no directory given"
    real="$(realpath -e -- "$dir")" || die "remove_checked: no such path: $dir"
    root="$(realpath -e -- "$fallback_root")" || die "remove_checked: no scratch root"
    [[ "$real" != "$root" ]] || die "remove_checked: refusing the scratch root itself"
    [[ "$(dirname -- "$real")" == "$root" ]] || die "remove_checked: $real is not directly under $root"
    [[ -f "$real/$MARKER" && ! -L "$real/$MARKER" ]] || die "remove_checked: $real has no marker"
    [[ "$(cat -- "$real/$MARKER")" == "$(basename -- "$real")" ]] || die "remove_checked: marker does not name $real"
    rm -rf -- "$real"
}

if [[ -x "$scratch_helper" ]]; then
    scratch="$("$scratch_helper" new ram deb)"
    scratch_remove() { "$scratch_helper" rm "$1" >/dev/null; }
else
    mkdir -p -- "$fallback_root"
    scratch="$(mktemp -d -- "$fallback_root/deb.XXXXXX")"
    basename -- "$scratch" > "$scratch/$MARKER"
    scratch_remove() { remove_checked "$1"; }
fi
cleanup() { scratch_remove "$scratch" || printf 'build-deb.sh: could not remove %s\n' "$scratch" >&2; }
trap cleanup EXIT

src="$scratch/gnome-system-monitor-rh"
mkdir -p -- "$src"
git -C "$repo" archive "$rev" | tar -x -C "$src"

for path in "${overlay[@]}"; do
    [[ "$path" != /* && "$path" != *..* ]] || die "overlay paths are relative to the repo and stay inside it: $path"
    [[ -e "$repo/$path" ]] || die "overlay path does not exist: $path"
    mkdir -p -- "$src/$(dirname -- "$path")"
    cp -a -- "$repo/$path" "$src/$(dirname -- "$path")/"
done

# SOURCE_DATE_EPOCH from the revision, for reproducible timestamps.
SOURCE_DATE_EPOCH="$(git -C "$repo" log -1 --format=%ct "$rev")"
export SOURCE_DATE_EPOCH

# Development channel: a changelog entry with the dev version, in the export
# only. The release number comes from the export's newest entry, so a dev build
# of a revision sorts below the release it leads to (dpkg: '~' sorts before
# anything, even the end of the string). Written by hand: no devscripts needed.
if "$dev"; then
    changelog="$src/debian/changelog"
    [[ -f "$changelog" ]] || die "--dev needs debian/changelog in the exported revision"
    release_version="$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$changelog")"
    [[ -n "$release_version" && "$release_version" != *'~'* ]] || die "cannot read a release version from $changelog"
    commit_day="$(TZ=UTC git -C "$repo" log -1 --format=%cd --date=format-local:%Y%m%d "$rev")"
    short_sha="$(git -C "$repo" rev-parse --short=7 "$rev^{commit}")"
    dev_version="${release_version}~dev.${commit_day}.g${short_sha}"
    maintainer="$(sed -n 's/^Maintainer: //p' "$src/debian/control")"
    {
        printf 'gnome-system-monitor-rh (%s) UNRELEASED; urgency=low\n\n' "$dev_version"
        printf '  * Development build of commit %s (not a release).\n\n' "$short_sha"
        printf ' -- %s  %s\n\n' "$maintainer" "$(date -R -u -d "@$SOURCE_DATE_EPOCH")"
        cat -- "$changelog"
    } > "$changelog.new"
    mv -T -- "$changelog.new" "$changelog"
    printf 'build-deb.sh: development version %s\n' "$dev_version" >&2
fi

if [[ -n "$distribution" ]]; then
    [[ "$distribution" =~ ^[a-z][a-z0-9-]*$ ]] || die "--distribution must be a plain name such as noble"
    head -n 1 -- "$src/debian/changelog" | grep -qF " UNRELEASED;" || die "the newest changelog entry is not UNRELEASED; nothing to stamp"
    sed -i "1s/ UNRELEASED;/ $distribution;/" "$src/debian/changelog"
fi

# The formatting test asks git for the file list; give the export a repository.
git -C "$src" init --quiet
git -C "$src" add -A
git -C "$src" -c user.name=build -c user.email=build@localhost commit --quiet -m export

# Wrap the build: memory cap and slot queue when agent-run exists, and the
# peak resident size of the largest process when GNU time exists.
runner=()
if command -v agent-run >/dev/null; then
    runner=(agent-run -m "${GSM_RH_MEM:-6G}" --wait "${GSM_RH_WAIT:-540}" --)
else
    echo "build-deb.sh: agent-run not found; building without a memory cap" >&2
fi
timer=()
if [[ -x /usr/bin/time ]]; then timer=(/usr/bin/time -v -o "$scratch/time.txt"); fi

status=0
(cd "$src" && "${runner[@]}" "${timer[@]}" dpkg-buildpackage -b -us -uc) || status=$?
if [[ -f "$scratch/time.txt" ]]; then
    grep -E 'Maximum resident|Elapsed' "$scratch/time.txt" | sed 's/^[[:space:]]*/build-deb.sh: /' >&2 || true
fi
((status == 0)) || { printf 'build-deb.sh: dpkg-buildpackage failed (exit %d)\n' "$status" >&2; exit "$status"; }

[[ -n "$out" ]] || out="$repo/build-release"
mkdir -p -- "$out"
shopt -s nullglob
files=("$scratch"/*.deb "$scratch"/*.buildinfo)
((${#files[@]})) || die "no .deb produced"
for f in "${files[@]}"; do
    cp -- "$f" "$out/"
    printf '%s\n' "$out/$(basename -- "$f")"
done
