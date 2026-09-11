<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Cleaning up unreferenced audio files

## The problem

A stick accumulates audio files that no catalog references any more:
rekordbox's `export.pdb`, Engine's `Database2/m.db` and OneLibrary all
have no row pointing at them. They are invisible to every feature we
have, because every feature starts from a catalog and works outwards.

They arise the way you would expect: a duplicate cleanup (ours, or the
DJ software's, or a hand edit) removes the *rows* for a redundant copy
and never removes the *file*. "Delete Orphaned Files" covers the case
where we did it ourselves and recorded it in
`.seabass-pending-deletions.jsonl`. It cannot cover a cleanup that
happened before that manifest existed, on another machine, or in
rekordbox itself -- the manifest simply has no entry, so the file is
lost to us forever.

## What is actually out there (RV2, 2026-09-08)

Measured on the real 30 GB stick, 96% full with 1.5 GB free:

| | |
|---|---|
| audio files on disk | 2100 |
| referenced by no catalog | **632** (8.66 GB) |
| of those, readable by a tag parser | 632 |
| with both artist **and** title in their tags | 632 |
| with a duration | 632 |
| grouping with a catalogued track on artist+title+duration±2s | **632** |
| grouping with nothing at all | 0 |

Comparing each against its best catalogued twin, byte for byte:

- **598 are byte-identical** to a file the catalog still references
  (8.14 GB) -- free space with no judgement call in it at all
- **28** are a lower-or-equal quality copy: the ordinary duplicate
  decision `DuplicateCleanupPlanner` already makes
- **6** are the same size but *not* byte-identical -- almost certainly
  the same audio with differently-padded tags, but not proven so, and
  "not proven" is the only category that matters when the next step is a
  delete. They go through the ordinary duplicate review like any other
  group. (An `ffmpeg -f md5` comparison of the decoded audio was tried
  and thrown away: run against a busy USB bus it reported five
  *byte-identical* pairs as having different audio, which is impossible.
  A method that produces false "these differ" answers is useless for
  clearing files, so the byte-level hash is the one we trust.)
- **0** are better than anything the catalog has -- nothing here is a
  file we would want to keep *over* a catalogued one

The names say where they came from: `-1`/`-2` copy suffixes and
truncated rekordbox export names, i.e. exactly the "rows removed, files
left" shape described above.

So the metadata in the files agrees with what the catalogs recorded, for
every single one. This needs no fuzzy matching and no new grouping
rules: the existing `DuplicateTrackFinder` key (artist + title +
duration within tolerance) already works on it, once something puts the
tags in front of it.

## Reading the metadata: TagLib

We already have `TrackDurationProbe` / `QtMultimediaDurationProbe`, so
the obvious question is whether to widen that. We should not.

`QMediaPlayer` metadata is a side effect of opening a demuxer:
asynchronous, needs a `QCoreApplication` and a nested event loop, and
*which* fields get populated depends on the backend -- FFmpeg on Linux,
MediaFoundation on Windows and AVFoundation on macOS do not agree. That
is a per-platform behaviour difference feeding a destructive code path.
It also gives no dependable bitrate, and bitrate is precisely what
`DuplicateCleanupPlanner` picks the survivor by.

TagLib is the small, self-contained, cross-platform thing this calls
for. Pure C++, no Qt, no event loop, synchronous. One open yields
artist, title, album *and* `audioProperties()` -> length, bitrate,
sample rate: everything both the finder and the planner need. It reads
headers and tag frames rather than the stream, so it moves far fewer
bytes over USB than a demuxer open. It is 326 kB of shared library.

Measured on this machine, same warm 400 files, apples to apples:

| | total | per file | gives |
|---|---|---|---|
| TagLib 1.13.1 | 29 ms | **0.07 ms** | artist, title, duration, bitrate |
| `QtMultimediaDurationProbe` spike | 2997 ms | 7.5 ms | duration |

**103x faster**, and it answers three more questions per file. Over the
whole stick, all 2100 files: 22-66 s cold over USB (I/O bound, varies
with where the reads land), 151 ms fully warm. Coverage was total --
2100/2100 gave a duration and a bitrate, 2099/2100 gave artist+title.

Because it is Qt-free it can sit below `seabass_core` and work in
`seabass-cli` and the Qt-free `corpus_test`, which the Qt probe never
can.

Packaging is a one-liner everywhere we build: `libtag1-dev` on
Debian/Ubuntu (note: *not* `taglib-dev`, and `apt-cache search taglib`
does not match it), `mingw-w64-ucrt-x86_64-taglib` in MSYS2 alongside
every other dependency in `docs/windows-build.md`, homebrew or vcpkg on
macOS. Vendoring as a submodule is the fallback, the same shape as
libdjinterop -- use 1.x, since 2.x pulls in utf8cpp. Licence is
LGPL-2.1/MPL-1.1, fine to link from our GPL-2.

Alternatives considered and rejected: `dr_libs`/`miniaudio` are
single-header and cross-platform but decode-only, with no tag reading at
all; `libavformat` directly is heavy and a packaging problem on Windows.

### The one caveat, and why it decides a safety rule

For a VBR MP3 with no Xing/VBRI header, TagLib does not know the real
length -- it estimates it from the bitrate, and can be off by seconds.
The grouping tolerance is 2 seconds, so an estimate can put a file in
the wrong group, or in no group.

How often does that bite? On RV2, never: of the 1857 MPEG files on the
stick, **every single one carries a Xing or VBRI header**, so TagLib
reads a real length rather than estimating one. The other 243 files are
MP4/M4A, whose length comes from the container header and is exact by
construction. Zero files on this stick land in the uncertain bucket, and
zero of the 632 unreferenced ones do.

The rule below therefore costs us nothing here. It still has to exist,
because another stick, another rip or an older encoder will produce a
headerless VBR MP3 eventually, and the cost of noticing that only after
deleting the file is unrecoverable.

That never matters for a track we merely *display*. It matters enormously
here, because the outcome of this feature is deleting a file that no
database will ever mention again. So:

> **A file whose duration we had to estimate is never deleted.**

Implemented slightly wider than that sentence, deliberately: one
estimate holds back *every* stray in its group, not only the file whose
length was guessed. The doubt an estimate creates is about the grouping,
not about that one file -- a guess that dragged a foreign file into the
group could as easily make the group's other copies look redundant, and
those are the ones that would then be deleted. Holding the whole group
costs nothing on real data (zero files on RV2) and closes the case where
the rule as written would still lose a track.

We list it, we say why, and we leave it on the stick. Being wrong costs
the DJ a track they own; being conservative costs them disk space they
can reclaim by hand. Those are not comparable. The probe therefore has
to report duration *confidence*, not just a number -- a bare `double`
cannot express "roughly", and a caller that cannot tell the difference
will eventually treat a guess as a fact.

Detecting it is cheap and exact: `TagLib::MPEG::Properties::xingHeader()`
returns null precisely when TagLib had to fall back to
`streamLength / bitrate`. (`XingHeader` covers VBRI too.) Every non-MPEG
format we meet reports a container-derived length, which is never
estimated.

## What the rules would actually delete (RV2)

Simulated over all three catalogs, with the DJ metadata each row really
carries, using a field-for-field port of `DuplicateTrackFinder::find`
and `DuplicateCleanupPlanner::plan`:

| | files | size |
|---|---|---|
| unreferenced on disk | 632 | 8.66 GB |
| **proposed for deletion** | **632** | **8.66 GB** |
| held back by `differs`, or by an estimated duration | 0 | 0 |
| (would have been held back by `hasUnpreservableDataAtRisk`) | 2 | 0.02 GB |

Two numbers in that table moved after they were first written down, and
both moves were decisions rather than measurements:

- **575 → 630** when play counts left `hasUnpreservableDataAtRisk`. The
  57 files / 0.91 GB the first draft recorded were nearly all a
  rekordbox play count meeting an Engine last-played timestamp. With
  that comparison gone, the flag holds back **2** stray files.
- **630 → 632** with the decision below: the flag no longer holds a
  *file* back at all, only the row-level cleanup.

435 duplicate groups contain a stray file (247 with one stray, 182 with
two, 6 with three or four). **Every one of the 435 carries at least two
catalogued rows**, because OneLibrary mirrors rekordbox track for track
-- which is why a flag about two rows disagreeing could reach so many
files that have no row at all.

Zero groups consist only of unreferenced files, so the "a stray may be
survivor when the whole group is stray" rule never fires on this stick.
It is insurance for other sticks; this data does not exercise it.

The "survivor must be catalogued" rule overrides the planner's own pick
far more often than the simulation suggested, and how often depends on
which catalog the page is working in -- the Clean Up page groups one
catalog's rows plus the strays, so each page gets its own answer:

| page | groups where the rule fired | of 435 |
|---|---|---|
| rekordbox | 86 | |
| OneLibrary | 86 | |
| **Engine** | **435** | every single one |

**Every group on the Engine page.** Engine stores no bitrate at all --
0 kbps on all 1564 rows -- so any stray, whose bitrate TagLib reads off
the file, outranks the catalogued row it was copied from. Without the
rule, a cleanup run from the Engine side would have kept the file no
catalog knows about and proposed deleting the catalogued one, in all 435
groups. That is the rule's entire job, and this stick exercises it
completely.

On the other two pages it fires 86 times. The simulation said 9, and the
difference is worth more than the number:

- **9** are the case this document originally described: no catalogued
  row in the group knows its own bitrate (Engine leaves it unset until
  it analyzes a track), while TagLib reads one off the file. Without the
  rule the planner would keep the abandoned copy purely because it was
  the only one that knew its own bitrate.
- **77** are a rounding difference. rekordbox stores 256 kbps for an
  M4A; TagLib computes 257 kbps from the same bytes. Same file, same
  size, 1 kbps apart -- and that is enough to make the uncatalogued copy
  score "higher quality" than the catalogued one it was copied from. The
  simulation could not see these because it read the stray files with
  mutagen, which happens to round the way rekordbox does.

The second group is the useful finding. Over a 1 kbps rounding
difference, 77 groups would have kept the uncatalogued file. Together
with Engine's 435, the conclusion is the same one twice: **a probe-read
bitrate and a catalog-stored one are not the same measurement.** One is
computed from the bytes, the other is whatever the exporting application
chose to record, and the two disagree both by rounding and by absence.
That is a caution against the "fill in bitrate on catalogued rows" idea
noted above -- it would be mixing exactly these two -- and it is the
reason the survivor rule is a rule rather than a tie-break.

### 630 or 632 (once "575 or 632"): decided, 632

`hasUnpreservableDataAtRisk` flags a group when a copy being removed
carries a rating or comment the survivor lacks, and such groups default
to excluded. A stray file has no database row, so it carries neither and
can never *cause* the flag -- but it was caught by it whenever two
catalogued rows in its group disagreed, which on a three-catalog stick
is an ordinary thing for them to do.

**Decided 2026-09-08: the two are decoupled.** Deleting the stray *file*
loses none of the data that flag protects -- the disagreement is between
two catalog rows, and the file is party to neither. The flag still
defaults the group to excluded for the row-level cleanup, unchanged;
it simply no longer holds back the file deletion, which is what makes
the table above read 632 rather than 630. `DuplicateCleanupPlanner`
states this at the point where it splits the strays, so it reads as the
decision it is rather than as an oversight.

The rules that *do* hold a stray back are the two about the group's
identity rather than its data: `differs`, and an estimated duration
anywhere in the group.

## Design

### 1. `TrackMetadataProbe`

A new port beside `TrackDurationProbe`, returning the whole set at once:

```
struct FileMetadata {
    std::string title, artist, album;
    double durationSeconds = 0.0;
    bool durationIsEstimated = false;   // see the safety rule above
    int bitrate = 0;
    int sampleRate = 0;
};
```

`TagLibMetadataProbe` implements it in a new `seabass_taglib` target,
optional at configure time exactly as `Qt6::Multimedia` already is, with
a null implementation when TagLib was not found. `fillMissingDurations`
keeps working off `durationSeconds`, so existing callers are unaffected
-- and get a much faster probe for free.

### 2. Metadata cache

Mirror `DurationCache` exactly: JSON Lines at
`<stickRoot>/.seabass-metadata.jsonl`, one flat object per line, paths
relative to the stick root so a cache written on Linux still reads on
Windows, and an entry trusted only when size **and** mtime both still
match. Same strictness for the same reason: a stale answer here feeds a
destructive caller.

Scanning 2100 files costs 22-66 s cold; with the cache the second scan
of an unchanged stick costs a `stat` per file. Worth having from the
start rather than bolted on later.

### 3. Finding the unreferenced files  *(done)*

Walk `Contents/` via `infrastructure/long_paths.hpp` -- **not**
`fs::recursive_directory_iterator`, whose MAX_PATH limits that header
documents are real and would silently skip deep paths on Windows.
Subtract every path referenced by rekordbox **and** Engine **and**
OneLibrary. Checking a single format would delete files the other
catalog still plays; that is the one bug in this feature that would be
unforgivable, so the "all catalogs" requirement belongs in the API
shape, not in a comment. `CatalogTracks` therefore names the three
catalogs as separate optional fields rather than taking one pre-merged
`vector<Track>`, and the result reports which catalogs it actually
consulted so a partial check reaches the screen instead of dying in a
log. `findUnreferencedFiles()` with no catalog at all returns
`usable = false` rather than "everything is unreferenced".

That shape immediately earned itself: the first run of
`tools/unreferenced_scan` on RV2 consulted only rekordbox and Engine,
and said so in its output -- the stick does have a third catalog
(`PIONEER/rekordbox/exportLibrary.db`, 1644 rows). Wiring OneLibrary in
left the answer at 632 files, so the earlier figure was right, but it
was right by luck until the tool proved it.

Path comparison normalizes separators **and case**. Case matters because
exFAT and NTFS are case-insensitive: a catalog row saying
`Contents/A/b.mp3` and a directory entry saying `Contents/a/b.mp3` are
one physical file, and a case-sensitive comparison would call it
unreferenced and offer it for deletion. Normalizing case can only move a
file from "unreferenced" to "referenced", which is the harmless
direction.

### 4. Grouping

Probe each unreferenced file, build a `domain::Track` with
`format = "disk"`, and feed it into the *existing*
`DuplicateTrackFinder::find()` alongside the catalogued tracks. No new
matching logic.

`DuplicateCleanupPlanner` needs one new rule, and it is subtler than
"never pick an unreferenced file":

> An unreferenced file may be the survivor **only when every copy in the
> group is unreferenced.** If any copy in the group is catalogued, the
> survivor must be a catalogued one.

The reason for both halves: keeping an unreferenced file over a
catalogued one would leave the catalog pointing at a deleted file (we
would have to rewrite the row to point at the survivor -- a different,
larger feature). But a group of *only* unreferenced files is a real
case: several stray copies of a track that fell out of every catalog. We
should still collapse those down to one, and the best copy wins on the
usual bitrate/length rules. The result is a single stray file, which is
then a candidate for re-import rather than for deletion.

### 5. What the DJ sees

Unreferenced files are **offered on the Clean Up Duplicates page
first**, as part of the normal duplicate review, so they can be
deduplicated with everything else and with the same survivor/merge
information in front of the DJ. Only what survives that review -- files
confirmed redundant and not kept -- is routed onward to "Delete
Orphaned Files" for the actual deletion. Not the other way round: the
deletion page is the last step, never the place a file is first
encountered.

`resolvePendingDeletions`' fresh-scan re-check stays the final safety
gate, unchanged in spirit but widened to check every catalog rather than
one format's.

Two things fell out of building it that the plan had not said:

- **Saving a group is now two different acts, and the page says which.**
  A catalogued copy loses its rows; a stray file only gains a manifest
  line. They are counted apart in the staged description and marked
  apart on each copy ("REMOVING" / "FILE ONLY" / "KEPT BACK"), because
  "removes 3 copies" would otherwise cover both.
- **A group of nothing but stray files opens no write session at all.**
  There is no row to remove, no survivor row to merge onto and nothing
  to back up, so it skips the writers entirely and appends to the
  manifest, which is append-per-call precisely so it needs no session.
  Without that guard the survivor's own *path* would have been handed to
  a cleanup writer as a row id.

And one refusal, which is the shape the rest of this area already has:
if a catalog's database is present on the stick but could not be read,
no stray file is listed at all and the page says why. Absent is fine;
unreadable is not, and the two must not collapse into each other.

Files matching nothing are never touched, only listed. Zero of those on
RV2, but on another stick they are real music that fell out of the
database -- re-import candidates, not deletion candidates.

Optional, off by default: a "verify byte-identical" step that content-
hashes an unreferenced file against its catalogued twin, turning
"metadata says duplicate" into certainty. That is what established
598/632 above. It is an 8 GB read over USB, so it belongs behind a
toggle rather than in the default path.

## Order of work

1. ~~`TrackMetadataProbe` port + `TagLibMetadataProbe` + optional CMake
   wiring + unit tests~~ **done**
2. ~~`MetadataCache` mirroring `DurationCache`, with its tests~~ **done**
3. ~~Unreferenced-file walk (all three catalogs), with tests~~ **done** --
   verified on RV2 by `tools/unreferenced_scan`, which reads all three
   catalogs with the project's own readers and reproduces the 632 files /
   8.66 GB above, path for path, against the earlier analysis
4. ~~`DuplicateCleanupPlanner` survivor rule + the estimated-duration
   exclusion, with tests for both~~ **done** -- plus
   `unreferencedFilesAsTracks()`, the probe-and-cache step that turns
   walked files into `domain::Track`s so they can go through the
   existing `DuplicateTrackFinder`. Two fields carry the rules:
   `Track::isUnreferenced` and `Track::durationIsEstimated`. Re-running
   the RV2 simulation against the shipped rules reproduces this
   document's numbers: 632 files / 8.66 GB proposed, 9 groups where the
   survivor rule forced a catalogued copy over a better stray, 0 groups
   of only strays, 0 held back
5. ~~Clean Up Duplicates page shows unreferenced files in the review~~
   **done** -- `scanStrayFiles()` (`infrastructure/cleanup/`) composes
   walk + `findUnreferencedFiles` + probe + cache the way
   `audio/duration_fill.hpp` composes the duration probe, and the Clean
   Up scan runs it against *every* catalog on the stick before grouping
   the result with the current format's rows. `UnreferencedFilesNotice`
   says what was found and what it was checked against; the group rows
   mark which copies are files rather than rows, and which are held back
6. ~~Route confirmed-redundant ones to Delete Orphaned Files~~ **done**
   -- a stray file is already orphaned, so saving appends it to
   `.seabass-pending-deletions.jsonl` and touches no catalog at all.
   The deletion itself still happens only there, behind
   `resolvePendingDeletions()`' fresh all-catalog re-check
7. ~~Live run on RV2 against the numbers in this document~~ **done** --
   `tests/stray_scan_live_test.cpp`, run with `SEABASS_LIVE_STICK` (and
   optionally `SEABASS_LIVE_EXPECT_*` to pin a known stick's figures);
   it skips, passing, without a stick, so it lives in the normal suite
   rather than in a checklist. On RV2 it reproduces this document:
   632 files / 8.66 GB found and all 632 proposed, 435 groups with a
   stray, 0 of them all-stray, 0 held back, and -- checked independently
   of the code that produced the list -- not one of them referenced by
   any catalog. Cold scan 3.4 s (cache deleted, stick remounted), warm
   0.2 s. The 22-66 s in "Reading the metadata" was for probing *every*
   file on the stick; only the 632 unreferenced ones are probed here.
   It measures once per catalog, because that is what the page composes
   (one format's rows plus the strays), and once over every catalog at
   once for the "what is on this stick" answer; it fills in missing
   lengths first, exactly as `LibraryCatalogCache` does, since without
   that step Engine's 1213 length-less rows would make it measure a
   different library than the app sees
