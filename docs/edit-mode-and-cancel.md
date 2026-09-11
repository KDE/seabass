<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Edit mode, library locks, and cancellation

How Seabass keeps a library consistent while it is being edited, written,
cancelled, or edited by two instances at once. The rules here are the
ones the code enforces; the "why" for each is next to it. Everything
below lives in `src/gui/edit/`, `src/application/ports/cancellation_token.hpp`,
`src/application/ports/library_edit_lock_store.hpp`, and the QML in
`qml/common/`. None of it is gated behind Experimental: it is a safety
feature, and it is on for everyone.

## Vocabulary

- **Library**: one stick root with its rekordbox (`PIONEER`) and Engine
  (`Engine Library`) catalogs, plus that library's disk backup archive
  (`~/Seabass Backups/<label>.zip`) and its on-stick `.seabass-backups`.
  Identified by a **library id**: the filesystem UUID, else
  `<label>-<capacity>` (`application::StickIdentity::libraryId()`, the
  same rule `StickHardwareInfo::stickIdentifier` uses for backup manifests,
  so cookies and archives agree).
- **Staged change** (`PendingChange`): one user-visible unit of work
  (a track, a cue, a duplicate group, a setting) kept in memory and not
  written until Save. Every library-editing page stages: Sync, Clean Up,
  Duplicates, Device Settings, Library Health, Stray Cues, Add Cue (from
  Browse), Local Cue merge.
- **Direct write**: an operation that writes as soon as it is confirmed,
  because staging it makes no sense: full stick backup/restore/clone/
  compact, Format USB, Create Engine Library, Manage Backups restore/
  prune/delete, deleting orphaned files. These get the write-mode
  semantics (lock, progress, cancel where safe, summary) without a
  staging step.
- **Edit lock** (cookie): a small JSON file in
  `<app data>/seabass/edit-locks/<library id>.json`, on the local machine,
  never on the stick. Held while a library has staged changes or is being
  written. Other instances on the same machine see it and go read-only.
- **Write lock**: the pre-existing on-stick `.seabass-backups/.write.lock`
  (`StickWriteLock`), taken for the duration of any actual write. This is
  the mutual exclusion; the edit lock is the user-facing one.

## Session state machine

`LibraryEditSession` (one per open library, refcounted by the pages
through `EditSessionRegistry.openSession/closeSession`):

```
idle ----stage()---- editing ----save()---- writing
 ^                    |  ^                    |
 |   discard()/last   |  |   save finished    |
 |   unstage()        |  +--------------------+
 +--------------------+      (changes remain: editing; none: idle)
```

- `idle`: a page is open, nothing staged, no lock held. A second instance
  sees nothing.
- `editing`: the first `stage()` acquired the edit lock. If another
  instance holds it, `lockRefused(holder)` fires, nothing is staged, and
  the page shows the locked-library dialog. The Save overlay button is
  enabled exactly when this state is reached.
- `writing`: `save()` runs the save loop on a worker thread. The window
  cannot close, the page cannot be left, the write-progress dialog is
  up with "Do not remove your USB stick".
- Back to `editing` with whatever was not applied (a cancel or a
  failure), or to `idle` when everything landed. The lock is released
  when the session is clean and the last page referencing it closes, or
  on discard.

## Invariants

1. **A change is either fully on the stick or fully pending.** Cancel is
   checked only between two changes (`runSaveLoop`), never inside one.
   A cancelled save reports "k of N <unit> written" and the remaining
   N-k stay staged.
2. **A scratch copy commits only completed items.** `FormatWriteSession`
   writes a whole-file format (rekordbox `export.pdb`) to a scratch copy
   and commits it back atomically when the loop ended OK or at least one
   item was applied; with zero items applied it is discarded.
3. **Backups happen once per file per save** (`SaveContext::backupOnce`),
   and the set of backups made by one save is the unit "Undo Last Save"
   restores (`RestoreBackupsChange`).
4. **Reads that are cancelled leave nothing behind.** Readers throw
   `OperationCancelled` at the next track; `LibraryCatalogCache` never
   caches a scan that threw; the page pops and the next visit rescans.
5. **The edit lock is local; the write lock is on the stick.** A cookie
   on the stick would come back days later on another machine as a
   confusing stale lock. Local cookies carry pid and process start id,
   so a dead owner is a fact, not a guess.
6. **A stale cookie is one whose owner is provably gone**: same host and
   the pid is dead (or its start id differs), or another host and no
   heartbeat for 10 minutes. Stale cookies are replaced silently.
   "Remove Lock" is the only way past a live one, and the dialog says so.
7. **Direct writes hold the same edit lock** (`DirectWriteHold`) for the
   run, so a staged edit and a full-stick restore cannot race; a clone
   or a restore takes both libraries involved, or neither.
8. **Nothing is written while rekordbox or Engine DJ runs.** Every write
   task starts with `refuseIfDjSoftwareRunning()`.

## Cancellation

| Operation | Cancellable | Granularity | After cancel |
|---|---|---|---|
| Any library scan | yes | per track | page pops, nothing cached |
| Save of staged changes | yes | between changes | summary, rest stays staged |
| Manage Backups: Clean Up | yes | between backups | summary "k of N backups deleted" |
| Manage Backups: restore / delete one | no | one step | summary |
| Delete orphaned files | yes | between files | summary; manifest keeps the rest |
| Create Engine Library | until "Copying to stick" | between tracks (scratch build) | "Cancelled, nothing was created" |
| Full stick backup / clone / compact | yes (pre-existing) | per file, keep/discard partial | own result frame |
| Format USB | no | one call | result |

The summary dialog (`OperationSummaryDialog`) is the same everywhere:
"<written> of <total> <unit> <verb>." with a cancelled or error line.
OK returns the user to where they came from after a cancelled direct
write or read; after a cancelled save it returns to the edit page with
the rest still staged; after a completed action it stays.

## Process guard

`DjSoftwareGuardController` owns one `QTimer`: stopped while no library
is in edit or write mode, 5 s while one is, 1 s while its modal dialog
is open. The dialog (`DjSoftwareRunningDialog`) has no buttons and
closes itself once the process is gone. It names whichever app is
running.

## USB removed while editing

`MediaController::detect()` diffs the mounted sticks by identity
(`application/stick_presence_diff.hpp`) and emits `stickRemoved` /
`stickReturned`. The registry marks the session's `stickPresent` and
`Main.qml` shows `StickRemovedDialog` for a session that holds a lock:
"Discard Changes" or "Understood", the latter enabled only when the very
same stick is back (hardware serial when known, else filesystem UUID;
label plus size only counts as weak and the dialog says so). A stick
pulled mid-write fails the save at the next I/O error; the summary says
"k of N, then: <error>", and the session stays dirty.

## Two instances

- A second window sees READ ONLY on every library-manipulating card of
  a library another instance is editing (`ActionCard.readOnly`, refreshed
  on page activation and every 2 s). Browse and Library Statistics stay
  plain.
- Clicking a read-only card opens `LockedLibraryDialog` with the holder
  line; "Stay on the Safe Side" is the default.
- `seabass-cli` probes the same cookie before its writes and refuses
  with the holder unless `--force`.
- The old single-instance `QLockFile` is gone.

## Quitting

Closing the window with unsaved changes asks "Discard Changes" or
"Save". Discard drops every session and quits. Save runs every dirty
session; the summary shows once all are done (or the user cancelled),
and the app quits on OK. While a write is running the window does not
close at all.

## Adding a staged change

1. Subclass `PendingChange`: `id()` (stable, unique per unit),
   `description()`, `unit()` ("tracks", "cues", ...), `formatsTouched()`,
   and `apply(SaveContext &)`. Use `ctx.backupOnce()` for every file you
   overwrite, `ctx.shared<T>(key, factory)` for a per-save writer or a
   `FormatWriteSession`, `ctx.log()` for the on-stick log, `ctx.status()`
   for the progress label. Never check the cancel token inside `apply`.
2. In the controller: resolve the session with
   `registry->sessionFor(registry->libraryIdForPath(path))`, `stage()`
   the change, mark the row staged in the model, and react to
   `changeApplied(id)` (update the model) and `changesDiscarded()`.
3. In the page: `EditSessionHost` with the library id and paths, and
   route Back/Home through `editHost.requestLeave(fn)`.

## Live verification (2026-09-07, RV2 scratch stick, 1161 rekordbox / 1469 Engine tracks)

`tests/qml-live/run-live.sh` on the real stick, real binary code paths,
offscreen:

- a cancelled Browse scan leaves no rows and no cache; the rescan reads
  all 1161 tracks
- Device Settings: stage one field, Save ("1 of 1 settings"), the new
  value is on disk, Undo Last Save puts the old one back
- Sync: 2388 plans staged, Save cancelled after the first tick, "38 of
  2388 tracks written (cancelled)", the rest discarded, the 38 undone
- Stray cues: 201 staged, "201 of 201 cues written", undone in one step
- Manage Backups: prune "2 of 2 backups deleted", restore "1 of 1"
- a foreign live lock: READ ONLY cards, the locked dialog, Remove Lock,
  then the same stage succeeds
- a process named rekordbox: the guard blocks within 5 s of staging and
  clears within 1 s of the process ending; meanwhile `seabass-cli` is
  refused with the holder and writes with `--force`
- the stick unmounted while editing: the removed-stick dialog, Understood
  disabled, enabled again when the same stick is back ("hardware" match)
- the real `seabass` binary on the desktop shows READ ONLY for a planted
  lock

Found and fixed by that run: two confirm dialogs whose body had landed
inside a `Connections` block (Delete Orphaned Files, Create Engine
Library), one backup record per file instead of per save (201 stray
cues made 201 records; now `BackupStore::addToBackup` grows one record
per label per save), the removed-stick dialog without the stick label.
Not verified live: Library Health repairs and Delete Orphaned Files
(the stick had nothing repairable and no pending deletions), Create
Engine Library (the stick already has one), the quit flow.
