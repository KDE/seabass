<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Are we still thinking in two libraries anywhere?

Seabass began with two catalogs and grew a third (OneLibrary /
`exportLibrary.db`). This is the audit of whether any code still assumes
two, or assumes one where it needs all of them. Done 2026-09-08 after
the deletion gate turned out to be exactly that mistake.

## The dangerous shape

A function that takes **one merged `vector<Track>`** and concludes
something is *absent*. Absence is the only conclusion that gets worse
when a catalog is missing from the input: "no row points at this file"
read from an incomplete set means "delete it". Everything else --
statistics, disk usage, grouping, matching -- degrades into a less
complete answer, not a wrong and destructive one.

Two functions draw that conclusion. Both now take
`application::CatalogTracks`, which names rekordbox, Engine and
OneLibrary as separate fields so an omission is visible at the call
site, and both refuse to answer at all when handed nothing:

- `resolvePendingDeletions()` -- the gate before audio is deleted
- `findUnreferencedFiles()` -- the stray-file scan

## Every catalog read, and what it feeds

| Call site | Reads | Verdict |
|---|---|---|
| `sync_controller` | all three | **correct** -- see below |
| `stick_statistics_controller` | all three | correct (reporting only) |
| `scan_controller` | all three + sibling rekordbox | correct (browse) |
| `stick_catalogs` | all present | correct (new, feeds the gate) |
| `cleanup_controller` scan | one format | correct by design: the page is scoped to one library, and its *deletion* path now reads all three |
| `duplicates_controller` | one format | correct: consolidates cues within one catalog, concludes nothing about absence |
| `local_cue_controller` | one format | correct: restores cues into one catalog |
| `library_consistency_controller` | one format | correct **and deliberate** -- see `LibraryConsistencyChecker::check()`, which documents that a healthy rekordbox copy says nothing about a broken Engine row. Deletes rows, never files. |
| `add_cue_controller`, `library_fingerprint_reader`, `engine_library_creator_controller` | one format | correct: single-catalog operations |

## Sync really is three-way

Worth recording, because it is the place the two-library era would most
likely have survived. `SyncController::analyze()` runs
`SyncLibraries::execute()` over **all three pairs** (rekordbox↔Engine,
rekordbox↔OneLibrary, Engine↔OneLibrary), and then --- the part that
makes it genuinely three-way rather than three independent pairs ---
hands the result to `CrossSourceConflictDetector`, which catches the
case no pairwise planner can see: two different pairs both targeting the
same third catalog's track. Those become conflicts needing a manual
pick, not silent last-writer-wins.

`SyncLibraries` itself is deliberately named `tracksA`/`tracksB` rather
than after formats, which is why it survived the third catalog unchanged.

## What to keep in mind for new code

Ask which of the two kinds of question a new function asks:

- **"Which of these is X?"** -- one catalog is fine. A partial answer is
  a smaller answer.
- **"Is this absent from everything?"** -- it needs `CatalogTracks`, and
  it needs to say which catalogs it actually saw. A partial answer here
  is a wrong answer that looks like a confident one.

The second kind is rare. That is exactly why it gets missed.
