<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Experimental features

Features gated behind `AppSettingsController::experimentalFeaturesEnabled`
(Preferences → More settings → "Enable experimental features"). On by
default on master and on the release train (`Seabass/X.Y`) alike: the test
releases of a train ship with it on, so testers see what is being built
(Sebastian, 2026-11-23), and 1.0 shipped with it on as well. Nothing turns
it off for a release; that would be the default in `AppSettingsController`'s
constructor, changed on the train by hand. A setting stored by an earlier
install wins over the default. See that property's own doc comment and
`ActionCard.qml`'s `experimental` property for how the gate works, and the
`SEABASS_EXPERIMENTAL` CMake option for the build-time switch: with it off
the toggle is compiled out and `experimentalFeaturesEnabled` is a constant
false, so every experimental feature is hidden. It compiles out nothing
else (QML has no conditional compilation), so a graduated feature is in
such a build like any other.

New non-trivial features default to this list. Move an entry to "Graduated
to stable" (and drop `experimental: true` from its `ActionCard` and the
`ExperimentalBadge` from its page) once it's
seen real, successful use — most importantly, an actual write/apply path
exercised live against real hardware, not just a read-only scan. A
graduated entry keeps whatever checks it still owes, written as "Still
owed": graduating says the feature is for everyone, not that every
platform has been through it.

Deliberately *not* gated: edit mode, the per-library edit lock,
cancellation, and the process guard (`docs/edit-mode-and-cancel.md`).
They exist to keep the user's data safe, so they are on for everyone
from day one.

## Currently experimental

- **Track import** (added 2026-11-25) — audio files and folders from
  the computer put onto the stick as tracks, without rekordbox. Ways in,
  all in Browse and all shown only with Experimental on: a playlist's
  menu ("Import tracks...", "Import a folder..."), files dropped on a
  playlist row, on "All tracks" (no playlist), on the empty part of the
  playlist list or its last row (a new playlist, named in a dialog), and
  on the track list (the playlist shown, at the row dropped before when
  the list is in the playlist's own order). `TrackImportController`
  reads, plans and rehearses the batch off the GUI thread and stages it
  into the stick's session; Save copies each file, verified, and writes a
  row into every library the stick has (`ImportTrackChange`: export.pdb,
  exportLibrary.db, Engine's m.db), then the playlist entries. One batch
  at a time, into a session with nothing else staged. Before anything is
  staged, Seabass analyses each file with its own analyser
  (`lib/musicanalyze`): the beat grid, the waveforms and, for a file
  whose tags name none, the key. The save writes them as rekordbox's
  three analysis files (.DAT, .EXT, .2EX) named in both Pioneer rows, and
  into Engine's row, which is then marked analysed. The cover in the
  file's tags goes into every library. The plan counts the room all of
  that takes on the stick (`domain::importSideWrites`), not only the
  audio. Switching Experimental off stages nothing new; what is staged
  already stays, for Save or to be taken back.

  Until Save the staged tracks show in the track list, read from the
  staged changes themselves (`TrackImportController::stagedImport`): in
  "All tracks" at its end and in the playlist at the place the save gives
  them, greyed and in italics as a staged add is, marked "new", with
  their tags, the analysis's tempo and key, and the file's cover. They do
  not play, open their details, drag or take part in Merge, Find matching
  or Add to until saved, and say so. "Don't import" in a row's menu takes
  that one track back with its playlist entry; the rest of the batch
  keeps its place, and the last row takes the batch back whole, the new
  playlist with it. The preparing has one bar from the drop to what is
  staged (`ImportBar`), each step a stretch as long as its planned work,
  the steps with their counts in the strip's panel. Escape stops it from
  anywhere on the page but a text field, the open strip (where it closes
  the panel) and a dialog or menu.

  **Analyse again** (added 2026-11-26), in a playlist's menu: every track
  of the playlist analysed again from its audio file on the stick, one
  change staged per track (`ReanalyseTrackChange`). The new grid and
  waveforms replace the old ones; everything else in the analysis files
  stays as it was, and a key a row has is never replaced. A track is left
  alone, with its reason, when its file is missing or not the size its
  rows say, when the analysis finds no grid, and on Engine when the
  player's grid was locked or moved by hand. Cue points and loops sit on
  beats of the old grid, so the save can move them onto the new one. A
  review lists each track whose cues would move and by how much, and a
  switch per track says whether they move. Tracks whose every move is
  under a threshold (10 ms unless changed, 0 to 100, kept in the
  settings) start switched on; the rest start off and wait for the
  person. A switch set by hand stays as set when the threshold changes.
  A track whose grid changed too much to carry its cues keeps them where
  they are.

  **Keeping the original, and Restore original analysis** (added
  2026-11-26): before Analyse again first replaces a track's analysis
  that rekordbox made, it keeps it on the stick
  (`Seabass/rekordbox-analysis/`, one file per track): the track's .DAT,
  .EXT and .2EX as they were, and the tempo and key of its rows in
  export.pdb and exportLibrary.db. Engine's row is kept the same way it
  always was (`Seabass/engine-analysis/`). Both are written by the save,
  so a save that goes back and Undo Last Save leave them as they were.
  Beside each kept file is its record (`<name>.seabass.tsv`), small and
  written again by every save that adds to it, before the analysis files
  are written: every grid Seabass wrote over the original since, whether
  Analyse again also kept the player's analysis, and whether the original
  is rekordbox's or, where Seabass's record of its own analyses had been
  lost, only "the analysis that was on the stick before". The kept file
  and its record alone tell Seabass's grids from one changed since, for
  their track, whatever became of the analysis ledger. A grid Seabass
  wrote itself (a record lists it, or any entry of the analysis ledger
  has it) is nothing to keep, and a track only Seabass ever analysed (an
  imported one) has nothing kept. A kept original is never replaced:
  analysing again leaves the first original in place, and a track whose
  grid is neither the kept one nor Seabass's (rekordbox analysed it anew,
  or the DJ set its grid, and it was exported over the stick) is left
  alone, and Restore leaves it alone too: removing the kept original in
  Free Up Space lets Seabass analyse it again, which discards the
  original Seabass kept. Analyse again decides this on every analysis
  file it writes (DeviceLibrary's and OneLibrary's rows may name two),
  each against its own kept original, and leaves the track alone when
  its rows name other analysis files than the ones the original was kept
  from (a new export moved them): written, the kept original would have
  no way back. A track is left as it is, with the reason in the log and
  the save's summary, when its audio file's size cannot be read or is not
  the size it was analysed at, when the analysis ledger cannot be read
  and the grid is not known, when its kept original has no record that
  can be read, and when the stick has no room for what is to be kept. A
  ledger moved aside (damaged, or reset from the command line) is
  followed by one that says it is incomplete; a track it does not list
  then has what is there kept as the analysis that was on the stick
  before, its maker unknown. Library Health's relink of a moved file
  carries what is kept, and both ledgers' lines, to the track's new name,
  as far as it can: it never fails for them, and a kept original already
  under the new name stays there. An entry it writes under the new name
  whose record cannot then be written is taken away again, and the old
  pair stays. A relink with nothing else in the save leaves an analysis
  ledger or beat grid ledger it cannot believe (damaged, unreadable)
  exactly as it is: nothing replaced, nothing moved aside, no warning,
  a line in the log. Where the analysis ledger cannot be read, the
  command line moves it aside, deleting nothing:
  `seabass-cli analysis-ledger --reset`, with the stick plugged in and
  Seabass closed (it finds the stick itself; `--rekordbox PATH` or
  `--engine PATH` names a library on it, as for the other commands). The
  save's warning and summary name the tool and point to this page for the
  command, which they cannot quote.

  Seabass can take an earlier analysis of its own for rekordbox's in
  three cases: writing the analysis ledger failed after an import (the
  save says so); Analyse again ran on a track that had nothing to keep;
  or the stick's Seabass folder was deleted, or the stick copied without
  it. A later Analyse again then keeps that earlier analysis of Seabass's
  as the original, and Restore puts it back rather than rekordbox's. No
  data is lost: rekordbox's own analysis was no longer on the stick in
  any of the three, and what Restore puts back is what was there before.

  Restore original analysis and Free Up Space's list of what is kept are
  not behind the Experimental switch: with the switch off they still
  show, but only when something is kept on the stick, since they are the
  way back from Analyse again. With nothing kept, neither shows at all.

  "Restore original analysis", in a playlist's menu, puts back what is
  kept for each of its tracks, in every library that has something kept:
  the analysis files as they were, except for their cue lists, which stay
  as they are now (cue points set since are kept); a .2EX that Analyse
  again added is taken away again; every row that names the file gets the
  old tempo and key of its library, the key by its name (a new export can
  give the kept key id to another key), and a library whose rows were
  renumbered by a new export keeps its tempo and key, which the log says;
  Engine's row its own analysis where Analyse again kept it (one Beat
  Grids & Waveforms kept is put back there). The track is then no longer Seabass's
  analysis in the ledger, and the kept copy is removed in the same save
  (Undo brings it back). Cue points that Analyse again moved with the grid
  can be moved back with it, reviewed per track like Analyse again's, the
  switch on by itself only where every move is under the same threshold.
  A track is left as it is, with the reason, when its audio file is not
  the size it was, when its rows name other analysis files than the ones
  its original was kept from (the kept original then has no way back and
  can only be removed in Free Up Space), when its record is missing or
  damaged, or when its grid is neither the original nor one Seabass
  wrote: Seabass never writes over that; such a track is left out when
  the restore is prepared, under "Left as they are" with that reason, and
  never staged. The playlist menu's number counts what is kept by name
  alone, so it can include a track the restore then leaves out. Cue points follow the grid of their own
  library: where a library's grid goes back, the cue points Analyse
  again moved there go back, only where they are still exactly where it
  put them and never onto another cue; where Engine keeps the grid it
  has (the player's analysis kept is Copy Beat Grids', or none was
  kept), its cue points stay on it. The libraries may then hold
  different cue points, as they hold different grids, and the save's
  summary says which library went back, which stayed and why, and in
  each library whether the moved cue points went back, stayed because
  they are no longer where Analyse again put them, or stayed off the
  restored grid because they were not moved back: one note per outcome
  with the number of tracks, each track's in the log. With the switch
  on, a track whose cue points cannot go back (DeviceLibrary and Device
  Library Plus hold different ones, two rows of one library do, or an
  analysis file a row names cannot be read) is refused whole, nothing
  written and what is kept kept, and the summary says why and that
  restoring it with its switch off puts the grid back without them. The
  review already shows the first two on the track, its switch off and
  disabled. After a restore where Engine keeps the grid Copy Beat Grids'
  analysis is kept for, Engine's moved cue points do not follow a later
  Restore Engine's Analysis, and the summary says so. What is kept is removed only for what went
  back: the player's analysis kept by Copy Beat Grids, and its beat grid
  ledger line, stay for Restore Engine's Analysis. `seabass-cli kept-analysis` counts what
  is kept on a stick.

  Free Up Space lists what is kept, with its size: per track about 235 kB
  for rekordbox's (measured on a rekordbox 7 collection, median; up to
  about 320 kB) and 2 to 8 kB for Engine's. Removing it deletes the grids
  and waveforms rekordbox or the player made for those tracks for good;
  Seabass's analysis stays and the tracks play as they do now, but
  Restore original analysis (and, for the player's, Restore Engine's
  Analysis) is no longer possible for them. What is kept for tracks not
  on the stick under that name any more is listed apart, unticked like
  the rest: deleting is always the person's choice.

  **The analysis ledger** (added 2026-11-26): after a save that wrote an
  analysis of Seabass's, by the import or by Analyse again, the stick's
  `Seabass/analysis.tsv.gz` records for each track which revisions of the
  analyser made it, from which file and with which tempo range, and the
  grid each library was left with. A later Seabass can tell from it a
  track an older analyser made, which it may improve, from one whose grid
  was changed since by hand or on a player, which it leaves alone. A save
  that goes back, and Undo Last Save, leave the ledger as it was.

  Verified with this app's own readers and, for export.pdb's rows, byte
  for byte against rekordbox's (`pdb_row_encoding_test`,
  `import_track_change_test`, `track_import_controller_test`); no player
  has loaded an imported or analysed-again track. Promote once one has on
  each family (XDJ-RX2, OMNIS-DUO, Prime 4), with the grid on the beat and
  the waveform drawn, and rekordbox has opened the stick. Still owed
  besides: a WAV's file type measured from a rekordbox export (11 is the
  published number), and cues moved by Analyse again checked on a player.

- **Save as FLAC** (added 2026-11-26 as "Keep this recording", #147, Own
  Recordings step 3) — on Clean Up Recordings, a WAV the DJ hardware
  recorded onto the stick becomes a FLAC with artist, track name, album,
  genre, year and a front cover in a folder on this computer ("Save as
  FLAC..." on the row, a form, then the passive progress strip: encoding,
  checking, tags and cover, saving, with Cancel and a queue). Browse's
  Own Recordings row (2026-11-27) offers it on a recording's menu too,
  with the same form (`KeepRecordingForm.qml`), the same strip
  (`KeepRecordingProgress.qml`, in the sidebar) and the same controller,
  and the row says how its save fares as the Recordings page's does; the
  menu's other entry, "Show on the Recordings page", opens Clean Up
  Recordings at that recording, where deleting stays. `KeepRecordingController` runs
  `application::keepRecording` on a worker with the Qt Multimedia encoder
  (the FLAC decoded again and compared with the WAV sample for sample),
  the QImage cover preparer and the TagLib tag writer. The FLAC is encoded
  to a temporary file, checked, tagged in place and checked again (tags,
  cover, STREAMINFO and the audio frames byte for byte), and only then
  given its name by a rename that never replaces: nothing under the
  target's name is ever replaced; a failure or a cancel before that leaves
  no file in the folder, a cancel after it keeps the FLAC; the recording
  on the stick is only read and never deleted here. A recording whose WAV header was not completed (a data size of 0
  or 0xFFFFFFFF in a plain RIFF, or more than the file holds) is marked
  on its row with the length the file holds, and the length the header
  declares where it declares one, and saved only when "Keep the audio the
  file holds" is ticked: every whole frame the file holds, the WAV never
  repaired. A plain RIFF over 4 GB without a size is such a recording; a
  declared length over 100 hours (or an RF64 one over a thousand times
  what the file holds) is said to be not usable rather than printed. A header that declares less than the file holds, or a finished
  file followed by more than a second of zero bytes or bytes it does not
  know, is refused with both lengths; RF64 takes its size from ds64 only.
  A WAV's header is read for this only with Experimental on: then the
  listing opens each WAV (its header, the chunks after the audio, and at
  most a second of audio's worth of zero bytes after it) for the rows'
  header states, and lengths are cut to whole seconds as the texts of a
  kept job count them. With Experimental off the page is the released one:
  no header is read, the rows say nothing of one, the length is rounded to
  the nearest second, and nothing is offered to save. The keep controller
  lives with the window, not the page: the work goes on when the page is
  left and its strip is there again when the page is opened again; taking
  out the stick a recording being kept (or waiting) is on cancels the work.
  Leftover temporary files (`*.seabass-encode-tmp`) older than a day are
  swept from the target folder at the start of a job. A file a job leaves
  in the folder (a temporary file that could not be removed, or a second
  name of a kept FLAC that a rename through a hard link left) is named in
  the strip with its path. Verified with synthetic WAVs
  (`keep_recording_test`, `keep_recording_controller_test`,
  `recording_encoder_test`, `recording_tag_writer_test`,
  `local_keep_recording_files_test`); no real recording has been kept
  yet. Promote once Sebastian has kept real recordings of each kind
  (Engine OS 16 and 24 bit, an unfinished one, a Pioneer REC file)
  and a Bluesound player has shown the cover. "Add to the library" on a
  recording's menu in Browse (2026-11-27) imports the FLAC Save as FLAC
  made, while it is there (never the WAV), through the track import into
  the stick playlist "Own Recordings", at its top, made in every library
  at the first add; a playlist of that name already on the stick is that
  playlist. Browse's fixed row Own Recordings is then that playlist: its
  tracks as any playlist's (staged ones before Save included), the
  recordings after them, the playlist not listed a second time; a WAV
  whose FLAC is in it (as long to the sample at the same rate) is marked
  "In the library as FLAC". Files dropped on the row or its list and
  tracks dragged onto the row go into that playlist (made first when the
  stick has none), a track taken out of the row leaves the playlist only,
  the row has no playlist menu (no rename, no delete) and its summary is
  the playlist's. Verified on a scratch stick through Save in
  all three libraries (`tst_OwnRecordings`). Still owed: Free Up Space
  naming a recording that is in the library as FLAC, and Windows and
  macOS.

- **Moved files: Point Library at File** (added 2026-11-19, #103) — the
  second repair on Library Health's Moved files page (`MovedFilesPage.qml`,
  `RelinkMovedFileChange`): every catalog's row for a track whose file
  moved on the stick is pointed at the file where it is now, instead of
  the file being moved back. Engine and OneLibrary are plain updates;
  export.pdb is rewritten in place, field by field, which is the least
  proven write path in Seabass, and it takes only a new path and file
  name of exactly the old lengths. Verified with this app's own readers
  and byte for byte through the generated parser
  (`moved_file_change_test`); no player has browsed a relinked track yet.
  Promote once one has, on a stick whose export.pdb was relinked. Moving
  the file back (repair A) writes no catalog and is not gated.

- **Beat grid and waveform conversion** (added 2026-11-19) — rekordbox's
  beat grid (PQTZ) and waveform preview (PWV6, or PWV4 for an export
  without a .2EX) converted into Engine's beat data, overview and
  overview file, written onto the stick's Engine tracks. Two ways in: the
  Beat Grids & Waveforms page (its card is `experimental: true`, the page
  carries the `ExperimentalBadge`), and the two Preferences switches
  "Copy beat grids/waveforms from rekordbox to Engine when syncing"
  (shown only with Experimental on), which Sync after Rekordbox Export
  and Sync Cue Points obey for the tracks they write to Engine and Create
  Engine Library for every track it creates. With Experimental off the
  switches do nothing anywhere, whatever is stored
  (`application::SyncAnalysisPolicy` reads both off); the stored choice
  comes back with it. It replaces analysis a Denon player made: before a
  row the player analysed changes, its own analysis is kept on the stick
  (`Seabass/engine-analysis/`) and "Restore Engine's Analysis" puts it
  back byte for byte; "Let the Player Analyse Again" hands a row back to
  the player. A grid locked or moved on the player is never replaced by a
  sync. A row the player has not analysed yet (a created one included)
  is completed all or nothing, so only with both parts. Measured: the
  conversion was calibrated on 192 tracks of one library against what a
  Prime 4 analysed itself, and a completed row's length is the sample
  count a Prime 4 stores (the container's count for FLAC, AIFF and M4A,
  an MP3's frames times 1152; testshark_t73). Never met hardware: a
  converted grid or waveform loaded and played on a player, a completed
  row the player then leaves alone (rather than analysing again), a
  library made by Create Engine Library with the conversion on, and
  Restore Engine's Analysis read back by a player. Before it graduates, a
  Denon player (Prime 4 or Prime GO+) must load converted tracks with the
  grid on the beat and the overview drawn, leave completed rows as they
  are, and take a restored row back as its own. The command line's
  `copy-beatgrids` (with `--waveforms` an experiment of its own) has no
  experimental gate, as the command line has none.

## Graduated to stable

- **Create Engine Library** (added 2026-08-30, graduated 2026-11-19, on
  Sebastian's call) — builds a brand-new Engine Library database from
  scratch out of an existing rekordbox export, for a stick/SD card that
  has never been prepared for Engine OS hardware. The first feature in
  this app that fabricates an entire new database rather than modifying
  one Engine itself already created. Carried over: title, artist, BPM,
  key, duration, bitrate, rating, comment, hot and memory cues, the
  playlists with their folders and order, and cover art (written the way
  Engine stores it, under `Artwork/`, since libdjinterop's own album_art
  API is unfinished). Every track is handed to the player unanalysed, as
  Engine's own rekordbox import hands them over, so the player analyses
  it on load; with both "Copy beat grids/waveforms from rekordbox to
  Engine" switches on in Preferences (experimental, see above) every
  track that can be completed gets rekordbox's grid and waveform instead.
  The card is in the Sync group of a writable stick without an Engine
  Library (`StickToolCards.qml`, `showCreateEngine`). On hardware: the
  first round (2026-09-17, Prime 4) failed, the player calling the
  library corrupt; the cause was libdjinterop's 3.0.2 Information row
  landing at id 2 and every track flagged analysed with nothing in it,
  both fixed in the creator. On 2026-11-08 a Prime 4 on current firmware
  took a created 3.x (schema 3.0.2) library, analysed every track on load
  and played the cues; 3.x has been the default generation since.
  `libdjinterop_engine_library_creator_test` reads a created library
  back at all three generations (and checks the Information row at
  each); `tools/make_engine_library.cpp` builds one headless. Still owed:
  a created 1.x or 2.x library on hardware (no unit on Engine Prime 1.x
  or Engine OS 2.x has seen one), a 3.x one on a Prime GO+ or any unit
  but the Prime 4, and a created library with the beat grid and waveform
  copy on (owed under that feature above).

- **Browse playlist editing** (added 2026-10-05, graduated 2026-11-19,
  on Sebastian's call) — in Browse Library: creating, renaming and
  deleting a playlist (the sidebar's last row and a playlist's menu),
  adding a track to a playlist ("Add to" in a row's menu), taking one out
  ("Remove from" the playlist shown), dragging a track onto a playlist,
  and dragging one within the playlist shown to reorder it
  (`ScanPage.qml`, `PlaylistEditController`, `PlaylistListView`,
  `TrackDragItem`, `PlaylistDropGap`). The Matching panel's results have
  the same row menu and drag. Every edit is staged into the stick's edit
  session, shown at once (greyed and italic) and written by Save into
  every library on the stick that has the playlist; a staged batch
  survives a crash (`docs/edit-mode-and-cancel.md`). The QML tests in
  `tst_ScanPage` run with the setting off. Still owed: nothing in the
  docs or tests records a player (Pioneer or Denon) loading a playlist
  Seabass created, renamed, reordered or changed; that check on each.

- **Anonymize Library** (graduated 2026-11-19, on Sebastian's call) —
  writes a de-identified copy of a stick's rekordbox and Engine libraries
  into one zip on this computer, for the project's test fixture or for a
  bug report (`AnonymizeLibraryPage.qml`, the same code as
  `seabass-cli anonymize`, which never had a gate). A maintainer tool,
  not a per-stick card: "Export Anonymized Library for Testing…" at the
  end of Preferences → More settings, under the experimental toggle, in
  every build (the section used to be compiled out with the toggle).
  Still owed: nothing records the page itself run against a real stick;
  the committed fixture (`tests/fixtures/anonymized_library/`) was made
  with `seabass-cli anonymize`.

All of the following graduated together on 2026-09-17, on Sebastian's call: the
write paths they add have been exercised on real sticks through this
project's live tests and everyday use, and keeping them behind a setting
was hiding work that is done. Create Engine Library stayed gated then,
because it fabricates a whole database and had never met real Denon
hardware; it graduated on 2026-11-19 (above).

- **USB Stick Performance** (added 2026-09-11, graduated 2026-09-17) — its
  own page from the stick's card grid: reads real files on the stick the
  three ways a DJ player reads it (streaming, 4 KiB random reads, small
  analysis-file opens), turns that into a DJ Workload Score with a verdict
  per player generation, and offers an optional write test that writes
  throwaway files into a hidden `.seabass-write-test` folder and removes
  them again, and a "Check for Wear" that reads every file once and
  reports unreadable and abnormally slow ones. Every measurement is
  appended to `~/Seabass/metadata/stick-performance-history.tsv` (this
  computer, never the stick; twenty lines per stick) so the page can say
  whether a stick is getting slower. The read measurements are harmless;
  the write test is a write path. It has run once on real hardware (a 32
  GB exFAT stick on a USB 2.0 port, Linux, 2026-09-11, via
  `stick_performance_live_test` with `SEABASS_LIVE_STICK` and
  `SEABASS_LIVE_WRITE=1`): the scratch folder came and went, free space
  was unchanged. Still owed: a run on Windows, and the verdict thresholds
  checked against at least one known-slow stick. Replaced the read-speed
  benchmark and its history table that Library Statistics used to carry.

- **Format USB Stick** (added 2026-09-05, graduated 2026-09-17) — erases
  and reformats a removable drive as FAT32 or exFAT (always MBR), for a
  stick that's never been prepared for CDJ/XDJ/Engine OS hardware, or one
  being reused. The only feature so far that can destroy an entire drive
  outright, not just modify or consolidate library data on one: layered
  with its own extra safety net beyond the experimental gate itself (a
  must-dismiss warning popup when the selected drive already has a
  recognized DJ library, a red "Data will be lost" badge for any non-blank
  drive, no drive preselected by default, and a final confirm dialog
  naming the exact drive/path with a type-to-confirm gate). The actual
  destructive call (udisks2's CreatePartitionAndFormat on Linux,
  PowerShell's Format-Volume on Windows) is unverified against real
  hardware on either platform: `linux_usb_formatter.cpp` flags this in its
  own comment, and there's no Windows machine in this dev environment to
  exercise `windows_usb_formatter.cpp` at all. Still owed on both
  platforms: a format confirmed against real hardware.

- **Backup USB Stick: create / update from another stick** (added
  2026-09-06, graduated 2026-09-17) — with two sticks inserted, an empty
  one gets a "Create Backup USB Stick" card naming the stick to copy from,
  and a stick that holds an older copy of the same library gets an "Update
  from <X>" card, where X is the newest copy: another mounted stick, or
  the disk backup. Copies are chained, never direct: an incremental full
  stick backup of the source into its own archive, then a restore of that
  archive onto the target (`application::CloneStick`,
  `CloneStickPage.qml`, pushed from `StickListPage.qml`; the disk-backup
  case reuses the restore page). The advice comes from `adviseStickBackup`
  seeing every mounted stick as a peer: same library by content
  fingerprint, in sync by database fingerprint, ordered by the catalog's
  mtime. Still owed: the live checks in `docs/stick-backup-plan.md`
  ("Stick-to-stick clone").

- **Stick Restore and Update** (added 2026-09-05, as the restore half of
  Full Stick Backup and Restore; graduated 2026-09-17) — restores a full
  stick backup onto the same stick or a fresh one, and brings a stick up
  to date from its backup or another mounted stick. Once-gated surfaces: the
  "Restore a Stick Backup" tool button on `StickListPage.qml`'s header
  (top-level, like Format USB Stick, because the target is often a blank
  replacement drive), the restore/clone card on each stick, and "Update
  Stick" in the home's Backup group (`common/StickToolCards.qml`). Design: `docs/stick-backup-plan.md`. It
  overwrites files on a stick, so it keeps its own confirmations. Still
  owed: a restore onto a fresh exFAT stick that Engine DJ or a player then
  reads without complaint.

- **Matching** (added 2026-09-04, graduated 2026-09-17, PREVIEW badge
  removed 2026-11-19 on Sebastian's call) — a panel on the Library page
  (`MatchingPage.qml`) that finds tracks compatible in key
  (Camelot-wheel Harmonic/Nearby matching, or Ignore Key), BPM and
  rating with whichever Browse row you've marked as the one you're
  editing, scoped to This Playlist, to help build out a playlist around
  it. It was never an `ActionCard`: the panel and the playlist drawer are
  the Library page's own layout. It wore a `PREVIEW` badge because its
  Before/After buttons and row reorder arrows only reported "preview, not
  saved". Those controls are gone (2026-10-16, with Browse's playlist
  editing): a result now has Browse's row menu ("Add to", and "Remove
  from" when it is in the playlist shown) and drags onto a playlist like
  a Browse row, all staged into the edit session and written by Save, so
  the panel writes for real through Browse playlist editing (above).
  Placing a result at a position, as Before/After meant to, is done by
  dragging it into the playlist shown. Still not there: the Genre filter
  is a disabled "Genre: Ignore" button ("Genre isn't read from your
  library yet"; `domain::Track` has no genre field), and the hint under
  the panel's title before a track is picked still reads "Search This
  Playlist, then Before/After the track you're editing in Browse",
  naming buttons that no longer exist.

- **Full Stick Backup** (added 2026-09-05, graduated 2026-09-17) — backs a
  whole stick up into one browsable `.zip` on this computer
  (`~/Seabass Backups/<label>.zip`, changeable in App Settings, whose
  backup-folder section graduated with it) and keeps it current
  incrementally, in the ZIP64/STORE format with a manifest and a
  crash-safe append-only update protocol (`docs/stick-backup-plan.md`).
  The pages kept their EXPERIMENTAL pill after the setting stopped
  gating them (the Restore section of `StickBackupPage.qml`, the headers
  of `RestoreStickBackupPage.qml` and `CloneStickPage.qml`) until
  2026-09-25; the QML tests now check each of them carries none.

- **Library Health** (added 2026-08-29, graduated 2026-08-30) —
  cross-catalog consistency scan/repair, plus the 0:00-junk-memory-cue
  cleanup.

- **Stick Statistics** (added 2026-08-29, graduated 2026-08-30) —
  filesystem/hardware info, per-catalog library stats, and a
  Filelight-style disk usage breakdown. Its read-speed benchmark moved to
  USB Stick Performance on 2026-09-11.
