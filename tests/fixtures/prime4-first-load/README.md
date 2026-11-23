# What a Prime 4 does to a rekordbox-import row on its first load

Captured 2026-11-23 on a Prime 4 running Engine OS 5.3.0, from one row of a
test stick's `Engine Library/Database2/m.db`: an MP3 at 44.1 kHz that
Engine's rekordbox import had made and no player had loaded
(`PerformanceData.thirdPartySourceId = 1`), with three hot cues (pads 2, 3
and 4), one loop (slot 1) and no main cue. Seabass had not written the row.

| File | Column | When |
|---|---|---|
| `marked-quickCues.bin` | `quickCues` | before the load, mark set |
| `marked-loops.bin` | `loops` | before the load, mark set |
| `loaded-quickCues.bin` | `quickCues` | after the track was loaded once |
| `loaded-loops.bin` | `loops` | after the track was loaded once |

The bytes are the columns as stored. They hold positions, the labels
"Cue 2" to "Cue 4" and "Loop 1" and pad colours, nothing of the track
itself.

The file's lead-in, as `mp3_lead_in_dump` reads it: an Info frame with a
"Lavc" tag, encoder delay 576, padding 1261, the Info frame's channel mode
not the audio's, MPEG-1 at 44.1 kHz. Seabass's model gives that file a
shift of 1105 samples (25.06 ms), not the 2292 the player moved it by.
Which of the two puts the cue on the sound is not settled by this capture:
the test uses the capture for how a row is moved (what moves, and that it
all moves alike), with a lead-in made up to give 2292, and says nothing
about the amount.

What the player did, and what
`libdjinterop_engine_analysis_import_test` holds Seabass to:

- every hot cue 2292 samples earlier;
- both ends of the loop 2292 samples earlier, the same amount;
- the main cue, which was not set (0), left at 0;
- labels, colours and empty slots as they were;
- the mark taken off (read from the column after the load; none of the
  four files holds it).
