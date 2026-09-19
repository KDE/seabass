<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# Seabass test A, B and C

Three small libraries of real music with real defects, built by
`tools/library-build/make_test_library.py` and living outside the repository
(`~/Music/SeabassTestLibrary` by default, `SET.txt` marking them as
non-anonymized).

```
Test A   31 files   403 MB   matching and metadata torture
Test B   58 files   690 MB   repair and cleanup torture
Test C   15 files   153 MB   near-duplicates, across two playlists
```

Eight tracks are in both A and B, so cross-stick sync has something to match. Each set
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
| `C1`/`C2`/`C3` | near-duplicate pair, **8s / 7s / 6s** apart, halves in different playlists | 15 such pairs survived the real rebuild |
| `C4-deep-*` | same audio, 7s of silence prepended | built here, so the answer is known |
| `C5-deep-*` | same audio, 9s cut from the end | built here, so the answer is known |
| `C6-collision-*` | **different recordings, same length to the second** (159s vs 159s) | a library of techno is full of these |
| `C7-cues-at-zero` | memory@0.000, hot@0.000, hot@0.999, **loop@0.000** | hot cues at 0:00 turned up at 7ms and 109ms |
| `C8-cue-boundary` | hot cues at 0.999 and 1.001, either side of the line | an off-by-one here deletes a real cue |
| `C0-in-both-crates` | **one file, listed by both playlists** | 652 of the real library's 1469 tracks |

### What test C must produce

The ids above are only useful with the answers attached, so a test can
assert rather than a person can squint. `MANIFEST.md` beside the audio
repeats these per file; this is the contract.

| Fixture | Must happen | Must NOT happen |
|---|---|---|
| `C1`/`C2`/`C3` | reported as candidates, across playlist boundaries | merged automatically -- no tolerance reaches 8s without swallowing real edits |
| `C4`, `C5` | matched as the same recording by content | rejected because the durations differ |
| `C6` | kept apart | merged -- duration alone says they are identical |
| `C7` | the memory cue, and **both** hot cues, removed | the loop at 0.000 removed; it is exempt |
| `C8` | the 0.999 cue removed | the 1.001 cue removed |
| `C0` | listed under both playlists, counted **once** | reported as a duplicate, or offered for deletion |

`C6` is the one that decides whether the set is worth anything. A fixture
with only true duplicates in it scores full marks for merging everything,
which is the failure that loses tracks.

The cue merge cases, planted as `POSITION_MARK`s in the XML: `C1` carries
three hot cues on one side and none on the other, so a merge has to carry
them across; `C2` has the **same hot slot at two positions and two
colours**, so one has to win and the loser must be reported rather than
vanish; `C3` has a memory cue on one side and a loop on the other, which a
union keeps whole. Ratings, comments and genres disagree across every
pair, so a merge that reconciles cues and silently drops the other side's
rating fails here and nowhere else.

Planted as cues by the XML (see below): a memory cue at exactly 0:00, one at
0.539 s, and a loop at 0.1 s that must **survive** the clean-up.

The loop matters because the rule is about position and not kind: a cue inside
the first second is noise whatever it is, since the track already starts there,
while an intro loop on the first bar is real work and carries an end as well as
a start. The set exists partly to keep that distinction honest — it is exactly
the kind of rule that gets widened by one commit and quietly eats real cues.

## Test C, and why it is separate

A and B each hold one playlist. C holds **two, in one XML**, because that
is what the real library looks like: its collection has 40 playlists and
652 of its 1469 tracks are in more than one of them. A duplicate check
that only ever compares within a playlist finds nothing there.

So C splits each near-duplicate pair across the two crates, and plants one
file that *both* playlists list. That one is the control, and it is the
distinction the whole project rests on: a file duplicates, a row does not.
Anything counting playlist entries instead of files reports it and offers
to delete a track still sitting in a crate.

The near-duplicates are the 15 pairs the real rebuild could not resolve --
same artist, same title, both 320 CBR, durations 2 to 8 seconds apart. The
three widest go in C. There is no duration bracket that gets these right:
the gaps run in a smooth gradient with no natural cut-off, so widening it
to catch the 8-second pair merges everything closer than 8 seconds, and
some of those are genuinely different edits. They are not there to be
merged automatically; they are there to be *offered*, which Seabass
already supports. A run that silently merges them is as wrong as one that
never mentions them.

Nobody knows which of those three are truly the same recording, so C also
carries material where the answer is not a matter of opinion, built here
rather than found: one pair with 7 seconds of silence prepended, one with
9 seconds cut off the end, both provably the same audio (correlation 1.000
at offsets of +7.00s and 0.00s). And the negative that matters more than
either -- two genuinely different recordings **the same length to the
second**, which duration alone would merge.

C also pins the junk-cue threshold from both sides, which nothing else
does. `domain::isJunkCue` is `!isLoop && positionMs < 1000`, so one file
carries a memory cue at 0.000, a hot cue at 0.000, a hot cue at 0.999 and
a loop at 0.000 -- three to remove and one to keep -- and another carries
hot cues at 0.999 and 1.001, one either side of the line. Note the hot cue
at 0.000 is junk: it counted as deliberate until 2026-09-18.

Finally it is the only set carrying ratings, comments and genres, all
deliberately disagreeing between the halves of a pair, because a merge
that reconciles cues and silently drops the other side's rating is a
regression nothing else would catch. Rating is stars x 51 in rekordbox's
XML, so 5 stars is 255.

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

## Once they are on a stick

A and B and C become a single fixture the moment rekordbox exports them
and a player imports them: three catalogs, real cues, and the damage only
a database can hold. That stick, what is on it, what every platform
should see, and the bugs it has already found are in
[test-stick-fixture.md](test-stick-fixture.md).

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
