<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Library Health: repairing a divergent rekordbox / OneLibrary pair

## What this is for

`export.pdb` and `exportLibrary.db` are one library written in two formats,
not two catalogs. Every Seabass write now keeps them level -- Sync mirrors,
and so do Add Cue, Local Cue restore and stray-cue removal -- and Sync
deliberately does not plan them as a pair, because the mirror makes them
identical by construction (see `sync_controller.cpp`).

That fixes the future and does nothing for a library that arrived crooked.
On the committed fixture: **1161 rekordbox tracks and 1644 OneLibrary
tracks**, every rekordbox track appearing in OneLibrary.

**The 483 figure this document used to quote was wrong.** It came from
1644 - 1161 and treated every OneLibrary row without a rekordbox twin as a
divergence. Measured properly (`tools/pdb_capacity_probe.cpp`), 193 of
those rows have no resolvable file path at all -- streaming entries and
unresolved rows, which have no `export.pdb` equivalent to be missing and
must not be offered for repair. The real divergence is **290 tracks**,
which is also, exactly, the figure measured on RV2.

Nothing repairs that today. This is where it belongs: Library Health is
already where cross-catalog disagreement is reported, already scans all
three catalogs (`m_pendingScanFormats` includes `onelibrary`), and already
stages repairs that are backed up and undoable.

## Why the existing checker cannot do it as it stands

`LibraryConsistencyChecker` is per-catalog on purpose, and says so:

> a healthy rekordbox copy says nothing about whether a broken Engine row
> is safe to repair

That is right for Engine, and it is exactly wrong for OneLibrary -- which
is not an independent catalog but the same library in a newer format, so
each half genuinely can vouch for the other. Any work here must be scoped
to that one pair and carry that reasoning, or it reintroduces the bug the
existing comment prevents.

## The three shapes of divergence

**1. Both halves have the track; one has cues, the other does not.**
Unambiguous: copy across. The common leftover from writes that predate the
mirror.

**2. Both halves have the track and their cues genuinely differ.** A
conflict. Merge where the union is unambiguous, refuse otherwise -- the
stance `DuplicateCueConsolidator` already takes. Never arbitrate by mtime:
both files sit on the same stick and rekordbox writes both, so "newer"
does not mean "more correct".

**3. The row exists in one half only.** Two causes that look identical on
disk -- the track was added on one side, or removed on the other -- and
nothing on the stick distinguishes them. So this one asks.

## The dialog for shape 3 (decided 2026-09-09)

Per track, or per selection:

1. **"Track was added to <A>, so add it to <B> as well"** -- preselected,
   because added is the common case.
2. **"Track was removed from <B>, so remove it from <A> as well"**
3. **Ignore, don't touch**

## What each option actually costs, which is not evenly distributed

| Option | rekordbox side | OneLibrary side | Buildable today |
|---|---|---|---|
| 2. remove | `PdbRowWriter::removeTrack` (clears the presence bit) | `removeTrackByPath` | **yes** |
| 3. ignore | -- | -- | **yes** |
| 1. add | nothing exists | nothing exists | **no** |

Neither format has an "insert a track" capability. `PdbRowWriter` can clear
presence bits and overwrite existing fields in place; it cannot add a row.
`OneLibraryCueWriter` can write cues, remove rows and propagate missing
fields into an existing row; it cannot create one.

**So the option chosen as the default is the only one that cannot be
built yet**, and the measured direction points it at the harder of the two:
every divergent row exists in OneLibrary and not in rekordbox -- so "add it
to the other half" means inserting a row into `export.pdb`.

There is one precedent for creating catalog rows --
`libdjinterop_engine_library_creator` builds a whole Engine library -- but
it does it through libdjinterop, a maintained library that knows the Engine
schema. Nothing equivalent exists for pdb or OneLibrary.

## What inserting into export.pdb actually requires (measured 2026-09-09)

Measured with `tools/pdb_capacity_probe.cpp` against the committed fixture,
because three of the claims above were guesses and two of them were wrong.

**There is no room in the existing pages.** 204 track pages hold 779296
bytes of rows and 43898 bytes of free heap -- a mean of 215 free bytes per
page against a mean row size of 457. Six pages of 204 could take one more
row. Insertion therefore cannot be a bounded overwrite inside the pages a
library already has; it means growing the file. That is exactly the leap
`PdbRowWriter`'s design comment refuses:

> a precise, bounded overwrite of a handful of already-existing bytes --
> never a structural change to the file

**But the format expects to grow, and says where.** The file is 351 pages
of 4096 bytes; `next_unused_page` is **354**, already pointing past the end
of the file, and each table carries an `empty_candidate` (351, 352 and 353
for the three largest). Appending is a bounded set of edits rather than a
rewrite: write a page at the end, link it from the previous page's
`next_page`, update the table's `last_page`, bump `next_unused_page` and
`sequence`.

**And the table is not sorted, so an appended row may land anywhere.** 494
of 1161 track ids step backwards through the page chain. Nothing requires a
new row to be inserted at a particular position -- which is what would have
made this genuinely hard. No table indexes tracks by row position either;
only `playlist_entries` refers to them, and it does so by id.

**The cost is not the page, it is the nine foreign keys.** A `track_row`
references album, artist, artwork, composer, genre, key, label,
original_artist and remixer, and its strings live in the page's own heap
addressed by `ofs_strings`. Adding a track whose artist has no `artist_row`
means inserting there too, by the same mechanism, recursively. So the unit
of work is not "write a row" -- it is a small allocator over a page chain
that ten tables share. That is the honest size of it: much larger than a
field overwrite, and much smaller than "become an exporter".

**Deleted rows are not a shortcut.** The fixture holds 546 rows whose
presence bit is clear, and every one of their bodies is still readable in
the heap -- so restoring a *previously deleted* track really is a one-bit
change. It does not help here: **zero** of the 290 divergent tracks are
among them. They were never in `export.pdb` to begin with.

One warning about that 546, because getting it wrong looks like data rather
than like a bug. Counting every slot a row group exposes gives 2103
instead, since the row index always presents 16 slots per group regardless
of how many were ever allocated; only slots below `num_row_offsets` are
real, and the invented ones parse cleanly into duplicate paths. `num_rows`
is not the answer either -- it reads 1501 against the 1161 the presence
bits call live, and the presence bits are what both the reader and
`removeTrack` actually use.

## Proposed sequencing

**Step 1 -- see it.** Detect and report all three shapes, repair none.
Cheap, immediately useful, and it turns "OneLibrary has 483 more rows than
rekordbox" from a number in a test into something a user can look at. The
comparison is the reusable part and is worth having before any repair
exists.

**Step 2 -- shapes 1 and 2.** Cue divergence, repaired with the writers
that already exist, staged through `RepairIssueChange`'s machinery. Reuses
`Repairable` / `Conflict` unchanged.

**Step 3 -- shape 3, options 2 and 3.** Removal and ignore. Option 1 is
shown but disabled, with the reason stated in the dialog rather than
hidden: Seabass can remove a row from either half and cannot yet add one.
A default that silently does nothing would be worse than an honest
disabled option.

**Step 4 -- option 1.** Row insertion, as its own piece of work. The
measurements above turn it into a page allocator over the pdb page chain
plus foreign-key resolution across nine tables -- bounded, but its own
project, and not something to smuggle in as a repair. Sequenced after
step 3 for a reason beyond size: steps 1 to 3 make the divergence visible
and give the user two honest answers, and that is worth shipping first.

Whatever comes of it needs verification on real hardware before it is
offered to anyone. Every measurement here is a read of one fixture; a pdb
that Seabass has *grown* is a shape no CDJ in this project's history has
ever been asked to load, and rekordbox writing is already the least-proven
part of Seabass.

## Where it plugs in

- **Comparison:** new, and the only genuinely new domain logic. Identity is
  by file path, which works now that `toContentPath()` trims the
  space-padding `export.pdb` puts on fixed-length fields.
- **Reading:** `readAllStickCatalogs()` already does it.
- **Issue model:** `LibraryConsistencyIssue` needs a kind for this, or a
  flag naming which half is the survivor. The existing kinds already
  express shapes 1 and 2.
- **Repair:** `RepairIssueChange` already stages a cue write plus a row
  removal, backs up first and is undoable. The one new thing it must learn
  is writing to the *other* half than the one the issue was found in --
  every existing repair writes to the catalog it scanned.
- **Backups:** unchanged. `filesToBackup()` names both halves already.

## What would make this wrong

- Arbitrating by mtime.
- Letting the comparison run against Engine. It is not the same library,
  and the per-catalog rule exists for it.
- Offering "add to the other half" as a working default before insertion
  exists.
- Counting divergence as `OneLibrary rows - rekordbox rows`. That is where
  the wrong 483 came from: a row with no resolvable path is not a
  divergence, it is a row with nothing to compare.
- Treating a clear presence bit as free heap. The body is still there and
  `used_size` still counts it.
