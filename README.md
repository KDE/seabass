<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Seabass

Seabass's home is on [KDE Invent](https://invent.kde.org/sebas/seabass);
the [GitHub mirror](https://github.com/sebasje/seabass) exists for wider
reach, but KDE Invent is the canonical repository. The mirror used to live
at `sebasje/djconvert`, which is now an archived pointer to this one.

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
  cues), and `backups` (list/prune the backups seabass-cli makes before any
  write). Run `seabass-cli --help` for full usage.
- **Seabass** (`seabass`) -- a Qt6 desktop app covering the same
  ground with a UI: browse all three catalogs (including read-only
  OneLibrary browsing), play tracks with waveform/cue display, manually
  merge duplicate tracks, add cues by clicking the waveform, clean up
  orphaned files, and manage backups.

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
step.

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

Scanning, duplicate-track cue consolidation, and cue writing are
implemented for both rekordbox (`RekordboxCueWriter`, via the ANLZ PCO2
sections) and Engine (`LibdjinteropEngineCueWriter`, via `libdjinterop`).
OneLibrary support is read-only (browsing and a best-effort mirror of
writes made through rekordbox/Engine); there is no direct OneLibrary write
path yet. Full bidirectional sync between rekordbox and Engine works via
`seabass-cli sync`, matching by filename and duration.

## License

Seabass's own code is licensed under **GPL-2.0-only OR GPL-3.0-only OR
LicenseRef-KDE-Accepted-GPL** -- the GNU General Public License version 2
or 3, or any later version accepted by the membership of KDE e.V., which
acts as a proxy under section 14 of GPLv3. That is the form KDE's
licensing policy expects of an application.

Every file carries an SPDX tag and the full licence texts are in
[`LICENSES/`](LICENSES/), following [REUSE 3.0](https://reuse.software/)
as the policy requires. Files that cannot hold a header -- the binary test
fixtures, vendored sources that must stay byte-identical to upstream --
are covered by [`.reuse/dep5`](.reuse/dep5). `reuse lint` checks the lot
and runs in CI.

### Third-party components

This project vendors a few pieces of other software, each under its own
license (see [`cmake/dependency-submodules.txt`](cmake/dependency-submodules.txt)
and `specs/README.md` for exact sources):

| Component | Location | License |
|---|---|---|
| `libdjinterop` | `third_party/libdjinterop/` (git submodule) | LGPL-3.0-or-later |
| `kaitai_struct_cpp_stl_runtime` | `third_party/kaitai_struct_cpp_stl_runtime/` (git submodule) | MIT |
| rekordbox PDB/ANLZ format specs, and the C++ parser generated from them | `specs/*.ksy`, `src/infrastructure/rekordbox/generated/` | EPL-2.0, provisionally (from [Deep-Symmetry/crate-digger](https://github.com/Deep-Symmetry/crate-digger)) |

**An open licensing question, narrower than it once looked.** The two
`.ksy` specs declare `license: EPL-1.0` in their own `meta:` block, and
the FSF does not consider EPL-1.0 compatible with the GPL for combined
works. Read on that alone, this project could not ship them.

But crate-digger's actual `LICENSE` is **EPL-2.0**, with Secondary
Licenses of MPL-2.0 or LGPL-3.0 -- the `meta:` field simply predates the
move and was never updated. Taking the `LICENSE` as authoritative and
electing LGPL-3.0 under the secondary-licence clause, the specs combine
with this project on exactly the footing `libdjinterop` already does.

Two things are being confirmed upstream rather than assumed:
[crate-digger#49](https://github.com/Deep-Symmetry/crate-digger/issues/49)
reports the stale `meta:` field, and
[#50](https://github.com/Deep-Symmetry/crate-digger/issues/50) asks
whether the election reaches the parser Kaitai generates from the specs.
Until #50 is answered, [`.reuse/dep5`](.reuse/dep5) marks both the specs
and the generated parser EPL-2.0 provisionally, and says so at the point
where the answer would change something.

`libdjinterop`'s LGPL-3.0 combines cleanly either way: the disjunction
above offers GPL-3.0-only, and a GPLv3 combined work takes LGPLv3 code
without difficulty.
