<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Full stick backup and restore: design plan

Status: design agreed 2026-09-05; implemented the same day on branch
`worktree-stick-backup`, one commit per phase (primitives, archive format,
crash-safe update, stick side, compaction, restore, GUI), every phase with
its tests green. Still to do before it can graduate: the live verification
list at the end of this document on real sticks, and the optional CLI
subcommands. Ships gated behind
`AppSettingsController::experimentalFeaturesEnabled` (add an entry to
`experimental-features.md` when it lands). Principle for v1: **get it
working correctly first, optimize later** — see "Optimization pass" at the
end for what was deliberately deferred.

## Why this exists

A DJ's USB stick is the single copy of a lot of work: cue points, loops,
playlists, ratings all live in the Engine database on the stick, next to
the audio. Users trust a backup absolutely — a backup that restores a torn
database or a mis-named file is worse than no backup, because it is
discovered at the worst moment. Every design choice below optimizes for
*provable* integrity first, then for not re-reading a slow USB stick more
than necessary.

## Constraints that drove the design

- Single file, sized to the actual data — not a device image (which copies
  unused space and cannot be browsed without mounting).
- Individual files must be accessible without extracting everything.
- Audio and cover art are already compressed; re-compressing gains nothing.
- Reads from the stick should be sequential with as few seeks as possible.
- Updating the backup after small changes must not re-read the whole stick
  ("rsync-like").
- Must be correct on Linux, macOS and Windows — not just compile there.

## Decisions

1. **Format: ZIP64, STORE method (no compression), `.zip` extension.**
   ZIP's central directory (CD) is an index at the tail, giving O(1) access
   to any entry. STORE means no CPU spent on incompressible data and entry
   bytes are a plain slice of the file. ZIP64 is mandatory: a stick backup
   exceeds both 32-bit limits (4 GB total, 65 535 entries) easily.
2. **We own both writer and reader.** A new writer/reader pair under
   `infrastructure/stick_backup/` (ZIP64 + STORE + streaming, working on an
   `ArchiveFile` stream abstraction so fault-injection tests run in memory);
   the existing `infrastructure/zip_archive_writer` (32-bit, Deflate,
   slurps whole files) stays as it is for the anonymizer -- grafting ZIP64
   onto it would risk that path for no gain. minizip-ng was considered and rejected: its append mode seeks to
   the start of the old CD and overwrites it — exactly the in-place write the
   crash-safe design forbids — so we would own the CD merge anyway, and the
   dependency would have to be vendored across four build variants with all
   optional backends disabled. Shared-misconception risk between our writer
   and reader is covered by the manifest (independent source of truth) and
   CI cross-validation against Python `zipfile` / 7-Zip / `unzip`.
3. **Integrity manifest with SHA-256.** ZIP's CRC32 protects entry content
   only; the CD (names, offsets, sizes) has no protection at all, and a
   flipped byte in a CD filename silently restores a file under the wrong
   name — which breaks Engine's path references while every byte is
   "correct". The manifest is an archive entry rewritten on every update:
   per file path, size, mtime, SHA-256; plus a hash over the manifest
   itself. Hashing happens while streaming from the stick, so it is free in
   wall-clock terms (USB is the bottleneck by an order of magnitude).
   SHA-256 needs no new dependency.
4. **Engine databases: raw byte-exact copy of the DB set, hash-before/after.**
   See "Database capture" below. Refuse to start the backup at all if Engine
   DJ or rekordbox is running; keep checking during the run.
5. **Compaction: copy-to-temp + atomic rename, with a free-space preflight.**
   Low-space compaction is a phase-2 item. See "Dead space and compaction".

## Archive layout

```
[local header + data]  ...  [local header + data]   <- entries, STORE
[manifest entry]                                    <- last entry before CD
[central directory][zip64 EOCD][zip64 locator][EOCD]
```

Entry requirements:

- UTF-8 flag (general purpose bit 11) set; names stored as the bytes the OS
  enumerated. Without this, Windows tools decode names as CP437.
- Extended timestamp extra field (0x5455) so mtime keeps 1 s resolution
  rather than DOS 2 s local time.
- ZIP64 extra field (0x0001) whenever any 32-bit field would saturate.
- Empty directories get explicit `dir/` entries or they are lost on restore.
- Symlinks are never stored as symlinks and never created on restore.
- Stream entries in chunks (1-4 MB); never slurp whole files. Use
  `posix_fadvise(SEQUENTIAL)` / `FILE_FLAG_SEQUENTIAL_SCAN` so hundreds of GB
  do not evict the page cache.

## Change detection (what to read from the stick)

One stat-only walk of the stick, compared against the previous backup's own
CD/manifest. Only files whose size or mtime differ are read. This is rsync's
default heuristic, not its rolling-checksum delta — audio files are added,
removed or wholesale-replaced, never partially edited, so byte-level diffing
buys nothing here.

Known pitfalls, all handled by design:

- FAT/exFAT mtimes have 2 s resolution: compare with a 2 s window.
- FAT stores local time; a timezone/DST change makes *every* mtime appear
  shifted by 1800/3600 s. Detect the uniform-shift pattern (most files off
  by exactly that amount with unchanged size) and treat as unchanged —
  otherwise a DST change triggers a full re-read of the stick.
- macOS enumerates NFD, Windows/Linux NFC. v1 compares exact bytes: a
  mismatch only costs a re-read (bytes are stored as enumerated), never
  correctness, and there is no ICU and no macOS build to validate a
  normalization table against. See "Optimization pass".
- Excluded from the walk: `-shm` files (regenerated by SQLite), the
  `.write.lock`, and OS metadata directories (`System Volume Information`,
  `$RECYCLE.BIN`, `.Trashes`, `.Spotlight-V100`, `.fseventsd`).
  `.seabass-backups/` and `.seabass.log` are included -- they are part of
  the stick's state.
- Prefer size/mtime from the directory enumeration (`FindNextFile` returns
  them; POSIX often re-stats) — 50 k files on USB is minutes of per-file
  latency regardless of bytes.
- Stat each file again after streaming it; if it changed during the read,
  re-read or skip and report. The process refusal (below) makes this rare.

A rename is delete+add to this heuristic and therefore a full re-read of
the renamed data plus equivalent dead space. Accepted for v1; see
"Optimization pass".

## Crash-safe incremental update

Never overwrite the old CD/EOCD in place. Only ever append. A compliant
reader finds the archive by scanning backward from EOF for the EOCD
signature and trusting whatever CD it points to; the *last* fully written
EOCD is therefore authoritative, and everything before it is untouched.

1. **Write the journal.** `backup.zip.journal` next to the archive records
   the pre-update file length and EOCD offset. fsync it *before* the first
   append. Deleted only after step 6 succeeds.
2. **Read the existing CD** -> list of live entries with their unchanged
   offsets. Diff the stick against it.
3. **Open for append** (no truncate).
4. **Stream new/changed entries** (local header + STORE data) onto the
   growing tail, hashing on the fly. fsync (see barrier notes).
   *File is now [old valid archive][orphan bytes].*
5. **Append the new manifest, then a new CD** (kept entries at original
   offsets + new entries - deleted entries) **and EOCD.** fsync.
   *The instant this lands, backward-scanning readers see the new archive.*
6. **Reopen fresh and verify:** EOCD found, entry count as expected, CRC32
   of every newly written entry (cheap: STORE, no inflate), manifest hash.
   Only then report success and delete the journal.

Why the journal is required, not optional: the backward EOCD scan only
looks ~64 KB from EOF (max comment length). If step 4 appended 2 GB and we
crashed before step 5, the old EOCD is 2 GB from EOF and every standard tool
reports "not a zip file". The data is intact but only *we* can recover it.
On open, a present journal means an unconfirmed update: truncate back to
the recorded length (old archive restored exactly), then re-run the normal
diff. If the crash happened between steps 5 and 6 the archive is already
correct; the diff finds nothing to do and the journal is cleared.

Recovery must not blindly truncate: a journal can outlive a *completed*
update if the crash lands between the journal's clear and its barrier.
On open with a journal present, first try to parse the archive at its
current length (EOCD at EOF, CD valid, manifest hash correct, length past
the journaled pre-update length): if that succeeds the update had
completed -- only clear the journal. Only when the archive does not parse
is it truncated back. A journal that itself fails its CRC was being
written when the crash hit, i.e. before any append -- the archive is
intact, clear the journal.

### Cancelling a backup

Cancel stops after the current file. At that point the archive is in the
"orphan bytes + journal" state above, and the two follow-up choices are
exactly the two existing recovery paths:

- **Keep for later** -- commit a central directory over the entries that
  completed, with the manifest status `partial-cancelled`. The next run's
  stat-diff sees those files as already backed up and resumes from there.
- **Discard** -- truncate back to the journaled pre-update length. On an
  update the previous backup is untouched; on a first backup the archive
  file is removed.

If the app dies before the user chooses, journal recovery discards -- the
safe default (nothing on the stick is ever at risk, only read time). The
mid-run process detection uses the same primitive but decides on its own:
it commits the completed entries as `partial-conflict` and skips the DB
set, since data already read is worth keeping and nothing about it is
uncertain.

fsync barrier notes — the placement between steps 4 and 5 is what prevents
a power loss from producing a structurally valid archive whose new entries
are zeros:

- Linux: `fsync(fd)`; rename durability additionally needs `fsync` on the
  parent directory.
- macOS: `fsync` does **not** flush the drive cache; use
  `fcntl(fd, F_FULLFSYNC)`.
- Windows: `FlushFileBuffers`; rename via `MoveFileEx(MOVEFILE_REPLACE_EXISTING
  | MOVEFILE_WRITE_THROUGH)`, and expect transient sharing violations from
  scanners/Explorer — retry with backoff.

Note the read-back in step 6 is served from the page cache: it verifies our
writer logic, not the media. Media rot is caught by the scheduled verify and
by compaction (which reads everything anyway).

## Dead space and compaction

Because we are the only writer, dead space is exact arithmetic on the CD,
no scanning:

```
live_bytes = sum over live entries of (local header + data)
overhead   = latest CD + zip64 EOCD + locator + EOCD
dead_bytes = file_size - live_bytes - overhead
```

Old CDs, replaced and deleted entries all fall out as dead bytes. The CD
itself never accumulates stale entries — every update writes a fresh one.

Trigger check after every update (free). Suggest compaction when
`dead_ratio >= 20%` **or** `dead_bytes >= 5 GB`, whichever first. **Never
auto-run it**: compaction is O(archive), the exact cost incremental updates
exist to avoid. Show the exact reclaimable number ("Compacting will reclaim
4.2 GB, 18% of this 23 GB backup") and let the user decide, or gate behind an
opt-in "compact when idle".

Procedure (v1):

1. Preflight: free space on the destination volume must be
   `>= live_bytes + margin`. If not, refuse with exact numbers ("needs 412 GB
   free, 80 GB available") and offer to write the compacted copy to another
   volume. A bloated archive is wasted disk, not a broken backup — refusing
   is safe.
2. Stream live entries from the old archive into a temp file next to it,
   verifying every entry against the manifest on the way (full verify for
   free). Never touches the stick; works with the stick unplugged.
3. Fresh manifest, CD, EOCD. fsync file and directory.
4. Atomic rename over the old archive. The old file stays valid and
   browsable until this instant; a crash leaves it intact plus a temp file
   to clean up.

Precedent: SQLite `VACUUM` and Postgres `VACUUM FULL` require the same 2x
space for the same reason. The initial backup gets the same kind of
free-space preflight against the stick's used bytes.

## Archive location and identity

Default location `~/Seabass Backups/<stick label>.zip` (`%USERPROFILE%\Seabass
Backups\` on Windows), changeable in App Settings -- a user-visible,
browsable place, not the hidden app-data directory: the whole point of a
plain `.zip` is that the user can find it, copy it elsewhere and open it
with 7-Zip or `unzip`. The manifest header records the stick's filesystem
UUID (`StickHardwareInfo.stickIdentifier`) next to the label. On open, a
UUID mismatch (same label, different stick -- e.g. after Format USB, which
issues a new UUID) warns and lets the user continue with this archive or
pick another file; it never silently mixes two sticks' histories.

## Database capture (`m.db`, `hm.db`, `Database2/m.db`, legacy `p.db`)

The smallest and most important files on the stick. Decision: **raw,
byte-exact copy** — the restored file is bit-for-bit what was on the stick.
(`sqlite3_backup` would also have been correct — it copies pages below the
schema, libdjinterop's reverse-engineered knowledge is not involved — but
byte-exact makes restore->re-backup trivially a no-op and removes any
dependence on the SQLite library's pager.)

- **The DB set is a unit:** `X.db` + `X.db-wal` + `X.db-journal` (skip
  `-shm`, it is regenerated). Committed transactions can sit in the WAL; a
  leftover rollback journal means the main file is mid-transaction. A
  byte-exact copy of `m.db` alone can be silently *incomplete* and no hash
  check catches that. Hash, copy and restore the set together.
- **Hash-before/after:** copy with hash in flight, then re-hash the on-stick
  set. Any write during the window changes bytes one pass saw and the other
  did not, so a torn copy always mismatches. Retry up to 3 times with a
  short pause; if still changing, fail the DB capture loudly and mark the
  backup incomplete. Never store a torn DB.
- **Skip check** so an unchanged multi-GB DB is not double-read every run:
  size + mtime + SQLite header change counter (bytes 24-27) + WAL header
  salts and size. Unchanged -> reuse the previous entry.
- **Process gate:** refuse to *start* if Engine DJ or rekordbox is running
  (extend the existing `rekordbox_process_detector` pattern with Engine DJ
  on all three OSes). During the run the use case polls a process probe
  between chunks, at most every 2.5 s (process enumeration is
  milliseconds) -- a probe rather than a watcher thread, so tests can
  drive it deterministically. On detection mid-run: commit the entries
  that fully completed (append-only makes this free), skip the DB set,
  flag the backup incomplete until a rerun captures the DB.
- **> 1 GiB refusal:** SQLite's byte-range locks live at offset 0x40000000
  (the never-used "pending byte" page). Above that size a raw read of that
  region can fail with `ERROR_LOCK_VIOLATION` on Windows if a connection
  holds a lock. Rather than reason about it, refuse to back up a DB above
  1 GiB and tell the user we cannot make this safe enough and they must
  back it up manually. Applied on all OSes for simplicity; the case is
  rare.
- Read-only remounting of the stick was considered and rejected: needs
  privilege escalation or an unmount cycle on every OS, fails if anything
  has a file open, and on Windows the only route is a *persistent* volume
  attribute that outlives a crash. Buys nothing the process gate plus
  hash-before/after does not.

## Restore

- Full restore streams entry by entry to the target; single-file restore is
  one seek + one read. There is no "extract everything to a staging area"
  step in our own tool ever.
- Target must be a pre-formatted exFAT/FAT32 volume — we restore files, not
  partitions. Document this as the difference from imaging.
- **Sanitize every entry name** before writing: reject `..` segments,
  absolute paths, drive letters, symlink entries; on Windows reject
  reserved names (`aux.mp3` written on a Mac cannot exist on Windows) and
  use the `\\?\` prefix — long artist/title paths exceed `MAX_PATH`. Every
  rejection is reported per file, never silently skipped.
- Restore mtimes, so the next incremental backup against the restored
  stick sees zero changes.
- Restore the DB set together.
- After restore, open the restored `m.db` with the existing libdjinterop
  reader and assert every referenced track path exists in the restored
  tree. That is the real "did it work" check for this domain.

Third-party access, documented for users: macOS Finder double-click hands
the file to Archive Utility, which unpacks the *entire* archive — recommend
Keka / The Unarchiver or `unzip backup.zip path/to/file` instead. Windows
Explorer's built-in zip support did not handle ZIP64 until recently —
recommend 7-Zip, which browses and extracts selectively.

## Testing (non-negotiable before this leaves Experimental)

Synthetic fixtures are fine; all of this must be automated. Suggested files
follow the existing convention: `backup_archive_roundtrip_test.cpp`,
`backup_archive_incremental_update_test.cpp`,
`backup_archive_truncation_fuzz_test.cpp`,
`backup_archive_crossvalidation_test.cpp`, `backup_restore_test.cpp`,
`backup_database_capture_test.cpp`.

1. **Round trip, no faults.** Mixed sizes (0 B, 1 B, multi-GB via sparse
   files), non-ASCII names in NFC and NFD, deep paths, same-size-different-
   mtime and vice versa. Full backup -> every hash matches. Incremental
   update after random mutation -> archive equals new tree, and a fake
   filesystem asserts **zero bytes read** from files whose stat did not
   change. N rounds of random mutation -> no drift; computed `dead_bytes`
   equals an independently measured value. Compaction -> same content,
   dead space ~0, smaller file.
2. **Fault injection — the load-bearing tier.**
   - Exhaustive truncation, *in memory against a fake stream*, on a tiny
     synthetic append (a few KB, so every offset is affordable); for larger
     appends every byte within +-64 of each structural boundary plus a
     seeded random sample. Acceptable outcomes after our open-and-recover
     routine runs: exactly the old archive, or exactly the new one. Anything
     else fails. (Naive on-disk exhaustive truncation is ~200 GB of test
     I/O; do not do that.)
   - Reordering model: writes before the last fsync barrier are durable,
     writes after it are lost or zeroed at random. This is what proves the
     barriers are in the right places.
   - Byte-flip corruption in the appended region -> detected (CRC or
     structure), never silently returned.
   - Crash between steps 5 and 6 -> next run detects "already up to date",
     no double append.
3. **Cross-validation.** Every synthetic output is opened by an
   independent implementation — Python `zipfile` (OS-agnostic), and
   `unzip -t` / `7z t` where available. Our reader never grades our
   writer's homework alone.
4. **Restore.** Byte-and-name-exact full restore; restore onto a FAT32 and
   an exFAT loopback image (Linux CI); **restore then immediate incremental
   backup reports zero changes**; single-file restore; restore of a
   compacted archive; adversarial archives (zip-slip names, symlinks,
   absolute paths, reserved names, over-long paths) rejected and reported;
   restored `m.db` opens and all referenced paths exist.
5. **Database capture.** Torn-read detection with a concurrent SQLite
   writer; WAL-mode and rollback-mode fixtures; the skip check; the > 1 GiB
   refusal; on Windows, a > 1 GiB DB with a concurrent connection holding a
   lock.
6. **Cross-platform.** Tiers 1-5 run in CI on native Linux, native macOS
   and a real Windows runner — compiling under MinGW is not testing NTFS
   semantics. Source-side stat-diff tests against FAT32/exFAT loopbacks for
   the 2 s resolution, DST shift and case-insensitivity behaviour.

## Live verification before graduating (not yet done)

On a **scratch copy** of a real stick first, never the live stick for the
first runs: first backup -> VERIFIED badge; add/change/remove files ->
the "since last backup" line shows the right counts and only those bytes
are read; cancel mid-run -> Keep, rerun resumes; cancel a first backup ->
Discard removes the file; `unzip -l` / 7-Zip open the archive; replace
files then Compact -> exact reclaim numbers; restore onto a scratch exFAT
stick -> the backup preview of the restored stick shows zero changes and
Engine DJ / a player reads it; start Engine DJ or rekordbox -> refusal
banner and refused run. Then the same on the real stick, over weeks.

## Optimization pass (deferred on purpose — revisit after v1 works)

- **Low-space compaction.** Streaming copy with hole-punching
  (`fallocate(PUNCH_HOLE)` / `F_PUNCHHOLE` / sparse `FSCTL_SET_ZERO_DATA`)
  over the consumed prefix of the old file: peak overhead ~one entry
  instead of 2x. Needs a roll-forward journal and falls back to
  copy+rename on exFAT destinations. Preferred over pure in-place moves,
  which overlap their own source when the dead gap is smaller than the
  entry and cannot be restarted from zero. Once it exists, dispatch by free
  space is a ten-line decision. Note: in-place moves *less* data than
  copy+rename (only entries after the first hole), so this is also the
  faster path, not the slower one.
- **Rename detection.** For size-matched delete/add pairs, hash a head+tail
  sample from the stick and on a manifest match copy the bytes from the
  *local* archive with a fresh local header — avoids re-reading a renamed
  20 GB folder from USB. Dead space is still unavoidable (the local header
  embeds the name).
- **NFC/NFD-aware path comparison** in the stat-diff (a `pathCompareKey()`
  seam exists), so a stick moved between macOS and Windows/Linux does not
  re-read every non-ASCII filename once. Needs a validated normalization
  table or a small dependency, and a macOS build to test against.
- **Scheduled background full verify** against manifest hashes, throttled,
  to catch media rot on the local disk months later.
- **Read-ahead pipeline** for the stat walk and parallel reads on UASP
  sticks — only if per-file latency turns out to dominate in practice.
- **Full read-back verify on the initial backup** doubles its local I/O;
  keep it but report progress ("Verifying...") rather than running silently.

## Stick-to-stick clone (chained), added 2026-09-06

"Create Backup USB Stick from A" onto an empty stick B, and "Update B
from A" when B holds an older copy of A's library, are **a backup of A
followed by a restore onto B** (`application::CloneStick`). No second
copy engine: the write discipline, the journal, the database-set
handling and the free-space checks are the ones above, and the disk
backup of A that is left behind is what a backup stick is for. A direct
filesystem-to-filesystem copier would save the double I/O and belongs in
the optimization pass, not here.

What the stick list decides (`adviseStickBackup`, every mounted stick
seeing the others as peers):

- *Same library*: content fingerprint verdict Same or SameCollection-
  DifferentState. *In sync*: identical DB-set fingerprints (Engine), or
  identical content fingerprints (rekordbox-only sticks carry no SQLite
  database).
- *Newer*: the catalog's mtime (max over export.pdb, m.db, m.db-wal,
  hm.db; `libraryCatalogModifiedAt`) with a 2 s tolerance for FAT; a
  disk backup's creation time. Fingerprints say same or different, never
  which side moved. A stick that is itself the newest copy gets no offer.
  The disk backup being newer than the stick is the new `BehindBackup`
  state: update the stick, not the backup.
- *Diverged*: both this stick and the peer differ from the disk backup
  they both match, and both are newer than it. Flagged, still offered
  (the typed confirmation stays), never hidden.
- *Space*: used bytes of the source volume against the target's free
  bytes plus the restore margin; the clone page measures again.

Cancel during the backup step keeps the partial archive (the next run
resumes); a backup outcome without the database (DJ software appeared,
database too large or unstable) stops before the target is touched. The
restore planner now compares the manifest's DbSetFingerprint against the
target's live database: size + mtime alone missed a database that
changed within the same 2 s and kept its size (found by the clone test).

Live checks before graduating, on two scratch sticks: A with a library,
B formatted with a stray file -> B's card says "Create Backup USB Stick
from A"; run it -> `A.zip` appears, B holds the library, the stray file
survives (overlay), B reads as in sync, Engine DJ or a player opens B.
Add a cue on A -> B shows "Update from A"; run -> incremental backup,
small restore, in sync again. Edit B instead -> A shows "Update from
B". Edit both -> the diverged warning. Insert only B with `A.zip` on
disk -> "Restore" as before; touch A's archive newer than B's catalog
-> "Update from backup" via the restore page.
