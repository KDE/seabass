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
#     cue-free duplicate aside, the test repairs, saves and undoes, and the
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
build="${SEABASS_BUILD_DIR:-$root/build}"
export SEABASS_LIVE_STICK="$stick"
export QT_QPA_PLATFORM=offscreen
failed=0

run() {  # full "TestCase::function" name
    echo "=== $1"
    local log; log="$(mktemp)"
    "$build/seabass_qml_tests" -input "$root/tests/qml-live" "$1" 2>&1 | tee "$log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || failed=1
    # A skip proves nothing, and QtTest exits 0 for it.
    if grep -q "^SKIP" "$log"; then
        echo "   SKIPPED, which counts as a failure here"
        failed=1
    fi
    rm -f "$log"
}

run LiveEditMode::test_08_addCueSaveUndo
run LiveEditMode::test_09_cleanupOneGroupSaveUndo

echo "=== planting a repairable Library Health issue"
if "$build/rig_plant_repairable" "$stick" --plant; then
    # Planted, so there is something to repair: a skip would pass silently.
    SEABASS_RIG_REQUIRE_REPAIRABLE=1 run LiveEditMode::test_10_libraryHealthRepairSaveUndo
else
    failed=1
fi
echo "=== putting the planted file back"
"$build/rig_plant_repairable" "$stick" --restore || failed=1

if [ -n "$baseline" ]; then
    echo "=== catalog files against $baseline"
    grep -F "$stick/" "$baseline" | sha256sum -c || failed=1
fi

echo "RIG RESULT: $([ $failed = 0 ] && echo PASS || echo FAIL)"
exit $failed
