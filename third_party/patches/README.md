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

`0001-libdjinterop-treat-path-strings-as-utf8-on-windows.patch` is here now:
libdjinterop hands the same `std::string` directory to sqlite (which reads
UTF-8) and to `stat()`/`_mkdir()` (which read the ANSI code page on
Windows), so a library in a folder named outside the code page can never be
both found and opened there. The patch routes those through
`std::filesystem::path` built from `std::u8string` on Windows and documents
the strings as UTF-8. It is not applied: Seabass's GUI exe declares
`activeCodePage=UTF-8` in its manifest (`src/gui/win/app.manifest`), which
makes those narrow calls take UTF-8 on Windows 10 1903 and later, and the
tests and CLI carry no manifest, so they are ANSI-only in that one respect
until the patch is upstream and the pin moves. It goes upstream as a PR once
Sebastian has approved it. The pin stays at upstream `17ea4f70`: a pointer at
a commit only one machine holds broke every fresh clone and the Craft jobs
for an evening on 2026-09-24 (`d9dcc356`, reverted).

One patch lived here before it: a fix for libdjinterop's Boost test gate (`Boost_FOUND` is true for a
headers-only install under `CMP0167 NEW`, so the test targets configured with
an empty `${Boost_LIBRARIES}` and failed to link). It went upstream as
xsco/libdjinterop#200, merged as `810c105`, and was removed from here once the
pinned checkout carried it.

The other fix this project sent upstream never was a patch: the Engine 3.0.2
`Information` row landing at id 2, which a Prime 4 calls a corrupt database,
was corrected in our own code instead (`EngineLibraryCreator`) until
xsco/libdjinterop#202 was merged as `17ea4f70`. Looking for it in this
directory's history will find nothing; see sebasje/seabass#31.
