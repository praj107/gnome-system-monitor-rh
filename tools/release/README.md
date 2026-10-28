# Release and development channel tooling

Three scripts, all unprivileged, none of which installs anything system-wide:

| Script | What it does |
|---|---|
| `build-deb.sh` | Builds the gated `.deb` from a git revision into `build-release/`. `--dev` stamps a development version. |
| `dev-channel.sh` | User-prefix A/B install: versioned builds, atomic switch, rollback. The pipeline's testbed. |
| `publish.sh` | Turns `.deb` files into a signed local flat apt repository. |

Releases are a signed `.deb` that replaces the distribution's
`gnome-system-monitor` (`Provides`/`Conflicts`/`Replaces`). Development builds
are user-prefix installs that never touch `/usr`.

## Versions

- Release: `50.0+rh.N` (a native package: no Debian revision).
- Development: `50.0+rh.N~dev.<YYYYmmdd>.g<short sha>`, where the date is the
  commit's UTC day. `~` sorts before everything in dpkg, so a dev build always
  sorts below the release it leads to (`dpkg --compare-versions`).

## Making a release

1. Bump the entry in `debian/changelog` and commit.
2. `tools/release/build-deb.sh` builds `HEAD` in an RAM scratch directory with
   `dpkg-buildpackage`. The build is a release profile (`buildtype=release`,
   `dev_ipc=false`, `development=false`). Gates in `debian/rules` fail the build
   if meson was configured otherwise, if the tests fail, or if any installed
   ELF file defines a `dev_ipc` symbol or contains the `GSM_DEV_IPC` switch.
   The result is `build-release/gnome-system-monitor-rh_<version>_amd64.deb`.
   `GSM_RH_GATE_SELFTEST=dev_ipc` proves the gate can fail.
3. Publish into a staging repository you own (the key is yours, gpg asks for
   its passphrase):

   ```sh
   tools/release/publish.sh --repo ~/gsm-rh-staging --key YOURKEYID \
       build-release/gnome-system-monitor-rh_50.0+rh.1_amd64.deb
   ```

   `publish.sh` re-checks the `.deb` for dev IPC symbols, refuses a version that
   is already in the repository with different contents, rebuilds `Packages`,
   `Packages.gz` and `Release`, signs `InRelease` and `Release.gpg`, verifies
   both with `gpgv` against the exported public key (as apt does) and writes the
   key as `gsm-rh-archive-key.gpg`.
4. Copy the staging repository into the root-owned repository directory. In
   production that directory is owned by root so that the user's own processes
   cannot publish. This step needs root and is the owner's:

   ```sh
   sudo rsync -a --delete ~/gsm-rh-staging/ /srv/gsm-rh-apt/
   ```

   Keep the last few `.deb` files in the repository to allow a rollback, so omit
   `--delete` if you want old versions to stay.

### One-time setup (root, the owner's)

The repository directory must be readable by the `_apt` user, so it cannot live
under a home directory at mode 750.

```sh
sudo install -d -m 755 /srv/gsm-rh-apt
sudo install -D -m 644 ~/gsm-rh-staging/gsm-rh-archive-key.gpg /etc/apt/keyrings/gsm-rh.gpg
printf 'Types: deb\nURIs: file:///srv/gsm-rh-apt\nSuites: ./\nSigned-By: /etc/apt/keyrings/gsm-rh.gpg\n' \
    | sudo tee /etc/apt/sources.list.d/gsm-rh.sources
```

Upgrades then arrive through `apt upgrade` or the update manager and ask for
the owner's password. Nothing upgrades unattended.

### What an install replaces, and how to roll back

Installing `gnome-system-monitor-rh` removes the distribution's
`gnome-system-monitor` package (`Conflicts`/`Replaces`). It installs
`/usr/bin/gnome-system-monitor-rh` (with `/usr/bin/gnome-system-monitor` as a
symlink to it), the helpers in `/usr/libexec/gnome-system-monitor/`, the polkit
policy, the D-Bus service, the desktop files, the schemas and the sampler's
user unit under `/usr`.

```sh
# To an older fork release still in the repository:
sudo apt install gnome-system-monitor-rh=50.0+rh.1 --allow-downgrades
# Back to the distribution's package:
sudo apt remove gnome-system-monitor-rh && sudo apt install gnome-system-monitor
```

## The development channel

```sh
tools/release/dev-channel.sh plan                 # what a real install would create/replace/keep; writes nothing
tools/release/dev-channel.sh build                # HEAD -> ~/.local/opt/gsm-rh/<version>
tools/release/dev-channel.sh build --rev HEAD~3   # another revision, side by side
tools/release/dev-channel.sh status
tools/release/dev-channel.sh switch <version>     # current -> version; the old current becomes previous
tools/release/dev-channel.sh rollback             # swap current and previous
tools/release/dev-channel.sh prune                # remove versions that are neither current nor previous
```

`--root DIR` (default `~/.local/opt/gsm-rh`) and `--export DIR` (default
`~/.local/share`) point it elsewhere; all tests of the tooling use a scratch
directory for both.

- `build` configures a release profile with `dev_ipc=false`, installs with the
  version as prefix, and only then writes the `.installed` file that makes a
  version switchable. It runs the same dev IPC checks as the package gates
  (meson options read back, `nm` and `GSM_DEV_IPC` over the installed ELF
  files). `--test` also runs the test suites except formatting. Building a
  version that exists is a no-op.
- `switch` points `<root>/current` at the version with one `ln -s` and one
  `mv -T`, then exports the desktop files, the D-Bus service file and the schema
  files into the export directory, rewritten to name `<root>/current`, and
  compiles the schemas there. The export files as they were before the first
  switch are saved in `<root>/original-export/` (a `MANIFEST` lists those that
  did not exist).
- A running instance is not restarted; quit it
  (`gapplication action org.gnome.SystemMonitor quit`) and launch again.
- `prune` removes only directories whose real path is directly under the root,
  that carry the marker file naming them, and that are neither `current` nor
  `previous`. It refuses the root itself and any directory without its marker.

### Limits of a user prefix

The Health tab's privileged probes (SMART, per-process I/O) and the
kill/renice polkit actions cannot work from a user prefix. polkit reads action
files only from `/usr/share/polkit-1/actions` (polkit(8)), and `pkexec` matches
the helper's exact path (pkexec(1)), so those figures show "unknown" with a
reason. The sampler unit lands in `share/systemd/user`, which is not exported.
Test those parts with the signed package.

## Tools used

`dpkg-buildpackage`, `dpkg-deb`, `apt-ftparchive`, `gpg`, `gpgv`, `meson`,
`nm`. `tools/scratch.sh` and `agent-run` are used when present and replaced by
checked fallbacks when not.
