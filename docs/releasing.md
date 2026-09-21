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
`releases.json`, and the app's own update check.

Semantic: `X.Y.Z`. Every `X.Y` gets a branch `release/X.Y`, cut from
master at the `.0` and never merged back. `X.Y.Z` releases are tagged
from that branch, so a fix for something already shipped does not have to
carry whatever master has moved on to.

Tags are `releases/<channel>/X.Y.Z`, channel being `alpha`, `beta` or
`stable`. The tag is what CI builds packages from, and the channel is
compiled into the build: a beta binary knows it is a beta and checks the
beta channel for its updates.

A tag whose version disagrees with `CMakeLists.txt` is refused, by
`tools/release.sh` before the push and by `linux:package` in CI after it.
That one mistake -- a tag saying 0.3.0 on a tree that builds 0.2.0 -- is
what the whole scheme exists to prevent, because every artefact
downstream inherits the wrong answer.

## Releasing

```sh
# 1. Bump the version and commit it (on master for a .0, on release/X.Y
#    for a patch).
$EDITOR CMakeLists.txt        # project(seabass VERSION 0.2.0 ...)
git commit -am "Seabass 0.2.0"

# 2. See what would happen, then do it. Cuts release/0.2 if this is a
#    .0, runs the whole suite, tags, and pushes to Invent then GitHub.
tools/release.sh alpha 0.2.0
tools/release.sh alpha 0.2.0 --go

# 3. Wait for CI and bring the packages down.
tools/fetch-release.sh alpha 0.2.0 --watch

# 4. Verify them (below), then publish from the website repository.
../project/website/scripts/publish-release.py alpha 0.2.0 --go
```

`tools/release.sh` does nothing at all without `--go`: run it once to
read what it intends to do. It refuses a dirty tree, a version that
disagrees with `CMakeLists.txt`, a tag that already exists (a released
version is never re-tagged -- bump the patch instead), and a patch
release with no series branch to tag from.

## What CI builds

A release tag runs the full lane: Linux build and the whole `ctest`
suite including the `integration` label, Windows build, test, installer
and installer test, and the Craft macOS job that signs and notarises a
`.dmg`.

| Platform | Job | Package |
|---|---|---|
| Linux | `linux:package` | `seabass-<version>_<channel>_linux.tar.gz` |
| Windows | `windows:package` | `seabass-<version>_<channel>_windows.exe` |
| macOS | `craft_macos_arm64_qt6` | `seabass-<version>_<channel>_macos.dmg` |

`tools/fetch-release.sh` puts them in
`~/Seabass/releases/<channel>/` under exactly those names, which are also
the names the website serves, so publishing is a copy rather than a
rename. It prints each file's SHA-256. If a platform's job did not run or
did not produce a package it says so and exits non-zero: a release
missing a platform has to be a decision, never something nobody noticed.

## Verifying a build

Before anything is published, on each platform:

1. Install the package the way a user would -- the installer on Windows,
   the `.dmg` on macOS, unpack the tarball on Linux.
2. Launch it. Settings shows the version and channel: check they are the
   ones being released.
3. Point it at a **scratch** stick, not a reference one, and do one save
   and one undo. `docs/manual-testing.md` is the fuller list.
4. On Linux and Windows, run the shakedown rig against the packaged build
   if the release is a stable one.

## A universal macOS package

Craft builds one architecture per root and has no universal mode, so the
two packages are built separately and merged afterwards. Both steps run on
a Mac -- `lipo` has a Linux equivalent in `llvm-lipo`, and `rcodesign` can
sign there, but the `.dmg` itself needs `hdiutil`, and both halves are
produced on Macs anyway -- while being scriptable from the Linux publisher
over ssh: each script is non-interactive and exits non-zero on any fault.

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
# catalogs -- it reads once per architecture and compares the answers
tools/macos-verify-dmg.sh seabass-<version>_<channel>_macos.dmg <stick or folder>
```

Three things the merge is not allowed to get wrong, each of which it
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
- **The macOS package has to be universal, and CI does not merge it yet.**
  Rosetta translates x86_64 to ARM and never the reverse, so an arm64-only
  `.dmg` mounts on an Intel Mac and refuses to launch -- and
  `publish-release.py` has one macOS slot, which is the right shape only if
  what goes in it carries both architectures. `tools/macos-universal-dmg.sh`
  merges an arm64 and an x86_64 Craft bundle into one package; see "A
  universal macOS package" above. Until the merge runs in CI, the published
  `.dmg` is not the build CI tested, and the release notes have to say so.
- **The release text.** `publish-release.py` proposes one from the
  commits on the tag, grouped and trimmed, and will not publish until a
  person has edited it. A changelog nobody read is a changelog nobody
  should ship.
