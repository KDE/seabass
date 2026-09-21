#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig: the edit checks that need more than run-live.sh gives them,
# against a TEST stick.
#
#   tools/rig-edits.sh /media/you/STICK [catalog baseline file]
#   SEABASS_BUILD_DIR=~/builds/seabass ...   a build outside <repo>/build
#
# W2  add a cue (rekordbox and its OneLibrary mirror), save, undo
# W5  Clean Up one duplicate group, save, undo
# W6  Library Health: tools/rig_plant_repairable moves one copy of a
#     duplicate aside, the test repairs, saves and undoes, and the
#     file is moved back whatever the test did
#
# Every write goes through the app's backup path and is undone. With a
# baseline file (sha256sum lines, as rig check R6 keeps them) the stick's
# catalog files must be byte-identical to it afterwards.
#
# The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL".
set -u

stick="${1:?mount point of the test stick}"
baseline="${2:-}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/rig-platform.sh"
. "$here/rig-parts.sh"
build="${SEABASS_BUILD_DIR:-$root/build}"
export SEABASS_LIVE_STICK="$stick"
export QT_QPA_PLATFORM=offscreen
# See rig-shakedown.sh's own copy of this: without it, a real failure on
# Windows prints nothing at all (Qt routes it to OutputDebugString instead
# of stderr when there is no attached console), so `run()` below would
# capture an empty log for a genuine, ordinary assertion failure.
export QT_FORCE_STDERR_LOGGING=1
failed=0

# One board row per test: W5 skipping for want of a duplicate group on the
# stick used to take W2 and W6 red with it, and a reader could not tell.
rig_parts_declare W2-add-cue W5-clean-up-group W5-import-prompt-not-armed \
    W6-library-health-repair W6-import-prompt-not-armed
[ -z "$baseline" ] || rig_parts_declare edits-wrote-nothing
trap rig_parts_finish EXIT

run() {  # board id, full "TestCase::function" name
    local part="$1"; shift
    echo "=== $1"
    local log; log="$(mktemp)"
    local bad=0
    "$build/seabass_qml_tests" -input "$root/tests/qml-live" "$1" 2>&1 | tee "$log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || bad=1
    # A skip proves nothing, and QtTest exits 0 for it.
    if grep -q "^SKIP" "$log"; then
        echo "   SKIPPED, which counts as a failure here"
        bad=1
    fi
    rm -f "$log"
    rig_part_rc "$part" "$bad"
    [ "$bad" -eq 0 ] || failed=1
}

run W2-add-cue LiveEditMode::test_08_addCueSaveUndo

# Clean Up is the save issue #42 actually names. It says export.pdb's
# sequence moves whenever the library is rewritten, and that Clean Up and
# the duplicate cleanup do rewrite it -- so a dedup today should mean a
# player offering, tomorrow, to overwrite the Engine library the dedup
# just tidied.
#
# The same check around W6 came back level (engine 513, library 513,
# still level afterwards, round 7), which answers one of that issue's own
# open questions: a Library Health repair does not move it. That is why
# this one is here rather than only there. If this comes back level too,
# that is a bigger finding than the issue expects, and either way the log
# prints both numbers so the answer is readable without re-running.
cleanup_state="$(mktemp)"
"$build/rig_import_prompt" "$stick" --record "$cleanup_state" || failed=1
run W5-clean-up-group LiveEditMode::test_09_cleanupOneGroupSaveUndo
if "$build/rig_import_prompt" "$stick" --compare "$cleanup_state"; then
    rig_part W5-import-prompt-not-armed PASS
else
    rig_part W5-import-prompt-not-armed FAIL
    failed=1
fi
rm -f "$cleanup_state"

echo "=== planting a repairable Library Health issue"
# What a Denon player would say about this stick before the repair, so
# the check after it can tell the difference between "Seabass armed the
# import prompt" and "this stick was already going to be asked about".
# See tools/rig_import_prompt.cpp and issue #42: a repair rewrites
# export.pdb, which moves the sequence Engine compares against, so a
# player offers to rebuild the Engine side from the rekordbox one -- and
# accepting that undoes the very repair this check just made.
import_state="$(mktemp)"
"$build/rig_import_prompt" "$stick" --record "$import_state" || failed=1
if "$build/rig_plant_repairable" "$stick" --plant; then
    # Planted, so there is something to repair: a skip would pass silently.
    SEABASS_RIG_REQUIRE_REPAIRABLE=1 run W6-library-health-repair LiveEditMode::test_10_libraryHealthRepairSaveUndo
    # Its own row. The repair passing and the player being left asking
    # are two different answers, and a reader needs both: W6 is about the
    # repair Seabass made, this is about what the next insert does to it.
    if "$build/rig_import_prompt" "$stick" --compare "$import_state"; then
        rig_part W6-import-prompt-not-armed PASS
    else
        rig_part W6-import-prompt-not-armed FAIL
        failed=1
    fi
else
    # The setup, not the test: its own row says so rather than the whole
    # bundle going red for a fixture that had nothing to plant.
    rig_part W6-library-health-repair FAIL
    # Nothing was repaired, so nothing can be said about what a repair
    # does to the prompt. Unreported would let the board keep last
    # round's answer, which is the failure this rig is built against.
    rig_part W6-import-prompt-not-armed FAIL
    failed=1
fi
rm -f "$import_state"
echo "=== putting the planted file back"
"$build/rig_plant_repairable" "$stick" --restore || failed=1

if [ -n "$baseline" ]; then
    echo "=== catalog files against $baseline"
    if grep -F "$stick/" "$baseline" | sha256sum -c; then
        rig_part edits-wrote-nothing PASS
    else
        rig_part edits-wrote-nothing FAIL
        failed=1
    fi
fi

echo "RIG RESULT: $([ $failed = 0 ] && echo PASS || echo FAIL)"
exit $failed
