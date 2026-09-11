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
  verified against real hardware.
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
