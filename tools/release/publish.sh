#!/usr/bin/env bash
# publish.sh — put .deb files into a signed local flat apt repository.
#
#   publish.sh --repo DIR --key KEYID [--gnupghome DIR] DEB...
#
# Checks every .deb first, then (and only then) copies them into DIR, rebuilds
# Packages, Packages.gz and Release, signs InRelease (clear-signed) and
# Release.gpg (detached), verifies both signatures with gpgv against the
# exported public key (what apt does), and writes the public key beside them
# as gsm-rh-archive-key.gpg. A source entry for the result:
#
#   Types: deb
#   URIs: file:///srv/gsm-rh-apt
#   Suites: ./
#   Signed-By: /etc/apt/keyrings/gsm-rh.gpg
#
# (the exact path with a trailing slash is a "flat" repository: no dists tree,
# sources.list(5); the archive layout is the Debian repository format,
# https://wiki.debian.org/DebianRepository/Format).
#
# Refused, with nothing written:
#   * a .deb whose ELF files under usr/bin or usr/libexec define a dev_ipc
#     symbol or carry the GSM_DEV_IPC switch (the developer control socket, a dev-only
#     surface that must never ship; the same test as debian/gate-check.sh),
#   * a .deb whose package, version and architecture are already in the repo
#     with different contents (a version is published once; bump it).
# Publishing the same file again is a no-op for that file.
#
# The signing key is the owner's; gpg asks for the passphrase itself, so run
# this from a terminal as the key's owner. In production the repository
# directory is root-owned (the user's own processes cannot publish): build the
# repository in a user-owned staging directory with this script, then copy it
# over as root. See tools/release/README.md. This script never uses sudo.
#
# Optional helpers, as in build-deb.sh: tools/scratch.sh for the scratch
# directory (fallback: a marked directory under ${XDG_RUNTIME_DIR:-/tmp},
# removed by a checked function here).

set -euo pipefail

die() { printf 'publish.sh: %s\n' "$*" >&2; exit 2; }
note() { printf 'publish.sh: %s\n' "$*" >&2; }

repo_src="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
readonly SCRATCH_MARKER=".gsm-rh-publish"
readonly KEY_FILE="gsm-rh-archive-key.gpg"

repo=""; key=""; gnupghome=""; debs=()
while (($#)); do
    case "$1" in
        --repo) repo="${2:?--repo needs a directory}"; shift 2 ;;
        --key) key="${2:?--key needs a key id or fingerprint}"; shift 2 ;;
        --gnupghome) gnupghome="${2:?--gnupghome needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --*) die "unknown option: $1" ;;
        *) debs+=("$1"); shift ;;
    esac
done
[[ -n "$repo" && -n "$key" ]] && ((${#debs[@]})) || die "usage: publish.sh --repo DIR --key KEYID [--gnupghome DIR] DEB..."
for tool in dpkg-deb apt-ftparchive gpg gpgv gzip nm sha256sum; do
    command -v "$tool" >/dev/null || die "needs $tool"
done
for deb in "${debs[@]}"; do [[ -f "$deb" ]] || die "no such file: $deb"; done

gpg_cmd=(gpg --batch --yes)
if [[ -n "$gnupghome" ]]; then gpg_cmd+=(--homedir "$gnupghome"); fi
if [[ -t 0 ]]; then GPG_TTY="$(tty)"; export GPG_TTY; fi

# ----------------------------------------------------------------- scratch

scratch=""
scratch_fallback_root="${XDG_RUNTIME_DIR:-/tmp}/gsm-rh-publish"
remove_scratch_checked() {
    local real root
    real="$(realpath -e -- "$scratch")"; root="$(realpath -e -- "$scratch_fallback_root")"
    [[ "$real" != "$root" && "$(dirname -- "$real")" == "$root" ]] || die "refusing to remove $real"
    [[ -f "$real/$SCRATCH_MARKER" && ! -L "$real/$SCRATCH_MARKER" \
        && "$(cat -- "$real/$SCRATCH_MARKER")" == "$(basename -- "$real")" ]] || die "refusing to remove $real: no matching marker"
    rm -rf -- "$real"
}
if [[ -x "$repo_src/tools/scratch.sh" ]]; then
    scratch="$("$repo_src/tools/scratch.sh" new ram publish)"
    drop_scratch() { "$repo_src/tools/scratch.sh" rm "$scratch" >/dev/null; }
else
    mkdir -p -- "$scratch_fallback_root"
    scratch="$(mktemp -d -- "$scratch_fallback_root/publish.XXXXXX")"
    basename -- "$scratch" > "$scratch/$SCRATCH_MARKER"
    drop_scratch() { remove_scratch_checked; }
fi
trap 'drop_scratch || note "could not remove $scratch"' EXIT

# ------------------------------------------------------------------ checks

# The same test as debian/gate-check.sh symbols: no ELF file defines a
# dev_ipc symbol or carries the GSM_DEV_IPC run-time switch.
check_no_dev_ipc() { # deb
    local tree="$scratch/tree" file checked=0
    mkdir -p -- "$tree"
    dpkg-deb -x "$1" "$tree" || die "cannot unpack $1"
    for file in "$tree"/usr/bin/* "$tree"/usr/libexec/*/*; do
        [[ -f "$file" && ! -L "$file" ]] || continue
        [[ "$(head -c 4 -- "$file" | od -An -c | tr -d ' ')" == '177ELF' ]] || continue
        checked=$((checked + 1))
        if { nm -D --defined-only "$file"; nm "$file" 2>/dev/null || true; } | grep -qi 'dev_ipc'; then
            die "REFUSED $1: ${file#"$tree"} defines a dev_ipc symbol (dev-only surfaces never ship)"
        fi
        if grep -aq 'GSM_DEV_IPC' "$file"; then
            die "REFUSED $1: ${file#"$tree"} contains the GSM_DEV_IPC switch (dev-only surfaces never ship)"
        fi
    done
    ((checked > 0)) || die "REFUSED $1: no ELF files under usr/bin or usr/libexec, so nothing could be checked"
    # Only the check's own extraction is left behind: the scratch dir goes at exit.
    note "$1: dev_ipc gate passed ($checked ELF files)"
}

# An existing .deb in the repo with the same package, version and
# architecture must be byte-identical.
check_version_free() { # deb
    local pkg ver arch other
    pkg="$(dpkg-deb -f "$1" Package)"; ver="$(dpkg-deb -f "$1" Version)"; arch="$(dpkg-deb -f "$1" Architecture)"
    [[ -n "$pkg" && -n "$ver" && -n "$arch" ]] || die "REFUSED $1: missing Package, Version or Architecture"
    [[ -d "$repo" ]] || return 0
    for other in "$repo"/*.deb; do
        [[ -f "$other" ]] || continue
        [[ "$(dpkg-deb -f "$other" Package)" == "$pkg" && "$(dpkg-deb -f "$other" Version)" == "$ver" \
            && "$(dpkg-deb -f "$other" Architecture)" == "$arch" ]] || continue
        if ! cmp -s -- "$1" "$other"; then
            die "REFUSED $1: $pkg $ver ($arch) is already in the repo as $(basename -- "$other") with different contents; a version is published once, bump it"
        fi
    done
}

"${gpg_cmd[@]}" --list-secret-keys -- "$key" >/dev/null 2>&1 || die "no secret key '$key' in ${gnupghome:-the default keyring}"
for deb in "${debs[@]}"; do
    check_version_free "$deb"
    check_no_dev_ipc "$deb"
done

# ----------------------------------------------------------------- publish

mkdir -p -- "$repo"
repo="$(realpath -e -- "$repo")"
archs=()
for deb in "${debs[@]}"; do
    pkg="$(dpkg-deb -f "$deb" Package)"; ver="$(dpkg-deb -f "$deb" Version)"; arch="$(dpkg-deb -f "$deb" Architecture)"
    name="${pkg}_${ver//:/%3a}_${arch}.deb"
    archs+=("$arch")
    if [[ -f "$repo/$name" ]]; then note "$name is already published (identical)"; continue; fi
    cp -- "$deb" "$repo/.$name.new"
    chmod 644 -- "$repo/.$name.new"   # apt reads as another user; do not inherit a private umask
    mv -T -- "$repo/.$name.new" "$repo/$name"
    note "added $name"
done

# Indexes, Release and signatures are built and verified in scratch, then
# moved into the repo one file at a time, signatures last.
idx="$scratch/index"
mkdir -p -- "$idx"
(cd -- "$repo" && apt-ftparchive packages .) > "$idx/Packages"
[[ -s "$idx/Packages" ]] || die "apt-ftparchive produced an empty Packages"
gzip -9nc -- "$idx/Packages" > "$idx/Packages.gz"

architectures="$(for d in "$repo"/*.deb; do dpkg-deb -f "$d" Architecture; done | sort -u | tr '\n' ' ')"
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=gnome-system-monitor-rh \
    -o APT::FTPArchive::Release::Label=gnome-system-monitor-rh \
    -o APT::FTPArchive::Release::Architectures="${architectures% }" \
    release "$idx" > "$scratch/Release.new"   # outside $idx, or Release would list itself
cp -- "$scratch/Release.new" "$idx/Release"

"${gpg_cmd[@]}" --local-user "$key" --clearsign --output "$idx/InRelease" "$idx/Release"
"${gpg_cmd[@]}" --local-user "$key" --armor --detach-sign --output "$idx/Release.gpg" "$idx/Release"
"${gpg_cmd[@]}" --export "$key" > "$idx/$KEY_FILE"
[[ -s "$idx/$KEY_FILE" ]] || die "could not export the public key of $key"

# Verify the way apt does: gpgv against the public key alone, in a home that
# holds nothing else.
gv_home="$scratch/gpgv-home"
mkdir -m 700 -- "$gv_home"
gpgv --homedir "$gv_home" --keyring "$idx/$KEY_FILE" "$idx/InRelease" 2>&1 | sed 's/^/publish.sh: InRelease: /' >&2
[[ "${PIPESTATUS[0]}" -eq 0 ]] || die "InRelease does not verify"
gpgv --homedir "$gv_home" --keyring "$idx/$KEY_FILE" "$idx/Release.gpg" "$idx/Release" 2>&1 | sed 's/^/publish.sh: Release.gpg: /' >&2
[[ "${PIPESTATUS[0]}" -eq 0 ]] || die "Release.gpg does not verify"

for file in Packages Packages.gz "$KEY_FILE" Release Release.gpg InRelease; do
    cp -- "$idx/$file" "$repo/.$file.new"
    chmod 644 -- "$repo/.$file.new"
    mv -T -- "$repo/.$file.new" "$repo/$file"
done
note "published to $repo; both signatures verified; public key: $repo/$KEY_FILE"
