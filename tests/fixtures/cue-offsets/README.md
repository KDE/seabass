# Cues a few milliseconds off (#121)

A whole small stick in `stick/`: rekordbox's own export (export.pdb,
exportLibrary.db and the analysis files) and a Prime 4's own Engine analysis
of seven click tracks (60 s, a click every 0.5 s from 1.000 s), one per kind of
file. Built 23 November 2026 by `make_fixture.py` (kept with the project
notes) from the 8 November click-track stick copy, with pad 1 of each Engine
row put into one of the states found on a real stick.

Cut down for the repository: `beats-ref-44100.wav` (10 MB) is left out
(regenerate it with `tools/mp3-offset-probe/make-beat-click-mp3s.py`; the WAV
row is never judged, it has no lead-in), and `PIONEER/USBANLZ` keeps only the
seven click tracks' analysis folders. The other catalog rows point at files
that are not here, as on any stick whose files were moved.

rekordbox's side is untouched, and it is a measurement: rekordbox's first
beat against Engine's own agrees with Seabass's lead-in model within 1.2 ms
on all six MP3 classes.

## The rows (pad 1 / rekordbox pad A sits on the first click)

| File | Right shift | State of the Engine row | A repair must |
|---|---|---|---|
| beats-ref-44100.wav | 0 | control | change nothing |
| beats-A-notag-44100.mp3 | 0 | import mark set, never loaded | change nothing |
| beats-C-badcrc-44100.mp3 | 51.179 ms | correct | change nothing |
| beats-D-lame-48000.mp3 | 47.021 ms | same number as rekordbox; log: copied rekordbox to Engine | move Engine to 998.979 ms |
| beats-B-noencoder-44100.mp3 | 26.122 ms | same number as rekordbox; log: copied Engine to rekordbox | move rekordbox (and OneLibrary) to 1076.122 ms |
| beats-B-lavc-44100.mp3 | 25.057 ms | moved 2292 samples by the player | move Engine to 998.943 ms |
| beats-D-lame-44100.mp3 | 51.179 ms | same number as rekordbox, no log line | list it, change nothing |

`stick/expected.json` carries the same in numbers. `stick/Seabass/seabass.log`
holds the two log lines.

The rows hold more than pad 1, from the stick they were cut from: every
rekordbox row a memory cue on pad A's click, Engine's D-lame-48000 row a loop
on slot 2, rekordbox's Lavc and D-lame-44100 rows a pad 2 Engine lacks. Sync
Cue Points has things to copy for most rows because of them; pad 1 is never
one of them.

### The player's shift on a file without a tag (229 samples)

Measured on the Prime 4 on 23 November: the first load of an import-marked
row moves its hot cues and loops by 2292 samples when the file has a valid
encoder tag (FFmpeg's, or LAME's with a correct CRC), by 229 when it has none
(no Info frame, an Info frame without an encoder string, a LAME tag whose CRC
is wrong), and not at all on a WAV. The stick pins 2292 on the Lavc row; the
229 case is made by the test on a copy, from the no-tag row:

| File | Right shift | State made | A repair must |
|---|---|---|---|
| beats-A-notag-44100.mp3 | 0 | mark gone, pad 1 at 1024 ms less 229 samples (1018.807 ms) | move Engine to 1024.000 ms |

## Tests (`cue_offset_repair_test`, one ctest per case)

1. `classify`: each of the seven rows gets the state above; the three "change
   nothing" rows are not listed at all.
2. `repair`: on a copy, Move All Known and Save. The three moved cues sit
   within one sample of `moveTo_ms` on Engine, within one millisecond on
   rekordbox and OneLibrary (rekordbox keeps whole milliseconds); the other
   four rows' Engine rows and analysis files are byte for byte as they were;
   the save backed up first; a second scan lists only the unknown row; Undo
   puts m.db back.
3. `unknown`: with the log removed, the two logged rows are unknown too and
   nothing moves them on its own; Move All Known moves only the player's row;
   the DJ's pick of a side moves one.
4. `player`: the player's shift is recognised only on a row whose mark is
   gone, only within one sample, and only the shift the file's tag gets:
   2292 on the Lavc file and 229 on the untagged one, never the other way
   round. The 229 case above is moved back onto rekordbox's number.
5. `loops`: a loop slot (all three states) and a memory cue (the two copy
   states; the player was not measured moving a set cue point) move with the
   hot cue.
6. `order`: a sync and a repair of one pair, staged on two Sync Cue Points
   pages over one library and saved together, either first. The second
   writes nothing (its cues are not what it read; the save says so), is not
   counted as synced, both pages read the stick again, and Undo after that
   save takes back what the first wrote.
   `handles`: the check before a write reads Engine on a connection of its
   own and closes it: mid-save, nothing holds m.db.
7. `mixed`: one track with a cue the player moved and one at the same
   number with no log line. The known one moves on Engine whatever the DJ
   picks; the unknown one moves only on the pick.
8. `pairing`: Engine's pad 3 at rekordbox pad 1's number is not pad 1's
   copy: a hot cue pairs only with its own pad.
9. `sync`: Sync Cue Points calls pad 1 the same cue on all seven rows (half a
   beat at 120 BPM is 250 ms) and never makes it a choice: the repair owns
   these, not Sync's tolerance.
