#!/usr/bin/env bash
# first-install.sh — the owner's one-time install of the signed package.
#
#   first-install.sh [--dry-run] [--deb FILE] [--staging DIR]
#
# Run it from a terminal as the key's owner. It
#   1. checks that the key and the release .deb are there and nothing is
#      half-done,
#   2. publishes the .deb into a staging repository, signed with the release
#      key (gpg asks for the key's passphrase),
#   3. with sudo: creates the root-owned repository directory, copies the
#      staging repository into it (nothing is deleted from it), installs the
#      repository's public key and the deb822 source file, refreshes apt's
#      list for that one source,
#   4. asks a running app to quit (an action, never a signal),
#   5. retires the old ~/.local install (retire-user-install.sh: files are
#      moved, not deleted),
#   6. installs the package with apt (which removes the distribution's
#      gnome-system-monitor: Conflicts/Replaces), and
#   7. verifies the result (check-install.sh).
# Every step is printed before it runs; --dry-run prints them all and changes
# nothing. If anything fails, rollback.sh undoes it.
#
# Tests only: --sysroot DIR writes the root-owned parts below DIR without
# sudo and replaces the install with `apt-get -s` against DIR's repository;
# --home DIR is the home whose ~/.local is retired; --key FPR and --gnupghome
# DIR (allowed only with --sysroot) sign with a throwaway key.
#
# Settings (key fingerprint, paths): release.conf.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd -- "$here/../.." && pwd -P)"

dry=false; sysroot=""; home_dir="${HOME:-}"; deb=""; staging=""; key_override=""; gnupghome=""
while (($#)); do
    case "$1" in
        --dry-run) dry=true; shift ;;
        --sysroot) sysroot="${2:?--sysroot needs a directory}"; shift 2 ;;
        --home) home_dir="${2:?--home needs a directory}"; shift 2 ;;
        --deb) deb="${2:?--deb needs a file}"; shift 2 ;;
        --staging) staging="${2:?--staging needs a directory}"; shift 2 ;;
        --key) key_override="${2:?--key needs a fingerprint}"; shift 2 ;;
        --gnupghome) gnupghome="${2:?--gnupghome needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'first-install.sh: unknown argument: %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
# shellcheck source=release.conf
source "$here/release.conf"
if [[ -n "$sysroot" ]]; then
    [[ "$sysroot" == /* && -d "$sysroot" ]] || die "--sysroot must be an existing absolute directory"
elif [[ -n "$key_override" || -n "$gnupghome" ]]; then
    die "--key and --gnupghome are for tests under --sysroot; the release key is fixed in release.conf"
fi
[[ "$home_dir" == /* && -d "$home_dir" ]] || die "--home must be an existing absolute directory"
fpr="${key_override:-$GSM_RH_RELEASE_KEY_FPR}"
gsm_root_paths
[[ -n "$staging" ]] || { [[ -n "$sysroot" ]] && staging="$sysroot/staging" || staging="$GSM_RH_STAGING_REPO"; }
gpg_home=(); [[ -z "$gnupghome" ]] || gpg_home=(--homedir "$gnupghome")

version="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Version)"
distribution="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Distribution)"
[[ -n "$deb" ]] || deb="$repo/build-release/${GSM_RH_PACKAGE}_${version}_amd64.deb"
n=0; steps=7
step() { n=$((n + 1)); printf '\n[%d/%d] %s\n' "$n" "$steps" "$*"; }

# Scratch space for the key check and the source file: tools/scratch.sh when
# present (RAM), else a marked directory removed by a checked function (real
# path, directly under the root chosen here, marker naming it).
scratch=""
readonly SCRATCH_MARKER=".gsm-rh-first-install"
scratch_root="${XDG_RUNTIME_DIR:-/tmp}/gsm-rh-first-install"
make_scratch() {
    if [[ -x "$repo/tools/scratch.sh" ]]; then scratch="$("$repo/tools/scratch.sh" new ram first-install)"; return; fi
    mkdir -p -- "$scratch_root"
    scratch="$(mktemp -d -- "$scratch_root/run.XXXXXX")"
    basename -- "$scratch" > "$scratch/$SCRATCH_MARKER"
}
drop_scratch() {
    local real root
    [[ -n "$scratch" && -d "$scratch" ]] || return 0
    if [[ -x "$repo/tools/scratch.sh" ]]; then "$repo/tools/scratch.sh" rm "$scratch" >/dev/null; scratch=""; return 0; fi
    real="$(realpath -e -- "$scratch")"; root="$(realpath -e -- "$scratch_root")"
    [[ "$real" != "$root" && "$(dirname -- "$real")" == "$root" \
        && -f "$real/$SCRATCH_MARKER" && "$(cat -- "$real/$SCRATCH_MARKER")" == "$(basename -- "$real")" ]] \
        || { printf 'first-install.sh: not removing %s\n' "$real" >&2; return 0; }
    rm -rf -- "$real"
    scratch=""
}
trap 'drop_scratch || printf "first-install.sh: could not remove %s\n" "$scratch" >&2' EXIT

# ------------------------------------------------------------------ 1 checks
step "Checks: key, release .deb, nothing half-done"
for tool in dpkg-deb dpkg-parsechangelog gpg apt-get; do command -v "$tool" >/dev/null || die "needs $tool"; done
[[ "$distribution" != UNRELEASED ]] || die "debian/changelog is still UNRELEASED: finalise and commit it first"
if "$dry"; then
    say "    would check: secret key $fpr is in ${gnupghome:-the default keyring}"
else
    gpg "${gpg_home[@]}" --list-secret-keys --with-colons -- "$fpr" 2>/dev/null | grep -q "^fpr:::::::::$fpr:" \
        || die "no secret key with fingerprint $fpr"
fi
[[ -f "$deb" ]] || die "no release package at $deb; build it with tools/release/build-deb.sh"
[[ "$(dpkg-deb -f "$deb" Version)" == "$version" ]] || die "$deb is not version $version"
[[ "$(dpkg-deb -f "$deb" Package)" == "$GSM_RH_PACKAGE" ]] || die "$deb is not package $GSM_RH_PACKAGE"
say "    release $GSM_RH_PACKAGE $version ($distribution): $deb"
if [[ -z "$sysroot" ]] && dpkg-query -W -f='${Version}' "$GSM_RH_PACKAGE" 2>/dev/null | grep -qx -- "$version"; then
    die "$GSM_RH_PACKAGE $version is already installed; use release.sh for a later release"
fi
if [[ -e "$R_SOURCE" ]] && ! grep -q -- "URIs: file://$R_REPO\$" "$R_SOURCE"; then
    die "$R_SOURCE exists and names another repository; look at it before going on"
fi
if [[ -e "$R_KEYRING" ]] && ! "$dry"; then say "    note: $R_KEYRING exists; it is replaced by the key just published (same key on a re-run)"; fi

# --------------------------------------------------------------- 2 publish
step "Publish into the staging repository $staging (gpg asks for the passphrase)"
publish=("$here/publish.sh" --repo "$staging" --key "$fpr")
[[ -z "$gnupghome" ]] || publish+=(--gnupghome "$gnupghome")
run "${publish[@]}" "$deb"

# ------------------------------------------------------------- 3 root setup
step "Root setup: repository directory, public key, apt source, package list"
if ! "$dry"; then
    [[ -f "$staging/gsm-rh-archive-key.gpg" ]] || die "staging has no gsm-rh-archive-key.gpg"
    # The key apt will trust must be the release key and nothing else.
    make_scratch
    mkdir -m 700 -- "$scratch/gh"   # gpg does not create its home directory
    keyfprs="$(gpg --homedir "$scratch/gh" --show-keys --with-colons "$staging/gsm-rh-archive-key.gpg" 2>/dev/null | awk -F: '$1 == "pub" {pub = 1; next} $1 == "sub" {pub = 0} pub && $1 == "fpr" {print $10; pub = 0}')"
    [[ "$keyfprs" == "$fpr" ]] || die "the staged public key is $keyfprs, not $fpr"
    say "    staged public key is $fpr"
fi
owner=(); [[ -n "$sysroot" ]] || owner=(-o root -g root)
root_run install -d -m 755 "${owner[@]}" "$R_REPO"
# Package files first, indexes next, signatures last: apt never sees a signed
# index that names a file that is not there yet. Nothing is deleted, so older
# packages stay available for a rollback to them.
if "$dry"; then
    say "    (for each of the staging directory's *.deb, then Packages, Packages.gz, gsm-rh-archive-key.gpg, Release, Release.gpg, InRelease:)"
    files=("$staging"/'*.deb' "$staging/Packages" "$staging/Packages.gz" "$staging/gsm-rh-archive-key.gpg" "$staging/Release" "$staging/Release.gpg" "$staging/InRelease")
else
    files=("$staging"/*.deb "$staging/Packages" "$staging/Packages.gz" "$staging/gsm-rh-archive-key.gpg" "$staging/Release" "$staging/Release.gpg" "$staging/InRelease")
fi
for f in "${files[@]}"; do root_run install -m 644 "${owner[@]}" -t "$R_REPO" -- "$f"; done
root_run install -D -m 644 "${owner[@]}" "$staging/gsm-rh-archive-key.gpg" "$R_KEYRING"
source_text="$(printf 'Types: deb\nURIs: file://%s\nSuites: ./\nSigned-By: %s\n' "$R_REPO" "$R_KEYRING")"
say "    the apt source ($R_SOURCE):"
printf '%s\n' "$source_text" | sed 's/^/      | /'
if ! "$dry"; then printf '%s\n' "$source_text" > "$scratch/gsm-rh.sources"; fi
root_run install -D -m 644 "${owner[@]}" "${scratch:-<scratch>}/gsm-rh.sources" "$R_SOURCE"
# Refresh only this source: no network, and the other lists are left alone.
if [[ -n "$sysroot" ]]; then
    sim_apt_options fork
    mkdir -p -- "$sysroot/var/lib/apt/lists/partial" "$sysroot/var/cache/apt/archives/partial"
    run apt-get "${SIM_APT[@]}" update
else
    run sudo apt-get update -o Dir::Etc::sourcelist="sources.list.d/${GSM_RH_SOURCE_FILE##*/}" \
        -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0
fi

# ------------------------------------------------------------------ 4 quit
step "Quit the running app (its own quit action, no signal)"
if [[ -n "$sysroot" ]]; then say "    skipped under --sysroot: the live app is not touched"; else quit_app; fi

# ----------------------------------------------------------------- 5 retire
step "Retire the old ~/.local install (files are moved, never deleted)"
retire=("$here/retire-user-install.sh" --home "$home_dir")
[[ -z "$sysroot" ]] || retire+=(--force-not-running-check)
if "$dry"; then retire+=(--dry-run); fi
printf '    $ %s\n' "${retire[*]}"
"${retire[@]}" | sed 's/^/    /'

# ---------------------------------------------------------------- 6 install
step "Install $GSM_RH_PACKAGE (apt removes the distribution's $GSM_RH_DISTRO_PACKAGE)"
if [[ -n "$sysroot" ]]; then
    sim_apt_options fork
    if "$dry"; then run apt-get -s "${SIM_APT[@]}" install "$GSM_RH_PACKAGE"; else
        run apt-get -s "${SIM_APT[@]}" install "$GSM_RH_PACKAGE" | tee "$sysroot/install-simulation.txt"
    fi
else
    run sudo apt-get install "$GSM_RH_PACKAGE"
fi

# ---------------------------------------------------------------- 7 verify
step "Verify"
if "$dry"; then
    say "    would run: $here/check-install.sh --version $version --home $home_dir"
    say "    (package installed and the distribution's gone; /usr/bin wins on PATH; pkaction names"
    say "    the /usr/libexec helpers; the system D-Bus service file names /usr/bin; the sampler is"
    say "    not enabled system-wide; nothing in ~/.local/share shadows the package; --help runs)"
    exit 0
fi
if [[ -n "$sysroot" ]]; then
    grep -qF "Remv ${GSM_RH_DISTRO_PACKAGE} " "$sysroot/install-simulation.txt" || die "the simulated install does not remove $GSM_RH_DISTRO_PACKAGE"
    grep -qF "Inst ${GSM_RH_PACKAGE} (${version} " "$sysroot/install-simulation.txt" || die "the simulated install does not install $GSM_RH_PACKAGE $version"
    say "    simulation: $GSM_RH_DISTRO_PACKAGE removed, $GSM_RH_PACKAGE $version installed (nothing was installed)"
    exit 0
fi
# The checks live in check-install.sh (read-only, also usable on its own).
fail=0
"$here/check-install.sh" --version "$version" --home "$home_dir" || fail=1
if ((fail)); then
    say ""; say "Some checks failed. To go back: $here/rollback.sh"
    exit 1
fi
say ""; say "Installed. Upgrades arrive through apt and the update manager and ask for your password."
say "To go back to the distribution's package and the old install: $here/rollback.sh"
