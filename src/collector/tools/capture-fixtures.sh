#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Capture procfs fixtures for the collector's tests into DEST/<pid>/<file>.
#
#   capture-fixtures.sh DEST PID...      copy the allowed files of those pids
#   capture-fixtures.sh DEST --standard  pid 1, pid 2, one kernel worker, and a
#                                        sleep and a shell started here
#
# The fixtures are PUBLIC files, so only the four files the collector reads
# are ever copied. Any other name is refused: never cmdline, environ, maps or
# fd (session environments carry tokens). Run it unprivileged.
set -euo pipefail

readonly ALLOWED_FILES=(stat status schedstat cgroup)

usage() {
  echo "usage: $0 DEST PID... | $0 DEST --standard" >&2
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
  local pid=$1 name=$2 dest=$3
  if ! is_allowed "$name"; then
    echo "refusing to capture '$name': not one of ${ALLOWED_FILES[*]}" >&2
    exit 1
  fi
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || { echo "not a pid: $pid" >&2; exit 1; }
  # A vanished or unreadable file is skipped, as the collector would see it.
  cp -- "/proc/$pid/$name" "$dest/$pid/$name" 2>/dev/null || echo "skipped /proc/$pid/$name" >&2
}

capture_pid() {
  local pid=$1 dest=$2 name
  mkdir -p -- "$dest/$pid"
  for name in "${ALLOWED_FILES[@]}"; do
    capture_file "$pid" "$name" "$dest"
  done
}

first_kernel_worker() {
  local stat_file pid comm
  for stat_file in /proc/[0-9]*/stat; do
    pid=${stat_file#/proc/}; pid=${pid%/stat}
    comm=$(cut -d' ' -f2 "$stat_file" 2>/dev/null) || continue
    if [[ "$comm" == "(kworker/"* && "$pid" -gt 2 ]]; then
      echo "$pid"
      return 0
    fi
  done
  return 1
}

[[ $# -ge 2 ]] || usage
dest=$1; shift
[[ -d "$dest" ]] || { echo "DEST is not a directory: $dest" >&2; exit 1; }

if [[ "$1" != "--standard" ]]; then
  for pid in "$@"; do capture_pid "$pid" "$dest"; done
  exit 0
fi

# Processes this script starts itself; it signals only these two, by the
# pids it recorded. mutate: ignore -- signals, owner policy 2026-10-18.
sleep 300 &
sleep_pid=$!
bash -c 'while :; do sleep 1; done' &
shell_pid=$!
trap 'kill "$sleep_pid" "$shell_pid" 2>/dev/null || true' EXIT
sleep 0.3

worker_pid=$(first_kernel_worker || true)
for pid in 1 2 $worker_pid "$sleep_pid" "$shell_pid"; do capture_pid "$pid" "$dest"; done
echo "captured: 1 2 ${worker_pid:-<no kworker>} $sleep_pid $shell_pid"
