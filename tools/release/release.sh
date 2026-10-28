#!/usr/bin/env bash
# release.sh — a later release: build the .deb from HEAD, publish it signed into
# the staging repository, and print the one sudo command that puts it where apt
# reads it. Never installs, never uses sudo, never changes a version: edit
# debian/changelog (a real distribution, not UNRELEASED) and commit it first.
#
#   release.sh [--dry-run] [--deb FILE]
#
#   --deb FILE  publish this already built package instead of building HEAD
#   --dry-run   print the steps; build and publish nothing
#
# gpg asks for the release key's passphrase when it signs, so run this from a
# terminal as the key's owner. The first release is first-install.sh.
#
# Tests only: --key FPR --gnupghome DIR --staging DIR sign with a throwaway
# key into another staging directory.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo="$(cd -- "$here/../.." && pwd -P)"
dry=false; deb=""; key_override=""; gnupghome=""; staging=""; sysroot=""
while (($#)); do
    case "$1" in
        --dry-run) dry=true; shift ;;
        --deb) deb="${2:?--deb needs a file}"; shift 2 ;;
        --key) key_override="${2:?--key needs a fingerprint}"; shift 2 ;;
        --gnupghome) gnupghome="${2:?--gnupghome needs a directory}"; shift 2 ;;
        --staging) staging="${2:?--staging needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'release.sh: unknown argument: %s (see --help)\n' "$1" >&2; exit 2 ;;
    esac
done
# shellcheck source=release.conf
source "$here/release.conf"
gsm_root_paths
fpr="${key_override:-$GSM_RH_RELEASE_KEY_FPR}"
[[ -n "$staging" ]] || staging="$GSM_RH_STAGING_REPO"

version="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Version)"
distribution="$(dpkg-parsechangelog -l "$repo/debian/changelog" -S Distribution)"
[[ "$distribution" != UNRELEASED ]] || die "debian/changelog is still UNRELEASED: finalise and commit it first"
if [[ -z "$deb" ]]; then
    git -C "$repo" diff --quiet HEAD -- debian/changelog || die "debian/changelog has uncommitted changes; the build uses HEAD"
    deb="$repo/build-release/${GSM_RH_PACKAGE}_${version}_amd64.deb"
fi
say "Release $GSM_RH_PACKAGE $version ($distribution)"

say ""; say "[1/3] Build the package from HEAD"
if [[ "$deb" == "$repo/build-release/"* ]]; then run "$here/build-deb.sh"; else say "    skipped: using $deb"; fi
if ! "$dry"; then
    [[ -f "$deb" ]] || die "no package at $deb"
    [[ "$(dpkg-deb -f "$deb" Version)" == "$version" ]] || die "$deb is not version $version"
fi

say ""; say "[2/3] Publish into $staging (gpg asks for the passphrase)"
publish=("$here/publish.sh" --repo "$staging" --key "$fpr")
[[ -z "$gnupghome" ]] || publish+=(--gnupghome "$gnupghome")
run "${publish[@]}" "$deb"

say ""; say "[3/3] Yours to run: copy into the root-owned repository (nothing is deleted from it)"
cat <<COPY
    sudo install -m 644 -o root -g root -t $R_REPO -- \\
        $staging/*.deb $staging/Packages $staging/Packages.gz \\
        $staging/gsm-rh-archive-key.gpg $staging/Release $staging/Release.gpg $staging/InRelease
    sudo apt update && sudo apt install $GSM_RH_PACKAGE
COPY
say "    (or wait for the update manager to offer it). To go back: tools/release/rollback.sh"
