#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Capture cgroup v2 fixtures for the collector's tests into DEST/<cgroup path>/.
#
#   capture-cgroup-fixtures.sh DEST CGROUP_PATH...
#
# CGROUP_PATH is relative to /sys/fs/cgroup ("/" for the root). Each path and
# all its ancestors are captured, because the collector treats a directory
# without cgroup.controllers as a group that was removed. Only the files the
# collector reads are ever copied; any other name is refused. The fixtures are
# PUBLIC. Run it unprivileged.
set -euo pipefail

readonly CGROUP_ROOT=/sys/fs/cgroup
readonly ALLOWED_FILES=(
  cgroup.controllers
  memory.current memory.peak memory.max memory.high memory.stat memory.events
  cpu.stat pids.current
  memory.pressure cpu.pressure io.pressure
)

usage() {
  echo "usage: $0 DEST CGROUP_PATH..." >&2
  exit 2
}

is_allowed() {
  local name=$1 allowed
  for allowed in "${ALLOWED_FILES[@]}"; do
    [[ "$name" == "$allowed" ]] && return 0
  done
  return 1
}

# The only function that reads a source file: it checks the name first.
capture_file() {
  local group=$1 name=$2 dest=$3
  if ! is_allowed "$name"; then
    echo "refusing to capture '$name': not one of ${ALLOWED_FILES[*]}" >&2
    exit 1
  fi
  # An absent file is skipped, as the collector would see it.
  cp -- "$CGROUP_ROOT$group/$name" "$dest$group/$name" 2>/dev/null || true
}

capture_group() {
  local group=$1 dest=$2 name
  [[ "$group" == /* && "$group" != *..* ]] || { echo "not a cgroup path: $group" >&2; exit 1; }
  group=${group%/}
  [[ -d "$CGROUP_ROOT$group" ]] || { echo "no such cgroup: $group" >&2; exit 1; }
  mkdir -p -- "$dest$group"
  for name in "${ALLOWED_FILES[@]}"; do
    capture_file "$group" "$name" "$dest"
  done
}

[[ $# -ge 2 ]] || usage
dest=$1
shift
[[ -d "$dest" ]] || { echo "no such directory: $dest" >&2; exit 1; }
for path in "$@"; do
  # Ancestors first, so the tree is whole.
  walked=""
  capture_group "/" "$dest"
  IFS=/ read -ra parts <<<"${path#/}"
  for part in "${parts[@]}"; do
    [[ -n "$part" ]] || continue
    walked="$walked/$part"
    capture_group "$walked" "$dest"
  done
done
