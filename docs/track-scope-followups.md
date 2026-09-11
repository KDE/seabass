<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Follow-ups: extending TrackScope / LibraryCatalogCache beyond Sync

## Why this exists

`domain::TrackScope` (`src/domain/track_scope.hpp`) and
`gui::LibraryCatalogCache` (`src/gui/library_catalog_cache.hpp`) were built
as generic, reusable pieces — not one-offs for Sync Cue Points — but the
first pass only wired the cache into **two** controllers
(`SyncController`, `StickStatisticsController`), deliberately. A repo-wide
grep at the time this was written found `ScanLibrary(reader).execute()` (a
full catalog read) called independently from **8 controllers, 26 call
sites total** (the table below; `ScanController` itself was missed by that
grep and turned out to be a ninth, found later — see item 1's status).

Migrating all of them in one pass wasn't attempted: this is a careful,
heavily-commented, "verify against real hardware" codebase (see
`README.md`'s own beta-software warning), not one to mechanically touch 8
controllers at once without individually verifying each against real
stick data.

## 1. Migrate every controller to `LibraryCatalogCache` — done

| Controller | `ScanLibrary` call sites | Status |
|---|---|---|
| `cleanup_controller.cpp` | 7 | done |
| `add_cue_controller.cpp` | 3 | done |
| `stick_statistics_controller.cpp` | 3 | done (first pass) |
| `library_consistency_controller.cpp` | 3 | done |
| `local_cue_controller.cpp` | 3 | done |
| `duplicates_controller.cpp` | 3 | done |
| `sync_controller.cpp` | 3 | done (first pass) |
| `engine_library_creator_controller.cpp` | 1 | done |
| `scan_controller.cpp` (missed by the original grep — it's `ScanPage`'s own browse/format-toggle scan, not caught because the earlier search was scoped to a fixed controller list rather than a fresh repo-wide grep) | 1 (+1 sibling-rekordbox scan for Engine artwork borrowing) | done |

Every controller now goes through `gui::LibraryCatalogCache::instance()
.tracksFor(format, path, progress)` instead of constructing its own reader
+ `application::ScanLibrary`. Every write path that mutates a catalog
(Clean Up's apply/undo/pending-deletion, Add Cue, Library Health's
repair/junk-cue removal, Local Cue's restore/undo, Duplicates' apply/undo,
Sync's apply/undo) calls `LibraryCatalogCache::instance().invalidate(format,
path)` for the catalog(s) it touches — called *before* the write is
attempted where the write and the invalidate happen inline in the same
function (Add Cue, Engine Library Creator: a write that throws partway
through may still have modified the file on disk, so invalidating only on
success would leave the cache trusting a stale pre-write entry), or
unconditionally in a shared completion handler right after the write's
background task finishes either way (Clean Up, Local Cue, Duplicates,
Library Health, Sync). The handful of rekordbox-primary writers that also
best-effort mirror into OneLibrary's `exportLibrary.db` when one exists
alongside `export.pdb` (Clean Up, Add Cue, Local Cue) invalidate
`"onelibrary"` too via the shared `LibraryCatalogCache::
invalidateWithOneLibraryMirror(format, path)` helper, rather than each
repeating the same `if (format == "rekordbox") invalidate("onelibrary",
path)` conditional inline.

`application::ScanLibrary`/the per-format reader includes were removed
from every `src/gui/` controller above once nothing local called them
anymore — `library_catalog_cache.cpp` is the only remaining direct
`application::ScanLibrary` caller *under `src/gui/`*. `src/cli/main.cpp`
and a handful of `tests/` files (`anonymized_fixture_integration_test
.cpp`, `libdjinterop_engine_loop_cue_test.cpp`,
`libdjinterop_engine_sync_scratch_replace_test.cpp`,
`libdjinterop_engine_library_creator_test.cpp`) still call it directly and
were never in scope for this migration — the CLI has no shared cache to
go through (it's a one-shot process, not a long-lived session with
multiple pages that could double-scan), and the tests are deliberately
exercising `ScanLibrary` itself.

Switching the Library page's format toggle (Rekordbox/Engine/OneLibrary)
back and forth no longer re-reads a catalog from disk that another page
already scanned this session — the original complaint this whole effort
started from.

## 2. Wire `TrackScope` into Clean Up (and friends) for playlist/selection scoping

This is the feature Sebas actually asked for when `TrackScope` was
designed: "make pretty much all cleanup and sync operations operate on a
subset of tracks... so we can clean up our library in small steps." Sync
Cue Points has it (a `Playlist:` picker in its header, see
`SyncPage.qml`).

**Done: Clean Up Stray Cues** (`library_consistency_controller.cpp` /
`JunkCuePage.qml`). `LibraryConsistencyController::scan()` now takes the
same `playlistName` parameter `SyncController::analyze()` has (empty =
whole library); `runScanTask()` tallies each format's own playlist
membership *before* scoping (so the picker's own choices never shrink),
then filters through `domain::filterByScope(tracks,
domain::TrackScope::playlist(name))` before junk-cue detection and the
consistency check both run on the (possibly scoped) result. Since this
controller's scan is progressive (one format at a time, see
`scanNextPendingFormat()`), the cross-catalog playlist-name union is
folded together incrementally as each format's scan completes
(`LibraryConsistencyController::mergePlaylistSummary()`) rather than
built in one pass the way `SyncController`'s own
`collectPlaylistSummary()` does — same max-count-per-name semantics,
different accumulation shape to fit the progressive scan. `scanTracks()`
was also migrated to `LibraryCatalogCache::instance().tracksFor()` as
part of this (a prerequisite for (1) below), and `onWriteFinished()` now
invalidates all three catalogs' cache entries before its post-write
re-scan, same convention as `SyncController::onWriteFinished()`.
`JunkCuePage.qml` reuses `PlaylistPickerCombo.qml` unmodified for the
picker UI.

**Still open: Clean Up Duplicates** (`cleanup_controller.cpp` /
`CleanupPage.qml`, a separate controller/page from the one above despite
the similar name — see `CleanupPage.qml`'s own `BackBreadcrumb` title
"Clean Up Duplicates"). Once (1) is done for `cleanup_controller.cpp`,
adding a scope here is the same shape as what Clean Up Stray Cues just
got:

- Give `CleanupController`'s entry point(s) the same `playlistName`
  parameter, and filter each scanned catalog's tracks through
  `domain::filterByScope(tracks, domain::TrackScope::playlist(name))`
  before whatever Clean Up Duplicates does with them — same seam, same
  pattern.
- Reuse `PlaylistPickerCombo.qml` for the picker UI — already generic
  (`model` + `currentIndex` + `playlistPicked` signal), no Sync-specific
  assumptions baked in.
- `CleanupController` will need its own `playlistNames`/
  `playlistTrackCounts` properties. Whether that's a one-pass tally (like
  `SyncController`'s `collectPlaylistSummary()`) or an incremental merge
  (like `LibraryConsistencyController`'s `mergePlaylistSummary()`, added
  above) depends on whether `CleanupController`'s own scan ends up
  progressive per-format or all-at-once — check that before copying
  either pattern verbatim.

## 3. `TrackScope::arbitrary()` + manual multi-select

`TrackScope` already has an `Arbitrary` kind (an explicit
`std::set<TrackId>`, i.e. `(format, sourceId)` pairs) for exactly this,
but nothing produces one yet — no checkbox/multi-select UI exists on any
track table in this app. Building that is real, separate UI work (a
selection-tracking mechanism threaded through whichever `TrackListModel`-
backed view, most likely `ScanPage.qml`'s own track table), out of scope
for this note beyond flagging that `TrackScope`'s own design already has
the seam ready for it.

## 4. `ScanController`/`ScanPage`'s own filtering, for large libraries

Separate from the above (this one's about performance on `ScanPage`'s own
already-existing search/playlist/sort UI, not adding scope to a new
feature): `ScanController::applyFilters()` still rebuilds and copies a
whole `std::vector<domain::Track>` on every filter/search/sort change,
then calls `TrackListModel::setTracks()`, which resets the entire Qt
model — full `beginResetModel()`/`endResetModel()`, discarding scroll
position and forcing QML to recreate every visible delegate. For a very
large library, on every keystroke in the search box, that's real
overhead.

The idiomatic fix is a `QSortFilterProxyModel` (or a custom subclass
overriding `filterAcceptsRow`/`lessThan`) sitting in front of
`TrackListModel`, with the *current* `TrackScope` (playlist/search) as
its filter predicate — filtering becomes incremental row add/remove
instead of copy-everything-and-reset. `TrackScope::matches()` is already
the right shape to drive `filterAcceptsRow`. Not attempted here: this
touches `ScanPage`'s own core browsing UI, the highest-traffic page in
the app, and deserves its own dedicated pass with real before/after
verification against a large library — not a incidental addition to a
Sync-focused change.
