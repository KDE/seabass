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
# W10 Sync after Rekordbox Export (tools/rig_engine_update): plants a
#     baseline lacking one track and removes that track's Engine row, so
#     the stick reads as "rekordbox added it since"; the proposal names
#     it, the page's controller syncs it, the proposal is empty after,
#     the row is back in its playlists, the record is at export.pdb's
#     sequence; the undo gives back the planted catalogs byte for byte,
#     and the plant is put back so the stick is as it was
#
#   RIG_EDITS_PARTS="W2 W5 W6 W10"   the checks to run (that is the default)
#   RIG_ENGINE_UPDATE_TRACK=<path>  W10's track, stick-relative; else
#                                   rig_engine_update --pick chooses one
#   RIG_SCRATCH_STICK=1             the "stick" is a folder laid out as one
#                                   (refuse_unless_scratch_folder in
#                                   rig-platform.sh); W10 only, as the QML
#                                   checks were never proved that way
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
# Never beside a working stick or a reference (see rig-platform.sh), unless
# pointed at a scratch folder standing in for a stick (RIG_SCRATCH_STICK).
if [ -n "${RIG_SCRATCH_STICK:-}" ]; then
    refuse_unless_scratch_folder "$stick"
else
    refuse_if_protected_sticks_inserted
fi
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

# Which checks run: rig-shakedown.sh runs W10 as a check of its own, so
# RIG_ONLY can name it, and the rest as the W2-W5-W6-edits bundle.
parts="${RIG_EDITS_PARTS:-W2 W5 W6 W10}"
wants() { [[ " $parts " == *" $1 "* ]]; }
for part in $parts; do
    case "$part" in
        W2|W5|W6|W10) ;;
        *) echo "RIG_EDITS_PARTS names '$part', which is no check of this script"
           echo "RIG RESULT: FAIL"
           exit 1 ;;
    esac
done

if [ -n "${RIG_SCRATCH_STICK:-}" ] && [ "$(echo $parts)" != "W10" ]; then
    echo "RIG_SCRATCH_STICK runs W10 alone: set RIG_EDITS_PARTS=W10"
    echo "RIG RESULT: FAIL"
    exit 1
fi

# One board row per test: W5 skipping for want of a duplicate group on the
# stick used to take W2 and W6 red with it, and a reader could not tell.
wants W2 && rig_parts_declare W2-add-cue
wants W5 && rig_parts_declare W5-clean-up-group W5-import-prompt-not-armed
wants W6 && rig_parts_declare W6-library-health-repair W6-import-prompt-not-armed
wants W10 && rig_parts_declare W10-engine-update-sync-undo
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

wants W2 && run W2-add-cue LiveEditMode::test_08_addCueSaveUndo

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
if wants W5; then
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
fi

if wants W6; then
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
fi

# W10: Sync after Rekordbox Export, headless (tools/rig_engine_update.cpp;
# docs/sync-after-rekordbox-export-plan.md, "Tests", the rig). Each
# assertion prints its own "RIG RESULT: W10 <what>: PASS|FAIL" line, and
# one that could not run because an earlier step failed prints FAIL too:
# a step that did not run has proved nothing.
w10_assertions="catalogs-recorded-before-plant track-picked planted catalogs-recorded-after-plant \
plan-proposes-something plan-names-the-track synced plan-empty-after-sync engine-row-back-in-its-playlists \
record-at-export-sequence undone catalogs-as-planted record-as-planted plant-restored \
catalogs-as-before-plant record-as-before-plant"
w10_seen=""
w10_failed=0
w10() {  # assertion, exit status
    w10_seen="$w10_seen $1"
    if [ "$2" -eq 0 ]; then
        echo "RIG RESULT: W10 $1: PASS"
    else
        echo "RIG RESULT: W10 $1: FAIL"
        w10_failed=1
    fi
}
w10_unreached() {
    local name
    for name in $w10_assertions; do
        case " $w10_seen " in
            *" $name "*) ;;
            *) echo "RIG RESULT: W10 $name: FAIL (it never ran)"; w10_failed=1 ;;
        esac
    done
}
# The record's state as one word: its sha256, or "absent".
w10_record_state() {
    local file="$stick/Seabass/rekordbox-baseline.tsv.gz"
    if [ -e "$file" ]; then sha256sum "$file" | cut -d' ' -f1; else echo absent; fi
}
eu() { "$build/rig_engine_update" --rekordbox "$stick/PIONEER" --engine "$stick/Engine Library" "$@"; }

engine_update_check() {
    local state; state="$(mktemp -d)"
    local planted=0
    echo "=== W10: Sync after Rekordbox Export, planted, synced and undone"
    if [ ! -x "$build/rig_engine_update" ]; then
        echo "$build/rig_engine_update is missing: build the rig_engine_update target"
        w10_unreached
        rm -rf "$state"
        return 1
    fi
    eu --catalog-shas "$state/before-plant.txt"
    w10 catalogs-recorded-before-plant $?
    local record_before; record_before="$(w10_record_state)"

    local track="${RIG_ENGINE_UPDATE_TRACK:-}"
    [ -n "$track" ] || track="$(eu --pick)"
    echo "track: ${track:-(none)}"
    [ -n "$track" ]
    w10 track-picked $?

    if [ -n "$track" ]; then
        eu --plant-baseline-without "$track"
        local rc=$?
        # Whatever it says, it may have changed the stick: put back below.
        [ -e "$stick/RIG-ENGINE-UPDATE/planted.tsv" ] && planted=1
        w10 planted "$rc"
        if [ "$rc" -eq 0 ]; then
            eu --catalog-shas "$state/after-plant.txt"
            w10 catalogs-recorded-after-plant $?
            local record_planted; record_planted="$(w10_record_state)"

            eu --plan > "$state/plan-before.txt"
            rc=$?
            cat "$state/plan-before.txt"
            [ "$rc" -eq 1 ]
            w10 plan-proposes-something $?
            grep -qF "  add $track  " "$state/plan-before.txt"
            w10 plan-names-the-track $?

            eu --sync
            w10 synced $?

            eu --plan > "$state/plan-after.txt"
            rc=$?
            cat "$state/plan-after.txt"
            [ "$rc" -eq 0 ]
            w10 plan-empty-after-sync $?

            eu --check-back "$track"
            w10 engine-row-back-in-its-playlists $?

            # The save's own record, not the planted one left standing:
            # the plant signs its record "rig_engine_update", a save signs
            # with the Seabass version.
            local sequence header recorded writer
            sequence="$(sed -n 's/.*export.pdb at \([0-9][0-9]*\)$/\1/p' "$state/plan-after.txt" | head -1)"
            header="$(gzip -dc "$stick/Seabass/rekordbox-baseline.tsv.gz" 2>/dev/null | head -1)"
            recorded="$(printf '%s' "$header" | cut -f3)"
            writer="$(printf '%s' "$header" | cut -f6)"
            echo "record at export ${recorded:-(none)} by ${writer:-(nobody)}, export.pdb at ${sequence:-(unread)}"
            [ -n "$sequence" ] && [ "$recorded" = "$sequence" ] && [ -n "$writer" ] && [ "$writer" != rig_engine_update ]
            w10 record-at-export-sequence $?

            eu --undo
            w10 undone $?

            eu --catalog-compare "$state/after-plant.txt"
            w10 catalogs-as-planted $?
            [ "$(w10_record_state)" = "$record_planted" ]
            w10 record-as-planted $?
        fi
    fi
    if [ "$planted" -eq 1 ]; then
        eu --restore-plant
        w10 plant-restored $?
        eu --catalog-compare "$state/before-plant.txt"
        w10 catalogs-as-before-plant $?
        [ "$(w10_record_state)" = "$record_before" ]
        w10 record-as-before-plant $?
    fi
    w10_unreached
    rm -rf "$state"
    return "$w10_failed"
}

if wants W10; then
    if engine_update_check; then
        rig_part W10-engine-update-sync-undo PASS
    else
        rig_part W10-engine-update-sync-undo FAIL
        failed=1
    fi
fi

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
