<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# The test-stick fixture

A real USB stick carrying test sets A, B and C through a full round trip:
exported from rekordbox, imported on a Prime 4, then damaged on purpose.
It is what the shakedown and `corpus_test` run against when a platform
needs more than the committed fixture, and the numbers below are what
every platform should see.

The audio is real music and is **not** anonymized, so the stick itself is
not in this repository and never will be. What is here is everything
needed to recognise a correct copy of it, and to tell a platform
difference from a regression.

Built 2026-09-19. FAT32, so every platform can mount it.

## What is on it

```
100 audio files, 1.2 GB
  PIONEER/           rekordbox export.pdb + USBANLZ   (exported from rekordbox)
  Engine Library/    Database2/m.db + friends         (imported on a Prime 4)
  Contents/          the audio
  SET-EXPECTATIONS.txt
  Engine Library/Database2/m.db.before-defects        the catalog before planting
```

Four playlists, in one collection:

| Playlist | Entries |
|---|---|
| Seabass test A | 30 |
| Seabass test B | 55 |
| Seabass test C -- Crate One | 8 |
| Seabass test C -- Crate Two | 8 |

A exported 30 of its 31 tracks and B 55 of 57: rekordbox declined the
deliberately broken files (`B4-zerofilled`, `B5-truncated`), which is
correct and is itself part of the expected shape.

## Recorded expectations

`SET-EXPECTATIONS.txt` lives on the stick, so a restored copy carries it
and `corpus_test` compares against it automatically. It is repeated here
so a platform can check a copy without mounting it, and so a change is
visible in review:

```
engine.cues 123
engine.opensPerBatch 1
engine.tracks 103
engine.tracksWithAlbum 92
engine.tracksWithCues 99
engine.writeRefusals 0
matrix.addCue.durableWritesPerSave 4
matrix.addCue.pdbParses 1
matrix.cleanup.durableWritesPerSave 6
matrix.cleanup.pdbParses 2
matrix.copyCues.durableWritesPerSave 4
matrix.deleteOrphan.durableWritesPerSave 2
matrix.deviceSetting.durableWritesPerSave 3
matrix.mergeCues.durableWritesPerSave 4
matrix.repair.durableWritesPerSave 5
matrix.repair.pdbParses 1
matrix.strayCue.durableWritesPerSave 8
matrix.strayCue.encryptedOpensPerSave 1
matrix.strayCue.pdbParsesPerSave 1
matrix.sync.durableWritesPerSave 2
matrix.sync.engineOpens 1
onelibrary.tracks 100
rekordbox.cues 131
rekordbox.tracks 100
rekordbox.tracksWithAlbum 90
rekordbox.tracksWithCues 99
sync.matched 90
```

`engine.tracks` is 103 rather than 100 because three of the planted
defects add a second row for a file that already has one.

`matrix.cleanup.durableWritesPerSave` was 5 when the stick was built and
is 6 since `7f0276ee`, which made Clean Up's pending-deletion record
durable. The reference backup a shakedown restores from is
`SHAKEDOWN_8.zip` (2026-09-24), which carries the corrected file.

The Prime 4's import leaves `Track.lastEditTime` NULL on every row it
creates -- all 100 in `m.db.before-defects`. Sync reads that column as a
track's own edit time, so every imported track reads as never edited.
`corpus_test` checks that the reader reports exactly what the database
holds, not that every track carries one.

## The catalogs disagree, on purpose and by accident

| | rekordbox | Engine (Prime 4) |
|---|---|---|
| cues | 131 | 123 |
| hot / memory / loop | 119 / 8 / 4 | 123 / 0 / 0 |
| inside the first second | 96 | 91 |

Two things to read out of that, both measured here rather than assumed:

**The Prime 4's rekordbox import keeps hot cues and drops memory cues and
loops.** Every one of them, including a loop at 0.000 that survived
rekordbox's own round trip. This is not a gap in the reader --
`libdjinterop_engine_reader.cpp` reads `tr.loops()` -- the rows are not
there. For a project whose purpose is moving cues between these two
formats, that asymmetry is the headline.

**rekordbox writes a hot cue inside the first second on 87 of 100
tracks,** all named `1.1Bars`, its auto-placed first-bar marker.
`domain::isJunkCue` is `!isLoop && positionMs < 1000`, so all 87 are junk
by the current rule. That rule's own header justifies covering hot cues
on the grounds that they "turn up one or two to a library"; a freshly
exported stick says 87%. Worth deciding deliberately rather than meeting
it as a surprise in a cleanup dialog.

## Planted catalog damage

`tools/library-build/plant_defects.py <stick> --count 3 --backup`, which
is what produced the state recorded above. Three rows each except where
noted:

| Defect | What it is |
|---|---|
| `unreadable-sample-rate` | `PerformanceData.trackData` too short to decompress |
| `dangling-row` | path points at a file that is not there |
| `streaming-row` | TIDAL row whose path names a cache on another machine |
| `missing-artwork` | `albumArtId` pointing at nothing |
| `same-file-two-paths` | one file, two rows, the second padded with a trailing space |
| `repairable-case` | path uppercased |
| `repairable-backslashes` | path written with backslashes |
| `repairable-nfd` | path decomposed (2 rows: it needs an accent to decompose) |
| `unrepairable-path-is-a-directory` | path names `Contents` (1 row) |

The three `repairable-*` families are the ones worth understanding. Those
files are **present and undamaged on disk**; only the spelling in the
catalog differs, and `application::path_key` folds exactly those four
differences. None of them may ever appear in a missing-file list. A
fixture holding only genuinely absent files would never catch a
normalisation that quietly stopped working, and the failure mode there is
not a missing file -- it is Seabass offering to delete music that is
still referenced.

## Running against it

`corpus_test` wants a directory whose *children* are sets, so point it at
a parent, not at the stick:

```sh
mkdir -p /tmp/corpus && ln -s /Volumes/SANDISK_1 /tmp/corpus/SEABASS-TEST-ABC
SEABASS_CORPUS=/tmp/corpus ./corpus_test
```

It never modifies a set; every write happens on a scratch copy.

The shakedown takes its sticks from the environment, so nothing is
hardcoded to one machine:

```sh
RIG_STICK_A=/path/to/this/stick tools/rig-shakedown.sh
```

## Known failures, as of 2026-09-19

These fail today and are findings, not flakiness. A platform seeing
exactly these is seeing the fixture work correctly; a platform seeing
*more* has found something new.

**`every Engine track carries Track.lastEditTime` -- 93 of 103 do not.**
The Prime 4's import leaves it unset on almost everything. Sync uses edit
times to decide which side is newer, so this is not cosmetic.

**`track kept every other cue it had` -- tracks 2, 32 and 92.** Removing a
stray cue loses the track's other cues. Three different causes behind one
symptom:

- **track 2** is the planted unreadable sample rate, doing exactly what it
  was planted for: a stored sample offset converted to a time meets a
  zero.
- **track 32** is `Trailing space .mp3` and **track 92** is
  `C6-collision-a.m4a`. Neither has a `PerformanceData` row at all -- the
  Prime 4 declined to analyse a filename ending in a space, and an m4a.
  Both are fixture edge cases, and neither was planted at the catalog
  level; the Prime 4 produced this by itself.

That last one is a data-loss path, which is the class this project cares
about most.

## Getting a copy

The stick is backed up as a full stick backup named `TESTRIG_ABC`.
Restore it onto a FAT32 stick and the whole fixture comes with it,
`SET-EXPECTATIONS.txt` included.

Do not re-record `SET-EXPECTATIONS.txt` to make a failure go away. It is
deleted and re-recorded only after a deliberate change to the data, and
the reason goes in a comment at the top of the file -- see
`tests/fixtures/anonymized_library/SET-EXPECTATIONS.txt`, which carries
three such notes.
