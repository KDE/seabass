<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Sync Hub: Cue Points + Ratings & Comments

A new first-level page, **Sync Hub**, with today's "Sync
Cue Points" moving under it and a new **Ratings & Comments** subpage
beside it. Same job, same machinery, different payload: take one track
that several catalogs on the stick each know about, and make them agree.

Reviewed against the code on 2026-09-08; the review corrected four
claims the first draft got wrong. They are marked **(review)** below so
the reasoning is visible, not just the conclusion.

## Why this is mostly a re-use exercise

The sync code was written generic and has stayed that way. Concretely:

- `domain::SyncMatch` holds two whole `Track`s named `trackA`/`trackB`,
  not by format, and says so in its own comment. **Reusable unchanged.**
- `domain::TrackMatcher::match()` pairs tracks by resolved file path
  first, falling back to title+artist+duration. Nothing about it is
  cue-specific. **Reusable unchanged.**
- `SyncController::analyze()` already runs all three catalog pairs,
  handles per-catalog mtimes, playlist and search scoping, staged edits,
  backup and undo, and hands results to
  `CrossSourceConflictDetector` for the case two pairs target the same
  third catalog. **All reusable.**

**(review)** The first draft said exactly one thing is cue-typed. It is
three, plus a gap:

- `domain::SyncPlan` -- payload is `std::vector<CuePoint> cuesToApply`,
  `Kind` is named around cues (`NoCues`, `AlreadyConsistent`, ...).
- `gui::SyncPlanChange` -- the pending-change/undo unit calls
  `writeHotCues()` / `writeCuesForPath()` directly
  (`sync_controller.cpp`, the apply block).
- `gui::SyncFormatWriter` -- instantiates the three *cue* writers by
  format and owns the backup policy: rekordbox cue writes go to
  per-track ANLZ files and are backed up per item; Engine/OneLibrary go
  through `FormatWriteSession`'s scratch copy with the database backed
  up once.
- There is no write port for track metadata at all. The only sync port
  is `application::CueWriter::writeHotCues()`. Ratings & Comments needs
  a `TrackMetadataWriter` port (rating + comment, per format) and its
  own change class. On the rekordbox side that also moves the backup
  unit from "this track's ANLZ file" to "`export.pdb`, once" -- which
  `cleanup_controller.cpp` already does for the same file, so it is
  known ground, not new ground.

### How to share it

**Do not** template `SyncPlan` over a payload -- it crosses into the Qt
list models, and a template there buys nothing but obscurity.

**Do** extract the decision itself, which is the part worth not
duplicating and the part that would silently drift if copied:

```
// domain/sync_decision.hpp
enum class SyncSide { Neither, AOnly, BOnly, BothAgree, BothDiffer };
SyncSide compare(bool hasA, bool hasB, bool equal);
SyncPlan::Direction resolve(SyncSide, ConflictPolicy, mtimeA, mtimeB);
```

`ConflictPolicy` is the one knob the two pages set differently -- see
"Conflict policy" below. Then `CueSyncPlanner` and
`MetadataSyncPlanner` are each ~20 lines over that shared rule, with
their own plan struct and their own payload. `CrossSourceConflictDetector`
needs the same treatment: its logic is "two pairs target the same third
track", which is payload-independent.

## The gate: what can actually be written

The play-count lesson applies directly -- check writability *before*
promising a sync. Measured against the current code:

| | rating read | rating write | comment read | comment write |
|---|---|---|---|---|
| rekordbox `export.pdb` | yes, 0-5 u1 (`specs/rekordbox_pdb.ksy` `track_row.rating`) | **cheap** -- one byte at a fixed offset in a row `PdbRowWriter` already rewrites in place | yes | **only into an existing span of equal or larger size** -- see below |
| Engine `m.db` | yes (0-100, readers divide by 20) | **yes** -- `djinterop::track::set_rating()` (API present, not yet used anywhere) | yes | **yes** -- `set_comment()` (already used by the library creator) |
| OneLibrary `exportLibrary.db` | **no** | no | **no** | no |

**(review) The first draft said rekordbox↔Engine comment sync needs no
new writer. That is wrong for the case that matters.**
`PdbRowWriter::overwriteTrackText()` re-encodes text into the *exact
byte span the current value already occupies*; it never resizes or
reflows a row, by design. Consequences:

- A rekordbox comment that is currently **empty has capacity zero**, so
  "fill Engine's comment into rekordbox" -- the AOnly case, the common
  one -- writes nothing.
- A longer Engine comment is **silently truncated** to fit. The next
  analyze then sees the two sides differ again, and proposes again,
  forever; under last-write-wins it could even propose writing the
  truncated text back onto Engine, destroying the long one.
- `fitAsciiToCapacity()` was written for anonymization placeholders and
  is ASCII-only: a DJ's comment with an umlaut would be byte-split.

So comment *into* rekordbox needs a row-reflowing pdb writer, which is
a different and larger piece of work than anything `PdbRowWriter` does
today. Until it exists, comment sync is: Engine→rekordbox only where
the existing span fits the whole text (and is ASCII), otherwise reported
as "can't be written here" rather than proposed; rekordbox→Engine
freely. The equality rule has to be truncation-aware: if the rekordbox
value is a prefix of the Engine value, that is *not* a conflict.

**OneLibrary is the real work**, and it is reader work before it is
writer work: `onelibrary_reader.cpp` reads title, artist, bpm, length,
path, filename, bitrate, fileSize, key, play count and artwork -- no
track rating, no track comment. (Its only `comment` is a *cue* comment.)
The columns have to be found in `exportLibrary.db` and confirmed against
a real edit before anything can be synced into it.

## What survives a rekordbox re-export

**(review)** `docs/ratings-comments-plan.md` already established these
empirically (2026-09-02), and the first draft of this plan waved that
doc away as "a different feature". Its *scope* (master.db on the
computer) is a different feature; its *facts* decide what this one is
worth:

- rekordbox's export already carries rating **and** comment into
  `export.pdb`, and Engine re-imports them. So rekordbox→Engine sync is
  mostly what Engine does by itself the next time it imports the stick.
  It still has a use -- the stick is consistent *now*, before any
  re-import -- but it is not where the value is.
- Engine never writes back to `export.pdb`. Deck-side edits (ratings set
  on the Prime 4) live only in `m.db`.
- A rekordbox re-export regenerates `export.pdb` from master.db and
  re-triggers Engine's import, which can wipe deck-side `m.db` ratings.

So the direction with real value is **Engine→rekordbox** (deck-side
edits onto the rekordbox side), and it survives exactly as long as
today's cue sync does: until the next rekordbox export. That is an
accepted trade-off for cues and it is the same trade-off here, but the
page has to say so, in the words that doc already wrote:

> Ratings set on the Prime 4 are stored only on the USB stick and are
> erased the next time rekordbox re-exports to it.

## Conflict policy: mtime is noise here

**(review)** `SyncPlanner` breaks a genuine conflict by last-write-wins
on the catalog *file's* mtime: `export.pdb`'s and `m.db`'s
(`sync_controller.cpp`, `fileMtime()` calls). `m.db` is touched every
time the player runs; `export.pdb` on every export. For cues that was
already labelled a heuristic. For a 1-of-5 number it is a coin flip
dressed as a decision.

Recommendation: on Ratings & Comments, `BothDiffer` resolves to
`Direction::None` and is surfaced for a manual pick, the way
`CrossSourceSyncConflict` already is. Conflicts will be a handful per
stick; the pick costs nothing. That is the `ConflictPolicy` knob above:
`LastWriteWins` for cues (unchanged), `Manual` for metadata.

### Absence vs. deliberately cleared

Neither format distinguishes "never rated" from "rating cleared", and
both readers erase what little there is (`kaitai_rekordbox_reader.cpp`
keeps `> 0` only; `libdjinterop_engine_reader.cpp` maps `<= 0` to
nullopt). So a rating cleared on the Prime 4 (swipe left past the first
star) reads as "Engine has none", and an AOnly fill re-applies
rekordbox's four stars. Every run. Same for a comment deleted on one
side.

This is not solvable from the data. It is stated on the page, and fills
are shown as their own kind ("fill", not "overwrite") so a DJ who just
cleared something can untick the row.

## Navigation

```
Stick
├── Browse Library
├── Sync Hub                       <- new first-level card
│   ├── Cue Points                 <- today's "Sync Cue Points", moved
│   └── Ratings & Comments         <- new
├── Housekeeping
│   └── Duplicate Tracks -> …
└── Backups -> …
```

Follow `DuplicatesHubPage`/`BackupsHubPage` exactly: a hub page fanning
out to subpages, each subpage navigated to deliberately. Nothing about
this is novel.

One existing bug to fix while moving the card: "Sync Cue Points" is
gated on `hasRekordbox && hasEngine` (`StickListPage.qml`), which hides
it on a stick that has rekordbox and OneLibrary but no Engine -- a real
configuration, and one the analyze() code below the card already
handles. The hub should be enabled whenever **any two** catalogs are
present.

## Does this help deduplication? Mostly no, and that matters

The tempting story is: two copies disagree on rating, so send the DJ to
Ratings & Comments, sync, come back and the group cleans up. **Measured
on RV2, that story is wrong**, and shipping it would send people to a
page that cannot help them.

Both groups still flagged by `hasUnpreservableDataAtRisk` look like this:

```
Moritz Hofbauer - Busy Ants (Olympe Remix)
   rekordbox   rating=-    SAME FILE as survivor   <- survivor
   engine      rating=1    DIFFERENT FILE
   onelibrary  rating=-    SAME FILE as survivor
```

The rating is on an Engine row for a **different physical file**. Sync
matches the *same* file across catalogs; it has nothing to say about two
different files. Syncing would change nothing here.

### What would actually fix these two

Propagate rating and comment onto the survivor the same way
`DuplicateCleanupPlanner` already propagates bpm, key and artwork: fill
a gap when the survivor has no value and exactly one other copy does.
In both RV2 cases exactly one copy carries a rating and the survivor
carries none -- an unambiguous fill with nothing lost, no human needed.

**(review)** "Onto the survivor" means onto every catalog row for the
survivor file: `export.pdb` (cheap), the Engine row for that file
(cheap, `set_rating()`), and OneLibrary (blocked on its reader/writer).
So until step 4 lands, the fill is "written where writable, reported
where not" -- and the group is still held, because a rating that
reaches two of three catalogs is a rating that will disagree again. It
releases both groups only once OneLibrary can be written. (The two stray
*files* in those groups are no longer waiting on this: that flag was
decoupled from file deletion -- see
`unreferenced-file-cleanup-plan.md`. What is still held is the row-level
cleanup of the groups themselves.)
Comment fill into `export.pdb` hits the span problem above and is not
attempted until there is a reflowing writer.

### What to say in the meantime

The general rule, on the Clean Up page, and true regardless of any of
the above:

> Deduplication only removes a file when nothing it carries would be
> lost. Where copies disagree on ratings or comments, that group is left
> for you. Synchronizing metadata first resolves more of them.

No link, until there is a page that would actually resolve the case in
front of the reader.

## Stray File Statistics: read-only in v1

Decided: the audit page ships in v1 **read-only** -- it reports, it
never deletes. Content and layout come from
`docs/deduplication-roadmap.md`.

Two ways in, both from the Clean Up page's own explanation of what is
going on:

1. **The space diagram itself is clickable.** `SpaceReclaimBar` already
   shows reclaimable space against real capacity; clicking it opens the
   audit behind those numbers. Needs a cursor change and a hover
   affordance so it does not look inert -- a bar that is a link must
   look like one.
2. **A button beneath it**, for everyone who does not guess that a chart
   is clickable. Same destination. This is the discoverable path; the
   clickable diagram is the fast one.

Read-only means no survivor picking, no deletion, no staging: the tier
breakdown, the counterpart check, the per-rule outcomes, and which
catalogs were consulted. Everything it needs is already computed by
`walkAudioFiles` + `findUnreferencedFiles` + `DuplicateCleanupPlanner`.

## Order of work

1. **Hub page + move Cue Points under it.** Pure navigation, no logic.
   Fixes the two-catalog gating bug on the way.
2. **Extract the shared decision** (`sync_decision.hpp`, with
   `ConflictPolicy`), re-express `SyncPlanner` on top of it. No
   behaviour change; the existing sync tests are the safety net.
3. **Ratings & Comments, rekordbox↔Engine.** New `TrackMetadataWriter`
   port; Engine adapter over `set_rating()`/`set_comment()`; rekordbox
   adapter writing the rating byte and the comment *only where the
   existing span fits*. Truncation-aware equality. Manual conflicts.
   The re-export caveat and the cleared-vs-absent note on the page.
   Ships useful: deck-side ratings onto the rekordbox side.
4. **OneLibrary rating/comment reader**, verified against a real edit
   (same snapshot-and-diff method as `docs/ratings-comments-plan.md`),
   then its writer, then it joins the pair loop.
5. **Gap-fill rating in `DuplicateCleanupPlanner`**, written where
   writable; the group is released only when every catalog row for the
   survivor could take it (in practice: after step 4).
6. **Stray File Statistics, read-only**, with both entry points.
7. **Row-reflowing comment write for `export.pdb`.** Its own piece of
   work with its own risks; not on the v1 path unless it turns out to be
   needed for something else first.

Steps 1-3 are self-contained and shippable on their own. Step 4 is the
one with unknowns in it and should not block the rest.

## Not this feature

`docs/ratings-comments-plan.md` is a different *scope* and stays
separate: it is about rekordbox's `master.db` on the DJ's *computer* as
a source of truth, a scope expansion this project has deliberately not
taken. Everything here is stick-local, between catalogs already on the
stick. Its empirical findings, on the other hand, apply here in full --
see "What survives a rekordbox re-export".
