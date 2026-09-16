#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Drives the live edit-mode tests in this directory against a real
# (scratch!) stick: the plain flows, then the three orchestrated
# scenarios that need something to happen outside the test process (a
# foreign lock cookie, a fake rekordbox process, the stick going away).
# See docs/testing.md, "Live tests against a real stick".
#
#   tests/qml-live/run-live.sh /media/you/STICK /dev/sdX1 [screenshot dir]
#   SKIP_PLAIN=1 ...   runs only the three orchestrated scenarios
#   SEABASS_BUILD_DIR=~/builds/seabass ...   a build outside <repo>/build
#
# The device is required: the stick-pull scenario unmounts and mounts it
# again with udisksctl, and a bundle that leaves that scenario out has not
# run. Every test writes to the stick through the normal backup path; do
# not point this at a stick you cannot afford to restore.
set -u

stick="${1:?mount point of the scratch stick}"
device="${2:?device of the scratch stick, e.g. /dev/sdb1 -- the stick-pull scenario needs it}"
shots="${3:-}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# Builds usually live outside the repository: SEABASS_BUILD_DIR says where.
build="${SEABASS_BUILD_DIR:-$root/build}"
bin="$build/seabass_qml_tests"
cli="$build/seabass-cli"
export SEABASS_LIVE_STICK="$stick"
export SEABASS_SCREENSHOT_DIR="$shots"
export QT_QPA_PLATFORM=offscreen
failed=0

# Always a full "TestCase::function" name: a bare TestCase name makes
# the QtQuickTest runner exit 1 without a word.
# A skipped test is a failure here: this is the last gate before people
# see the build, and a check that did not run must never read as green.
run() {  # name, extra env assignments...
    local name="$1"; shift
    # Its own file: $out belongs to rig-shakedown.sh and does not exist
    # here, so under set -u naming it killed this script at its first test.
    local log; log="$(mktemp)"
    echo "=== $name"
    env "$@" stdbuf -oL "$bin" -input "$here" "$name" 2>&1 | tee "$log" \
        | grep -E "^(PASS|FAIL|SKIP|QDEBUG|XFAIL|Totals)|^   (Actual|Expected|Loc)" \
        | sed 's/SeabassGuiQmlTests::[A-Za-z]*:://; s/^QDEBUG : [a-zA-Z0-9_]*() .\[34m[a-zA-Z0-9_]*.\[0m://'
    # The test binary's own status, not the filter's: a pipeline ends with
    # sed, which succeeds whatever the tests did, so a run full of FAILs
    # used to exit 0 and a caller counted it as passed.
    [ "${PIPESTATUS[0]}" -eq 0 ] || failed=1
    # And a skip: QtTest exits 0 for it, but a test that did not run has
    # proved nothing.
    if grep -q "^SKIP" "$log" 2>/dev/null; then
        echo "   SKIPPED, which counts as a failure here"
        failed=1
    fi
    rm -f "$log"
}

# 1. The plain flows, one process per test function so a failure in one
#    cannot take the others down with it.
[ -n "${SKIP_PLAIN:-}" ] || for t in test_01_scanCancel test_02_settingsStageSaveUndo test_03_syncStageSaveCancel \
         test_04_junkCuesStageSaveUndo test_07_pendingDeletionsCancel; do
    run "LiveEditMode::$t"
done

# test_05 needs a damaged library, which a healthy stick does not have and
# the test cannot make for itself: rig_plant_repairable moves one copy of a
# cue-free duplicate aside, the same way rig-edits.sh sets up test_10. With
# something planted a skip would be a silent pass, so it is required to run.
if [ -z "${SKIP_PLAIN:-}" ]; then
    echo "=== planting a repairable Library Health issue for test_05"
    if "$build/rig_plant_repairable" "$stick" --plant; then
        run "LiveEditMode::test_05_libraryHealthLeaveDiscards" SEABASS_RIG_REQUIRE_REPAIRABLE=1
    else
        echo "   could not plant a repairable issue, so test_05 proves nothing"
        failed=1
    fi
    # Back even if the test failed: the next check compares this stick
    # against its reference.
    "$build/rig_plant_repairable" "$stick" --restore || failed=1
fi

# 2. A foreign, live lock: a cookie owned by a sleep process on this host.
# Where the app keeps its edit-lock cookies: paths::localMetadataDir(), which
# honours SEABASS_HOME.
lockdir="${SEABASS_HOME:-$HOME/Seabass}/metadata/edit-locks"
mkdir -p "$lockdir"
libid="$(lsblk -no UUID "$(findmnt -no SOURCE "$stick")" 2>/dev/null | head -1)"
if [ -n "$libid" ]; then
    sleep 600 & holder=$!
    startid="$(awk '{print $22}' "/proc/$holder/stat")"
    now="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '{"heartbeatUnix":"%s","hostname":"%s","instanceId":"run-live-foreign","libraryId":"%s","mountPoint":"%s","pid":"%s","processStartId":"%s","startedAtUtc":"%s","stickLabel":"%s"}\n' \
        "$(date +%s)" "$(hostname)" "$libid" "$stick" "$holder" "$startid" "$now" "$(basename "$stick")" \
        > "$lockdir/$libid.json"
    run "LiveLock::test_01_stickListShowsReadOnly" SEABASS_LIVE_LOCKED=1
    run "LiveLock::test_02_firstStageRefusedThenRemoveLock" SEABASS_LIVE_LOCKED=1
    kill "$holder" 2>/dev/null
    rm -f "$lockdir/$libid.json"
else
    # The harness's own skips count too: a scenario that did not run has
    # proved nothing, whoever decided to leave it out.
    echo "=== LiveLock skipped: could not read the stick's filesystem UUID"
    failed=1
fi

# 3. The process guard and the CLI probe. A copy of sleep named rekordbox
#    is what the detector sees; it is killed 25 s in, and the CLI runs
#    while the test still holds the lock.
cp /bin/sleep /tmp/rekordbox
/tmp/rekordbox 300 & fake=$!
( sleep 25; kill "$fake" 2>/dev/null
  sleep 8
  echo "--- CLI while the GUI holds the lock:"
  "$cli" backups --engine "$stick/Engine Library" --clean --keep 1000 2>&1 | sed 's/^/    /'
  echo "--- CLI with --force:"
  "$cli" backups --engine "$stick/Engine Library" --clean --keep 1000 --force 2>&1 | sed 's/^/    /' ) &
run "LiveGuard::test_guardBlocksWhileEditingAndCliIsRefused" SEABASS_LIVE_GUARD=1
wait
rm -f /tmp/rekordbox

# 4. The stick goes away while editing.
if [ -n "$device" ]; then
    ( sleep 12; udisksctl unmount -b "$device" >/dev/null 2>&1 && echo "--- unmounted $device"
      sleep 15; udisksctl mount -b "$device" >/dev/null 2>&1 && echo "--- mounted $device again" ) &
    run "LiveStickPull::test_stickPulledWhileEditing" SEABASS_LIVE_STICK_PULL=1
    wait
    udisksctl mount -b "$device" >/dev/null 2>&1 || true
else
    echo "=== LiveStickPull skipped: no device given"
    failed=1
fi

# Non-zero when any test above failed.
exit $failed
