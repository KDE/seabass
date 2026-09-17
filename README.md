<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Seabass

Seabass's home is on [KDE Invent](https://invent.kde.org/multimedia/seabass);
the [GitHub mirror](https://github.com/sebasje/seabass) exists for wider
reach, but KDE Invent is the canonical repository.

Reads and manages DJ track libraries across the three catalogs found on a
rekordbox/Engine DJ USB stick: rekordbox's classic per-device export
(`export.pdb`), rekordbox 7's newer unified OneLibrary export
(`exportLibrary.db`), and Denon Engine DJ's library (`m.db`, via the
vendored `libdjinterop`) -- hot cues, memory cues, playlists, and (for
rekordbox/Engine) beatgrid-aware cue writing. See `specs/README.md` for
details on the rekordbox format support, and the project plan for
architecture and current status.

Two ways to use it:

- **`seabass-cli`** -- a command-line tool: `scan` (read-only reporting plus
  duplicate-track cue consolidation), `sync` (match tracks between a
  rekordbox and an Engine source by filename/duration and reconcile their
  cues), `backups` (list/prune the backups seabass-cli makes before any
  write), and `anonymize` (a de-identified copy of a library, for bug
  reports and test fixtures). Run `seabass-cli --help` for full usage.
- **Seabass** (`seabass`) -- a Qt6 desktop app covering the same ground
  with a UI, and more: browse all three catalogs, play tracks with
  waveform/cue display, add cues by clicking the waveform, merge
  duplicate tracks, sync cues between rekordbox and Engine, check the
  three formats against each other (Library Health), clean up orphaned
  files, back up and restore a whole stick, clone a stick, keep a
  metadata backup on the computer, format a stick, measure how a stick
  performs, and anonymize a library. Every workflow that writes to a
  stick backs it up first.

See [`docs/write-path-performance.md`](docs/write-path-performance.md) for how write
performance against real sticks is measured and what the current numbers are.

See [`docs/testing.md`](docs/testing.md) for the test suite (including the committed
real-library integration fixture) and how to submit your own library to help test
against hardware Sebas doesn't have.

## AI-assisted

Seabass development is assisted by AI tools.

## Building

Just run `cmake` as usual (`cmake -B build && cmake --build build`) --
the two vendored dependencies under `third_party/` (git submodules) are
fetched and initialized automatically as part of the CMake configure
step. Seabass builds and runs on Linux and on Windows; for the native
MSYS2 build, the test suite there and the installer, see
[`docs/windows-build.md`](docs/windows-build.md).

**Why there's no `.gitmodules` file in the tree:** KDE Invent, this
project's canonical host, rejects any pushed commit that contains a
file literally named `.gitmodules` at its commit-audit step. The real
submodule configuration instead lives in
[`cmake/dependency-submodules.txt`](cmake/dependency-submodules.txt) --
identical git-config-file syntax, just a different filename -- and
either `cmake`'s configure step or
[`scripts/init-submodules.sh`](scripts/init-submodules.sh) (for a
manual/CI `git submodule` workflow outside CMake) regenerates the real,
gitignored `.gitmodules` from it on demand. If you ever add or update a
vendored dependency, edit `cmake/dependency-submodules.txt`, not
`.gitmodules` directly -- a local `.gitmodules` edit is silently
overwritten on the next configure.

## Status

The three catalogs on a stick are one library written three times, and
Seabass treats them that way: a write reaches every format present.
Cue writing is implemented for rekordbox (`RekordboxCueWriter`, via the
ANLZ PCO2 sections) and Engine (`LibdjinteropEngineCueWriter`, via
`libdjinterop`); rekordbox 7's OneLibrary (`exportLibrary.db`) is read in
full and written as a best-effort mirror of those two
(`OneLibraryCueWriter`, see [`docs/onelibrary-format.md`](docs/onelibrary-format.md)),
never on its own. Bidirectional cue sync between rekordbox and Engine
works in both the CLI and the app, matching by filename and duration.
Features still marked experimental are listed in
[`docs/experimental-features.md`](docs/experimental-features.md), and what
needs real hardware to verify is tracked in
[`docs/manual-testing.md`](docs/manual-testing.md).

## License

Seabass is licensed under **GPL-2.0-only OR GPL-3.0-only OR
LicenseRef-KDE-Accepted-GPL**. Documentation is CC-BY-SA-4.0, build files
are BSD-2-Clause, test fixtures CC0-1.0. The licence texts are in
[`LICENSES/`](LICENSES/).

Bundled components keep their own licences:

| Component | Location | License |
|---|---|---|
| libdjinterop | `third_party/libdjinterop/` | LGPL-3.0-or-later |
| Kaitai Struct C++ runtime | `third_party/kaitai_struct_cpp_stl_runtime/` | MIT |
| zlib (Windows builds only; Linux uses the system library) | `third_party/zlib/` | Zlib |
| rekordbox format specs from [crate-digger](https://github.com/Deep-Symmetry/crate-digger), and the parser generated from them | `specs/`, `src/infrastructure/rekordbox/generated/` | EPL-2.0 OR MPL-2.0 OR LGPL-3.0-only (used under LGPL-3.0-only) |
| Breeze icons | `src/gui/qml/icons/breeze/` | LGPL-3.0-or-later |
| Seabass logo, `prime4display.jpg` | `src/gui/qml/icons/`, top level | CC-BY-SA-4.0 |

A binary build is therefore distributed under GPL-3.0.
