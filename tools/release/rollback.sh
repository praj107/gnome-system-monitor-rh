#!/usr/bin/env bash
# rollback.sh — undo first-install.sh: back to the distribution's package and
# the previous ~/.local install.
#
#   rollback.sh [--dry-run] [--disable-source]
#
#   1. asks a running app to quit (its own quit action, never a signal),
#   2. with sudo: installs the distribution's gnome-system-monitor; apt removes
#      gnome-system-monitor-rh because the two conflict,
#   3. with --disable-source: renames the apt source file to <name>.disabled so
#      apt stops offering the fork (nothing is deleted; rename it back to
#      re-enable it),
#   4. restores the retired ~/.local files (retire-user-install.sh --restore
#      latest), if there is a retired install to restore.
# Every step is printed before it runs; --dry-run prints them and changes
# nothing. To go to an older fork release instead, see tools/release/README.md.
#
# Tests only: --sysroot DIR (the root-owned parts below DIR, no sudo, apt-get -s
# against the machine's own package lists and a package status in which the
# fork replaces the distribution package) and --home DIR (the home whose
# ~/.local is restored).

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
dry=false; sysroot=""; home_dir="${HOME:-}"; disable_source=false
while (($#)); do
    case "$1" in
        --dry-run) dry=true; shift ;;
        --disable-source) disable_source=true; shift ;;
        --sysroot) sysroot="${2:?--sysroot needs a directory}"; shift 2 ;;
        --home) home_dir="${2:?--home needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'rollback.sh: unknown argument: %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
# shellcheck source=release.conf
source "$here/release.conf"
[[ -z "$sysroot" || ( "$sysroot" == /* && -d "$sysroot" ) ]] || die "--sysroot must be an existing absolute directory"
[[ "$home_dir" == /* && -d "$home_dir" ]] || die "--home must be an existing absolute directory"
gsm_root_paths
n=0; steps=4
step() { n=$((n + 1)); printf '\n[%d/%d] %s\n' "$n" "$steps" "$*"; }

step "Quit the running app (its own quit action, no signal)"
if [[ -n "$sysroot" ]]; then say "    skipped under --sysroot: the live app is not touched"; else quit_app; fi

step "Install the distribution's $GSM_RH_DISTRO_PACKAGE (apt removes $GSM_RH_PACKAGE)"
if [[ -n "$sysroot" ]]; then
    # A package status in which the fork is installed instead of the distribution
    # package, so that apt has something to remove. Written under the sysroot.
    deb="$(ls -- "$sysroot"/srv/gsm-rh-apt/"${GSM_RH_PACKAGE}"_*.deb 2>/dev/null | tail -n 1)"
    [[ -n "$deb" ]] || die "no $GSM_RH_PACKAGE package under $sysroot/srv/gsm-rh-apt: run first-install.sh --sysroot first"
    mkdir -p -- "$sysroot/var/lib/dpkg"
    status="$sysroot/var/lib/dpkg/status"
    awk -v pkg="$GSM_RH_DISTRO_PACKAGE" 'BEGIN { RS = ""; ORS = "\n\n" } $0 !~ "^Package: " pkg "\n"' /var/lib/dpkg/status > "$status"
    { dpkg-deb -f "$deb"; printf 'Status: install ok installed\n\n'; } >> "$status"
    GSM_RH_SIM_STATUS="$status"
    sim_apt_options real
    if "$dry"; then run apt-get -s "${SIM_APT[@]}" install "$GSM_RH_DISTRO_PACKAGE"; else
        run apt-get -s "${SIM_APT[@]}" install "$GSM_RH_DISTRO_PACKAGE" | tee "$sysroot/rollback-simulation.txt"
        grep -qF "Remv ${GSM_RH_PACKAGE} " "$sysroot/rollback-simulation.txt" || die "the simulated rollback does not remove $GSM_RH_PACKAGE"
        grep -qF "Inst ${GSM_RH_DISTRO_PACKAGE} " "$sysroot/rollback-simulation.txt" || die "the simulated rollback does not install $GSM_RH_DISTRO_PACKAGE"
    fi
else
    run sudo apt-get install "$GSM_RH_DISTRO_PACKAGE"
fi

step "Stop apt offering the fork (only with --disable-source)"
if "$disable_source"; then
    [[ -e "$R_SOURCE" ]] || "$dry" || die "no $R_SOURCE"
    root_run mv -T -- "$R_SOURCE" "$R_SOURCE.disabled"
else
    say "    skipped: the source stays ($R_SOURCE); rerun with --disable-source to rename it"
fi

step "Restore the retired ~/.local install"
shopt -s nullglob
backups=()
for d in "$home_dir"/.local/share/gsm-rh-retired-*; do [[ -f "$d/MANIFEST" && ! -e "$d/RESTORED" ]] && backups+=("$d"); done
shopt -u nullglob
if ((${#backups[@]} == 0)); then
    say "    no retired install to restore under $home_dir/.local/share"
else
    restore=("$here/retire-user-install.sh" --home "$home_dir" --restore latest)
    [[ -z "$sysroot" ]] || restore+=(--force-not-running-check)
    if "$dry"; then restore+=(--dry-run); fi
    printf '    $ %s\n' "${restore[*]}"
    "${restore[@]}" | sed 's/^/    /'
fi
if [[ -z "$sysroot" ]] && ! "$dry"; then
    printf '\nRolled back. Start the app again from the menu or with: %s/.local/bin/gnome-system-monitor-rh\n' "$home_dir"
fi
