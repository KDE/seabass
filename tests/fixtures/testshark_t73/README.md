<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC0-1.0
-->

# TESTSHARK, #73 session B (2026-11-13)

What rekordbox and the Denon players wrote onto the test stick TESTSHARK
during the #73 hardware round of 2026-11-13 (Sync after Rekordbox Export
after a second rekordbox export), so the checks done by hand that evening
run as `testshark_t73_test`. Nothing here was edited: every database,
analysis file and baseline is the stick's own bytes, copied off it after
each step. The audio is cut down, see below.

- Stick: TESTSHARK (FAT32), formatted by Seabass/1.0 at 98ff3458.
- rekordbox 7.2.18 on macOS (neo) wrote PIONEER/, exports 40 and 128.
- Seabass/1.0 at 98ff3458 created the Engine library (3.x) and made the
  saves; master had the same code for these paths.
- A Denon Prime 4 analysed tracks after each save; a Prime Go+ last.

The session's own notes and every verdict are in the private project
repository (evidence/session-b-73-2026-11-13/README.md).

## The steps, as the files name them

| step | what happened |
|------|---------------|
| S2   | rekordbox export 40: "T73 Rename Me" B01-B04, "T73 Delete Me" B05-B06, "T73 Order" B07-B12 |
| S3   | Seabass Create Engine Library, baseline recorded at export 40 |
| S4   | the Prime 4 analysed B04 and B11 |
| R3a/b | rekordbox export 128: "T73 Rename Me" renamed "T73 Renamed" (now B01-B04, B12), "T73 Delete Me" deleted, B10 taken out of "T73 Order", B08 moved to its end, B05 B06 B09 B10 gone from the stick (files too), N1 N2 N3 added in "T73 Folder/T73 New", B01 rated 4 stars with comment "T73 comment" |
| R3c  | Seabass saved the 24-row proposal (save 1), undid it, saved it again (save 2) |
| R3d  | the Prime 4 analysed N1, N2 and B11 |
| R1b  | the Prime Go+ played N1 and N3 (and analysed N3) |

## Files

| file | what it is |
|------|------------|
| `stick/PIONEER/` | the stick's PIONEER folder as copied after R1b: rekordbox's export 128, which no save changed (the proposal had nothing for rekordbox) |
| `stick/Contents/` | the eleven audio files on the stick at export 128, as heads (below) |
| `engine/m-before-R3.db` | Engine's m.db after S4, before save 1: the input of the proposal (byte-identical to m.db in Seabass's own backup taken by save 1) |
| `engine/m-after-R3c-save1.db` | m.db after save 1, as Seabass/1.0 wrote it on the stick |
| `engine/m-after-R3d.db` | m.db after save 2 and the Prime 4's analysis of N1, N2, B11 |
| `engine/m-after-R1b.db` | m.db after the Prime Go+ |
| `baseline/rekordbox-baseline-export40.tsv.gz` | Seabass/rekordbox-baseline.tsv.gz before save 1 (from save 1's backup) |
| `baseline/rekordbox-baseline-export128.tsv.gz` | the same file after save 2 |
| `expected/R3b-proposal-final.txt` | `seabass-cli sync-after-export` at export 128 against the baseline at 40: 24 changes, 0 conflicts |

Left out, because nothing reads them for these checks and they are big or
personal: the `.2EX` waveform files in USBANLZ, `djprofile.nxs`,
`extracted/`, the Engine library's hm.db, sm.db, stm.db and OverviewData,
and the macOS `._` files.

## The audio

The click files are 180 s (B) and 240 s (N) long: 130 MB together, too
much for the repository. What the code reads from them is their headers:
the planner needs the file to exist (an add whose file is missing is
refused), adding a track to Engine needs its sample rate (TagLib), and
Engine's cues on an MP3 need its lead-in (the Info frame's LAME tag). So
each file is cut down to its head by `make-audio-heads.py`, run on the
originals (sha256 in the project's prep folder):

- FLAC: the metadata blocks and 4 KB of frames. STREAMINFO keeps the
  whole sample count, so TagLib still reads 180 or 240 s.
- AIFF: every chunk, SSND cut to 4 KB of sound with the chunk and FORM
  sizes fixed. COMM keeps the whole frame count.
- MP3: the first 32 KB (ID3, the Info frame with its frame count and LAME
  tag, the first frames).

`testshark_t73_test` checks that TagLib reads every head as the stick's
metadata cache recorded the whole file (rate, length, MP3 lead-in), and on
2026-11-13 `seabass-cli sync-after-export` printed the same proposal,
byte for byte, over the heads as over the whole files.

The originals: 10 ms clicks on every beat from 0:00.000, made with ffmpeg.
B01-B12 at 100 + 2i BPM, 180 s, B04 and B09 MP3 320 kbit/s, B06 and B11
AIFF 48 kHz, the rest FLAC 44.1 kHz; N1 FLAC 48 kHz 120 BPM, N2 AIFF
48 kHz 125 BPM, N3 FLAC 44.1 kHz 128 BPM, 240 s.
