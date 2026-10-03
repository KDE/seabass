<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# The Engine "update the Rekordbox library" prompt, measured

Denon's Engine OS players (Prime 4, Prime Go+, and the rest of the line)
offer, on inserting a stick that carries both a rekordbox export and an
Engine Library:

> Would you like to update the Rekordbox library from 'A3 (USB 1)'?
> The device will be unavailable and the update could take some time.
> Existing playlist and track metadata will be overwritten.

This page is the protocol for finding out exactly what saying yes does to
the Engine side, so the answer can be quoted rather than guessed. The tool
is `engine_import_probe` (tools/engine_import_probe.cpp), driven by
`tools/engine-import-probe.sh`.

## What is measured already, and what is not

Measured (Prime 4 and Prime Go+, 2026-09-18):

- The prompt appears when export.pdb's header sequence (the sixth
  little-endian word of the file) differs from
  `Information.lastRekordBoxLibraryImportReadCounter` in m.db, and not
  when they are equal. See `src/infrastructure/engine/engine_import_state.hpp`.

Seen on real sticks, not yet measured under control:

- After an accepted import, AlbumArt rows hold
  `image://fileart//<path on the importing computer>` as their hash, which
  no player can resolve, so the covers silently vanish.
- Engine-side repairs Seabass made were gone afterwards.
- Imported tracks carry no analysis (`isAnalyzed = 0`, NULL
  `trackData`, `overviewWaveFormData`, `beatData`), and the player analyses
  each one on first load.
- `Track.pdbImportKey` equals the Information counter at the import on
  every imported row of the repository's fixture (1469 of 1564; the other
  95 are 0), which suggests the import stamps the rows it writes.

Guessed, and what this probe is built to settle:

- Whether the import replaces Engine's cues with rekordbox's, merges
  them, or leaves tracks it already knows alone.
- Whether it touches tracks or playlists rekordbox does not have.
- Whether Engine-only metadata (title, rating, comment, key, BPM, last
  played) survives.
- Whether it rebuilds rows (new ids) or updates them in place.
- Whether it clears analysis the player already did.
- Whether it reads export.pdb or exportLibrary.db (OneLibrary). The plant
  writes the rekordbox cues to both, so the answer to the other questions
  does not depend on this one.
- Whether it changes the rekordbox side at all (it should not).

## The protocol

Everything on a test stick (SHAKEDOWN8B, or another rig stick with tracks
in all three catalogs). Never WHALESHARK, WHALESHARK2 or CORSAIR: both the
tool and the script refuse those labels.

1. **Plant.** With the stick mounted on the computer:

       PROBE=<build dir>/engine_import_probe \
       tools/engine-import-probe.sh plant /media/<user>/SHAKEDOWN8B <work dir>

   This tars the stick's `Engine Library` and `PIONEER` folders into
   `<work dir>/pre-plant.tar`, plants the matrix below (`cases.tsv`),
   records the planted stick (`before.tsv`) and tars it again
   (`planted.tar`). The plant arms the prompt and prints both numbers
   before and after; it ends `PLANT RESULT: PASS` or `FAIL`.

2. **Eject** the stick from the computer properly.

3. **Insert** it into the player. The prompt appears. **Accept** it, and
   wait until the player says the update is done.

4. **Eject** it from the player's own menu, so the player finishes its
   writes.

5. **Record.** Mount it on the computer again:

       tools/engine-import-probe.sh record /media/<user>/SHAKEDOWN8B <work dir>

   This tars the stick first (`after-import.tar`, the raw evidence) and
   then records it (`after.tsv`).

6. **Compare.**

       tools/engine-import-probe.sh compare <work dir>

   prints, per planted case, KEPT, OVERWRITTEN (old -> new), DELETED,
   ADDED or MERGED, then everything that changed outside the matrix, and
   saves it as `report.txt`. The last line is
   `PROBE RESULT: <n> cases, <k> overwritten, <m> kept, ...`.

7. **Restore** for the next run (on the second player, say):

       tools/engine-import-probe.sh restore /media/<user>/SHAKEDOWN8B <work dir>

The only human steps are 2 to 4. Run it once on the Prime 4 and once on
the Prime Go+, each from a restored stick, with its own work dir: two
reports that agree are a finding; two that differ are a better one.

## The planted matrix

Every case has a track of its own, so whatever happens to it is
attributable. `--skip <substring>` (repeatable) keeps every track whose
stick-relative path contains the substring out of every case, observed
ones included, for tracks that carry other evidence. The plant ends by
listing every track it wrote to, per side, so exactly those can be
snapshotted. Tracks are chosen from those both catalogs list, in path
order, 90 seconds or longer.

The cue cases (a to g) write BOTH sides' complete cue sets: hot cue 1 at
10 s on both (a common cue, which should survive whatever the import
does), plus the one difference the case is about. Whatever cues those
tracks held before are replaced on both sides.

| case | Engine side | rekordbox side | written with |
|---|---|---|---|
| a | pad 8 at 30 s | no pad 8 | Seabass's Engine and rekordbox cue writers |
| b | no pad 8 | pad 8 at 30 s | same |
| c | pad 7 at 30 s | pad 7 at 45 s | same |
| d | pad 2 at 30 s, magenta (#FF00FF) | pad 2 at 30 s, no colour | same |
| e | hot loop pad 6, 60 s to 68 s | no loop | same |
| f | main cue at 20 s | no memory cue | same |
| g | no main cue | memory cue at 25 s | same |
| h1 | playlist "Seabass probe Engine only" with two tracks | no such playlist | libdjinterop `create_root_playlist` |
| h2 | an extra track in the shared playlist | not in it | libdjinterop `add_track_back` |
| h3 | a track removed from the shared playlist | still in it | libdjinterop `remove_track` |
| i | a track row rekordbox does not have | absent | see below |
| j1 | rating 1 or 5 stars, whichever rekordbox does not have | unchanged | Seabass `writeAnnotation` |
| j2 | comment "Seabass probe: Engine comment" | unchanged | Seabass `writeAnnotation` |
| j3 | key six steps round from what it was | unchanged | SQL on `Track.key` |
| j4 | BPM rekordbox's plus 7 | unchanged | Seabass `propagateMissingFields` |
| j5 | title "Seabass probe Engine title" | unchanged | libdjinterop `set_title` |
| k | cover as Seabass's repair writes it: a file under `Engine Library/Artwork/` | rekordbox's own artwork | Seabass `repairArtwork` |
| l | last played 2026-01-02 03:04:05 UTC, isPlayed 1 | unchanged | Seabass `setLastPlayedAt`, SQL for the flags |
| m | a track the player has already analysed, untouched | unchanged | nothing written: observed |
| n | a control track nobody touched | unchanged | nothing written: observed |

The shared playlist is "Seabass test A" on the rig sticks (or the first
name both catalogs hold with three or more tracks).

Per field, why plain SQL where it is used:

- **key (j3)**: libdjinterop's `set_key` also rewrites
  `PerformanceData.trackData`, which on a track the player has not
  analysed creates an analysis blob that was not there: two differences on
  one track. The SQL sets `Track.key` alone.
- **isPlayed, playedIndicator (l)**: not in libdjinterop's API.
  `playedIndicator` is set to `Information.currentPlayedIndiciator`, which
  is a guess at Engine's own convention.

Case i: when the stick already has an Engine-only row (a track m.db lists
and export.pdb does not), that row is the case and nothing is written.
Otherwise the plant copies one track's audio file to
`Seabass probe/engine-only.<ext>` at the stick root and makes an Engine
row for it with libdjinterop's `create_track`, from the original track's
snapshot, titled "Seabass probe Engine-only track".

Case k: Seabass's own repair (`repairArtwork`) is fed, in order of
preference, an imported `image://fileart//` cover whose image is on the
stick (exactly what the app's cover repair fixes), rekordbox's own image
for the track, or the `--cover` image given to `--plant` (the script
passes `COVER` from its environment).

Case m cannot be planted: the analysis is the player's to make. If the
stick has no analysed track, the case is reported as not planted, and
loading any track on the player once before planting makes one. The
library-wide counts (tracks with `isAnalyzed = 1`, tracks without beat
data) are in every report regardless.

**Arming the prompt.** export.pdb is never written. With `--no-arm` the
plant leaves Engine's counter alone and only reports both numbers: for a
stick whose pdb sequence a real rekordbox export has already moved (on
SHAKEDOWN8B, connecting it to rekordbox 7.2.18 moved it from 535 to 539
by itself), which is the realistic arming. Otherwise the plant sets
Engine's `lastRekordBoxLibraryImportReadCounter` to one less than the
pdb's sequence, which is what a rekordbox export made after the last
import looks like from the player's side. Whether the import behaves
differently between the two kinds of arming is itself untested; a
`--no-arm` run on a stick rekordbox has written answers the realistic
case.

## What a record holds

`--record` writes one line per value, `section, entity, field, value`,
tab separated, with tabs and newlines escaped. Tracks are keyed by their
audio file's path relative to the stick root, not by row id, so a row
the import rebuilds still lines up (its `id` field then shows the change).

- **ET** per Engine track: every `Track` column (`col.*`), every
  `PerformanceData` blob as length and digest (`perf.*`), the cue fields
  (`cue.hot.N`, `cue.hot.N.color`, `cue.hot.N.label`, `cue.loop.N`,
  `cue.main`), the cover (`art`: `imported:image://fileart//...` when the
  hash is an import's path, whether stored as text or as a blob, else
  `text:` and the hash, or `file:Artwork/...` for the file a blob hash
  names; `art.bytes`: the image held in the row), title, key, rating, BPM, comment, playlists, and `content`, a
  digest of what a DJ would call the track.
- **EP** per Engine playlist: row id, count, entries in the player's own
  order.
- **EI** the Information row; **EA** AlbumArt counts; **EF** every file
  under `Engine Library` with size and modification time.
- **RT, RP** the same for the rekordbox side, read through the app's own
  rekordbox reader (export.pdb plus the ANLZ cue files). The import is
  supposed to be one-way; the report says so when it was.
- **M** the pdb sequence, Engine's counter, and whether the player would
  ask.

## Reading the report

A case's verdict is that of its first watched value:

- **KEPT**: Engine holds what it held before.
- **OVERWRITTEN**: Engine holds something else; the line shows old and
  new, and says when the new value is rekordbox's.
- **DELETED**: the value is gone (a cue slot emptied, a row or playlist
  removed, a track no longer in a playlist).
- **ADDED**: a value Engine did not have (rekordbox's cue, a track put
  back into a playlist).
- **MERGED**: for sets (playlist entries): Engine now holds both sides'.

Under each case, "also changed on this track" lists every other field of
that track that moved, which is where rebuilt rows, new `lastEditTime`s,
`pdbImportKey` stamps and cleared analysis show up.

"Everything else" covers the tracks outside the matrix (counted per
field, with examples), covers rewritten to `image://fileart//`, analysis
counts, playlists added, removed or rebuilt, the Information row, files
under Engine Library added, removed or rewritten, and the rekordbox side.

## Dry run

A dry run against a copy of `tests/fixtures/anonymized_library` (laid out
as a stick: `rekordbox` as `PIONEER`, `engine` as `Engine Library`) shows
the matrix the plant produces, without any player. Never plant into the
fixture itself.
