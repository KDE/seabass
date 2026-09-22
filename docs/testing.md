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
| Boost.Filesystem | configure fails; libdjinterop's own suite needs it. `-DSEABASS_LIBDJINTEROP_TESTS=OFF` to build without those fourteen |

libdjinterop is built from a pinned checkout under `third_party/`, not
linked as a system package, and every Engine write goes through it -- so
its fourteen tests are coverage of code we ship and run alongside ours: a
bare `ctest` runs them with ours, so subtract fourteen for the count of
this project's own. The fourteen only changes when the pinned checkout
does.


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

## The suite only sees what its platform can draw

The QML suite runs on a real display. `seabass_qml_tests` is registered
under `xvfb-run`, with `QT_QPA_PLATFORM=xcb`, `QSG_RHI_BACKEND=opengl`
and `LIBGL_ALWAYS_SOFTWARE=1`, and on Linux a missing `xvfb-run` or
Qt6 ShaderTools stops the configure rather than quietly narrowing the
suite. It used to run under `QT_QPA_PLATFORM=offscreen`, and the reason
that changed is worth keeping, because it is not "offscreen missed a
bug".

Offscreen falls back to the software scene graph. There, every
`ShaderEffect` draws nothing at all, and every glyph is rasterised
natively no matter what the application asked for. Both of those are
whole classes of question that could not fail in that configuration --
not questions that happened to go unasked.

The glyph half is the measurable one. Two full builds of the same tree,
differing only in whether `main.cpp` calls
`QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering)`:

- through the suite's own `grabImage()` under `offscreen`, the two
  builds' page screenshots came out byte-identical, same md5;
- as real `seabass` binaries under `xvfb` with the GL backend forced,
  they differed by 0 pixels out of 1,600,000;
- and Sebastian, looking at the same two builds on a 2256x1504 panel at
  Plasma's 1.5 scaling, could tell them apart immediately: the letters
  that had looked shaved off along the baseline stopped looking that
  way.

A suite that cannot distinguish two binaries a person can distinguish at
a glance is not a strict suite. It is a blind one, and it reports green
either way.

### Two lanes, because one style cannot answer both questions

The first lane pins `QT_QUICK_CONTROLS_STYLE=Basic`, deliberately: on a
real display the KDE platform theme answers with Breeze, whose metrics
differ from Basic's, so an unpinned suite would measure Breeze on a
developer's machine and Basic in a container, and a green run on one
would say nothing about the other. That lane compares pixels like with
like.

It cannot tell you whether the program a Linux user runs behaves,
because `src/gui/main.cpp` deliberately does *not* set that variable and
the shipped app inherits the desktop's style. So there is a second lane
(`seabass_qml_desktop_style_tests`, 8d970086) running the same suite
under `org.kde.desktop`, with the same xvfb lock as the others so three
X servers cannot race. It registers only where `org/kde/desktop/qmldir`
is found, and says so rather than failing where it is not -- a container
without `kf6-qqc2-desktop-style` skips it. `ctest` reports three QML
lanes: pinned, desktop-style, shader.

The two answers have already differed, which is the whole argument for
the second lane. Under `org.kde.desktop` the playlist picker's popup
rows came out **zero wide and unclickable**, and three pages failed to
instantiate on a TypeError thrown inside the style's own `ComboBox`.
None of it was visible from the pinned lane.

The find did not stop at the lane, and that is the part worth copying.
The same delegate binding -- `width: ListView.view ? ListView.view.width
: implicitWidth`, a row sized by its own content whenever it is not
inside a list view -- was also in `LibrarySourceToggle`, where no lane
could see it and three people had been looking straight at it all
afternoon. It was found on another platform by someone reading the fix
and going to look for the same shape, not by running anything: one row
painting nothing at all, another's name band five pixels tall beside a
twenty pixel glyph (9b55c8e8). A lane tells you a thing is broken. The
fix tells you what to search for.

There were three. `PlaylistRowDelegate` carried the same binding and is
used both in a plain list and in the Matching page's combo (f5515ac1).
And measuring the original on macOS found it *resolving* there, rows at
220 where `implicitWidth` is 160 -- which is the useful version of "it
does not fail on this platform": the control was never correct, it was
lucky, and which way the luck falls is decided by how a particular style
builds its popup.

This is what seabass#18 asked for, and it is worth knowing that the
issue's recorded diagnosis was wrong: it said the popup is a separate
native window under this style and that a click to the test's window
never reaches it. It is not a window. The rows were there and had no
width. A cause written down confidently is still a guess until a lane
exists to check it.

### Looking at the app under xvfb, and what that is worth

The app runs there, with the font size a scaled desktop would give it:

```
XDG_CONFIG_HOME=$(mktemp -d) \
QT_QPA_PLATFORM=xcb QSG_RHI_BACKEND=opengl LIBGL_ALWAYS_SOFTWARE=1 \
QT_FONT_DPI=144 \
    xvfb-run -a -s "-screen 0 1600x1000x24" build/src/gui/seabass
```

`QT_FONT_DPI=144` is Plasma's 1.5 scaling of a 96 dpi screen, and it is
what puts every other baseline on a half pixel: 11 pt measures 15 px of
ink at 96 and 21 px at 144. `QT_SCALE_FACTOR=1.5` gives you the
fractional device pixel ratio as well. The redirected `XDG_CONFIG_HOME`
keeps the run out of the real settings store.

Two things it will not give you, both found by trying:

- **The shipped style, reliably.** `QT_QUICK_CONTROLS_STYLE=org.kde.desktop`
  is the obvious way to ask for it, and the desktop-style lane above now
  runs the whole suite that way, 498 of 498. It hung once here, from a
  plain `qml6` process under xvfb: no window, no warning, no output,
  until it was killed. That was never reproduced afterwards and the lane
  runs reliably, so treat it as something that happened rather than as a
  property of the environment -- and if it happens to you, the lane is
  the thing that works.
- **The desktop's font rendering.** Xvfb starts with no Xft resources, so
  Plasma's hinting style and subpixel order never reach the app; what
  fontconfig supplies through `HOME` stands in for them, and it is not
  the same settings. Do not reach for `xrdb` to close that gap: here
  `xrdb -merge` exits 0 and leaves `RESOURCE_MANAGER` unset -- `xprop
  -root RESOURCE_MANAGER` says "not found" afterwards -- so a run that
  looks like it took the desktop's settings has taken none of them. That
  was believed and repeated in this project for two days before anyone
  checked the property.

So a hinting or baseline question ends on a real panel, with a person
looking at it. What is above gets you close enough to ask the question.
It does not answer it.

### And measure the ink, not a threshold

The same week produced the other half of this lesson. A QML test
measured a glyph's centre as the midpoint between the first and last
pixel row clearing a contrast threshold, and reported a catalog glyph
4.5 px out of line with its label. Most of that glyph's outline is
fainter than the threshold, so the band collapsed to a sliver and the
midpoint moved by pixels nothing on screen had moved by. Re-measured as
an ink centroid -- every pixel weighted by its distance from the
background, which is subpixel and sees faint ink -- the real offset was
0.36 px, and the two renderings are indistinguishable at 6x.

A metric that cannot see what it claims to measure does not fail safe.
That one invented work rather than hiding it, which is the rarer and more
expensive direction. `tests/qml/tst_LibrarySourceToggle.qml` carries the
centroid helper and the reasoning.

### A test count is not an invariant

"The suite is 181 tests" is a statement about one machine, one configure,
and one moment. It is not a fact about a commit, and it must never be
used as a tripwire for "did everything build".

Most `add_test` calls sit under `SEABASS_TESTS` and `TARGET
Qt6::Core`, which is not the interesting part. Ten are gated on
something that varies by **machine or by configure**:

| Test | Gate |
|---|---|
| `seabass_qml_desktop_style_tests` | `SEABASS_KDE_STYLE_QMLDIR` (kf6-qqc2-desktop-style) |
| `seabass_qml_shader_tests` | `SEABASS_XVFB_RUN AND Qt6ShaderTools_FOUND` |
| `seabass_qml_tests` | `SEABASS_XVFB_RUN` (falls back to an offscreen registration) |
| `silence_probe_test` | `TARGET seabass_audio_qt` |
| `taglib_metadata_probe_test` | `TARGET seabass_taglib` |
| `taglib_duration_probe_test` | `TARGET seabass_taglib` |
| `stray_file_scan_test` | `TARGET seabass_taglib` |
| `stray_scan_live_test` | `TARGET seabass_taglib AND SEABASS_LIVE_STICK` |
| `change_summary_wording_test` | `TARGET seabass_edit` |
| `windows_removable_media_monitor_test` | `WIN32` |

On a macOS build directory, four of those ten do not register at all:
the two that need `xvfb-run` or the KDE style, the Windows one, and
`stray_scan_live_test`.

`stray_scan_live_test` settles it: it is gated on `SEABASS_LIVE_STICK`
**at configure time**, so two build directories on the same machine, at
the same commit, with the same compiler, legitimately register different
totals depending on whether a stick was plugged in when `cmake` last ran.

This was not theoretical. A count quoted from one machine as a property
of master was wrong by one on the platform it was measured on -- it had
been taken one commit earlier -- and wrong by two on the other platform,
for an entirely different reason. Both halves of the tripwire, green and
red, meant something narrower than they sounded.

The portable question is whether anything stopped being registered, and
the portable check is a name diff, not a number:

```sh
# on the base, then on the branch, in each build directory
ctest -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort > /tmp/names.before
ctest -N | sed -n 's/^ *Test *#[0-9]*: //p' | sort > /tmp/names.after

comm -23 /tmp/names.before /tmp/names.after   # gone: the real alarm
comm -13 /tmp/names.before /tmp/names.after   # new: should be exactly yours
```

One thing that check does *not* answer, because it is easy to assume it
does: `ctest -N` lists what CMake registered, not what compiled. A test
that fails to build still appears.

Measured rather than reasoned about, since that is the point of this
section. A build directory configured from a clean worktree, with
nothing built in it at all -- zero test binaries on disk -- reports:

```
Total Tests: 181
```

So a name diff cannot tell "built and registered" from "registered and
failed to compile". **"Did everything build" is the build's own exit
status, and nothing else.** That check belongs beside the name diff, not
downstream of it.

### The move behind all of these

Every failure in this section and the ones around it is the same move,
and it is worth naming because it does not look like carelessness from
the inside: a measurement that was correct in a narrow frame, carried
into a wider one.

A mirror read as a silent no-op -- true of a mirror that no-ops, not of
this one, which throws. A sweep for swallowed failures matching
`ctx.log().record` -- true, and blind to the site that uses a local
`log.record`. A suite total -- true of one build directory at one
commit. A sampler preferring plans that exercise the OneLibrary mirror --
true of mirror coverage, and it silently dropped Engine coverage, because
every such plan targets rekordbox by definition.

None of those is a wrong measurement. Each is a right one asked to answer
a question it was never measuring. The habit that catches them is not
more care; it is stating the frame out loud -- *true of what, on which
machine, at which commit* -- because the frame is what goes missing, and
a number never carries its own.

**The sharper version, after it happened twice more.** Most of these are
one specific shape: **an aggregate standing in for its parts.** A suite
total was one build directory's count read as a fact about master. This
file's own spec-vs-parser check had a floor -- "fewer than this many
fields means the test read nothing and is not evidence" -- set on the
total across two spec/parser pairs, so when one pair fell out of the
extraction entirely the other pair's numbers covered for it and the run
reported agreement having compared half of what it claimed.

The fix, both times, was to stop comparing the number and start comparing
the things the number was summarising: per-name diffs instead of a count,
per-pair floors instead of a total.

And the trigger is narrower than "a total exists", which matters because
totals are usually fine -- that is exactly why this catches people who
are not being careless. Both floors were correct when they were written.
They went wrong when **the parts changed after the number was chosen**: a
second spec pair arrived, a second platform started running the suite, a
second build directory appeared with a different configure. So the
question to ask of any total a check rests on is not "is this right?" but
"what is it a total *of*, and has that set changed since?"

The production-code half of the same week -- how a write path should
behave when it meets input it was not built for -- is in
[`docs/write-path-rules.md`](write-path-rules.md). It is a separate rule
for a separate audience, but it comes out of the same three bugs.

## Check the artifact, not the reasoning

Four times in one day, across three machines, a measurement was correct
and its conclusion was wrong, because the thing measured was not the
thing under test. The reasoning was sound every time. That is what makes
this class expensive: there is nothing in the argument to find a hole in.

What actually happened, kept because the shapes differ and the lesson
does not:

- A QML suite was verified as passing "in every configuration". Every run
  had inherited a developer's saved settings. Against an empty profile it
  failed, and the failure was reported to a colleague as a defect in
  their fix.
- A before/after screenshot was rendered under a style neither platform
  ships -- Basic on Linux, the native style on macOS -- and shown around
  as a shipping bug. Under the style each app actually starts with, both
  pictures were fine. Two people did this independently on the same day.
- A round recorded three checks as "wrote no test results at all". The
  checks had passed; the rig binary was forty minutes older than the
  source that was supposed to be under test.
- A branch was declared not to work, with 1758 log lines as evidence,
  from a binary that did not contain the branch's new source file at all.
  Checking out a branch that ADDS a file needs a reconfigure; `cmake
  --build` relinks the old objects and says nothing.

The habit that caught every one of them is the same, and it is cheap:

- `nm <binary> | grep <the symbol you changed>` before believing a result
  about a branch. If the symbol is absent, the run proved nothing.
- Compare the built binary's mtime against the source you edited. The rig
  scripts do this for their own tools; do it by eye for anything else.
- Ask what the APPLICATION does at startup that the harness does not.
  `src/gui/controls_style.hpp` exists because the answer was "picks a
  different Qt Quick Controls style", and the suite had been testing a
  program nobody runs.
- Run against an empty `SEABASS_HOME` before calling anything verified.
  Inherited settings are invisible and they are what ctest and a new user
  will not have.
- For an archive, compare the manifest sha256, not the counts it reports.
  Two backups of the same library agree on every count and share no
  bytes.

None of this replaces the suite. It answers a narrower question that the
suite cannot: whether the run that just went green was a run of the code
in question.

## A test that asserts its own fixture

`MetadataRestorePage`'s screenshot case required the scan to have found
proposals -- which exist only when the metadata store holds something a
track on the stick is missing. It passed on any machine where somebody
had been working, and failed the first time it met an empty profile or a
stick freshly restored to its reference. The page was never wrong; the
case was asserting the DATA it was photographing.

A picture of a page should not require the page to have found something.
The case now waits for the scan, requires no error and a non-empty grab,
logs what it found and takes the picture either way -- and the harness
seeds the store with a cue the stick lacks, so the usual run photographs
a real proposal rather than an empty list. The seed makes the picture
worth having; removing the assertion is what stops it failing.

Worth recording how it was found, because neither route would have found
it alone and both were running the same evening: on macOS by making the
case RUN for the first time (it had been skipping on an unset variable
since it was written), and on Linux by running it in a sandbox profile
where its assumption could not hold. One found a test that never
executed, the other a test that only executed where it happened to be
true.

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

## Plant your own precondition, never inherit one

A test that needs the world to be a certain way, and does not put it
that way itself, passes wherever somebody has already been working and
fails on a clean machine. Three of those turned up in one day, which is
what makes it a rule rather than an anecdote:

- a round started outside the sandbox profile, which S3 caught in ten
  seconds -- the check exists precisely because the rig cannot be
  trusted to have been launched correctly;
- an assertion that held only because the path it ran under happened to
  contain "Seabass";
- and a live screenshot case that asserted the data it was
  photographing.

The third is the instructive one (bff1919e). The rig points that case at
a stick to photograph whatever state the stick is in -- its own comment
says "fine for a screenshot and would not be for an assertion" -- and
the case then required the scan to produce proposals. It passed on a
developer machine, where the everyday profile holds a metadata store
somebody has been filling for weeks, and failed in the rig's sandbox
profile, where the store is empty at S1 time because the checks that
fill it run later.

Planting was tried first, from the second stick, the way
`test_12_metadataFromSecondStickSaveUndo` does, and rejected: storing a
hundred tracks' metadata reads a hundred tracks' artwork and costs ten
minutes a run, and it still yields nothing when both sticks were
restored from the same backup, because a proposal only exists where the
store holds metadata a stick track is missing. So the case now waits for
the scan to finish, requires no error and a non-empty grab, logs the
count and takes the picture either way.

Which is the other half of the rule: when a precondition cannot be
planted cheaply, the answer is to assert something the test can actually
own, not to keep asserting the thing it cannot.

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
- **Run the QML suite through `ctest`, not by starting the binary.** Two
  things go wrong otherwise, and both of them look like the code. Without
  `-input tests/qml` the runner takes whatever `tst_*.qml` it finds
  beside the binary, and a build directory can hold an old copy under
  `diag/` -- on 2026-09-21 that reported StickListPage at 6 passed and 22
  failed, with "Cannot create delegate" and uninitialised required
  properties, against a tree whose suite was green. And a direct run
  inherits the desktop's Quick Controls style, while `ctest` pins
  `QT_QUICK_CONTROLS_STYLE=Basic` along with a redirected
  `XDG_CONFIG_HOME` and a sandboxed `SEABASS_HOME`; under the desktop
  style a ComboBox popup is a separate window `grabImage()` cannot see,
  so the tests that measure painted ink fail for the environment rather
  than for the page. `ctest -R seabass_qml_tests` is the one that
  answers the question asked.

## Live tests against a real stick

`tests/qml-live/` drives the real pages with their real controllers
against a mounted stick: reads, staged edits, saves, cancels, undo,
the foreign lock, the process guard, the CLI probe, and
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
created for itself, `findByObjectName()`, and the stick list's row and
card finders (`stickRow()`, `cardInRow()`, `objectInRow()`), which
`tests/qml/tst_StickListPage.qml` imports across directories as well. Results and screenshots from the 2026-09-07 run on
the RV2 stick are noted in `docs/edit-mode-and-cancel.md`.

The QML test binary registers the same `SeabassGui` module as the app
(`SEABASS_GUI_*` lists in `src/gui/CMakeLists.txt`), so any page can be
instantiated in a test; `tst_PagesCompile.qml` checks that every page
at least compiles.

## Release rig: real sticks, end to end

Before a release, `tools/rig-shakedown.sh` runs every scripted check against
two TEST sticks and two reference full stick backups: restoring a reference
onto each stick, the read-only scans, edits saved and undone, full stick
backups (incremental, cancelled and kept or discarded, compacted, refused
while DJ software runs, restored onto the other stick), and Backup USB Stick
between the two. It ends by proving both sticks are exact copies of their
references again and the references were never written.

```
. ~/Seabass/e2e/env.sh        # a sandbox SEABASS_HOME and XDG_* profile
SEABASS_BUILD_DIR=~/builds/seabass RIG_STICK_A=/media/you/TEST1 RIG_STICK_B=/media/you/TEST2 \
    RIG_DEVICE_B=/dev/sdX1 RIG_REFERENCE_A=~/refs/A.zip RIG_REFERENCE_B=~/refs/B.zip \
    tools/rig-shakedown.sh ~/rig-out
```

Build the rig's own tools before a round, and rebuild them after every
pull. A `rig_*` binary older than the archive format it reads does not say
it is stale: it says the data is wrong. A Windows round in September 2026
lost real time to `rig_restore` rejecting a valid, freshly written archive
with "manifest header is not a seabass stick manifest", and a standalone
zip probe was written to chase a reader bug that did not exist. The same
round then lost R1-R3 on both sticks to a `rig_read` of the same vintage,
which reported "the backup recorded no fingerprint" about a backup that
records one: two tools, two different misleading messages, one stale
build. Rebuild before you doubt the data. `rig_fake_dj` matters most, because it is the
one whose absence is silent: without it, FB7 and the guard scenario have
nothing to detect.

Only one rig may run at a time, and `rig-shakedown.sh` now holds an
`flock` to enforce it (d9137a14): a second round is refused with the
holder's pid, output directory, start time and sticks, and the lock is
released however the first one dies, `kill -9` included. `RIG_NO_LOCK=1`
is the deliberate way past it, for a second rig against different
sticks.

The lock's first version could refuse a round and then not say who held
it: `exec 9>"$file"` opens for write, which truncates, and the process
being refused opens the file before it finds out it cannot lock it. It
erased the pid, directory, start time and sticks, and then printed the
empty file it had just made. `<>` instead, and the truncation moved to
after the lock is held (c0f28751). Worth knowing as a shape rather than
as a bash detail: the diagnostic was destroyed by the thing that was
about to print it, and only running two real rigs against a scratch lock
showed it.

It then happened for real, within the hour, to the round that was
running: a test invocation of a second rig -- made before the fix -- had
erased the live lock file, so when that round needed killing, the pid to
kill it by was gone and it had to be found with `ps`. The shape was
written down from reading the diff before it had cost anybody anything,
and then it cost somebody something.

It exists because on 2026-09-21 two rounds ran against the same two
sticks for about ten minutes, each restoring and writing under the
other, and both had to be thrown away. What makes it worth a lock rather
than a convention: neither round reported anything unusual while it
happened. The damage surfaced as ordinary check failures several steps
downstream, on a stick that had been changed by somebody else.

Both sticks are overwritten, several times. The references are only read;
their size, modification time and manifest checksum are recorded before the
run (`RIG_REFERENCE_PRINTS`) and compared after it. Each check writes
`<out>/<check>.log` and a `PASS`/`FAIL` line to `<out>/summary.tsv`; a run of
several hours that reports PASS in minutes is suspect, so read the logs.

Some checks run several tests. Those write one `PASS`/`FAIL` line per test
into `$RIG_PARTS` (`tools/rig-parts.sh` for shell, `tools/rig_parts.hpp` for
the rig's C++ programs, `RIG_PART_SUFFIX` to tell two runs of one program
apart) and `check()` puts those lines in the summary instead of its own
single verdict. `RIG_BUNDLES` at the top of `rig-shakedown.sh` lists them.
A test's name is its row on the shakedown board, so the board can say which
of the ten live edit-mode tests failed rather than reddening all ten -- and
every id those scripts write has to exist on the board, or the recorder
refuses it. Declare the ids up front with `rig_parts_declare`: a bundle
that dies halfway then still accounts for the tests below the fault, as
failures, instead of leaving the board to keep last round's green for them.

The sandbox profile (`SEABASS_HOME`, `XDG_*`) covers Linux fully. It does
not cover the app's settings on macOS or Windows, where Qt ignores
`XDG_CONFIG_HOME`: macOS writes a property list and Windows the registry
key `HKCU\Software\seabass\seabass` (measured; see
`gui/seabass_settings.hpp`). So on those two, S3 and X4 do not assert that
settings landed in the sandbox -- they read the everyday settings before
and after the round and fail if anything moved. On Windows that reading
needs `reg` on PATH, and both checks fail saying so if it is missing,
rather than comparing two placeholders and passing. Both, not just S3:
`RIG_ONLY` runs a check by name, and X4 on its own is then the only guard
there is.

The pieces run on their own too, each ending in `RIG RESULT: PASS` or
`FAIL` with a matching exit code:

- `rig_restore <archive> <stick> [--execute]`: exact restore, then zero
  changes left and every catalog file equal to the manifest's checksum.
  Without `--execute` it only checks.
- `rig_read <stick> [<archive>]`: catalog counts and the read-only scans,
  timed, without writing to the stick.
- `rig_fs_repair [--keep]`: Library Health's filesystem repair end to end,
  on a FAT32 loopback image the tool damages itself -- no stick and no
  root. It clears FAT32's clean-shutdown bit and wrongs the free-cluster
  summary (what an unclean unplug leaves), mounts the volume read-only,
  and then checks the whole path: Seabass sees it as read-only, the
  platform's repair reports it consistent again, the volume takes writes,
  and every file written before the damage is still whole. Linux attaches
  the image through udisks2, macOS through `hdiutil`; Windows has no
  unprivileged way to attach one, so there the repair stays manual.
- `rig_backup <stick> <archive> [--expect ... | --expect-refused | --cancel-at P keep|discard]`
  and `rig_compact <archive>`: a full stick backup verified, not hollow, and
  compaction freeing exactly what it promised.
- `rig_clone` and `rig_advise`: Create/Update Backup USB Stick between two
  sticks, and what the stick list advises for each.
  `rig_clone --expect-too-small` checks only the preview: the target has no
  room, which is what disables the card on the page, and nothing is written.
- `rig_delete_backup <backup dir> <archive>`: Manage Backups deleting one
  archive -- refused while a helper process holds the archive's write lock,
  archive and journal gone afterwards, every other backup still there with
  the same size and modification time. It deletes for real, so it refuses
  an archive outside the folder it was given, one that is or sits beside a
  reference named in `RIG_REFERENCE_A`/`RIG_REFERENCE_B`, one it cannot
  write, and one Manage Backups cannot read as a backup. Both
  `RIG_REFERENCE_A` and `RIG_REFERENCE_B` must be given (a full run exports
  them), or it refuses to delete anything at all unless
  `--no-reference-guard` says otherwise. It needs `fork()` to hold the lock
  for the refusal, so it refuses on platforms without one rather than
  deleting with that half of the check unproven.
- `tools/rig-edits.sh <stick> [baseline]`: add a cue, Clean Up one group and
  a Library Health repair, each saved and undone (`rig_plant_repairable`
  plants the repairable issue and puts the file back).
- `tools/rig-clones.sh`: the two-stick Backup USB Stick checks.

The live QML tests carry the checks that need real pages rather than a
tool, one test function per process (a bare `TestCase` name makes the
runner exit 0 without running anything):

- `tst_LivePages.qml`: Statistics and Stick Performance load with figures
  that fit the library, and Manage Backups lists both reference backups
  with their stick, size and counts and browses one read-only. The
  performance page is only ever measured here -- its write test, its
  scratch-file variant and the wear check all write to the stick.
- `tst_LiveQuit.qml`: leaving with unsaved changes, both ways out. Discard
  must leave the catalogs alone; Save must write everything staged and
  only then leave. The saved change is undone again.
- `tst_LiveFullStick.qml`, with `SEABASS_RIG_FULL_STICK`: a save on a
  stick with a few MB left. `rig-shakedown.sh` fills the stick with a
  filler file first and deletes it afterwards whatever happened, so no
  second, smaller stick is needed.
- `tst_LiveEditMode.qml`'s `test_13`, with `SEABASS_RIG_DELETE_ORPHANS`:
  Delete Orphaned Files, cancelled part-way and then finished. It plants
  its own list by saving a Clean Up and not undoing it, and deletes audio
  for good, so the runner restores the stick right afterwards.

`tools/rig_save_backups <stick>` lists the automatic backups a round's
saves left on the stick and asks the store whether each is restorable.

### On macOS, with disk images for sticks

The same scripts run on a Mac: `tools/rig-platform.sh` puts Homebrew's GNU
coreutils and findutils first on `PATH` (`brew install coreutils
findutils`) and swaps `udisksctl`/`lsblk`/`/proc` for `diskutil`/`ps`. With
one free USB port the sticks can be mounted disk images, which the app
lists only when `SEABASS_ACCEPT_DISK_IMAGES=1` (the rig sets it):

```
hdiutil create -size 32g -type SPARSE -fs "MS-DOS FAT32" -volname VSTICKA -layout MBRSPUD vsticka.sparseimage
hdiutil create -size 64g -type SPARSE -fs ExFAT -volname VSTICKB -layout MBRSPUD vstickb.sparseimage
hdiutil attach -nobrowse vsticka.sparseimage; hdiutil attach -nobrowse vstickb.sparseimage
SEABASS_BUILD_DIR=~/Seabass/builds/<name> RIG_REFERENCE_A=... RIG_REFERENCE_B=... tools/rig-shakedown.sh ~/rig-out
```

`RIG_STICK_A`/`RIG_STICK_B` default to `/Volumes/VSTICKA`/`VSTICKB` there,
and `RIG_DEVICE_B` to B's device node. A disk image is not a USB stick: it
proves the app's logic end to end, not how a real stick or its bus behaves,
so a release round still wants real sticks. Quit rekordbox and Engine DJ
first -- every write is refused while either runs.

A stick pull is simulated by unmounting and remounting the device
(`run-live.sh`); a pull in the middle of a save still needs someone at the
machine. What is left for a person: playing a restored stick on Pioneer
or Denon hardware, the Windows run with a physical replug, and filing an
issue for whatever failed.

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
    --hardware "what you use" --notes "what you'd like tested"
```

- **Nothing is sent anywhere automatically.** This command only ever writes files to `DIR`. There is no networking code in this project at all; review what's in `DIR` yourself, then attach it (zipped) to an email to **sebas@kde.org** if you're happy with it.
- Every real track is included. The command prints the size of the zip it wrote, so you can see what you would be attaching.
- `MANIFEST.txt` in the output tells you exactly what's included and excluded; read it before sending.
- This data may be published as part of the project's test suite (the same way `tests/fixtures/anonymized_library/` is). If there's anything in your `--hardware`/`--notes` text you wouldn't want published, leave it out of those fields and mention it directly in your email instead.
