#!/usr/bin/env bash
# release.sh — a later release, in one run: build the .deb from HEAD, publish it
# signed into the staging repository, copy it into the root-owned repository,
# refresh apt for that one source, and stop. Update Manager (or `apt upgrade`)
# then offers the upgrade; with --install this script runs it and verifies.
#
#   release.sh [--dry-run] [--deb FILE] [--install]
#
#   --deb FILE  publish this already built package instead of building HEAD
#   --install   after the refresh, quit a running app (its own quit action, no
#               signal), `sudo apt-get install` the package and run
#               check-install.sh
#   --dry-run   print every step; build, publish and copy nothing
#
# Every step is printed before it runs; sudo is used only where printed. gpg
# asks for the release key's passphrase when it signs, so run this from a
# terminal as the key's owner. Nothing is deleted from the repository, so
# older packages stay available for a rollback. The version is never changed
# here: edit debian/changelog (a real distribution, not UNRELEASED) and commit
# it first. The first release is first-install.sh.
#
# Tests only: --sysroot DIR writes the root-owned parts below DIR without sudo
# and replaces the install with `apt-get -s`; --key FPR and --gnupghome DIR
# (allowed only with --sysroot) sign with a throwaway key; --staging DIR;
# --home DIR is the home check-install.sh looks at.
#
# Settings (key fingerprint, paths): release.conf.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd -- "$here/../.." && pwd -P)"
dry=false; deb=""; key_override=""; gnupghome=""; staging=""; sysroot=""; install=false; home_dir="${HOME:-}"
while (($#)); do
    case "$1" in
        --dry-run) dry=true; shift ;;
        --install) install=true; shift ;;
        --deb) deb="${2:?--deb needs a file}"; shift 2 ;;
        --key) key_override="${2:?--key needs a fingerprint}"; shift 2 ;;
        --gnupghome) gnupghome="${2:?--gnupghome needs a directory}"; shift 2 ;;
        --staging) staging="${2:?--staging needs a directory}"; shift 2 ;;
        --sysroot) sysroot="${2:?--sysroot needs a directory}"; shift 2 ;;
        --home) home_dir="${2:?--home needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'release.sh: unknown argument: %s (see --help)\n' "$1" >&2; exit 2 ;;
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
gsm_root_paths
fpr="${key_override:-$GSM_RH_RELEASE_KEY_FPR}"
[[ -n "$staging" ]] || { [[ -n "$sysroot" ]] && staging="$sysroot/staging" || staging="$GSM_RH_STAGING_REPO"; }
gpg_home=(); [[ -z "$gnupghome" ]] || gpg_home=(--homedir "$gnupghome")
n=0; steps=6
step() { n=$((n + 1)); printf '\n[%d/%d] %s\n' "$n" "$steps" "$*"; }

# ------------------------------------------------------------------ 1 checks
step "Checks: key, version, package"
for tool in dpkg-deb dpkg-parsechangelog gpg apt-get; do command -v "$tool" >/dev/null || die "needs $tool"; done
if [[ -z "$deb" ]]; then
    # Building HEAD: the version is the committed changelog's.
    version="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Version)"
    distribution="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Distribution)"
    # A dry run only reports these: it changes nothing, so it may be run to
    # see the steps before the changelog is final.
    refuse() { if "$dry"; then say "    would refuse: $*"; else die "$*"; fi; }
    [[ "$distribution" != UNRELEASED ]] || refuse "debian/changelog is still UNRELEASED: finalise and commit it first"
    git -C "$repo" diff --quiet HEAD -- debian/changelog || refuse "debian/changelog has uncommitted changes; the build uses HEAD"
    deb="$repo/build-release/${GSM_RH_PACKAGE}_${version}_amd64.deb"
    build=true
else
    [[ -f "$deb" ]] || die "no package at $deb"
    [[ "$(dpkg-deb -f "$deb" Package)" == "$GSM_RH_PACKAGE" ]] || die "$deb is not package $GSM_RH_PACKAGE"
    version="$(dpkg-deb -f "$deb" Version)"
    distribution="(from --deb)"
    build=false
fi
say "    release $GSM_RH_PACKAGE $version $distribution"
if "$dry"; then
    say "    would check: secret key $fpr is in ${gnupghome:-the default keyring}"
else
    gpg "${gpg_home[@]}" --list-secret-keys --with-colons -- "$fpr" 2>/dev/null | grep -q "^fpr:::::::::$fpr:" \
        || die "no secret key with fingerprint $fpr"
fi
installed="$(dpkg-query -W -f='${Version}' "$GSM_RH_PACKAGE" 2>/dev/null || true)"
if [[ -n "$installed" ]]; then
    dpkg --compare-versions "$version" gt "$installed" \
        || die "$version is not newer than the installed $installed; edit debian/changelog"
    say "    upgrade: installed $installed -> $version"
else
    say "    $GSM_RH_PACKAGE is not installed; use first-install.sh for the first release"
fi
[[ -e "$R_SOURCE" ]] || die "$R_SOURCE does not exist: the repository was never set up (first-install.sh)"
grep -q -- "URIs: file://$R_REPO\$" "$R_SOURCE" || die "$R_SOURCE names another repository; look at it before going on"

# ------------------------------------------------------------------- 2 build
step "Build the package from HEAD"
if "$build"; then run "$here/build-deb.sh"; else say "    skipped: using $deb"; fi
if ! "$dry"; then
    [[ -f "$deb" ]] || die "no package at $deb"
    [[ "$(dpkg-deb -f "$deb" Version)" == "$version" ]] || die "$deb is not version $version"
fi

# ----------------------------------------------------------------- 3 publish
step "Publish into $staging (gpg asks for the passphrase)"
publish=("$here/publish.sh" --repo "$staging" --key "$fpr")
[[ -z "$gnupghome" ]] || publish+=(--gnupghome "$gnupghome")
run "${publish[@]}" "$deb"

# ------------------------------------------------------------------ 4 copy
step "Copy into the root-owned repository $R_REPO (nothing is deleted from it)"
owner=(); [[ -n "$sysroot" ]] || owner=(-o root -g root)
# Package files first, indexes next, signatures last: apt never sees a signed
# index that names a file that is not there yet (the same order as
# first-install.sh).
if "$dry"; then
    say "    (for each of the staging directory's *.deb, then Packages, Packages.gz, gsm-rh-archive-key.gpg, Release, Release.gpg, InRelease:)"
    files=("$staging"/'*.deb' "$staging/Packages" "$staging/Packages.gz" "$staging/gsm-rh-archive-key.gpg" "$staging/Release" "$staging/Release.gpg" "$staging/InRelease")
else
    files=("$staging"/*.deb "$staging/Packages" "$staging/Packages.gz" "$staging/gsm-rh-archive-key.gpg" "$staging/Release" "$staging/Release.gpg" "$staging/InRelease")
fi
for f in "${files[@]}"; do root_run install -m 644 "${owner[@]}" -t "$R_REPO" -- "$f"; done

# ------------------------------------------------------------------ 5 refresh
step "Refresh apt's list for this one source (no network; the other lists are left alone)"
if [[ -n "$sysroot" ]]; then
    sim_apt_options fork
    mkdir -p -- "$sysroot/var/lib/apt/lists/partial" "$sysroot/var/cache/apt/archives/partial"
    run apt-get "${SIM_APT[@]}" update
else
    run sudo apt-get update -o Dir::Etc::sourcelist="sources.list.d/${GSM_RH_SOURCE_FILE##*/}" \
        -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0
fi
if ! "$dry"; then
    # What the upgrade would do, as a simulation (changes nothing).
    if [[ -n "$sysroot" ]]; then sim_apt_options fork; else SIM_APT=(); fi
    run apt-get -s "${SIM_APT[@]}" install --only-upgrade "$GSM_RH_PACKAGE" | tee "${sysroot:-${staging}}/upgrade-simulation.txt" | sed 's/^/    /'
    if [[ -n "$installed" ]] && ! grep -qF "Inst ${GSM_RH_PACKAGE} [${installed}] (${version} " "${sysroot:-${staging}}/upgrade-simulation.txt"; then
        die "apt does not offer $installed -> $version; look at ${sysroot:-${staging}}/upgrade-simulation.txt"
    fi
fi

# --------------------------------------------------------------- 6 install
if ! "$install"; then
    step "Stop here: Update Manager will offer the upgrade"
    say "    $GSM_RH_PACKAGE $version is in the repository and apt's list is fresh. Update Manager (or"
    say "    \`sudo apt upgrade\`) offers it, and asks for your password; opening it and finding"
    say "    $version listed is the check that the upgrade path works. To install it from here instead:"
    say "    run this script again with --install (or: sudo apt-get install $GSM_RH_PACKAGE)."
    say "    To go back: $here/rollback.sh"
    exit 0
fi
step "Install $GSM_RH_PACKAGE $version and verify"
if [[ -n "$sysroot" ]]; then
    say "    skipped under --sysroot: the live app is not touched"
    sim_apt_options fork
    run apt-get -s "${SIM_APT[@]}" install "$GSM_RH_PACKAGE" | tee "$sysroot/install-simulation.txt" | sed 's/^/    /'
    "$dry" && exit 0
    grep -qF "Inst ${GSM_RH_PACKAGE} " "$sysroot/install-simulation.txt" || die "the simulated install does not install $GSM_RH_PACKAGE"
    grep -qE "Inst ${GSM_RH_PACKAGE} (\[[^]]*\] )?\(${version//+/\\+} " "$sysroot/install-simulation.txt" || die "the simulated install is not $version"
    say "    simulation: $GSM_RH_PACKAGE would be upgraded to $version (nothing was installed)"
    exit 0
fi
quit_app
run sudo apt-get install "$GSM_RH_PACKAGE"
"$dry" && { say "    would run: $here/check-install.sh --version $version --home $home_dir"; exit 0; }
"$here/check-install.sh" --version "$version" --home "$home_dir" || { say ""; say "Some checks failed. To go back: $here/rollback.sh"; exit 1; }
say ""; say "Installed $version. To go back: $here/rollback.sh"
