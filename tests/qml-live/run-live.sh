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
#   tests/qml-live/run-live.sh /Volumes/STICK /dev/diskNs1 [screenshot dir]   (macOS)
#   SKIP_PLAIN=1 ...   runs only the three orchestrated scenarios
#   SEABASS_BUILD_DIR=~/builds/seabass ...   a build outside <repo>/build
#
# The device is required: the stick-pull scenario unmounts and mounts it
# again (udisksctl, or diskutil on macOS), and a bundle that leaves that scenario out has not
# run. Every test writes to the stick through the normal backup path; do
# not point this at a stick you cannot afford to restore.
set -u

stick="${1:?mount point of the scratch stick}"
device="${2:?device of the scratch stick, e.g. /dev/sdb1 -- the stick-pull scenario needs it}"
shots="${3:-}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
. "$root/tools/rig-platform.sh"
# Never beside a working stick or a reference (see rig-platform.sh).
refuse_if_protected_sticks_inserted
. "$root/tools/rig-parts.sh"
# Builds usually live outside the repository: SEABASS_BUILD_DIR says where.
build="${SEABASS_BUILD_DIR:-$root/build}"
bin="$build/seabass_qml_tests"
cli="$build/seabass-cli"
export SEABASS_LIVE_STICK="$stick"
# Made here, not assumed: a directory that does not exist makes every
# screenshot throw, and grabImage().save() throwing inside a test fails it
# with "Can't save to ..." -- which reads like a product fault and cost a
# whole bundle run. Empty stays empty (screenshots off).
[ -z "$shots" ] || mkdir -p "$shots"
export SEABASS_SCREENSHOT_DIR="$shots"
export QT_QPA_PLATFORM=offscreen
# See tools/rig-shakedown.sh's own copy of this: without it, a real
# failure on Windows prints nothing at all (Qt routes it to
# OutputDebugString instead of stderr when there is no attached console).
export QT_FORCE_STDERR_LOGGING=1
# Also rig-shakedown.sh's own copy: without a real font directory, the
# offscreen platform's font backend on Windows has nothing to enumerate
# and QFontDatabase::families() comes back EMPTY, which asserts inside
# Qt itself the moment anything asks for a raw font.
if rig_is_windows; then
    export QT_QPA_FONTDIR="${SYSTEMROOT:-C:/Windows}/Fonts"
fi
failed=0

# One board row per test. Declared up front, so a bundle that dies in the
# middle still accounts for the tests below the fault -- as failures,
# because they proved nothing. SKIP_PLAIN leaves the plain flows out, and
# a test that was not asked for is not declared.
rig_parts_declare live-lock-read-only live-lock-refuses-stage live-dj-guard live-stick-pulled
[ -n "${SKIP_PLAIN:-}" ] || rig_parts_declare live-library-health-leave live-library-health-leave-wrote-nothing \
    live-scan-cancel live-settings-save-undo live-sync-save-cancel live-junk-cues-save-undo \
    live-pending-deletions-cancel
trap rig_parts_finish EXIT

# The board id for each test function, so the name lives in one place.
part_for() {
    case "$1" in
        *test_01_scanCancel) echo live-scan-cancel ;;
        *test_02_settingsStageSaveUndo) echo live-settings-save-undo ;;
        *test_03_syncStageSaveCancel) echo live-sync-save-cancel ;;
        *test_04_junkCuesStageSaveUndo) echo live-junk-cues-save-undo ;;
        *test_05_libraryHealthLeaveDiscards) echo live-library-health-leave ;;
        *test_07_pendingDeletionsCancel) echo live-pending-deletions-cancel ;;
        LiveLock::test_01*) echo live-lock-read-only ;;
        LiveLock::test_02*) echo live-lock-refuses-stage ;;
        LiveGuard::*) echo live-dj-guard ;;
        LiveStickPull::*) echo live-stick-pulled ;;
        *) echo "" ;;
    esac
}

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
    local bad=0
    [ "${PIPESTATUS[0]}" -eq 0 ] || bad=1
    # And a skip: QtTest exits 0 for it, but a test that did not run has
    # proved nothing.
    if grep -q "^SKIP" "$log" 2>/dev/null; then
        echo "   SKIPPED, which counts as a failure here"
        bad=1
    fi
    rm -f "$log"
    local part; part="$(part_for "$name")"
    [ -z "$part" ] || rig_part_rc "$part" "$bad"
    [ "$bad" -eq 0 ] || failed=1
}

# test_05 needs a damaged library, which a healthy stick does not have and
# the test cannot make for itself: rig_plant_repairable moves one copy of a
# cue-free duplicate aside, the same way rig-edits.sh sets up test_10. With
# something planted a skip would be a silent pass, so it is required to run.
if [ -z "${SKIP_PLAIN:-}" ]; then
    echo "=== planting a repairable Library Health issue for test_05"
    # What a Denon player would say about this stick before the test, to
    # compare against afterwards. test_05 leaves Library Health without
    # saving, so neither number may move at all: the sequence in
    # export.pdb only changes when something writes it, and a discard
    # that still moved it has written to the stick. See issue #42 and
    # tools/rig_import_prompt.cpp.
    leave_state="$(mktemp)"
    "$build/rig_import_prompt" "$stick" --record "$leave_state" || failed=1
    if "$build/rig_plant_repairable" "$stick" --plant; then
        run "LiveEditMode::test_05_libraryHealthLeaveDiscards" SEABASS_RIG_REQUIRE_REPAIRABLE=1
    else
        echo "   could not plant a repairable issue, so test_05 proves nothing"
        rig_part live-library-health-leave FAIL
        failed=1
    fi
    # Back even if the test failed: the next check compares this stick
    # against its reference.
    "$build/rig_plant_repairable" "$stick" --restore || failed=1
    # After the restore, so the planted file is not itself the change
    # being measured. Its own row: leaving without saving and leaving the
    # player alone are two claims, and a reader needs to know which one
    # broke.
    if "$build/rig_import_prompt" "$stick" --compare "$leave_state" --identical; then
        rig_part live-library-health-leave-wrote-nothing PASS
    else
        rig_part live-library-health-leave-wrote-nothing FAIL
        failed=1
    fi
    rm -f "$leave_state"
fi

# 1. The plain flows, one process per test function so a failure in one
#    cannot take the others down with it.
[ -n "${SKIP_PLAIN:-}" ] || for t in test_01_scanCancel test_02_settingsStageSaveUndo test_03_syncStageSaveCancel \
         test_04_junkCuesStageSaveUndo test_07_pendingDeletionsCancel; do
    run "LiveEditMode::$t"
done

# 2. A foreign, live lock: a cookie owned by a sleep process on this host.
# Where the app keeps its edit-lock cookies: paths::localMetadataDir(), which
# honours SEABASS_HOME.
lockdir="${SEABASS_HOME:-$HOME/Seabass}/metadata/edit-locks"
mkdir -p "$lockdir"
libid="$(stick_uuid "$stick")"
if [ -n "$libid" ]; then
    sleep 600 & holder=$!
    startid="$(process_start_id "$holder")"
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
    rig_part live-lock-read-only FAIL
    rig_part live-lock-refuses-stage FAIL
    failed=1
fi

# 3. The process guard and the CLI probe. A process named rekordbox is
#    what the detector sees; it is killed 25 s in, and the CLI runs while
#    the test still holds the lock. start_fake_dj (rig-platform.sh) is
#    the one place that starts one.
start_fake_dj "$build" 300 || exit 1
fake=$fake_dj_pid
( sleep 25; kill "$fake" 2>/dev/null
  sleep 8
  echo "--- CLI while the GUI holds the lock:"
  "$cli" backups --engine "$stick/Engine Library" --clean --keep 1000 2>&1 | sed 's/^/    /'
  echo "--- CLI with --force:"
  "$cli" backups --engine "$stick/Engine Library" --clean --keep 1000 --force 2>&1 | sed 's/^/    /' ) &
run "LiveGuard::test_guardBlocksWhileEditingAndCliIsRefused" SEABASS_LIVE_GUARD=1
wait

# 4. The stick goes away while editing.
#
# By hand on Windows, and said so rather than attempted. rig-platform.sh's
# own unmount_device()/mount_device() go through diskutil on macOS and
# udisksctl everywhere else, and neither exists here -- but that is a gap
# in these two shell helpers, not a platform limitation: Seabass's own
# eject button (WindowsRemovableMediaMounter::unmount(), used for real)
# does the identical FSCTL_LOCK_VOLUME -> FSCTL_DISMOUNT_VOLUME ->
# IOCTL_STORAGE_EJECT_MEDIA sequence Explorer's own "Safely Remove
# Hardware" performs, and it needs no elevation. Confirmed directly: a
# real stick ejected this way with no admin prompt at all.
#
# The reason this still has to be a person is the step after the eject,
# not the eject itself. mount()'s own comment already says it: Windows
# will not reassign a drive letter to an ejected volume without a
# physical reinsertion, unlike udisksctl's mount on Linux, which can
# genuinely bring an unmounted device back on its own. An unattended
# round that ejects a stick this way has no way to give it back, so it
# would sit waiting for an event that cannot arrive -- round 5 saw
# exactly that, for fourteen minutes, before it was killed. So this is a
# check for a person on this platform -- docs/manual-testing.md carries
# it -- and the round neither runs it nor claims it passed.
if rig_is_windows; then
    echo "=== LiveStickPull is a by-hand check on Windows: ejecting works, but Windows cannot bring the stick back without a physical reinsertion, see docs/manual-testing.md"
    # Recorded as failed, not quietly left out: the board's own rule is
    # that a check nobody ran has not passed. It is blocked rather than
    # broken, and the board says so when a person marks it that way.
    rig_part live-stick-pulled FAIL
elif [ -n "$device" ]; then
    ( sleep 12; unmount_device "$device" && echo "--- unmounted $device"
      sleep 15; mount_device "$device" && echo "--- mounted $device again" ) &
    run "LiveStickPull::test_stickPulledWhileEditing" SEABASS_LIVE_STICK_PULL=1
    wait
    # The stick has to come back, and this has to say so when it does not.
    #
    # It used to be `mount_device "$device" || true`: one attempt, its
    # answer thrown away. Round 7 unmounted /dev/sdb1 here, never got it
    # back, and reported this check PASS -- and then FB4, FB5, FB6, FB8
    # and W8 failed with "No such file or directory" about a stick that
    # was still plugged in, four checks downstream of the one that
    # actually broke. A check that takes the stick away and cannot give
    # it back has not passed, whatever the test process decided, so this
    # overwrites its row: read_summary takes the last line for an id.
    if ! mount_device_until_back "$device"; then
        echo "   the stick did not come back, so nothing after this can be trusted"
        rig_part live-stick-pulled FAIL
        failed=1
    fi
else
    echo "=== LiveStickPull skipped: no device given"
    rig_part live-stick-pulled FAIL
    failed=1
fi

# Non-zero when any test above failed.
exit $failed
