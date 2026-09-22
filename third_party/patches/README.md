<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Patches for vendored third-party code

Neither of the submodules under `third_party/` is patched in place: they are
checked out at the pinned commit and built as-is, so `git submodule status`
stays clean and `scripts/init-submodules.sh` needs no extra step. Anything
in this directory is a fix that belongs **upstream**, kept here so it can be
sent there and so a local build can apply it if needed. Nothing applies them
automatically.

Nothing but this README is here at the moment. One patch has lived here so
far: a fix for libdjinterop's Boost test gate (`Boost_FOUND` is true for a
headers-only install under `CMP0167 NEW`, so the test targets configured with
an empty `${Boost_LIBRARIES}` and failed to link). It went upstream as
xsco/libdjinterop#200, merged as `810c105`, and was removed from here once the
pinned checkout carried it.

The other fix this project sent upstream never was a patch: the Engine 3.0.2
`Information` row landing at id 2, which a Prime 4 calls a corrupt database,
was corrected in our own code instead (`EngineLibraryCreator`) until
xsco/libdjinterop#202 was merged as `17ea4f70`. Looking for it in this
directory's history will find nothing; see sebasje/seabass#31.
