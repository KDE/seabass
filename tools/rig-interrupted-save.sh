#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig check F6: a save killed mid-write, then read and undone
# (issue #48), against a TEST stick.
#
#   tools/rig-interrupted-save.sh <stick> <device or ""> [screenshot dir] [catalog baseline] [digest baseline]
#   RIG_REFERENCE_B=~/Seabass/e2e/backups/STICK.zip   the stick's reference (required)
#   SEABASS_BUILD_DIR=~/builds/seabass ...            a build outside <repo>/build
#
# Why a kill and not an unmount. A person pulling the stick is manual
# check P3, and it stays one. `udisksctl unmount --force` is no stand-in:
# it is a lazy unmount, the process with the database open keeps writing
# through its open descriptor, the transaction finishes and nothing is
# left hot. What a pull does to the files on the stick is what a crash of
# the writer does: the writer stops between one write and the next, and
# the stick keeps whatever had reached it. So this SIGKILLs the process
# that is saving (Windows: taskkill /F), and then, where the platform can,
# unmounts and mounts the stick again so the OS's own caches of it are
# dropped too (Linux udisksctl, macOS diskutil; Windows cannot bring an
# unmounted stick back without a hand on it, and the kill alone leaves the
# same files). One difference remains and is not hidden: data the killed
# process had handed the kernel still reaches the stick, where a pull
# could lose it. That is the kinder of the two cases for SQLite, whose
# journal and log are written before the database for exactly this.
#
# Four board rows:
#   F6-save-killed-mid-write  the save was really cut short: the marker the
#                             test writes once a track is written appeared,
#                             the process died of the kill, and the stick
#                             holds the save's note (Seabass/backups/
#                             .save-in-progress). A save that finished first
#                             is retried from the reference, three attempts.
#   F6-leftovers-recorded     what it left: journals (live or not), logs and
#                             the note's records, from tools/rig_journal_state
#   F6-reads-and-undoes       a fresh process reads every catalog, is offered
#                             the undo, takes it, and reads again
#   F6-stick-at-baseline      the catalogs are the baseline again, and a copy
#                             was kept on this computer exactly when a hot
#                             journal had to be rolled back
#
# The stick is put back at its reference first and restored from it again
# at the end, whatever happened: a killed save writes the stick for good.
# Without a baseline file (a standalone run) the baseline is taken from the
# stick after that first restore.
#
# The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL".
set -u

stick="${1:?mount point of the test stick}"
if [ $# -lt 2 ]; then
    echo "usage: $0 <stick> <device or \"\"> [screenshot dir] [catalog baseline] [digest baseline]" >&2
    exit 2
fi
device="$2"
shots="${3:-}"
given_baseline="${4:-}"
given_digests="${5:-}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/rig-platform.sh"
# Never beside a working stick or a reference (see rig-platform.sh).
refuse_if_protected_sticks_inserted
if is_protected_label "$(basename "$stick")" || is_protected_label "$(stick_label "$stick")"; then
    echo "REFUSED: $stick is a stick the rig never writes. Nothing was written." >&2
    exit 1
fi
. "$here/rig-parts.sh"
build="${SEABASS_BUILD_DIR:-$root/build}"
reference="${RIG_REFERENCE_B:?the reference backup of this stick, which it is restored from}"
bin="$build/seabass_qml_tests"
export SEABASS_LIVE_STICK="$stick"
export QT_QPA_PLATFORM=offscreen
# See rig-shakedown.sh's own copies of these two: without the first a real
# failure prints nothing on Windows, without the second the offscreen
# platform there has no fonts and asserts inside Qt.
export QT_FORCE_STDERR_LOGGING=1
if rig_is_windows; then
    export QT_QPA_FONTDIR="${SYSTEMROOT:-C:/Windows}/Fonts"
fi
[ -z "$shots" ] || mkdir -p "$shots"
export SEABASS_SCREENSHOT_DIR="$shots"

work="$(mktemp -d)"
marker="$work/save-started"
note="$stick/Seabass/backups/.save-in-progress"
# Where the readers keep their copy of a database before rolling its
# journal back: paths::localRoot() / "recovered", and localRoot() is
# SEABASS_HOME in a rig run (the sandbox's settings name no other).
recovered="${SEABASS_HOME:-$HOME/Seabass}/recovered"

rig_parts_declare F6-save-killed-mid-write F6-leftovers-recorded F6-reads-and-undoes F6-stick-at-baseline

failed=0
restored_at_end=0

restore_from_reference() {  # why
    echo "=== restoring $stick from $(basename "$reference") ($1)"
    "$build/rig_restore" "$reference" "$stick" --execute 2>&1 | grep -E "^(before|catalogs|RIG RESULT)"
    [ "${PIPESTATUS[0]}" -eq 0 ]
}

finish() {
    if [ "$restored_at_end" -eq 0 ]; then
        restored_at_end=1
        # The rig's rule for anything that writes a stick for good, and
        # whatever the checks above decided.
        if restore_from_reference "the end of F6, whatever happened"; then
            echo "=== $stick is back at its reference"
        else
            echo "=== $stick could NOT be put back at its reference: nothing after this can trust it"
            failed=1
        fi
    fi
    rig_parts_finish
    rm -rf "$work"
}

# The end of every run, early or not: the restore, then the verdict as the
# last line.
conclude() {
    trap - EXIT
    finish
    echo "RIG RESULT: $([ "$failed" = 0 ] && echo PASS || echo FAIL)"
    exit "$failed"
}
# And a run that dies of something unforeseen still restores the stick.
trap 'failed=1; conclude' EXIT

# One live test function in a process of its own, in the background so it
# can be killed; its pid in test_pid. Always a full "TestCase::function"
# name (a bare TestCase name exits without running anything).
test_pid=""
start_test() {  # full test name, log file
    SEABASS_LIVE_KILL_MARKER="$marker" "$bin" -input "$root/tests/qml-live" "$1" > "$2" 2>&1 &
    test_pid=$!
}

kill_test() {  # pid
    if rig_is_windows; then
        # MSYS's $! is not the Windows pid taskkill wants; /proc has both.
        # //F, not /F: MSYS rewrites a bare leading slash into a path.
        taskkill //F //PID "$(cat "/proc/$1/winpid")" >/dev/null 2>&1
    else
        kill -KILL "$1"
    fi
}

# Whether a process's exit status says it died of kill_test(). bash reports
# a SIGKILL as 128 + 9 on Linux and macOS. On Windows TerminateProcess
# leaves the exit code taskkill chose (1), which a test run with one
# failure also returns, so there it also takes a log with no QtTest
# "Totals:" line: a test binary that ran to its end prints one.
died_of_kill() {  # exit status, log
    if rig_is_windows; then
        [ "$1" -ne 0 ] && ! grep -q "^Totals:" "$2"
    else
        [ "$1" -eq 137 ]
    fi
}

# unmount_device(), but asked again while the volume is busy, and proved
# by the mount table rather than by the tool's exit status. Something on
# the machine can hold the stick for a moment after the kill (the second
# standalone run here was refused once, the first was not); one refusal
# is not the answer.
unmount_until_gone() {  # device
    local attempt
    for attempt in 1 2 3 4 5 6 7 8 9 10; do
        unmount_device "$1"
        if [ -z "$(device_mount_point "$1")" ]; then
            echo "--- $1 unmounted (attempt $attempt)"
            return 0
        fi
        sleep 2
    done
    echo "--- $1 is still mounted after 10 attempts" >&2
    return 1
}

show_log() {  # log
    grep -E "^(PASS|FAIL|SKIP|XFAIL|Totals)|^QDEBUG|^   (Actual|Expected|Loc)|^(onelibrary|engine):|rolled back" "$1" \
        | sed 's/^QDEBUG : [a-zA-Z0-9_]*() qml://' | sed 's/^/    /'
}

# ---- the stick at its reference, and the baseline ----------------------
if ! restore_from_reference "before the save"; then
    echo "   the stick is not at its reference, so no save on it can be measured"
    failed=1
    conclude
fi
if [ -e "$note" ]; then
    # An exact restore removes everything the reference does not hold, the
    # note included. Still there means the check below could not tell this
    # save's note from an old one.
    echo "   $note is still on the stick after the restore; the check could not tell a cut-short save from it"
    failed=1
    conclude
fi
if [ -n "$given_baseline" ]; then
    rig_catalog_baseline="$given_baseline"
    rig_catalog_digest_baseline="${given_digests:-$work/no-digests.txt}"
    [ -e "$rig_catalog_digest_baseline" ] || : > "$rig_catalog_digest_baseline"
else
    rig_catalog_baseline="$work/catalog-baseline.txt"
    rig_catalog_digest_baseline="$work/catalog-digest-baseline.txt"
fi
. "$here/rig-catalogs.sh"
if [ -z "$given_baseline" ]; then
    catalogs "$stick" > "$rig_catalog_baseline"
    catalog_digests "$stick" > "$rig_catalog_digest_baseline"
fi
echo "=== catalog baseline: $(grep -cF "$stick/" "$rig_catalog_baseline") file(s) from $rig_catalog_baseline"
mkdir -p "$recovered"
recovered_before="$(ls -1 "$recovered")"

# ---- 1. the save, killed mid-write -------------------------------------
cut_short=0
attempt=1
while [ "$attempt" -le 3 ]; do
    if [ "$attempt" -gt 1 ]; then
        # The finished save changed the stick; the next one starts where
        # the first did.
        if ! restore_from_reference "before attempt $attempt" || [ -e "$note" ]; then
            echo "   the stick could not be put back for another attempt"
            break
        fi
    fi
    rm -f "$marker"
    log="$work/killed-$attempt.log"
    echo "=== attempt $attempt: LiveInterruptedSave::test_saveKilledMidWrite"
    start_test LiveInterruptedSave::test_saveKilledMidWrite "$log"
    pid=$test_pid
    # Every 0.1 s, for at most 300 s. A test that ended on its own before
    # writing the marker has failed, and waiting on is pointless.
    seen=0
    for _ in $(seq 1 3000); do
        if [ -s "$marker" ]; then
            seen=1
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    killed=0
    if [ "$seen" -eq 1 ]; then
        if [ "$attempt" -gt 1 ]; then
            # Somewhere else in the save this time.
            delay="$(awk -v r="$RANDOM" 'BEGIN { printf "%.2f", r / 32767 * 2 }')"
            echo "--- marker seen; killing after another $delay s"
            sleep "$delay"
        fi
        kill_test "$pid" && killed=1
        echo "--- marker seen, kill sent: $killed"
    elif kill -0 "$pid" 2>/dev/null; then
        echo "--- no marker after 300 s; stopping the test"
        kill_test "$pid"
    fi
    wait "$pid"
    rc=$?
    show_log "$log"
    echo "--- the test process exited with status $rc"
    if [ "$seen" -eq 0 ]; then
        echo "   the save never reached its first track, so there was nothing to cut short"
        break
    fi
    if [ "$killed" -eq 0 ] || ! died_of_kill "$rc" "$log"; then
        echo "   the test process did not die of the kill"
        break
    fi
    if [ -e "$note" ]; then
        cut_short=1
        echo "--- $note is on the stick: the save was cut short"
        break
    fi
    echo "   the save finished before the kill, nothing was interrupted (attempt $attempt of 3)"
    attempt=$((attempt + 1))
done

# Dropping the OS's own view of the stick, where it can be given back.
remounted=1
if [ "$cut_short" -eq 1 ] && [ -n "$device" ] && ! rig_is_windows; then
    echo "=== unmounting $device and mounting it again"
    remounted=0
    if unmount_until_gone "$device" && mount_device_until_back "$device"; then
        where="$(device_mount_point "$device")"
        if [ "$where" = "$stick" ]; then
            remounted=1
        else
            echo "   $device came back at '$where', not $stick"
        fi
    else
        echo "   $device could not be unmounted and mounted again"
    fi
elif [ "$cut_short" -eq 1 ] && rig_is_windows; then
    echo "=== not remounted: Windows cannot bring the stick back unattended, and the kill alone leaves the files"
elif [ "$cut_short" -eq 1 ]; then
    echo "=== not remounted: no device given, and the kill alone leaves the files as they are"
fi

if [ "$cut_short" -eq 1 ] && [ "$remounted" -eq 1 ]; then
    rig_part F6-save-killed-mid-write PASS
else
    rig_part F6-save-killed-mid-write FAIL
    failed=1
    # Nothing below means anything without a save cut short: the parts
    # left are recorded as never run, which is a FAIL each.
    conclude
fi

# ---- 2. what it left -----------------------------------------------------
echo "=== what the interrupted save left on $stick"
state="$work/journal-state.txt"
"$build/rig_journal_state" "$stick" > "$state" 2>&1
state_rc=$?
sed 's/^/    /' "$state"
hot="$(sed -n 's/^hot-journals=\([0-9]*\) .*/\1/p' "$state")"
if [ "$state_rc" -eq 0 ] && [ -n "$hot" ]; then
    rig_part F6-leftovers-recorded PASS
else
    rig_part F6-leftovers-recorded FAIL
    failed=1
fi

# ---- 3. a fresh session reads it and undoes the save --------------------
log="$work/recovers.log"
stick_log="$stick/Seabass/seabass.log"
log_before=0
[ -f "$stick_log" ] && log_before=$(wc -l < "$stick_log")
echo "=== LiveInterruptedSave::test_recoversAndUndoes"
start_test LiveInterruptedSave::test_recoversAndUndoes "$log"
wait "$test_pid"
rc=$?
show_log "$log"
bad=0
[ "$rc" -eq 0 ] || bad=1
# QtTest exits 0 for a skip, and a test that did not run proved nothing.
if grep -q "^SKIP" "$log"; then
    echo "   SKIPPED, which counts as a failure here"
    bad=1
fi
if [ -f "$stick_log" ] && [ "$(wc -l < "$stick_log")" -gt "$log_before" ]; then
    echo "--- the stick's own log of the undo:"
    tail -n +$((log_before + 1)) "$stick_log" | sed 's/^/    /'
fi
rig_part_rc F6-reads-and-undoes "$bad"
[ "$bad" -eq 0 ] || failed=1

# ---- 4. the stick is where it was before the save -----------------------
echo "=== catalog files against the baseline"
at_baseline=0
unchanged_catalogs "$stick" || at_baseline=1
new_copies="$(comm -13 <(printf '%s\n' "$recovered_before" | sort) <(ls -1 "$recovered" | sort) | grep -c .)"
echo "--- copies kept in $recovered by this run: $new_copies (hot journals left by the save: ${hot:-unknown})"
comm -13 <(printf '%s\n' "$recovered_before" | sort) <(ls -1 "$recovered" | sort) | sed "s|^|    $recovered/|"
# One copy per database that had to be rolled back, and none when nothing
# was hot: a copy without a hot journal is a roll back nothing called for.
if [ -z "$hot" ] || [ "$new_copies" -ne "$hot" ]; then
    echo "   expected exactly ${hot:-(unknown)} kept cop(ies), one per hot journal"
    at_baseline=1
fi
rig_part_rc F6-stick-at-baseline "$at_baseline"
[ "$at_baseline" -eq 0 ] || failed=1

conclude
