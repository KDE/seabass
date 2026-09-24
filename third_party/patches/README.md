<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Patches for vendored third-party code

Neither of the submodules under `third_party/` is patched in place: they are
checked out at the pinned commit and built as-is, so `git submodule status`
stays clean and `scripts/init-submodules.sh` needs no extra step. Nothing in
this directory is applied automatically.

## libdjinterop: pinned to our fork

Since 2026-09-24, `third_party/libdjinterop` comes from
<https://github.com/sebasje/libdjinterop> (`cmake/dependency-submodules.txt`),
not from upstream xsco/libdjinterop directly. Our fixes land there first, on a
branch a ref points at, and are sent upstream as pull requests as well. Two
rules follow:

- A pin must be reachable from the fork's refs. A commit only one machine
  held broke every fresh clone, the Windows laptop and the Craft package jobs
  on Invent for an evening (`d9dcc356`, 2026-09-24): `git submodule update`
  fetches by hash, and a hash no branch or tag names is "not our ref".
- Keep the fork's branches in step with upstream's `master` so a pin reads as
  "upstream plus our fixes", and move the pin to the upstream merge commit once
  a fix is accepted there.

Current pin: `53ac679` on the fork's `seabass/utf8-paths`, upstream `17ea4f70`
plus one commit treating every path string as UTF-8 on Windows (upstream hands
the same `std::string` to sqlite, which reads UTF-8, and to `stat()`/`_mkdir()`,
which read the ANSI code page). Its pull request to xsco/libdjinterop is
pending Sebastian's go-ahead.

## Earlier fixes

One patch has lived in this directory: a fix for libdjinterop's Boost test gate
(`Boost_FOUND` is true for a headers-only install under `CMP0167 NEW`, so the
test targets configured with an empty `${Boost_LIBRARIES}` and failed to
link). It went upstream as xsco/libdjinterop#200, merged as `810c105`, and was
removed from here once the pinned checkout carried it.

The other fix this project sent upstream never was a patch: the Engine 3.0.2
`Information` row landing at id 2, which a Prime 4 calls a corrupt database,
was corrected in our own code instead (`EngineLibraryCreator`) until
xsco/libdjinterop#202 was merged as `17ea4f70`. Looking for it in this
directory's history will find nothing; see sebasje/seabass#31.
