<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# Seabass test A and B

Two small libraries of real music with real defects, built by
`tools/library-build/make_test_library.py` and living outside the repository
(`~/Music/SeabassTestLibrary` by default, `SET.txt` marking them as
non-anonymized).

```
Test A   31 files   403 MB   matching and metadata torture
Test B   58 files   690 MB   repair and cleanup torture
```

Eight tracks are in both, so cross-stick sync has something to match. Each set
fits an 8 GB stick with room for a rekordbox export and an Engine library
beside it, and each backs up in seconds rather than the half hour a real stick
takes.

## Why real audio

Everything the suite has today is either a catalog with no audio at all
(`tests/fixtures/anonymized_library`, whose titles are placeholders) or bytes
synthesised frame by frame (`tests/mp3_fixture.hpp`, every one of them 128 kbps
44.1 kHz CBR). `docs/real-data-testing.md` already records what that costs:

> `SyncLibraries` and `DuplicateTrackFinder` … run on synthetic strings whose
> fuzziness is nothing like real punctuation, feat. spellings, remix suffixes,
> accents or case. A matching regression will not necessarily show up here.

These sets close that gap, and they are built by copying from a real library
rather than by inventing one, so the distribution of oddities is the real
distribution.

## The defect catalogue

Every row was observed in a real library first, with a count — none of it is
imagined. The id is what the manifest, this table and any test that asserts on
the fixture share.

| id | What | Seen in the wild |
|---|---|---|
| `A1-feat` | `feat.` spellings | throughout |
| `A2-remix` | remix suffixes that split one recording in two | 17 groups |
| `A3-accents` | non-ASCII names | 111 files |
| `A4-multiartist` | three or more artists in one field | throughout |
| `A5-nfd` | NFD-decomposed filename; a stick spells it NFC | the macOS exFAT unlink bug, FB8 |
| `A6-longpath` | path over 300 characters | 313 chars here |
| `A7-trailingspace` | component ends in a space | `export.pdb` pads with them |
| `A8`/`A9`/`A10` | 128 CBR, 192 CBR, **true VBR with no Xing header** | duration has to be estimated |
| `A11`/`A12` | FLAC at 48 kHz and 96 kHz/24-bit | no fixture has ever had a rate but 44100 |
| `A13-flac-vorbis` | FLAC with Vorbis comments only | mutagen raises on ID3 keys here |
| `A14-flac-twin` | the FLAC half of an mp3/FLAC pair | cue sync has to cross formats |
| `A15-untagged` | no tags at all | 208 tracks; identity falls back to filename |
| `A16-controlbyte` | `0x0C` inside an artist frame | 1 track — and it made a whole XML unimportable |
| `B1`/`B2` | duplicate pair, both files present, **no cues** | rig W6 plants this itself today |
| `B3-orphan` | audio no catalog mentions | rig W8 |
| `B4-zerofilled` | right size, not one non-zero byte | **114 files, 1.5 GB**, 109 of them the only copy |
| `B5-truncated` | 45 KB of an mp3: no duration, **no sample rate** | 1 file |
| `B6-flac-truncated` | FLAC cut mid-stream | — |
| `B7-crossformat` | same recording as mp3 and FLAC | dedup and cue sync across formats |

Planted as cues by the XML (see below): a memory cue at exactly 0:00 and one at
0.539 s. The second is what Engine's own auto-placed `main_cue` looks like, and
on a real stick **1388 of 1448 junk cues read back at `-0.000 ms`** — a
negative position that a `positionMs >= 0.0` guard skips silently.

## What is planted where, and when

Files carry what a file can carry: tags, formats, sample rates, damage.
**Cues cannot live in a file**, so they are planted through the XML:

1. `make_test_library.py` writes `seabass-test-a.xml` / `seabass-test-b.xml`
   with `POSITION_MARK`s, including the junk ones.
2. rekordbox imports the XML and exports the playlist to a stick. That is what
   turns the cues into ANLZ sections — and, incidentally, is the only way to
   get a real `export.pdb`: nothing in this project builds one from a folder.
3. `tools/make_engine_library <stick> <out>` builds the Engine side from that
   rekordbox catalog, giving a stick with all three catalogs.
4. Catalog-level damage is planted last, on the exported stick, because it can
   only exist in a database: an Engine `Track` blob whose sample rate will not
   decompress (**283 rows on the real stick**), a hot cue at the same slot but
   a different position in two catalogs, a cue at a negative position.

`tools/library-build/plant_defects.py` does step 4. What it plants, and what
the Engine schema allows, verified against the committed fixture:

| Defect | How | Effect, measured |
|---|---|---|
| unreadable sample rate | `PerformanceData.trackData` set to three bytes | warnings 329 → 334 |
| dangling row | path/filename suffixed `.missing` | row survives, file does not |
| streaming row | `streamingSource = 'TIDAL'`, path into a cache | must never be treated as a local file |
| missing artwork | `albumArtId` pointed at a row that is not there | Engine's own `image://fileart` bug |
| same file, two paths | second row with a trailing space on the path | tracks 1564 → 1569, and duplicate detection reports it |

Two things the schema settles, found by trying:

- **`Track.path` is UNIQUE**, so one file listed twice under the *same* path is
  impossible in Engine. The real form of that defect is one file under two
  *spellings* — `export.pdb` pads paths with trailing spaces, so
  `"/Contents/a.mp3   "` and `"/Contents/a.mp3"` are one file and two keys.
  That is the case `path_key_test` calls "THE ONE THAT BIT".
- **`(originDatabaseUuid, originTrackId)` is UNIQUE** too, so a copied row has
  to arrive as a locally-added track rather than as the same import twice.

Files under `unreferenced/` are deliberately left out of the XML: "audio no
catalog mentions" is a fixture shape in its own right.

## Using them

- As corpus sets: point `$SEABASS_CORPUS` at a directory holding a raw stick
  copy (`PIONEER/` + `Engine Library/`) once step 2 has produced one.
- As a live stick: `-DSEABASS_LIVE_STICK=<mount>` for `tests/qml-live` and
  `stray_scan_live_test`.
- As rig sticks: restore onto `VSTICKA`/`VSTICKB` and run `e2e/run-round.sh`.
  The checks that currently skip for want of data — W5, W6, W8 and the live
  sync cases — have something to bite on here, and a skip counts as a failure
  in that rig.

Record the counts as `SET-EXPECTATIONS.txt` beside the set the first time it is
read, the same convention `tests/corpus_test.cpp` uses everywhere else.

## Rebuilding

```
python3 tools/library-build/make_test_library.py \
    --source ~/Music/WhalesharkLibrary --out ~/Music/SeabassTestLibrary
```

Deterministic: the same source library produces the same two sets, so a
regression is a regression and not a reshuffle.
