<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Experimental features

Features gated behind `AppSettingsController::experimentalFeaturesEnabled`
(Settings → Experimental features), off by default. See that property's own
doc comment and `ActionCard.qml`'s `experimental` property for how the gate
works, and the `SEABASS_EXPERIMENTAL` CMake option for the build-time
opt-in-mechanism switch.

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

- **Create Engine Library** (added 2026-08-30) — builds a brand-new
  Engine Library database from scratch out of an existing rekordbox
  export, for a stick/SD card that has never been prepared for Engine OS
  hardware. The first feature in this app that fabricates an entire new
  database rather than modifying one Engine itself already created.
  Deliberately narrow: title/artist/BPM/key/duration/bitrate/rating/
  comment/hot+memory cues plus a simple two-point approximate beatgrid;
  no cover art (libdjinterop's own album_art API is unfinished), no real
  per-beat grid, no waveform, no playlists. (Since 2026-11-19: with
  both "Copy beat grids/waveforms from rekordbox to Engine" switches on
  in Preferences, itself experimental, see below, every track it creates
  that can be completed gets rekordbox's real grid and waveform; with
  them off, or only one on, it is still as described here, every track
  left for the player to analyse.) Verified by creating a
  library and reading it back correctly with this app's own reader
  (`libdjinterop_engine_library_creator_test`); never tested against
  real Denon hardware. Exposes the Engine schema generation (1.x/2.x/3.x)
  as a user choice specifically because real firmware compatibility per
  generation is unverified. Promote to stable once Sebas has confirmed a
  created library works correctly on real Denon hardware (he has a Prime
  GO+ to test against).

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

All of these graduated together on 2026-09-17, on Sebastian's call: the
write paths they add have been exercised on real sticks through this
project's live tests and everyday use, and keeping them behind a setting
was hiding work that is done. Create Engine Library is the one feature
still gated, because it fabricates a whole database and has never met
real Denon hardware.

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

- **Matching** (added 2026-09-04, graduated 2026-09-17) — a panel on the
  Library page (`MatchingPage.qml`) that finds tracks compatible in key
  (Camelot-wheel Harmonic/Nearby matching, or Ignore Key) and BPM with
  whichever Browse row you've marked as the one you're editing, to help
  build out a playlist around it. Unlike every other entry on this list it
  was never an `ActionCard`: the panel, and the playlist drawer it moves
  the old always-on playlist pane into, are the Library page's own layout
  (`ScanPage.qml`'s `matchingEnabled`). It carries its own `PREVIEW`
  badge, because the search/filter side is real but the write side isn't:
  no format (rekordbox, Engine, OneLibrary) has a playlist-mutation writer
  yet, so Before/After and the row reorder arrows just report a "preview,
  not saved" status instead of touching anything on disk. The Genre filter
  is present but disabled for the same reason one level down —
  `domain::Track` has no genre field at all yet. Out of the experimental
  setting, but the PREVIEW badge stays until a real per-format playlist
  writer exists and Before/After actually writes.

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
