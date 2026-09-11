<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Testing

## Running the test suite

```
ctest                 # the whole suite, integration included -- this is the one to run
ctest -L integration  # just the integration suite (real-scale, does real file I/O)
ctest -LE integration # everything except the integration suite (faster, covers less)
```

A bare `ctest` runs **every** registered test. A CTest label does not
exclude anything by itself; only an explicit `-LE` does. (This section
used to say the opposite -- that a bare `ctest` skipped the integration
suite -- which was wrong in the direction that makes you run work twice.)

Nothing in the suite is allowed to pass without running. A missing test
dependency stops the **configure** with a message naming the package,
rather than quietly subtracting a target or turning a test into a skip:

| missing | you get |
|---|---|
| Qt6 | configure fails; `-DSEABASS_GUI=OFF` to build CLI + C++ tests only |
| Qt6 Test/QuickTest | configure fails; `-DSEABASS_TESTS=OFF` to build without any tests |
| python3 / unzip / 7z | configure fails; they are what `backup_archive_crossvalidation_test` checks our ZIP writer against |
| Boost.Filesystem | configure fails; libdjinterop's own suite needs it. `-DSEABASS_LIBDJINTEROP_TESTS=OFF` to build without those twelve |

libdjinterop is built from a pinned checkout under `third_party/`, not
linked as a system package, and every Engine write goes through it -- so
its twelve tests are coverage of code we ship and run alongside ours. A
bare `ctest` is 100 tests: our 88 plus those twelve.


`-DSEABASS_TESTS=OFF` is the only way to build without the suite, and it
is recorded in `CMakeCache.txt`, so skipping the tests is always someone's
stated choice rather than an accident of what happened to be installed.

Two tests need something the machine cannot be assumed to have, and both
are handled by *not registering* them unless you ask, never by passing
without checking anything:

- `stray_scan_live_test` needs a real stick: configure with
  `-DSEABASS_LIVE_STICK=/path/to/stick`. The binary is always built, so it
  cannot rot unnoticed; run without the variable it fails rather than
  reporting a scan that never happened.

Most of the suite is unit tests against small, synthetic, hand-built fixtures (a two-track `export.pdb`, a fresh `djinterop::engine::create_database()`, and so on) -- fast, and run by a bare `ctest`. One test, `anonymized_fixture_integration_test`, is tagged with the CTest label `integration` and runs the app's real use cases (`ScanLibrary`, `SyncLibraries`, `LibraryStatisticsCalculator`, `LibraryConsistencyChecker`, a real cue write) against `tests/fixtures/anonymized_library/` -- a committed, de-identified copy of a real ~1,400-track library. It's the only thing in this suite exercised at realistic scale and variety; run it before merging a larger change or cutting a release, not on every build. There's no CI in this repo (yet) to enforce that automatically -- this is a documented habit, not an automated gate.

## A new guard has to be seen failing

A test written from a review finding encodes the pre-fix behaviour by
construction, which is exactly why it can be written wrong and never
noticed: a case that cannot fail passes for the same reason a correct one
does. So before a fix is called done, its test is run against the code it
was written to catch.

The mechanics, for a fix commit `F`:

```
git worktree add --detach ../worktrees/guard F^
cd ../worktrees/guard
git checkout F -- tests/the_test.cpp        # the new case, the old code
cmake -G Ninja -B ../../builds/guard -DSEABASS_EXPERIMENTAL=ON
cmake --build ../../builds/guard --target the_test && ../../builds/guard/the_test
```

These are `assert`-based programs, so the first failure aborts and hides
the cases after it. To check a second case in the same file, delete the
block that already went red and run again.

Two things the run can tell you, and both are answers:

- **It fails on its own assertion.** The guard is real. Note *which*
  assertion -- a case that fails on the setup one line earlier has been
  proved to abort, not to guard anything.
- **It does not compile**, because the case asserts on a member or a
  function the fix introduced. Add the member to the old code, defaulted
  to what the un-fixed code effectively did (`false`, usually), and run
  again. The point is to see the assertion fail, not the linker.

And if it passes, the case is wrong and gets rewritten until it fails --
the failure scenario it was written from is usually narrower than the bug.
`open_stick_backup_test` case 5b is the worked example: written as "the
archive is replaced by something unreadable", which the un-fixed code
already survived because it refused before touching the cache. The
scenario that actually destroyed the cache was an archive that *reads*
fine and only then turns out to hold no catalog -- past the point of no
return. Rewritten that way it fails against the pre-fix code and passes
against the fix.

## The anonymized fixture

`tests/fixtures/anonymized_library/` holds a real rekordbox export and a real Engine Library, both de-identified: every track's title/artist/comment/filename/playlist name is replaced with placeholder text, artwork and detailed waveform-display data are stripped, but everything else (BPM, key, cue positions and colors, ratings, play counts, playlist structure, beatgrid) is real. See `MANIFEST.txt` inside that directory for the exact counts and field-by-field policy from when it was last generated, and `src/infrastructure/rekordbox/rekordbox_library_anonymizer.hpp` / `src/infrastructure/engine/libdjinterop_engine_anonymizer.hpp` for exactly what each step does.

It's generated by `seabass-cli anonymize` -- the same command described below for user submissions -- run once against a real stick and committed directly, since the output is already de-identified. To regenerate it (e.g. against a different or updated real library):

```
seabass-cli anonymize --rekordbox /path/to/PIONEER --engine "/path/to/Engine Library" \
    --out tests/fixtures/anonymized_library \
    --hardware "..." --notes "..."
```

If the track/cue counts change, update the exact numbers `anonymized_fixture_integration_test.cpp` asserts against (its own comment says as much).

One design point worth knowing if you're touching the anonymizer: the same real track's obfuscated title/filename comes out **identical** whether it's read from the rekordbox output or the Engine output, even though the two catalogs are anonymized in completely independent runs. That's deliberate -- `domain::TrackMatcher`'s primary cross-catalog matching signal is exactly the (normalized) filename, so if the two anonymizers assigned placeholders independently (e.g. a per-run sequential counter), the *same* real track would get *unrelated* obfuscated filenames in each catalog, and sync-matching tests against the fixture would look broken even though nothing in the real app is. See `src/infrastructure/anonymization_placeholder.hpp`'s own comment for the mechanism (a deterministic hash of the real filename, not a counter).

## Locks, sessions, and fake controllers

- `library_edit_lock_store_test` forks a child process (POSIX only) to
  hold a cookie, so the "owner is provably dead" staleness rule is
  tested against a real pid rather than a stub; the Windows build runs
  the remaining cases.
- `edit_session_save_loop_test` and `format_write_session_test` cover
  the save loop's cancel/failure points and the scratch-copy commit rule
  without Qt Quick (plain Qt Core).
- QML pages take their controllers and the edit registry as untyped
  properties (`property var controller`, `property var editRegistry`) so
  `tests/qml/tst_*.qml` pass plain JS objects with the properties and
  functions a test needs (see `fakeEditRegistry()` in
  `tst_StickListPage.qml`); the dialogs in `qml/common/` take a
  `session` the same way (`tst_EditModeDialogs.qml`,
  `tst_EditSessionHost.qml`). Pages guard their `Connections` with
  `ignoreUnknownSignals: true` for that reason.
- `SEABASS_SCREENSHOT_DIR=<dir> QT_QPA_PLATFORM=offscreen build/seabass_qml_tests -input tests/qml`
  saves a PNG per page the tests render; look at them for any visual claim.

## Live tests against a real stick

`tests/qml-live/` drives the real pages with their real controllers
against a mounted stick: reads, staged edits, saves, cancels, undo,
Manage Backups, the foreign lock, the process guard, the CLI probe, and
the stick being pulled. Nothing there runs under a default `ctest`; those
tests are registered only when the build was configured with
`-DSEABASS_LIVE_STICK` naming a mount point. Only ever
point it at a scratch copy of a library: it writes (through the normal
backup path, undoing where the flow has an undo).

```
tests/qml-live/run-live.sh /media/you/STICK /dev/sdX1 /tmp/shots
SKIP_PLAIN=1 tests/qml-live/run-live.sh ...   # only the orchestrated scenarios
```

The script runs one test function per process (a bare TestCase name
makes the QtQuickTest runner exit silently), plants a cookie owned by a
live `sleep` for the lock scenario, runs a copy of `sleep` named
`rekordbox` for the guard, calls `seabass-cli` while the test holds the
lock, and unmounts/remounts the device for the stick-pull scenario.
`LiveHelpers.js` has `findByType()` for reaching the controller a page
created for itself. Results and screenshots from the 2026-09-07 run on
the RV2 stick are noted in `docs/edit-mode-and-cancel.md`.

The QML test binary registers the same `SeabassGui` module as the app
(`SEABASS_GUI_*` lists in `src/gui/CMakeLists.txt`), so any page can be
instantiated in a test; `tst_PagesCompile.qml` checks that every page
at least compiles.

## Testing against real libraries

The donated-library corpus, what anonymizing keeps and what that costs a
test, and how the three purposes (integrity, stability, speed) map onto
every cleaning and sync function:
[`docs/real-data-testing.md`](real-data-testing.md). Short version: assert
work counts, not wall-clock seconds, because timing is a property of the
medium and not of the data.

## Measuring write performance

Benchmarking a save against real removable media has its own traps (a USB
stick's controller schedules its own garbage collection, so whichever thing
you measure first can look thirty times slower than the same thing measured
later). The method, the tools in `tools/`, and the running log of results are
in [`docs/write-path-performance.md`](write-path-performance.md).

## Submitting your own library for testing

If you'd like to help test Seabass against hardware or a library shape Sebas doesn't personally have, you can generate the same kind of anonymized export from your own stick and send it in:

```
seabass-cli anonymize --rekordbox [PATH] --engine [PATH] --out DIR \
    [--max-tracks N] --hardware "what you use" --notes "what you'd like tested"
```

- **Nothing is sent anywhere automatically.** This command only ever writes files to `DIR`. There is no networking code in this project at all; review what's in `DIR` yourself, then attach it (zipped) to an email to **sebas@kde.org** if you're happy with it.
- Every real track is included by default; pass `--max-tracks` if you'd rather send a smaller sample (the command prints the real output size and an estimated zipped size, so you can judge whether that's necessary before attaching it to an email).
- `MANIFEST.txt` in the output tells you exactly what's included and excluded; read it before sending.
- This data may be published as part of the project's test suite (the same way `tests/fixtures/anonymized_library/` is). If there's anything in your `--hardware`/`--notes` text you wouldn't want published, leave it out of those fields and mention it directly in your email instead.
