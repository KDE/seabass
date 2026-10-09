<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# The release train

Cutting a release is four commands and two decisions. The commands are
here; the decisions are which version, and whether what came out of CI is
good enough to offer people.

## The version

`project(seabass VERSION X.Y.Z ...)` in `CMakeLists.txt` is the only
place a version is written down. Everything else reads it: the generated
`seabass_version.hpp` the app shows in Settings, the Windows installer's
filename and Add/Remove Programs entry, the package names, the website's
`releases.json`, and the app's own update check. It is numbers only --
"alpha" is never part of it, see channels below.

## The train

| Version | What it is | Where it lives |
|---|---|---|
| `X.Y.8` | master between trains: in development, never released | `master` |
| `X.Y.9`, `.10`, `.11` ... | alphas and betas of `X.(Y+1).0` | `Seabass/X.(Y+1)` |
| `X.(Y+1).0` | the train's first stable release | `Seabass/X.(Y+1)` |
| `X.(Y+1).1` to `.7` | its point releases | `Seabass/X.(Y+1)` |

So the first alpha is 0.7.9, on `Seabass/0.8`, and master says 0.8.8
from the moment that branch exists. 0.7.10 and 0.7.11 follow on the same
branch (switching to beta is a different channel, not a different
number), then 0.8.0, then 0.8.1 to 0.8.7. A series has room for seven
point releases: 0.8.8 is master's development version and 0.8.9 is the
first alpha of 0.9.

Every comparison in the release path is numeric, part by part -- the
update check, `releases.json`, git's `--sort=v:refname` -- so 0.7.10 is
newer than 0.7.9 and 0.10.0 than 0.9.0.

**Fixes are made on the train's branch** while it is being stabilized,
and brought to master with `git cherry-pick -x`. The branch is never
merged back; history stays linear.

**Channels** come in two sets, and they are not the same words for the
same thing.

A **build channel** is `alpha`, `beta` or `stable`. It is compiled into the
binary, shown in Settings, and part of the tag, `releases/<channel>/X.Y.Z`.
It is what every command here takes.

A **website channel** is `testing` or `stable`, and it is what a user
chooses between: an alpha and a beta are both a build to try rather than
one to rely on, and asking people to rank two words nobody had defined for
them bought nothing. `releases.json` has those two lists, each entry
recording under `build` what it was actually built as. A stable build is
offered stable releases only. An alpha or a beta follows both lists, and
within testing the version number decides, because the numbers only ever
go up. A published `dev` package follows both lists as well (see
"Packages before the tag"); a `dev` build from a working tree never
checks.

Running an alpha, a beta or a published dev package is remembered on
that machine (the `updates/includeTesting` setting), so somebody who
tried a beta and moved to the stable it became is still offered the next
test build; somebody
who has only ever run stable releases stays on stable. Preferences shows
a checkbox for it only once it has been on: running a test build does
that, and so does the hidden way in, ten taps on the version line in
Preferences within five seconds.

The packages are served from
`downloads/<testing|stable>/<linux|mac|windows>/`, one directory per
platform. The version is in the filename, so the directory does not
repeat it, and `publish-release.py` writes each package's full path into
its entry rather than leaving the pages to rebuild the layout rule.

A tag whose version disagrees with `CMakeLists.txt` is refused, by
`tools/release.sh` before the push and by `linux:package` in CI after it.
**A tag is never moved.** If a package does not build from it, retry the
CI job when the failure had nothing to do with the code, and otherwise fix
it on the branch and release the next number: pre-release numbers cost
nothing.

## Releasing

```sh
# 1. Cut the train: from master as Invent has it. Makes Seabass/0.8,
#    commits 0.7.9 there, moves master to 0.8.8, pushes both. No tag.
tools/release.sh cut 0.7.9            # read what it would do
tools/release.sh cut 0.7.9 --go

# 2. Test the branch before anything is tagged: the shakedown rig, and
#    packages from CI (below). Fix on the branch, cherry-pick to master.

# 3. Tag it, from the branch. Runs the whole suite first.
git checkout Seabass/0.8
tools/release.sh alpha 0.7.9 --go

# 4. Wait for CI and bring the packages down.
tools/fetch-release.sh alpha 0.7.9 --watch

# 5. Publish them. This uploads and writes the entry with
#    "released": false, so the packages are on the server and nothing
#    offers them. It asks for the changelog and for a note to users.
../project/website/scripts/publish-release.py alpha 0.7.9 --go
../project/website/scripts/deploy.sh --live

# 6. Smoke-test what is actually on the server, reached through
#    https://vizzzion.org/seabass/get-it.html?unreleased
#    Then, and only then:
../project/website/scripts/publish-release.py release alpha 0.7.9 --go
../project/website/scripts/deploy.sh --live

# The next pre-release, and every later release of the train, on the branch:
$EDITOR CMakeLists.txt                # project(seabass VERSION 0.7.10 ...)
git commit -am "Seabass 0.7.10"
tools/release.sh alpha 0.7.10 --go
```

`tools/release.sh` does nothing at all without `--go`: run it once to
read what it intends to do. It refuses a dirty tree, a version that
disagrees with `CMakeLists.txt`, an `X.Y.8`, a pre-release number on the
stable channel or a stable number on alpha or beta, a tag that already
exists, a cut that is not at a train's first pre-release, a cut from
anything but Invent's master, and a tag from anywhere but the train's
branch.

Before it tags, it records the release in the AppStream metadata
(`src/gui/org.kde.seabass.metainfo.xml`, through
`tools/appstream-release.py`) and commits that on the branch, so the
tagged tree lists its own release and software centres show the version
history. An alpha or beta is a `development` release there, a stable one
`stable`; an entry that exists already keeps its description. A release
that is already recorded with today's date adds no commit.

## Packages before the tag

A tag is never moved, so a package that does not build from one costs a
release number. On a `Seabass/X.Y` branch every package job is available
as a button in the pipeline:

- `linux:package` -- the Linux tarball.
- `craft_windows_qt6_x86_64` -- the Craft Windows package. It sits in
  the last stage, `deploy`, so its button appears once the build and test
  stages have finished.
- `macos:universal` -- the universal macOS `.dmg`, Intel and Apple
  Silicon, macOS 12 and later, built on Qt 6.8 outside Craft (see "A
  universal macOS package"). It needs no other job, so its button is
  there at once; only on `Seabass/X.Y` is the result signed and
  notarised. The Craft macOS jobs are parked, see `.gitlab-ci.yml`.

**Signing depends on the ref, and tags are not signed.** KDE's signing
service clears a request by project and exact ref name
(`sysadmin/ci-utilities`, `signing/*-projects.yaml`). Seabass is cleared
on `Seabass/0.8` and `Seabass/1.0` for macOS signing and notarisation and
Windows signing, and on `master` for Windows signing only: `master` is in
neither macOS list. A tag is
its own ref name and is not listed, and the secure services key has not
reached our tag pipelines either (the tags are not protected). A tag pipeline's
packages come out unsigned and its jobs still pass; only the log says
"is not cleared for signing. Skipping."

The MSYS2 Windows chain (`windows:build` and what follows it) needs a
self-hosted runner tagged `windows`, and none is registered, so it does
not run anywhere until `SEABASS_WINDOWS_RUNNER: "yes"` is set in
`.gitlab-ci.yml` -- see the comment there. A job with no runner does not
fail, it waits forever, and holds every later stage with it.

Built without a tag they are channel `dev`, named
`seabass-<version>_dev_<os>`. That is what a pre-tag test build is, and
what testers are handed when one goes on the website's testing list
(entered under the build it stands in for, a beta say). Every package
job configures with `SEABASS_PUBLISHED_BUILD=ON`, so the app in a dev
package checks for updates as an alpha or a beta does, on both lists,
and finds its own entry by version on the testing list, which is how it
hears that its version was withdrawn. A dev package built after a tag
but before the version bump carries the tag's number and would hear
that tag's withdrawal as its own. A `dev` build from a working tree
(the flag off, the default) never checks: Settings says it is a
development build.

## What CI builds

A release tag runs the full lane: Linux build and the whole `ctest`
suite including the `integration` label, Windows build, test, installer
and installer test, and the universal macOS package. None of them is
signed on a tag: see "Signing depends on the ref" above.

| Platform | Job | Package |
|---|---|---|
| Linux | `linux:package` | `seabass-<version>_<channel>_linux.tar.gz` |
| Windows | `craft_windows_qt6_x86_64` (Craft; the MSYS2 `windows:package` only once a Windows runner exists) | `seabass-<version>_<channel>_windows.exe` |
| macOS | `macos:universal` (a button on a tag, see above) | `seabass-<version>_<channel>_macos.dmg` |

The `<channel>` in a filename is the build channel, which is what the
binary reports in its own Settings. The directory it is served from is the
website channel.

`fetch-release.sh` takes the macOS package from `macos:universal`,
unless one built by hand on a Mac is already in place, and refuses to
call the release complete without it.

`tools/fetch-release.sh` puts them in
`~/Seabass/releases/<channel>/` under exactly those names, which are also
the names the website serves, so publishing is a copy rather than a
rename. It prints each file's SHA-256. If a platform's job did not run or
did not produce a package it says so and exits non-zero: a release
missing a platform has to be a decision, never something nobody noticed.

## Verifying a build

Publishing is two steps, and this is what sits between them. The upload
writes `"released": false`, which puts the packages on the server and
tells nobody: the download page hides the entry and the app's update check
skips it. `get-it.html?unreleased` is how you get at it, which is the
whole point of that parameter, and the page says in as many words that the
build has not been smoke-tested. `publish-release.py release <channel>
<version> --go` is the sentence "I installed this and it started", and
nothing else in the system can say it for you.

Before flipping that flag, on each platform:

1. Install the package the way a user would -- the installer on Windows,
   the `.dmg` on macOS, unpack the tarball on Linux.
2. Launch it. Settings shows the version and channel: check they are the
   ones being released.
3. Point it at a **scratch** stick, not a reference one, and do one save
   and one undo. `docs/manual-testing.md` is the fuller list.
4. On Linux and Windows, run the shakedown rig against the packaged build
   if the release is a stable one.

## A universal macOS package

Since 2026-10-09 the Mac package is built on Qt 6.8, which is the last Qt
for macOS 12 and which Seabass stays on (so older Macs keep working), and
not in Craft: Craft's Qt and every library in its cache need macOS 13.3.
Qt's own 6.8 binaries are universal already, so
`tools/macos-monterey-dmg.sh` builds both architectures in one go: Qt from
aqtinstall, OpenSSL, SQLCipher and TagLib built by the script for both,
then Seabass, one bundle, one `.dmg`, checked to need nothing newer than
macOS 12.0.

CI runs exactly that in `macos:universal` and then
`tools/macos-verify-dmg.sh` against the committed fixture laid out as a
stick, with the fixture's `SET-EXPECTATIONS.txt`: each slice must report
exactly its rekordbox track and cue counts and its OneLibrary track count,
and a `sync --dry-run` that fails or prints no OneLibrary count fails the
job. It refuses when the runner cannot execute x86_64 code, rather than
check one slice and pass. On a `Seabass/X.Y` branch it also has the bundle
and the image signed and the image notarised by KDE's signing service, the
nightly runs it by itself, and a failure there fails the pipeline; a push
to that branch offers it as a button, signed as well. Everywhere else it
is a button and the package is signed ad hoc. master's nightly does not
run it: there is one macOS runner, and `linux:qt68` builds and tests every
push against the same Qt.

By hand, on a Mac:

```sh
# QT_DIR: aqt install-qt mac desktop 6.8.3 clang_64 -m qtmultimedia qtshadertools
QT_DIR=<qt>/6.8.3/macos SEABASS_RELEASE_CHANNEL=<channel> \
    tools/macos-monterey-dmg.sh <source tree> <work dir> seabass-<version>_<channel>_macos.dmg
```

The Craft route, parked with its CI jobs, merged two one-architecture
Craft bundles instead:

```sh
# on the Mac, once per architecture, from the same source tree
craft -i --src-dir <worktree> seabass && craft --src-dir <worktree> --package seabass

# merge the two PACKAGED bundles (not <root>/Applications/KDE/seabass.app,
# which is a four-file stub -- Qt is only inside the bundle after --package)
tools/macos-universal-dmg.sh \
    <arm64 root>/build/qt-apps/seabass/archive/Applications/KDE/seabass.app \
    <x86_64 root>/build/qt-apps/seabass/archive/Applications/KDE/seabass.app \
    seabass-<version>_<channel>_macos.dmg

# then prove it, against a stick or a restored reference carrying all three
# catalogs -- it reads once per architecture and compares the answers; a
# third argument (a SET-EXPECTATIONS.txt) also pins them to known counts
tools/macos-verify-dmg.sh seabass-<version>_<channel>_macos.dmg <stick or folder>
```

The Linux AppImage has its own proof, in a container with no SQLCipher or
SQLite of its own, so a copy on this machine cannot stand in for a missing
bundle:

```sh
tools/linux-verify-appimage.sh seabass-<version>_<channel>_linux.AppImage
```

It reads the encrypted OneLibrary fixture and compares the digest, checks
that libsqlcipher came from inside the AppImage, deletes the bundled copy
to show the read then fails for that reason, and checks the runtime needs
no libfuse2. The first AppImage CI ever built failed it: SQLCipher's own
`sqlite3_*` calls bound to the plain SQLite also loaded, and the first
encrypted library crashed (fixed by `RTLD_DEEPBIND` in `sqlcipher_dyn.cpp`).

Four things the merge is not allowed to get wrong, each of which it
checks rather than assumes:

- **The two Craft roots must hold the same package versions.** Roots whose
  clones of `craft-blueprints-kde` are days apart produce bundles that
  differ in ways `lipo` cannot see: libvpx 1.15.2 against 1.16.0 changed
  the soname and showed up as a file on one side only, but ffmpeg 8.1.1-4
  against 8.1.1-6 kept every filename and would have merged one ffmpeg per
  slice silently. The script diffs both roots' `install.db` and refuses on
  any difference; `git pull` in the older root's blueprint clone, then
  `craft --update <package>`, then re-package.
- **Every framework's `_CodeSignature/CodeResources` hashes its binary**,
  so it is wrong for a merged binary whichever side it came from. They are
  regenerated innermost-first, then the bundle is ad-hoc signed and
  verified `--deep --strict`. Signing and notarisation stay CI's.
- **A universal binary can carry an architecture it cannot run.** Both
  slices are executed before the `.dmg` is built, and the verify script
  reads a real library once per architecture and compares the counts: a
  cross-built SQLCipher that fails to decrypt is the failure nobody would
  notice until a user opened a Denon stick.
- **Every binary must load only from the bundle and the system.**
  `tools/macos-bundle-check.sh`, run by both scripts on each half, on the
  merged bundle and on the mounted image, refuses any load command that
  is an absolute path other than `/System/Library` or `/usr/lib` (a Craft
  root, a home directory, `/opt`, `/usr/local`), an `@rpath` load no
  `LC_RPATH` inside the bundle can find, a `Contents/Frameworks` without
  `QtCore.framework`, and a bundle under 100 MB unpacked, which is Craft's
  undeployed stub. An `LC_RPATH` outside the bundle is only a warning:
  dyld skips one that does not exist, and KDE's cache build of libwebp
  ships its build directory as one while all its loads resolve inside. Running the slices
  cannot catch this, because on the Mac that built them a path into its
  Craft root still resolves: two undeployed halves of 9.6 and 8.7 MB were
  merged, ran there, and would have started on no other Mac.

## Grave bugs

A release with a data-loss bug is not published. If one is found after
publishing, it is **withdrawn**, not deleted:

```sh
website/scripts/publish-release.py withdraw beta 0.2.1 \
    --reason "Cue sync could drop memory cues on Engine-only tracks." --go
```

That marks the release `withdrawn` in `releases.json` with the reason,
which takes it off the download page, stops the in-app check offering it,
and puts the reason where anybody who already has it will see it. The
files stay on the server: somebody is running that build and a dead link
helps nobody, and the checksums stay verifiable.

Old releases are kept for the same reason. Nothing is ever removed from
the download directory.

## What is still by hand

- **The Linux package is a tarball against the distribution's Qt 6**, not
  an AppImage or a Flatpak. Fine for alpha and beta, where the people
  downloading it can read a dependency list. Not good enough for a stable
  release aimed at DJs; that needs a self-contained build, and it is not
  written yet.
- **The macOS package has to be universal, and CI signs it only on a
  `Seabass/X.Y` branch.** `macos:universal` builds it on every ref, a tag
  included, but a tag's package is signed ad hoc, and Gatekeeper will say
  it cannot be verified.
  Rosetta translates x86_64 to ARM and never the reverse, so an arm64-only
  `.dmg` mounts on an Intel Mac and refuses to launch -- and
  `publish-release.py` has one macOS slot, which is the right shape only if
  what goes in it carries both architectures. `tools/macos-monterey-dmg.sh`
  builds both into one package; see "A universal macOS package" above. A
  `.dmg` built by hand is not the build CI tested, and the release notes
  have to say so.
- **The release text.** `publish-release.py` proposes one from the
  commits on the tag, grouped and trimmed, and will not publish until a
  person has edited it. A changelog nobody read is a changelog nobody
  should ship.
