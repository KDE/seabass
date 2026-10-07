<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Rekordbox OneLibrary / Device Library Plus format notes

Seabass reads and writes `<PIONEER root>/rekordbox/exportLibrary.db`
("OneLibrary", also called "Device Library Plus") on a stick. Writing
(`OneLibraryCueWriter`) happens alongside the older `export.pdb` writer
(`src/infrastructure/rekordbox/pdb_row_writer.*`) as a secondary,
best-effort mirror: see its class comment
(`src/infrastructure/onelibrary/onelibrary_cue_writer.hpp`) for why it
never rolls back a primary export.pdb/m.db write that already succeeded.
Reading (`OneLibraryReader`,
`src/infrastructure/onelibrary/onelibrary_reader.hpp`) is a normal,
independent `LibraryReader` implementation. Seabass's Browse Library
page uses it directly for read-only OneLibrary browsing, not routed
through the writer at all.

## What's confirmed

Everything below was verified empirically against a real `exportLibrary.db`
copied read-only from a real stick during development, cross-checked
against [pyrekordbox](https://github.com/dylanljones/pyrekordbox)'s source
(the closest thing to a reference implementation for this format).

- **Location**: `<PIONEER root>/rekordbox/exportLibrary.db`.
- **Encryption**: SQLCipher, using SQLCipher 4's own compiled-in defaults
  (kdf_iter, page size, HMAC/KDF algorithm), no `PRAGMA` overrides
  needed. Confirmed by decrypting a real file with exactly `PRAGMA key =
  '<derived key>';` and nothing else, via the `sqlcipher` CLI
  (`mingw-w64-ucrt-x86_64-sqlcipher`).
- **The key is universal**, not per-license or per-machine. The same
  key opens every OneLibrary export. Rekordbox obfuscates it in its own
  binary (base85 encode, XOR against the ASCII bytes of the string
  `"657f48f84c437cc1"` cycling per byte, zlib-compress) purely to avoid
  it appearing as a plaintext string in a binary scan, not as any kind
  of per-user secret. `onelibrary_key.cpp` reverses exactly this,
  cross-checked against pyrekordbox's `utils.py` (`deobfuscate()`) and
  `devicelib_plus/database.py` (the `BLOB` constant) source.
- **Schema**: `content` (tracks, keyed by `content_id`), `cue`
  (`content_id` FK, `kind`, `inUsec`/`outUsec` microsecond positions,
  plus several legacy frame-addressing columns, see "What's NOT
  handled" below), `hotCueBankList` / `hotCueBankList_cue` (a newer
  hot-cue-grouping feature, not required for a cue to work as a hot cue,
  see below), `playlist` / `playlist_content`. Table and column names
  confirmed directly via `.schema` against the real file, not just from
  pyrekordbox's docs.
- **`content_id` is a separate id space from export.pdb's track id.**
  The same file can have different numeric ids in each database (seen
  directly on a real stick: one track was `export.pdb` id 578 but
  OneLibrary `content_id` 566). `OneLibraryCueWriter` therefore matches
  tracks by file path (`content.path`, stick-root-relative,
  forward-slashed, e.g. `/Contents/Artist/Track.mp3`) rather than
  reusing a `sourceId` from the rekordbox side, the one identifier the
  two databases actually share.

## What's now confirmed via real read-side data

- **`cue.kind`'s exact meaning - confirmed.** The stick used during
  later development turned out to have real, pre-existing (genuinely
  rekordbox-written, not Seabass-written) cue data once
  `OneLibraryReader` was built to read the `cue` table back out
  (`src/infrastructure/onelibrary/onelibrary_reader.cpp`): `kind = 0` for
  a memory cue, `kind = 1..8` for a hot cue in that slot. Cross-checked
  directly against `export.pdb`'s own cues/playlist positions for the
  one track present in both catalogs with real cue data ("Voices In My
  Head") - exact match. This confirms the same convention
  `OneLibraryCueWriter` already assumed when writing (see below), no
  longer just the inferred-from-`master.db`-precedent reasoning this
  section used to describe.

## What's inferred, not confirmed - re-verify before fully trusting

- **`colorTableIndex`.** No color-lookup table exists anywhere in this
  schema (checked: none of the 26 real tables is a color palette), and
  no documentation of what indices map to what colors was found.
  Seabass always writes `0` rather than fabricate a mapping from
  `CuePoint::color`'s hex string. Cosmetic only - doesn't affect cue
  position/hot-cue-number correctness.

## Where a player takes cues from (2026-10-03, issue #59)

Measured with WHALESHARK2: the reads on a CDJ-3000X (3 October 2026), the
write on an OMNIS-DUO (1 October 2026). Evidence in
`project/evidence/omnis-cue-source-2026-10-01/`. The reads were first
written up as OMNIS-DUO results; that was wrong, the player was the 3000X.

- **Playlists and track identity come from `exportLibrary.db`** (CDJ-3000X).
  The player listed the playlists exactly as OneLibrary stores them,
  including a track DeviceLibrary's copy of that playlist lacks.
- **Cues come from the track's analysis file** (CDJ-3000X), the one
  `content.analysisDataFilePath` names (for example
  `/PIONEER/USBANLZ/P06D/0001F5E1/ANLZ0000.DAT`, with its `.EXT`). That
  is the same file `export.pdb`'s `analyze_path` names when DeviceLibrary
  holds the track. Where the `cue` table and the file disagreed, the
  player showed the file; a track with no `cue` rows showed the file's
  cues. And it showed the file's **legacy `PCOB` list**: on a file whose
  PCOB hot list held one pad and whose PCO2 list (and `cue` table) held
  five, the 3000X showed one, like the XDJ-RX2 (#33) and rekordbox 7
  (which showed the file in both of its library branches). See #60 for
  files whose two lists disagree.
- **Pads stored on the player went into the analysis file only** (OMNIS-DUO,
  1 October; the 3000X's write side is not measured yet). The
  `cue` table was left alone and `content.cueUpdateCount` did not move;
  the database gained only play history (`history`, `history_content`).

What Seabass does with this:

- `OneLibraryReader` reads a track's cues from that analysis file, with
  the same code DeviceLibrary's reader uses (`readAnalysisFileCues()`),
  and never from the `cue` table.
- `OneLibraryCueWriter::writeCuesForPath()` writes the analysis file
  first, through `RekordboxCueWriter` (PCOB in the .DAT, PCOB and PCO2 in
  the .EXT, with its read-back), leaving a file that already holds the
  cues alone, and then keeps the `cue` table in step. The table is never
  read back as a source.
- A DeviceLibrary row and a OneLibrary row naming the same analysis file
  are one cue source. Sync pairs Engine with OneLibrary only for the rows
  no DeviceLibrary row speaks for (`oneLibraryRowsToPairWithEngine()`).

- Seabass builds before `afa3dbdb` could write a row's `cue` table on its
  own, with another copy's cues (#57). Library Health's "OneLibrary cue
  tables" check lists every row whose table holds a cue its analysis file
  does not (`domain::auditCueTables()`; an empty table, or one the file
  holds more than, is fine), and its repair sets that row's table to the
  file's cues, writing nothing else.

Still unverified: where a CDJ-3000X writes a pad the DJ stores (the
OMNIS-DUO writes the file), and what rekordbox desktop does with the
`cue` table on import. Until the second is known, the table is written,
not dropped.

## Legacy and modern cue lists (issues #55 and #60)

An analysis file holds every cue twice: in the legacy PCOB lists (hot
cues 1-3 and the memory cues in the `.DAT`, 4-8 in the `.EXT`) and in the
modern PCO2 lists in the `.EXT`. Measured 2026-09-30 to 2026-10-03, the
XDJ-RX2 (firmware 1.43) and the CDJ-3000X show the legacy lists while
Seabass reads PCO2 first, and an RX2 that saves a memory loop onto a
legacy memory list whose header says "empty" hangs and has to be powered
off.

Seabass builds left four kinds of damage in these files:

1. **Legacy memory list header**: entries under a header that is not the
   last entry's index, or entries not linked to each other. Written by
   Seabass from `5282555e` (2026-09-18) to `6e0f1c09` (2026-09-30), which
   includes the published 0.7.11 and 0.7.12 alphas; 0.7.13 has the fix.
2. **Player-rewritten memory list**: the RX2's rewrite of such a list,
   sized for one more entry than it holds, the extra slot zero.
3. **Lists disagree**: the legacy and modern lists hold different cues
   (a hot cue compared by pad, a memory cue by position, within the
   reader's 500 ms tolerance, since rekordbox itself stores one cue 1 or
   52 ms apart in the two). Written by Seabass from 2026-09-02 to
   `5282555e`, which wrote PCO2 alone. rekordbox's own exports disagree
   too, routinely, so a disagreement alone is not damage.
4. **Stray analysis file**: an `ANLZ000N.DAT` beside the track's that no
   catalog names, with no `.EXT` and an empty beat grid and waveform: what
   a hung RX2 leaves.

Seabass does not repair them. On 2026-10-04 the owner decided not to ship
in-app repairs and removed the Library Health check that looked for them
(on a real stick nearly all of its "disagree" findings were rekordbox's
own export, which its default repair would have rewritten). Users of the
0.7.11 and 0.7.12 alphas are advised to re-export their stick from
rekordbox; the next beta's release notes say so.
`tools/stick_damage_audit.cpp` counts the damage read-only, through
`legacy_memory_list_audit.hpp`, for a developer checking a stick.

## What's NOT handled (deliberately out of scope this pass)

- **Legacy frame-addressing columns** on `cue`
  (`in150FramePerSec`/`inMpegFrameNumber`/`inMpegAbs`/
  `inDecodingStartFramePosition`/`inFileOffsetInBlock`/
  `inNumberOfSampleInBlock`, and their `out*` counterparts) are left
  NULL. Computing them correctly needs per-track encoding parameters
  (sample rate, bitrate, frame size) this pass didn't implement. As of
  when this was written, OneLibrary is only used by newer hardware
  (OPUS-QUAD, OMNIS-DUO, XDJ-AZ, CDJ-3000X) that primarily reads the
  modern `inUsec`/`outUsec` fields, so this is expected to be a
  reasonable simplification rather than a functional gap, but wasn't
  verified against real hardware. **Measured 2026-10-02 on a CDJ-3000X:
  it took its cues from DeviceLibrary (`export.pdb` and the ANLZ files),
  not from `exportLibrary.db`: a OneLibrary row holding five pads showed
  the one pad its ANLZ file had.** See "Where a player takes cues from" above
  for the full 3000X read-side result and the OMNIS-DUO's write.
- **`hotCueBankList`/`hotCueBankList_cue`** (the newer named-bank
  hot-cue-grouping UI feature) is left untouched. A cue's hot-cue-ness
  and slot number live entirely in `cue.kind` per the above; bank
  grouping is an additional organizational layer on top that this pass
  doesn't populate.
- **Write precedent**: no evidence was found anywhere (GitHub issues,
  forums, pyrekordbox's own test suite. It has no `devicelib_plus`
  tests at all) of anyone else having written to this format and
  reported back, good or bad. Absence of horror stories isn't proof of
  safety. Treat this writer with at least the same caution Seabass
  already applies to its (better-precedented) `export.pdb` writer.

## Deleting a `content` row (`OneLibraryCueWriter::removeTrackByPath()`)

No declared `FOREIGN KEY`/`REFERENCES` constraint has been confirmed
anywhere in this schema (neither the doc's own informal "FK" language
above nor any test fixture built to match a real `.schema` dump includes
one), and `PRAGMA foreign_keys` is never issued for this connection
anywhere in this codebase (`SqlCipherDb`'s constructor doesn't set it,
neither does `OneLibraryCueWriter`). Even if a constraint did exist on
the real file, it would not be enforced without that PRAGMA, the exact
same class of bug this project already found and documented for
libdjinterop's Engine `remove_track()` (its own comment claims an
`ON DELETE CASCADE` that never actually fires).

Practical consequence: deleting a `content` row requires deleting every
dependent row explicitly first, in this order, inside one transaction -
`hotCueBankList_cue` (via a `cue`-id subquery, same as
`writeCuesForPath()` already does) -> `cue` -> `playlist_content` ->
`content`. Getting the order wrong (e.g. deleting `cue` before
`hotCueBankList_cue`) leaves orphaned bank rows silently behind, exactly
the failure mode `writeCuesForPath()`'s own comment already warns about
one level down. `removeTrackByPath()` follows this order, wrapped in the
same `BEGIN IMMEDIATE`/`COMMIT`/best-effort-`ROLLBACK` pattern
`writeCuesForPath()` established, with the same staleness guard and a
post-commit verification re-read.

## Retracted: the "corrupt hot-cue numbers" were a broken test

Recorded 2026-09-09 as an open data-integrity bug, and withdrawn the same
day. There is no bug here.

The claim was that a hot cue written into `exportLibrary.db` came back with
a hot-cue number of 22229, and 23220 on the next run of identical input,
where untouched rows read back as 1..8. The varying value was read as the
signature of something uninitialised in the writer.

It was uninitialised, but in the test. `corpus_test` took the address of an
element of the vector `OneLibraryReader::readAll()` returns by value:

    for (const auto &t : reader.readAll()) {
        if (matches(t)) { found = &t; break; }   // dangles after the loop
    }

Every read through that pointer afterwards produced plausible-looking
garbage -- a numeric field that changed between runs, and a filePath that
printed as binary, which is what finally gave it away.

Checked directly against a real fixture before concluding: the writer puts
`kind=2, inUsec=45000000` in the `cue` table for a hot cue in slot 2 at
45 s, the real schema stores the hot-cue number in `kind` exactly as the
writer assumes, and `OneLibraryReader` reads that row back as `hot#2@45000`.
Writer and reader are both correct, and `corpus_test` now asserts the slot
number, not only the position.

Worth keeping as a caution: "the value differs between runs" is good
evidence of uninitialised memory, but it does not say whose.

## Loops and colours on rewrite (2026-09-11)

`OneLibraryCueWriter::writeCuesForPath` replaces a track's whole cue set.
It now writes a loop as `isActiveLoop = 1` with its out point in
`outUsec` (a cue point keeps `outUsec == inUsec`), and the reader takes
`isActiveLoop`/`outUsec` back into `CuePoint::isLoop`/`loopEndMs`, so a
loop survives a OneLibrary round trip. A cue that keeps its slot and
position also keeps the `colorTableIndex` the stick had for it; the
domain model carries no OneLibrary colour, and a rewrite used to reset
every cue to colour 0. Bank membership (`hotCueBankList_cue`) is still
dropped on rewrite.
