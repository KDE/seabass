<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# Rebuilding a library from a stick and a pile of files

`tools/library-build/build_library.py` assembles one clean library out of two
things that disagree: a stick's catalogs, and a folder of audio files that has
no catalog at all. It is the other half of `seabass-cli export-xml` — that
command says what the catalogs hold, this one decides where the files should
live — and it is **a prototype on purpose**. See [v2](#v2) for what should
become C++.

```
seabass-cli export-xml --rekordbox /Volumes/STICK/PIONEER \
    --engine "/Volumes/STICK/Engine Library" --prefer engine --out catalog.xml

python3 tools/library-build/build_library.py \
    --catalog-xml catalog.xml \
    --extra-root ~/Music/SomeFolder \
    --out ~/Music/MyLibrary            # reports only

python3 tools/library-build/build_library.py ... --apply --move-local
```

Without `--apply` it writes reports and moves nothing. That is the point: every
decision below is one a human should look at once before 40 GB of music is
rearranged.

## What it produces

```
MyLibrary/
  Tracks/<Artist>/<Artist> - <Title>.mp3      what rekordbox imports
  Lossless/<Artist>/<Artist> - <Title>.flac   the archive, not in the XML
  reports/*.tsv
```

| Report | What is in it |
|---|---|
| `plan.tsv` | every file, where it would go, and as what (`library`, `lossless-archive`, `transcode`) |
| `duplicates.tsv` | every copy not needed, what it lost to, and whether it was byte-identical |
| `weak-matches.tsv` | groups joined on a looser key than artist+title+length — **the review list** |
| `playlists.tsv`, `playlist-merges.tsv` | playlists found, and names that look like the same list twice |
| `artwork.tsv` | tracks with no embedded cover art |
| `corrupt-recordings.tsv` | recordings with no working copy anywhere, as artist/title |
| `unmatched.tsv` | everything skipped, with the reason |

## The decisions, and why they are what they are

**Same recording = artist + title + length (±2 s).** Tags first, because a
filename carries an export's playlist numbering (`079_Artist-Title.mp3`) and a
tag does not; the filename stem, minus that numbering, is the fallback for files
whose tags are empty. The tolerance is `DuplicateTrackFinder`'s, for the same
reason: stored lengths disagree slightly across formats.

**A second, looser pass, reported rather than trusted.** Two things defeat the
strict key on real files: a copy with *no* tags falls back to its filename and
can never match a tagged one, and a title carrying `(Extended Mix)` on one side
only splits a recording in two. Measured on one real library, 2 of 158 archived
FLACs had an mp3 twin the strict key missed. The second pass joins those on
title-without-parentheses plus length, and writes every join it makes to
`weak-matches.tsv` — because a looser key is also exactly how two different
mixes of one title get merged by accident.

Both failure modes of the strict key *under*-merge: an extra copy is kept, or a
FLAC is wrongly called lossless-only. Neither can delete the wrong file, which
is the safe direction for a matcher to be wrong in.

**A file that will not parse is not a copy of anything.** In the library this
was written for, 114 files were the right size and entirely zero bytes — and
109 of them were the only copy of their recording. Ranking copies by quality
and keeping the best available would have filled the library with silence that
looks like music until a deck refuses it. Unreadable copies can never be the
keeper; a recording with no usable copy at all goes to
`corrupt-recordings.tsv` as artist/title, because the next step for it is
buying or re-ripping, not file surgery.

**One spelling per artist.** The folder a track lands in follows whichever copy
won, so `Balthazar, JackRock` and `Balthazar, Jackrock` end up as two
directories differing only in case — untidy on a case-insensitive filesystem,
two genuinely separate folders on the exFAT stick the library is destined for.
The most common spelling wins, across both `Tracks/` and `Lossless/`.

**FLAC is archived, not catalogued — unless it is all there is.** An XDJ-RX2
cannot play FLAC, so the lossless copies sit in `Lossless/` and stay out of the
rekordbox XML. A recording that exists *only* as FLAC would then be missing from
the library entirely, so it is transcoded to MP3 320 CBR
(`flac -d -c | lame -b 320 --cbr`), tags and cover art carried across, and the
result is collected in one playlist named `from-flac` so what was generated
stays obvious and can be re-made if the encoder settings change. CBR rather
than V0 because a 2017 player's seek is happiest with one.

**Move what is already here, copy what is not.** `--move-local` renames files
that already sit on the library's own volume, which costs no space and is
instant; anything on the stick is copied, because the stick is not ours to
empty. Copies land on a `.partial` name first, so an interrupted run does not
leave something that looks like a finished track. Nothing is ever removed from a
source that was not moved into the library.

**Path components are exFAT-safe.** The characters exFAT refuses are replaced,
and a trailing dot or space is stripped — Windows drops those silently and then
cannot find the file again. Components are capped well under the 255-unit limit
so a disambiguating ` (2)` still fits.

## Known limits

- Playlist *merging* is proposed, never performed: `playlist-merges.tsv` lists
  the candidates ("Peaktime" / "Whaleshark Peaktime") and a human decides.
- Cover art is reported as missing but not yet filled in from the Engine
  `AlbumArt` table or `PIONEER/Artwork/`, which is where the art usually still
  is.
- Cue points are not the script's business at all — they live in the XML the
  export command writes, keyed by `Location`. Re-run `export-xml` with
  `--map <old prefix>=<new prefix>` after the files have moved.

## v2

The detection this leans on is already C++ and already tested — matching
(`domain::matchTracks`), duplicates (`DuplicateTrackFinder`,
`DuplicateCleanupPlanner`), junk cues (`domain::isJunkMemoryCue`). What has no
home yet is the part above: choosing a destination for a file, choosing between
copies, and placing them. That belongs beside the rest as a use case with a
`LibraryLayout` port, at which point:

- the GUI can show the same plan and the same review lists before anything moves;
- `weak-matches.tsv` becomes a list you tick, like the sync page's conflicts;
- the transcode becomes a job with progress, cancellation and a real failure path;
- and the artwork gap closes, because the Engine and pdb artwork readers are
  right there.
