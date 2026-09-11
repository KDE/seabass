<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Testing against real libraries

Seabass edits other people's music libraries. Synthetic fixtures prove the
code does what we think; only real libraries prove it survives what people
actually have. This describes the corpus of donated libraries, what each of
the three kinds of test needs from it, and how to extend both to every
cleaning and sync function in the app.

The three purposes, in the order they matter:

1. **Data integrity** — does the code produce correct results?
2. **Stability** — does it survive real-world data without falling over?
3. **Speed** — is processing inside a roughly acceptable envelope?

Every one of these must be automated. See the project principle in
`docs/testing.md`: a test somebody has to perform by hand will not get
performed.

## Before anything else: the anonymizer does not keep its promise

`MANIFEST.txt`, which is what a contributor reads before deciding to send
their library, states: *"REMOVED entirely: artwork images, the detailed
colour and scrolling waveform data ... original file paths."*

That promise is not kept. Verified directly against the committed,
already-published fixture in this repository:

| What leaks | Extent | Why |
|---|---|---|
| Real file paths inside every analysis file | **2744 of 2744** `.DAT` and `.EXT` files | The ANLZ anonymizer strips waveform sections and rewrites cue comments, but never touches the `PPTH` path section. The path reads `/Contents/<Artist>/<Album>/NNN_<Artist>-<Title>.mp3` |
| Engine `Track.filename` | all 1376 rows, 1154 distinct shapes | Only `path` is rewritten; `filename` is a separate column nothing writes |
| Engine `Track.album`, `genre`, `label` | 1375 / 12 / 12 rows | Never in the scrub list |
| `exportLibrary.db` | the whole 688 KB database | Swept in by the blanket copy of the `rekordbox/` directory. Encrypted, but with a key this project's own source derives, so anyone with the app can read the complete real library out of it |
| `exportExt.pdb` | 73 KB | Same blanket copy; holds the My Tag vocabulary |

Only `title`, `artist` and `path` are actually scrubbed, and only in the two
databases, not in the analysis files beside them.

Two claims I could not reproduce, and which should not be treated as fact
without further checking: real paths surviving in tombstoned `export.pdb`
rows, and a device volume label in `AlbumArt.hash`.

**What this means in practice.** The published fixture is the maintainer's
own library, so nothing has been leaked that its owner did not choose to
publish. The risk is entirely forward-looking: the flow is currently
soliciting other people's libraries under a promise it does not keep. Fix
the anonymizer and the verifier before accepting another submission, and
re-generate the committed fixture afterwards.

The fix is small in each case: scrub `PPTH` the way cue comments are already
scrubbed in place (the path is length-constrained UTF-16, exactly the
problem `obfuscateCueComments` already solves); add `filename`, `album`,
`genre` and `label` to the Engine scrub; and copy only the files the export
actually needs instead of the whole `rekordbox/` directory. Then add the
verifier described below, so the promise is checked rather than trusted.

## The corpus

Libraries donated through `seabass-cli anonymize` (the "help test Seabass"
flow). The committed one is `tests/fixtures/anonymized_library`, 37 MB,
1118 rekordbox tracks and 1376 Engine tracks. Further sets live outside the
repository and are pointed at by an environment variable, so the repo does
not grow by a gigabyte per contributor.

### What anonymizing keeps, and what that costs a test

Kept as-is, per the export's own manifest: file size, bitrate, duration,
BPM, key, hot and memory cue positions and colours, rating, play count,
last-played date, streaming flag, playlist membership and position, the
monochrome waveform preview, and the beatgrid. Replaced with placeholders:
title, artist, comment, cue comments, file path, playlist names. Removed:
artwork, the detailed waveform data this app never reads, original paths.

Verified on the committed fixture: Engine artists are `Artist <n>` for all
1376 rows in exactly one structural shape, titles are `<id> Track`, paths
are an eight-letter directory plus a numeric filename. The shape is
preserved (739 distinct artists across 1376 tracks) even though the content
is not.

**The cost is that title, artist and path are exactly the fields the
matching logic keys on.** `SyncLibraries` and `DuplicateTrackFinder` pair
tracks by title, artist and duration; Clean Up groups duplicates the same
way. Against anonymized data those matchers run on synthetic strings whose
fuzziness is nothing like real punctuation, feat. spellings, remix
suffixes, accents or case. A matching regression will not necessarily show
up here. Duration, BPM, key and file size are real, so anything keyed on
those is genuinely exercised.

## Does the current data suffice? Partly, and here is the evidence

### Integrity: yes for cue and structural work, no for matching

`tests/anonymized_fixture_integration_test.cpp` already asserts at real
scale: scan counts for both formats, statistics aggregates, a sync matching
floor of 95%, the consistency checker's classification, a real hot-cue
write that round-trips, and the orphaned-file deletion chain. That is a good
base, and it is the right shape.

What it does not touch, from a sweep of the use cases it names:
`DuplicateTrackFinder`, `DuplicateCleanupPlanner`, `ConsolidateDuplicateCues`,
`LibraryCleanupWriter`, `LibdjinteropEngineCueWriter`, `OneLibraryCueWriter`.
So Clean Up, Duplicates, stray-cue removal and both non-rekordbox write
paths have **no real-data coverage at all** today.

### Stability: no, and this is the sharpest gap

The proposed Engine optimisation (one `track::update(snapshot)` per item
instead of three setters) was rejected because `track::snapshot()` throws
for **30 of 50 tracks** on the RV2 stick, a libdjinterop decoder failure on
real performance-data blobs.

Neither the committed fixture nor the newly collected set reproduces it:

| Engine data set | Tracks refused by snapshot + update |
|---|---:|
| RV2 stick (live) | 30 of 50 |
| Collected set, raw Engine copy | 0 of 50 |
| Committed anonymized fixture | 0 of 50 |

All three are Engine schema 3.0.2, so the schema is not the differentiator;
the libraries simply differ. **One data set cannot establish stability.**
The corpus needs to span several libraries, and the harness needs to run
every set rather than one hardcoded path.

Note what those numbers measure: the *rejected* snapshot-and-update path.
The setter path the app actually uses already catches the one throw it
meets (`sample_rate()` on a short data blob) and writes anyway. Measured on
2026-09-07 with the first corpus runner: **0 of 50 writes refused** on both
the committed fixture and the RV2 set, while 27 tracks of the RV2 sample
warned about the blob and were written regardless. So the first baselines
are zero, and the baseline mechanism earns its keep on other people's
libraries, not on these two.

### Speed: no, and a corpus alone can never answer it

Timing is a property of the medium, not of the data. The same Engine write,
same code, same library:

| Where the database sits | Per item |
|---|---:|
| On the USB stick | 151.0 ms |
| On a ramdisk | 0.8 ms |

That is a factor of 190. A corpus test running from `/tmp` measures nothing
about how the app behaves on the hardware it exists to serve.

**So do not assert wall-clock seconds in the test suite.** Assert *work
counts* instead: how many times a save opens a database, parses
`export.pdb`, or writes a file. Those are deterministic, portable, fast,
and they catch exactly the regressions that matter here — every performance
bug found so far was "this is done once per item instead of once per save".
Wall-clock envelopes belong in the benchmark tools, run against a real
medium, and are recorded in `docs/write-path-performance.md`.

## The plan, in handover shape

Everything below is written so that it can be implemented without the
conversation that produced it. Decisions are recorded as decisions, with
the reason, and with the one place to change if the decision is wrong.
Paths are exact. "Today" means the tree as of 2026-09-07.

### What is already in the tree

- `src/infrastructure/work_counters.{hpp,cpp}`: a process-wide
  `WorkCounters` singleton with four relaxed atomics (Engine database
  opens, SQLCipher opens, `export.pdb` parses, durable whole-file writes),
  `snapshot()`, `reset()`, and `describe()`. Always compiled in; a counter
  behind a build flag is a counter nobody reads.
- The four increments, one per hot operation, each at the single choke
  point every caller goes through:
  `LibdjinteropEngineCueWriter` (both `load_database` calls),
  `SqlCipherDb::SqlCipherDb`, `pdb_lookup.cpp` (the Kaitai parse), and
  `durable_file_write.cpp` (`writeFileDurablyAtomic`).
- `tests/corpus_test.cpp`, registered as `corpus_test` with the
  `integration` label, working directory the source root. It discovers the
  committed fixture plus every subdirectory of `$SEABASS_CORPUS` that holds
  either layout (`rekordbox/` + `engine/` from the anonymizer, or
  `PIONEER/` + `Engine Library/` from `tools/extract_testdata`). Per set it
  runs one rekordbox cue round trip, an Engine write sample of 50 with
  per-track refusal counting against `REFUSAL-BASELINE.txt` beside the
  set, and a 20-item Engine work count. First run: both sets pass, 9 s for
  the 620 MB RV2 set, baselines `0 0`.
- `tools/extract_testdata.cpp`: copies a stick's metadata verbatim into
  `<dest>/<name>-<date>/` with a `.gitignore` of `*` and a `SET.txt`.

### Decision 1: the change classes move out of the controllers

**Problem.** Five of the nine functions in the matrix are orchestrated by
`PendingChange` subclasses that are file-local classes inside
`src/gui/*_controller.cpp`. A test cannot construct what it cannot name.

**Decision.** Move the ten classes, unchanged in behaviour, into
`src/gui/edit/changes/`, one header and source pair each, and build them
together with `save_context.cpp`, `save_loop.cpp` and
`format_write_session.cpp` into a static library `seabass_edit` that links
`seabass_core` and `Qt6::Core` only. The corpus runner links that library.
No event loop is needed: `format_write_session_test` already runs the
save-context code this way.

Not chosen: relocating the orchestration into `application/`. The classes
depend on `QString` and on `SaveContext`, which lives in `gui/edit`; moving
them further would be a larger change for no test benefit, and every one of
them already meets the rule that matters (state travels in by value, no
controller pointer, no signal). Verified: none of the ten references a
controller, `QObject`, `QPointer` or `emit`.

The ten, with where they are today:

| Class | Today | Constructor takes |
|---|---|---|
| `AddCueChange` | `add_cue_controller.cpp:37` | format, path, sourceId, positionMs, kind, hotCueNumber, ... |
| `CleanupGroupChange` | `cleanup_controller.cpp:558` | format, path, `DuplicateCleanupPlan`, itemCountHint |
| `CopyCuesChange` | `duplicates_controller.cpp:246` | format, path, groupKey, `DuplicatesCopyOp` |
| `RepairIssueChange` | `library_consistency_controller.cpp:456` | path, `LibraryConsistencyIssue`, itemCountHint |
| `DeleteOrphanChange` | `library_consistency_controller.cpp:569` | path, `LibraryConsistencyIssue` |
| `RemoveJunkCueChange` | `library_consistency_controller.cpp:646` | path, `domain::Track` |
| `MergeCuesChange` | `local_cue_controller.cpp:290` | format, path, `RestoreCandidate` |
| `DeviceSettingChange` | `settings_controller.cpp:84` | pioneerRoot, fileName, fieldLabel, oldValue, ... |
| `SyncPlanChange` | `sync_controller.cpp:622` | rekordboxPath, enginePath, `SyncPlan`, itemCountHint |
| `RestoreBackupsChange` | `edit/library_edit_session.cpp:24` | (stays where it is; it is the session's own) |

Each class calls a few helpers from its controller's anonymous namespace
(`junkKeyFor`, `cuesWithoutJunk`, `extAnlzPath`, `pathForFormat`,
`writeCuesForPath`, `makeReporter`, `oneLibWriter`, and similar). Move each
helper with the class that uses it, into the class's own source file, or
into `src/gui/edit/changes/change_helpers.{hpp,cpp}` when two classes share
one. The compiler lists them. Types the constructors take
(`DuplicatesCopyOp`, `RestoreCandidate`, `SyncPlan`) that are declared in
controller headers move to headers under `gui/edit/changes/` too, and the
controllers include them from there.

The controllers keep their `stage()` methods; they only lose the class
bodies. `src/gui/CMakeLists.txt` and the root `CMakeLists.txt` entries that
list the four `edit/*.cpp` files directly are replaced by linking
`seabass_edit`. Every existing test must still pass afterwards, with no
behaviour change: this step is a move.

### Decision 2: a refused item aborts the save, and the runner asserts that

**Problem.** The stability contract was drafted as "refuse one item and
the rest still lands". `runSaveLoop` does not do that: the first failing
change stops the loop, is reported in `failedId`, and it and every change
after it stay pending. Everything before it is on the stick.

**Decision.** Keep abort-on-first-failure. It is already documented on
`PendingChange` ("every change is either fully on the stick or still
pending, never half-written"), it keeps the retry story simple (the
per-item writers are idempotent, so Save again re-applies what is left),
and the measured refusal rate on the real write path is zero. The corpus
runner asserts exactly these semantics when it drives a batch through
`runSaveLoop` with one change made to fail: the changes before it read
back, the failed one and those after it do not, and `appliedIds` lists
precisely the ones that did.

If skip-and-report is wanted instead, the change is in one place:
`save_loop.cpp`, replace the `break` on `!outcome.ok` with recording the id
in a new `SaveLoopResult::failedIds` and continuing, then update the
summary text in `LibraryEditSession` and this assertion. Do not do both.

### Decision 3: raw sets are the only coverage for matching

Anonymized data cannot exercise `SyncLibraries` or `DuplicateTrackFinder`
matching, because title and artist are placeholders. The extracted sets
have real strings. So the Sync and Duplicates integrity cases run only on
sets found under `$SEABASS_CORPUS`, and the runner prints a line saying
they were skipped when the corpus is empty, rather than passing quietly.

### Decision 4: the corpus runner absorbs the fixture test

`tests/anonymized_fixture_integration_test.cpp` has eight cases that all run
against one hardcoded set. Move each into `corpus_test.cpp` as a per-set
case, keeping its assertions and its numbered message, then delete the old
executable and its CMake entry. The `SEABASS_SOURCE_DIR` compile
definition goes with it; the runner already finds the fixture by relative
path from the source root, which ctest sets as its working directory.

### Step-by-step

Each step leaves the tree building and every test passing.

1. **Extract the change classes** (Decision 1). Pure move. Verify with
   `ctest --test-dir build -LE integration -j16` and the QML suite.
   **Done**, commit 651b8a86.
2. **Fold the fixture test into the corpus runner** (Decision 4). **Done.**
   All eight cases now run per set, and `anonymized_fixture_integration_test`
   is gone. The counts they hardcoded to the committed fixture (1370 tracks,
   188 cues, and so on) moved into a `SET-EXPECTATIONS.txt` beside each set,
   written the first time a set is seen and asserted every time after, so a
   real library nobody inspects by hand gets the same regression guard. Two
   cases are conditional rather than universal: the placeholder-collision
   check is meaningless on a raw set, where duplicates are real, and the
   matching case states the 95% floor only for anonymized sets while every
   set records its own matched count as a floor that must not drop. Case 7
   also had to change one thing to be safe on real data: it writes its own
   stand-in file inside the scratch tree and points the manifest at that,
   because a raw set's audio paths point at files that really exist.
3. **Add the collection guard.** **Done.** `tools/verify_anonymized_export.cpp`,
   Qt-free, linked against `seabass_core`, taking a zip or a directory. It
   fails if anything exists outside `MANIFEST.txt`, `rekordbox/` and
   `engine/`; if any `.DAT`/`.EXT` PPTH path is not of the form
   `/Contents/<hex>.mp3`; if a sample of 200 Engine rows has a `title` not
   matching `^[0-9a-f]+ Track$`, an `artist` not matching `^Artist [0-9]+$`,
   or a `filename` not equal to the last path segment; and if the rekordbox
   tree holds anything but `export.pdb` and the four settings files. Run
   it from `AnonymizeLibrary::execute()` before zipping, and refuse to
   produce the zip when it fails. Add it as a ctest against the committed
   fixture. Until step 4 lands, that test is expected to fail on the
   committed fixture, so register it after step 4.
4. **Regenerate the committed fixture** with the fixed anonymizer. **Done:**
   regenerated from the RV2 set, 1161 rekordbox tracks and 1564 Engine
   tracks, 5976 analysis files, 29.2 MB (the old one was 37 MB despite
   holding fewer analysis files). It verifies clean, and the verifier is
   registered as `anonymization_verifier_test` under the integration label.
   The leak table at the top of this document describes the fixture that
   was replaced, and is kept as the record of what went wrong.

   Original instruction, for reference: **regenerate the committed fixture** with the fixed anonymizer
   (`seabass-cli anonymize` from the RV2 set), replace
   `tests/fixtures/anonymized_library`, and commit. The old fixture carries
   every leak in the table at the top of this document.
5. **Write the OneLibrary anonymizer.** **Done:**
   `infrastructure/onelibrary/onelibrary_anonymizer`. It scrubs titles,
   paths, filenames, comments, artists, albums, genres, labels, playlist
   names and cue comments, points artwork rows at a removed placeholder,
   and VACUUMs so the old text does not survive in the database's free
   pages. Placeholders key on the real filename, the same key the other two
   anonymizers use, so one real track gets the same placeholder in all
   three catalogs. Schema differences are handled by asking `PRAGMA
   table_info` what exists rather than assuming.

   `exportLibrary.db` is therefore back in the export, scrubbed, and the
   committed fixture carries it: all nine matrix rows now run against the
   fixture rather than eight, and OneLibrary has real-data coverage for the
   first time. `exportExt.pdb` is still excluded and still has no
   anonymizer.

   Original instruction, for reference: write the OneLibrary anonymizer,
   `src/infrastructure/onelibrary/onelibrary_anonymizer.{hpp,cpp}`: open
   `exportLibrary.db` with the project's own key derivation, and for every
   row apply the same `anonymizationPlaceholder(kind, realKey)` mapping the
   other two anonymizers use, so a track's title is the same placeholder in
   all three catalogs (the sync tests depend on that). Columns to scrub are
   whatever `OneLibraryCueWriter` and the reader touch plus every free-text
   column found by `PRAGMA table_info`; when in doubt, scrub. Then put
   `exportLibrary.db` back into the export and extend the guard to sample
   it. This is what gives the OneLibrary write path its only real-data
   coverage.
6. **Add `--zip` to `tools/extract_testdata`.** **Partly done:** the flag
   exists, the tool is a CMake target linking `seabass_core`, and the corpus
   runner reads a zipped set by unpacking it into scratch first, which
   needed a real zip reader (`infrastructure/zip_archive_reader`) since the
   project only had a writer. A zipped set keeps its recorded numbers in
   `<name>-EXPECTATIONS.txt` beside the archive, because the unpacked copy
   is deleted at the end of the run. Verified end to end by zipping the RV2
   set and running the whole corpus against the archive: same results as the
   directory form. **Still to do:** extract Sebastian's own live stick.
   WHALESHARK was not plugged in when this ran; the only stick present was
   a CORSAIR that holds a rekordbox catalog and no Engine library, and which
   appears to belong to someone else, so nothing was extracted from it.
   Delete `~/Seabass/testdata/whaleshark-sdcard-2026-08-31.zip` once the
   fresh extraction supersedes it: it holds raw `PIONEER/` and
   `Engine Library/` copies beside an anonymized tree carrying the old
   leaks. The empty `roy-corsair/` directory is now reported as skipped by
   name rather than passed over in silence.

   Original instruction, for reference: add `--zip` to `tools/extract_testdata`, using
   `infrastructure::writeZipArchive`, so a set can be produced as a single
   file. Link the tool against `seabass_core` for it (it is standalone
   today). Then extract the maintainer's own live stick:

   ```
   ./build/extract_testdata /media/sebas/WHALESHARK ~/Seabass/testdata whaleshark --zip
   ```

   producing `~/Seabass/testdata/whaleshark-<date>.zip`. Not anonymized;
   it is the maintainer's own library and stays on this machine. The
   runner must then also accept a `.zip` set by unpacking it into the
   scratch directory first. Delete
   `~/Seabass/testdata/whaleshark-sdcard-2026-08-31.zip` afterwards: it
   holds raw `PIONEER/` and `Engine Library/` copies beside an anonymized
   tree with the old leaks, and the new extraction supersedes it. The
   empty `roy-corsair/` directory is skipped by the runner today; make the
   runner print what it skipped and why.
7. **Fill the matrix** in the corpus runner. **Done:** all nine rows exist
   in `tests/corpus_test.cpp`, each staging its change class through
   `runSaveLoop` and asserting through a fresh reader. Eight run against the
   committed fixture; the orphaned-OneLibrary-row case needs a OneLibrary
   database, which only the real sets have until step 5 lands. OneLibrary
   itself now gets read and counted per set (case 2b), so the catalog the
   plan called out as having no coverage at all has some.

   Measured counts, recorded per set and asserted from then on. Against the
   committed fixture, which has no OneLibrary database:

   | Feature | pdb parses | SQLCipher opens (fixture / real stick) |
   |---|---:|---|
   | Add cue | 2 | 0 / 2 |
   | Stray cue removal | 2 per track | 0 / 1 per track |
   | Library Health repair | 2 | 0 / 4 |
   | Clean Up duplicates | 2 | 0 / 4 |

   The SQLCipher column is the 235 ms per item from
   `docs/write-path-performance.md`, now a portable number. It is zero on
   the fixture only because the fixture has no OneLibrary mirror, which is
   itself a reason to finish step 5.

   Original instruction, for reference: fill the matrix in the corpus runner, one case per row, each built
   the same way: copy the set into scratch, build a `SaveContext` over the
   scratch paths, stage the change class from step 1, run `runSaveLoop`,
   then construct a **fresh reader** over the scratch paths and assert on
   what it returns. A test that trusts the writer's return value is not a
   test. `WorkCounters::instance().reset()` before the loop and
   `snapshot()` after, and assert counts.
8. **Tighten the work-count expectations** as the performance work in
   `docs/write-path-performance.md` lands. The table below records both
   the count today, which the runner asserts now so that nothing gets
   worse unnoticed, and the target, which becomes the assertion when the
   corresponding optimisation is merged. Change the number in the test in
   the same commit as the optimisation.

### The matrix, with counts

Per save of N items, on a set that has all three catalogs.

| Function | Change class | Integrity assertion via fresh reader | Engine opens today / target | SQLCipher opens today / target | pdb parses today / target |
|---|---|---|---|---|---|
| Add cue | `AddCueChange` | the cue reads back at the same position and colour; a second write to the same hot slot replaces, count stays 1 | N / 1 | 2N / 1 | 2N / 1 |
| Stray cue removal | `RemoveJunkCueChange` | zero junk cues after save; exactly the original count after `RestoreBackupsChange`; Engine main cue cleared when the removed cue was the only one | N / 1 | 2N / 1 | 2N / 1 |
| Sync cue points | `SyncPlanChange` | every planned cue lands on the target and reads back equal; a conflict row is untouched on both sides | N / 1 | 2N / 1 | 2N / 1 |
| Copy cues between duplicates | `CopyCuesChange` | destination has the union; source unchanged | N / 1 | 2N / 1 | 2N / 1 |
| Clean Up duplicates | `CleanupGroupChange` | survivor has the union of cues and every playlist entry of the removed rows; every removed row is in the manifest and absent from a fresh scan | 1 / 1 | 0 | 1 / 1 |
| Delete orphaned files | `DeleteOrphanChange` | only files no live row references are gone; manifest cleared for exactly those | 0 | 0 | 1 / 1 |
| Library Health repair | `RepairIssueChange` | the repaired row resolves to an existing file; an ignored issue is byte-identical | 0 | 0 | N / 1 |
| Local cue restore | `MergeCuesChange` | restored cues equal the backed-up set | N / 1 | 2N / 1 | 2N / 1 |
| Device settings | `DeviceSettingChange` | the field reads back; every other byte of the file identical | 0 | 0 | 0 |
| Save loop | one deliberately failing change | Decision 2's semantics | - | - | - |

The "today" numbers are derived from the code paths in
`docs/write-path-performance.md`; the first run of each case is what
confirms them, and a mismatch on the first run means the derivation was
wrong, not the code. Record the confirmed number in the test and in this
table.

Durable file writes are not in the table because their count is the
correct one already: one per file the save touches, plus one per backup.
Assert that too, per case, as "touched files + backups", so a change that
starts rewriting files it does not need to is caught.

### What the runner must do on Windows and macOS

Nothing in the runner or the change classes is platform-specific: it is
`std::filesystem`, SQLite and the project's own readers. Three practical
points:

- `SEABASS_CORPUS` is read with `std::getenv`; on Windows point it at a
  directory on the system disk. The scratch copies go to
  `std::filesystem::temp_directory_path()`, which is `%TEMP%` there.
- A 620 MB set is copied three times per run today. When the matrix is
  full it will be copied once per case. Copy once per set into scratch and
  give each case its own sub-copy from that local copy; this keeps the run
  under a minute on an SSD and matters more on Windows, where per-file
  overhead is higher.
- Two libdjinterop Boost tests fail to link on Linux already and one
  OneLibrary test fails on Windows for a staleness-guard reason that is
  still open; neither is this runner's concern, but a Windows run of
  `ctest -L integration` should be recorded in `docs/testing.md` once it
  passes there.

### Verification

```
cmake --build build -j16 -- -k
ctest --test-dir build -LE integration -j16
SEABASS_CORPUS=$HOME/Seabass/testdata ctest --test-dir build -L integration --output-on-failure
QT_QPA_PLATFORM=offscreen ./build/seabass_qml_tests -input tests/qml
```

The integration run must print one block per set, name every skipped case
and why, and end with the counts table. A green run against an empty
corpus is allowed but must say so.
