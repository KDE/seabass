#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig: every scripted check in one run, against two TEST sticks and
# two reference backups. See docs/testing.md and the rig's results page.
#
#   tools/rig-shakedown.sh <out dir>
#
# Configured by environment (defaults are the rig machine's):
#   SEABASS_BUILD_DIR   the build to test                    (<repo>/build)
#   RIG_STICK_A         test stick A, gets reference A        (/media/sebas/RV2)
#   RIG_STICK_B         test stick B, gets reference B        (/media/sebas/A4-128GB)
#   RIG_DEVICE_B        stick B's partition, for the pull test (/dev/sdc1)
#   RIG_REFERENCE_A     reference backup for A (never written) (~/Seabass/e2e/backups/CORSAIR.zip)
#   RIG_REFERENCE_B     reference backup for B (never written) (~/Seabass/e2e/backups/WHALESHARK2.zip)
#   RIG_REFERENCE_PRINTS  size/mtime/manifest sha256 of both references, as check S4 recorded them
#
# Both sticks are overwritten, several times, and end as exact copies of
# their references. The references are only read. Run it with the sandbox
# profile (SEABASS_HOME, XDG_*) so nothing lands in a real profile.
#
# Each check writes <out dir>/<check>.log and one line "<check> PASS|FAIL"
# to <out dir>/summary.tsv. A failed check does not stop the run, except
# when a stick could not be put into the state the next checks need.
set -u

out="${1:?output directory}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
build="${SEABASS_BUILD_DIR:-$root/build}"
A="${RIG_STICK_A:-/media/sebas/RV2}"
B="${RIG_STICK_B:-/media/sebas/A4-128GB}"
deviceB="${RIG_DEVICE_B:-/dev/sdc1}"
refA="${RIG_REFERENCE_A:-$HOME/Seabass/e2e/backups/CORSAIR.zip}"
refB="${RIG_REFERENCE_B:-$HOME/Seabass/e2e/backups/WHALESHARK2.zip}"
prints="${RIG_REFERENCE_PRINTS:-$HOME/Seabass/e2e/reference-fingerprints.txt}"
# rig_delete_backup refuses to delete anything at these paths or beside them.
export RIG_REFERENCE_A="$refA" RIG_REFERENCE_B="$refB"
export QT_QPA_PLATFORM=offscreen

mkdir -p "$out" "$out/shots"
# F4 writes gigabytes to stick A. An interrupt between the fill and its
# removal would leave the stick full for every later check and for the
# next round; this costs nothing when there is no filler.
trap 'rm -f "$A/RIG-FILLER.bin" "$B/RIG-FILLER.bin" 2>/dev/null' EXIT
# For S3: what the everyday profile looks like before the run.
# The everyday profile, listed so that a file APPEARING counts as a change
# too: the run creating ~/.config/seabass/seabass.conf is the damage this
# looks for, and "not there" has to be recorded as plainly as a size.
# Defined here, before its first use: bash resolves a function when the
# call runs, so a definition further down would have left the baseline
# empty and failed S3 and X4 on every round.
real_profile_now() {
    for path in "$HOME/.config/seabass/seabass.conf" "$HOME/Seabass/metadata"; do
        if [ -e "$path" ]; then
            stat -c '%n %s %Y' "$path"
        else
            echo "$path ABSENT"
        fi
    done
}

real_profile_now > "$out/real-profile-before.txt"
summary="$out/summary.tsv"
: > "$summary"
a="$(basename "$A")"
b="$(basename "$B")"

# check NAME COMMAND... -- runs it into NAME.log; PASS when it exits 0 AND
# nothing inside it skipped. A skipped test proves nothing, and QtTest
# exits 0 for one, so a check that skips would otherwise read as green --
# the one thing this rig must never do before a release.
check() {
    local name="$1"; shift
    echo "$(date +%T) === $name"
    if "$@" > "$out/$name.log" 2>&1; then
        if grep -qE "^SKIP|^ *SKIPPED" "$out/$name.log"; then
            printf '%s\tFAIL\n' "$name" >> "$summary"
            echo "$(date +%T) --- $name: FAIL (it skipped; see $out/$name.log)"
            grep -E "^SKIP|^ *SKIPPED" "$out/$name.log" | head -5
            return 1
        fi
        printf '%s\tPASS\n' "$name" >> "$summary"
        echo "$(date +%T) --- $name: PASS"
        return 0
    fi
    printf '%s\tFAIL\n' "$name" >> "$summary"
    echo "$(date +%T) --- $name: FAIL (see $out/$name.log)"
    return 1
}

catalogs() {  # stick -> sha256sum lines of its catalog files
    find "$1/PIONEER/rekordbox" "$1/Engine Library/Database2" -maxdepth 1 -type f \
        \( -name '*.pdb' -o -name '*.db' -o -name '*.db-wal' \) 2>/dev/null | sort | xargs -r -d '\n' sha256sum
}

unchanged_catalogs() {  # stick -- against the baseline taken after the restores
    # A write-ahead log is part of the baseline but not part of the
    # library: SQLite checkpoints it into the database and removes it,
    # which a save followed by an undo really does. A missing -wal whose
    # database still matches is therefore not a change to the catalogs,
    # and failing on it would have the rig crying wolf on every round.
    # Anything else -- a changed byte, a missing database -- still fails.
    local checked=0
    local bad=0
    while read -r sum file; do
        if [ ! -e "$file" ]; then
            case "$file" in
                *-wal|*-journal|*-shm)
                    local main="${file%-*}"
                    if grep -qF " $main" <(grep -F "$1/" "$out/catalog-baseline.txt") && [ -f "$main" ]; then
                        echo "$file: gone (checkpointed into $(basename "$main"))"
                        continue
                    fi
                    ;;
            esac
            echo "$file: MISSING"
            bad=1
            continue
        fi
        checked=$((checked + 1))
        if [ "$(sha256sum "$file" | cut -d" " -f1)" = "$sum" ]; then
            echo "$file: OK"
        else
            echo "$file: DIFFERS"
            bad=1
        fi
    done < <(grep -F "$1/" "$out/catalog-baseline.txt")
    echo "$checked catalog file(s) compared"
    if [ "$checked" -eq 0 ]; then
        # The old one-liner failed here by accident (sha256sum -c refuses an
        # empty list); this one has to say so on purpose, or a baseline that
        # matches nothing turns every catalog check into a no-op.
        echo "NO catalog files matched the baseline for $1 -- nothing was compared"
        bad=1
    fi
    return $bad
}

references_unchanged() {
    local ok=0
    for ref in "$refA" "$refB"; do
        local name; name="$(basename "$ref")"
        local line; line="$(grep "^$name " "$prints")" || { echo "no fingerprint recorded for $name"; ok=1; continue; }
        local now="$name size=$(stat -Lc %s "$ref") mtime=$(stat -Lc %Y "$ref") manifest_sha256=$(unzip -p "$ref" SEABASS-MANIFEST.tsv | sha256sum | cut -d' ' -f1)"
        echo "recorded: $line"; echo "now:      $now"
        [ "$line" = "$now" ] || ok=1
        [ ! -w "$(readlink -f "$ref")" ] || { echo "$name is writable"; ok=1; }
    done
    return $ok
}

suite() {
    ctest --test-dir "$build" -j6 --timeout 900 --output-on-failure
}

# S2: the folder the app is pointed at holds links to the references, and
# the originals themselves cannot be written.
reference_links() {
    local ok=0
    for ref in "$refA" "$refB"; do
        local link="$(dirname "$refA")/$(basename "$ref")"
        [ -L "$link" ] || { echo "$link is not a link"; ok=1; }
        local target; target="$(readlink -f "$link" 2>/dev/null)"
        [ -f "$target" ] || { echo "$link points nowhere"; ok=1; }
        [ ! -w "$target" ] || { echo "$target is writable"; ok=1; }
        echo "$link -> $target ($(stat -Lc %A "$target"), $(stat -Lc %s "$target") bytes)"
    done
    return $ok
}

# S3: settings, metadata store and edit locks come from the throwaway
# profile. Proven by what the run wrote there, and by the everyday
# profile being untouched: the runner records it before and after.
sandbox_profile() {
    local ok=0
    # Unguarded expansions would abort the whole run under `set -u` -- and
    # XDG_DATA_HOME in particular is routinely unset.
    local home="${SEABASS_HOME:-}"
    local config="${XDG_CONFIG_HOME:-}"
    local data="${XDG_DATA_HOME:-}"
    echo "SEABASS_HOME=$home"
    echo "XDG_CONFIG_HOME=$config"
    echo "XDG_DATA_HOME=$data"
    case "$home" in "$HOME/Seabass/e2e"*) ;; *) echo "SEABASS_HOME is not the sandbox"; ok=1;; esac
    case "$config" in "$HOME/Seabass/e2e"*) ;; *) echo "XDG_CONFIG_HOME is not the sandbox"; ok=1;; esac
    case "$data" in "$HOME/Seabass/e2e"*) ;; *) echo "XDG_DATA_HOME is not the sandbox"; ok=1;; esac
    [ -f "$config/seabass/seabass.conf" ] || { echo "no settings in the sandbox"; ok=1; }
    [ -d "$home/metadata" ] || { echo "no metadata store in the sandbox"; ok=1; }
    real_profile_now > "$out/real-profile-after.txt"
    diff "$out/real-profile-before.txt" "$out/real-profile-after.txt" \
        && echo "the everyday profile is exactly as it was" \
        || { echo "THE EVERYDAY PROFILE CHANGED"; ok=1; }
    return $ok
}


# Run again at the very end of the round: S3 alone only proves the profile
# was untouched by the three checks before it.
sandbox_profile_still_clean() {
    real_profile_now > "$out/real-profile-end.txt"
    diff "$out/real-profile-before.txt" "$out/real-profile-end.txt" \
        && echo "the everyday profile is still exactly as it was" \
        || { echo "THE EVERYDAY PROFILE CHANGED DURING THE ROUND"; return 1; }
}

# W8: deletes audio for good, so the stick is restored right after.
delete_orphans() {
    SEABASS_RIG_DELETE_ORPHANS=1 live_test LiveEditMode::test_13_pendingDeletionsCancelThenComplete
}

# One test function per process, by its FULL name: a bare TestCase name
# makes the QtQuickTest runner exit 0 without running a thing, which is a
# check that silently passes -- the worst kind this rig can have.
live_test() {
    local failed=0
    # Without this every live test skips itself ("SEABASS_LIVE_STICK is not
    # set") and, before check() learnt to fail on a skip, wrote PASS having
    # run nothing. run-live.sh and rig-edits.sh export it themselves; these
    # checks call the binary directly and must too.
    # B by default, but a caller that names a stick keeps it: F4 fills A
    # and must run against A, and an unconditional export here beat its
    # prefix -- so F4 tested the stick that was never filled.
    export SEABASS_LIVE_STICK="${SEABASS_LIVE_STICK:-$B}"
    for name in "$@"; do
        echo "=== $name"
        local log="$out/live-${name//:/_}.txt"
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" "$name" 2>&1 | tee "$log"
        [ "${PIPESTATUS[0]}" -eq 0 ] || failed=1
        if grep -q "^SKIP" "$log"; then
            echo "   SKIPPED, which counts as a failure here"
            failed=1
        fi
    done
    return $failed
}

# R2 and R5: read-only pages. R5 is pointed at the folder of links.
live_pages() {
    SEABASS_RIG_REFERENCE_DIR="$(dirname "$refA")" \
        live_test LivePages::test_01_statisticsLoads LivePages::test_02_performanceLoads \
                  LivePages::test_03_manageBackupsListsTheReferences \
        && unchanged_catalogs "$B"
}

# F5: leaving with unsaved changes, both ways out.
quit_with_changes() {
    live_test LiveQuit::test_01_discardLeavesTheStickAlone LiveQuit::test_02_saveThenLeaveWritesEverything \
        && unchanged_catalogs "$B"
}

# F4: fill the stick to within a few MB, try to save, then put the space
# back whatever happened -- the filler is removed even on a failure.
full_stick() {
    # Stick A, deliberately: exFAT has no fallocate, so the filler is
    # written for real, and A's spare gigabytes cost minutes where B's cost
    # the better part of an hour for exactly the same proof.
    local filler="$A/RIG-FILLER.bin"
    local free_kb; free_kb=$(/bin/df -kP "$A" | awk 'NR==2 {print $4}')
    # About a megabyte: less than the backup of export.pdb alone, so a
    # save on this stick cannot fit however small the change is.
    local leave_kb=1024
    local size_kb=$((free_kb - leave_kb))
    local rc=0
    if [ "$size_kb" -lt 1024 ]; then
        echo "stick already has less than $leave_kb KB free; nothing to fill"
        return 1
    fi
    echo "filling $A: $free_kb KB free -> leaving about $leave_kb KB"
    dd if=/dev/zero of="$filler" bs=1M count=$((size_kb / 1024)) status=none || true
    sync
    /bin/df -hP "$A" | tail -1
    local left_kb; left_kb=$(/bin/df -kP "$A" | awk 'NR==2 {print $4}')
    if [ "$left_kb" -gt $((leave_kb * 4)) ]; then
        # The fill did not take, so a save that fits proves nothing about a
        # full stick. Said out loud rather than passed over: dd's own error
        # is swallowed so the filler is always removed.
        echo "the stick still has $left_kb KB free: the fill failed, F4 cannot be proven"
        rm -f "$filler"
        sync
        return 1
    fi
    SEABASS_RIG_FULL_STICK=1 SEABASS_LIVE_STICK="$A" live_test LiveFullStick::test_saveOnAFullStickFailsCleanly || rc=1
    rm -f "$filler"
    sync
    echo "filler removed; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free again"
    unchanged_catalogs "$A" || rc=1
    return $rc
}

cli_sync_dry_run() {
    "$build/seabass-cli" sync --rekordbox "$B/PIONEER" --engine "$B/Engine Library" --dry-run && unchanged_catalogs "$B"
}

live_edit_mode() {
    "$root/tests/qml-live/run-live.sh" "$B" "$deviceB" "$out/shots" && unchanged_catalogs "$B"
}

metadata_between_sticks() {
    SEABASS_LIVE_STICK="$A" SEABASS_LIVE_SECOND_STICK="$B" \
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" LiveEditMode::test_12_metadataFromSecondStickSaveUndo \
        && unchanged_catalogs "$A"
}

refused_while_dj_software_runs() {
    mkdir -p "$out/fake"
    cp /bin/sleep "$out/fake/rekordbox"
    "$out/fake/rekordbox" 120 &
    local fake=$!
    sleep 1
    "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --expect-refused
    local rc=$?
    kill "$fake" 2>/dev/null
    wait "$fake" 2>/dev/null
    return $rc
}

seed_fb3() {
    mkdir -p "$A/RIG-FB3"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/change-me.bin"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/remove-me.bin"
    sync
}

change_fb3() {
    sleep 2
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/change-me.bin"
    rm -f "$A/RIG-FB3/remove-me.bin"
    head -c 524288 /dev/urandom > "$A/RIG-FB3/added.bin"
    sync
    "$build/rig_backup" "$A" "$out/backups-fb/$a.zip" --expect 1,1,1,1572864
}

resume_after_keep() {
    "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --cancel-at 20 keep && "$build/rig_backup" "$B" "$out/backups-fb/$b.zip"
}

# ---- setup and suite -------------------------------------------------
check S1-suite suite
check S2-reference-links reference_links
check S3-sandboxed-profile sandbox_profile
check S4-references-unchanged references_unchanged

# ---- restores onto the test sticks -----------------------------------
check B1-restore-A "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not at its reference; stopping"; exit 1; }
check B3-restore-B "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not at its reference; stopping"; exit 1; }
{ catalogs "$A"; catalogs "$B"; } > "$out/catalog-baseline.txt"

# ---- reads -----------------------------------------------------------
check R1-R3-read-A "$build/rig_read" "$A" "$refA"
check R1-R3-read-B "$build/rig_read" "$B" "$refB"
check R2-R5-pages live_pages
check R4-sync-dry-run cli_sync_dry_run
check R6-catalogs-after-reads bash -c "grep -F '$A/' '$out/catalog-baseline.txt' | sha256sum -c && grep -F '$B/' '$out/catalog-baseline.txt' | sha256sum -c"

# ---- edits -----------------------------------------------------------
check W1-W3-W4-F1-F2-F3-live live_edit_mode
check W2-W5-W6-edits "$root/tools/rig-edits.sh" "$B" "$out/catalog-baseline.txt"
check W7-metadata-between-sticks metadata_between_sticks
check F5-quit-with-unsaved-changes quit_with_changes
check W9-saves-left-backups "$build/rig_save_backups" "$B" --expect-at-least 3

# ---- full stick backups ----------------------------------------------
mkdir -p "$out/backups-fb"
seed_fb3
check FB1-FB2-backup-A "$build/rig_backup" "$A" "$out/backups-fb/$a.zip"
check FB3-incremental change_fb3
check FB6-compact-A "$build/rig_compact" "$out/backups-fb/$a.zip"
check FB7-refused-while-dj-software-runs refused_while_dj_software_runs
check FB5-cancel-discard "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --cancel-at 10 discard
check FB4-cancel-keep-resume resume_after_keep
check FB6-compact-B "$build/rig_compact" "$out/backups-fb/$b.zip"
check FB8-restore-A-backup-onto-B "$build/rig_restore" "$out/backups-fb/$a.zip" "$B" --execute
# The round is done with A's archive by now: deleting it is the check.
check FB9-manage-backups-delete "$build/rig_delete_backup" "$out/backups-fb" "$out/backups-fb/$a.zip"
# These two leave the stick changed on purpose -- W8 deletes audio for
# good, F4 fills the stick up -- so they run last, right before the
# restores below put both sticks back at their references.
check W8-delete-orphans delete_orphans
check F4-stick-fills-up full_stick
check FB-restore-B-from-reference "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not back at its reference; stopping"; exit 1; }
check FB-restore-A-from-reference "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not back at its reference; stopping"; exit 1; }

# ---- Backup USB Stick between the two --------------------------------
mkdir -p "$out/backups-clone"
check C1-C5-backup-usb-stick "$root/tools/rig-clones.sh" "$A" "$B" "$out/backups-clone" "$refA" "$refB"
# C6: B's library does not fit on A. A preview only -- nothing is written.
mkdir -p "$out/backups-c6"
check C6-target-too-small "$build/rig_clone" "$B" "$A" "$out/backups-c6" --expect-too-small

# ---- both sticks back where they started -----------------------------
check X2-A-at-reference "$build/rig_restore" "$refA" "$A"
check X2-B-at-reference "$build/rig_restore" "$refB" "$B"
check X1-references-unchanged references_unchanged
# S3 again, at the end: the three checks before it prove nothing about the
# thirty that followed.
check X4-everyday-profile-untouched sandbox_profile_still_clean

echo
cat "$summary"
if grep -q "FAIL" "$summary"; then
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo "RIG RESULT: PASS"
