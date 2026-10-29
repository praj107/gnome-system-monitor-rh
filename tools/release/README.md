# Release and development channel tooling

The build, publish and development-channel scripts are unprivileged and install
nothing system-wide. The install scripts are run by the owner, from a terminal,
and use `sudo` only where it is printed first:

| Script | What it does |
|---|---|
| `build-deb.sh` | Builds the gated `.deb` from a git revision into `build-release/` (`--out DIR` elsewhere). `--dev` stamps a development version. |
| `dev-channel.sh` | User-prefix A/B install: versioned builds, atomic switch, rollback. The pipeline's testbed. |
| `publish.sh` | Turns `.deb` files into a signed local flat apt repository. |
| `first-install.sh` | The owner's one-time install: publish, root setup, retire the old `~/.local` install, `apt install`, verify. |
| `release.sh` | A later release in one run: build, publish, copy into the repository, refresh apt, then stop (Update Manager offers it) or `--install`. |
| `check-install.sh` | Read-only checks of an installed release (no `sudo`); used by `first-install.sh` and `release.sh --install`. |
| `rollback.sh` | Back to the distribution's package and the old `~/.local` install. |
| `retire-user-install.sh` | Moves an old `~/.local` install aside, with a manifest; `--restore` puts it back. |
| `release.conf` | Key fingerprint, repository, key and source paths shared by the above. |

Releases are a signed `.deb` that replaces the distribution's
`gnome-system-monitor` (`Provides`/`Conflicts`/`Replaces`). Development builds
are user-prefix installs that never touch `/usr`.

## Versions

- Release: `50.0+rh.N` (a native package: no Debian revision).
- Development: `50.0+rh.N~dev.<YYYYmmdd>.g<short sha>`, where the date is the
  commit's UTC day. `~` sorts before everything in dpkg, so a dev build always
  sorts below the release it leads to (`dpkg --compare-versions`).

## The first release (`50.0+rh.1`)

`debian/changelog` is finalised and committed, then:

```sh
tools/release/build-deb.sh                 # gated .deb into build-release/
tools/release/first-install.sh --dry-run   # every step printed, nothing done
tools/release/first-install.sh             # the real thing
```

`first-install.sh` prints each step before it runs it. The steps are:

1. Check that the release key (by fingerprint, in `release.conf`) is in your
   keyring, that the `.deb` is built and `debian/changelog` is not
   `UNRELEASED`, and that nothing is half-done.
2. Publish into `~/gsm-rh-staging` with `publish.sh`. gpg asks for the key's
   passphrase here; this is the only place the key is used.
3. With `sudo`: create `/srv/gsm-rh-apt` (root-owned, 0755; it cannot live
   under a home directory at mode 750 because the `_apt` user must read it),
   copy the staging repository into it (nothing is deleted, so older packages
   stay available), install the repository's public key as
   `/etc/apt/keyrings/gsm-rh.gpg` (first checking that it is the release key),
   write `/etc/apt/sources.list.d/gsm-rh.sources` (`Suites: ./`, `Signed-By`),
   and refresh apt's list for that one source.
4. Ask a running app to quit with its own quit action (no signal).
5. Retire the old `~/.local` install (below).
6. `sudo apt-get install gnome-system-monitor-rh`, which removes the
   distribution's `gnome-system-monitor`.
7. Verify with `check-install.sh`: the package is installed and the
   distribution's is gone, `gnome-system-monitor` resolves to `/usr/bin`,
   `pkaction` shows the helpers under `/usr/libexec/gnome-system-monitor`, the
   system D-Bus service file runs `/usr/bin`, the health sampler is not enabled
   system-wide, nothing in `~/.local/share` (schemas, D-Bus service, desktop
   files) shadows the package, and the binary runs `--help`.

If a step fails, run `rollback.sh`. Upgrades afterwards arrive through
`apt upgrade` or the update manager and ask for your password; nothing upgrades
unattended.

### Retiring the `~/.local` install

User files win over system ones: `~/.local/bin` precedes `/usr/bin` in `PATH`,
a user D-Bus service file is preferred over the system one, user `.desktop`
files override the package's, and a user `glib-2.0/schemas/gschemas.compiled`
shadows the system schemas. So the package would not be the app that runs.

`retire-user-install.sh [--home DIR] [--dry-run]` **moves** the install's files
(binary, helpers, desktop files, D-Bus service file, schemas, icons, metainfo,
the polkit policy, help pages, translations; the table is at the top of the
script) into `~/.local/share/gsm-rh-retired-<stamp>/files/` and lists them in
`MANIFEST` there. Nothing is deleted. If other schemas remain in the user
schema directory it recompiles them (the original `gschemas.compiled` is kept
in the backup); otherwise it moves the compiled file too. The app's own data
(history database, settings) is not touched. It refuses to run while the app is
running. `--restore <stamp>|latest` puts every file back, and refuses, moving
nothing, if any destination exists again. Empty directories it leaves behind
are left in place.

### What an install replaces, and how to roll back

Installing `gnome-system-monitor-rh` removes the distribution's
`gnome-system-monitor` package (`Conflicts`/`Replaces`; the fork also
`Provides` it). It installs `/usr/bin/gnome-system-monitor-rh` (with
`/usr/bin/gnome-system-monitor` as a symlink to it), the helpers in
`/usr/libexec/gnome-system-monitor/`, the polkit policy, the D-Bus service, the
desktop files, the schemas and the sampler's user unit under `/usr`. It also
adds `/srv/gsm-rh-apt`, `/etc/apt/keyrings/gsm-rh.gpg` and
`/etc/apt/sources.list.d/gsm-rh.sources`.

```sh
tools/release/rollback.sh                    # distro package back, old ~/.local files restored
tools/release/rollback.sh --disable-source   # also rename the apt source to gsm-rh.sources.disabled
# To an older fork release still in the repository:
sudo apt install gnome-system-monitor-rh=50.0+rh.1 --allow-downgrades
```

`rollback.sh` leaves `/srv/gsm-rh-apt` and the key in place (harmless without
the source); remove them yourself if you want them gone.

## Later releases

1. Edit `debian/changelog` (a new entry, a real distribution) and commit.
2. `tools/release/release.sh --dry-run` prints every step; then
   `tools/release/release.sh`. It checks the version is newer than the
   installed one, builds `HEAD`, publishes into the staging repository (gpg asks
   for the passphrase), copies package files, then indexes, then signatures into
   `/srv/gsm-rh-apt` (nothing is deleted; `sudo` is printed before it is used),
   refreshes apt's list for that one source and shows the simulated upgrade
   (`Inst gnome-system-monitor-rh [old] (new ...`). `publish.sh` refuses a
   version that is already published with different contents, and re-checks the
   `.deb` for dev IPC symbols. It never changes a version itself.
3. By default it stops there: open Update Manager, which should offer the new
   version (that is the check that upgrades reach this machine). Or run
   `release.sh --install`: it quits a running app with its own quit action,
   runs `sudo apt-get install gnome-system-monitor-rh` and then
   `check-install.sh --version <new>`.

`release.sh --deb FILE` publishes an already built package instead of
building `HEAD`.

### The health sampler is opt-in

The package installs the sampler's user unit but enables it for nobody
(`dh_installsystemduser --no-enable`); each user enables it from the Health tab,
which is when they consent to a record of which applications write how much.
`50.0+rh.1` enabled it for every user (a link under
`/etc/systemd/user/default.target.wants`), which the Health tab's off switch
cannot undo. Upgrading from a version below `50.0+rh.2` runs
`deb-systemd-helper --user purge` for the unit in the postinst: it removes that
system-wide link and the helper's enabled-state for it, and nothing else. A
user's own link in `~/.config/systemd/user` is not touched, so a sampler the
owner switched on stays on. To see that the migration ran,
`check-install.sh` fails while any link to the unit remains under
`/etc/systemd/user`. Rolling back to `50.0+rh.1` would enable the sampler for
everyone again.

`build-deb.sh` builds in a RAM scratch directory with `dpkg-buildpackage`; the
build is a release profile (`buildtype=release`, `dev_ipc=false`,
`development=false`) and gates in `debian/rules` fail it if meson was configured
otherwise, if the tests fail, or if any installed ELF file defines a `dev_ipc`
symbol or contains the `GSM_DEV_IPC` switch. `GSM_RH_GATE_SELFTEST=dev_ipc`
proves the gate can fail.

### Testing the scripts without root

`first-install.sh --sysroot DIR --home DIR --key FPR --gnupghome DIR` writes the
root-owned parts under `DIR`, signs with a throwaway key, retires a fake home
and replaces `apt install` with an `apt-get -s` simulation; `rollback.sh
--sysroot DIR --home DIR` simulates the way back; `release.sh --sysroot DIR --key FPR
--gnupghome DIR --deb FILE [--install]` runs a later release against a sysroot that
already holds a repository and its apt source. `build-deb.sh --out DIR
--distribution noble` builds a test package of an UNRELEASED changelog entry
without touching `build-release/`. These options exist for
tests; the real key is fixed in `release.conf` and the install scripts refuse a
different one outside a sysroot.

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
- `switch` is refused while the package is installed by dpkg and the export is the
  real user data directory: the exported schema, D-Bus and desktop files would
  hide the installed release. `plan` says so.
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

`dpkg-buildpackage`, `dpkg-deb`, `dpkg-parsechangelog`, `apt-ftparchive`,
`apt-get`, `gpg`, `gpgv`, `glib-compile-schemas`, `pgrep`, `meson`, `nm`. `tools/scratch.sh` and `agent-run` are used when present and replaced by
checked fallbacks when not.
