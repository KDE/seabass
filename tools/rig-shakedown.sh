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
#   RIG_STICK_A         test stick A, gets reference A        (/media/sebas/RV2; macOS /Volumes/VSTICKA)
#   RIG_STICK_B         test stick B, gets reference B        (/media/sebas/A4-128GB; macOS /Volumes/VSTICKB)
#   RIG_DEVICE_B        stick B's partition, for the pull test (/dev/sdc1; macOS: B's device node)
#   RIG_REFERENCE_A     reference backup for A (never written) (~/Seabass/e2e/backups/CORSAIR.zip)
#   RIG_REFERENCE_B     reference backup for B (never written) (~/Seabass/e2e/backups/WHALESHARK2.zip)
#   RIG_REFERENCE_PRINTS  size/mtime/manifest sha256 of both references, as check S4 recorded them
#
# On macOS the sticks may be mounted disk images (see docs/testing.md): the
# rig (tools/rig-platform.sh) sets SEABASS_ACCEPT_DISK_IMAGES=1 so the app
# lists them.
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
. "$here/rig-platform.sh"
build="${SEABASS_BUILD_DIR:-$root/build}"
if [ "$rig_os" = "Darwin" ]; then
    A="${RIG_STICK_A:-/Volumes/VSTICKA}"
    B="${RIG_STICK_B:-/Volumes/VSTICKB}"
    deviceB="${RIG_DEVICE_B:-$(stick_device "$B")}"
else
    A="${RIG_STICK_A:-/media/sebas/RV2}"
    B="${RIG_STICK_B:-/media/sebas/A4-128GB}"
    deviceB="${RIG_DEVICE_B:-/dev/sdc1}"
fi
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
    # macOS keeps the settings in a property list instead.
    for path in "$HOME/.config/seabass/seabass.conf" "$HOME/Library/Preferences/com.seabass.seabass.plist" \
                "$HOME/Seabass/metadata"; do
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
# RIG_ONLY="F4-stick-fills-up W8-delete-orphans" re-runs just those checks,
# after a fix say; the rest are neither run nor recorded, so the summary
# lists only what ran. Two things are never filtered: the restores that put
# a stick back at its reference (a partial run that skipped them left B off
# its reference, and the baseline would then be taken from a dirty stick),
# and the end of the run, which fails when a name matched nothing -- a typo
# must not read as a green run of zero checks.
rig_only_names() { printf '%s' "${RIG_ONLY:-}" | tr ',\t\n' '   '; }
rig_wants() {  # <check name> -> 0 when this run includes it
    [ -z "${RIG_ONLY:-}" ] && return 0
    # The restores from a REFERENCE only: FB8 also restores, from an
    # archive FB1 makes, and ran unasked against a missing one.
    [[ "$1" == B?-restore-? || "$1" == *-from-reference || "$1" == *-at-reference* ]] && return 0
    [[ " $(rig_only_names) " == *" $1 "* ]]
}
if [ -n "${RIG_ONLY:-}" ] && [ -z "$(rig_only_names | tr -d ' ')" ]; then
    echo "RIG_ONLY is set but names no check"
    echo "RIG RESULT: FAIL"
    exit 1
fi
ran=""

check() {
    local name="$1"; shift
    rig_wants "$name" || return 0
    ran="$ran $name"
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
    # XDG_CONFIG_HOME means nothing to Qt on macOS; there the checks run
    # the QML runner, which keeps its settings in a sandbox of its own, and
    # the everyday property list is watched below instead.
    if [ "$rig_os" != "Darwin" ]; then
        [ -f "$config/seabass/seabass.conf" ] || { echo "no settings in the sandbox"; ok=1; }
    fi
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
    SEABASS_RIG_DELETE_ORPHANS=1 live_test "$B" LiveEditMode::test_13_pendingDeletionsCancelThenComplete
}

# One test function per process, by its FULL name: a bare TestCase name
# makes the QtQuickTest runner exit 0 without running a thing, which is a
# check that silently passes -- the worst kind this rig can have.
live_test() {  # <stick> <full test name>...
    local failed=0
    local stick="$1"; shift
    # Without this every live test skips itself ("SEABASS_LIVE_STICK is not
    # set") and, before check() learnt to fail on a skip, wrote PASS having
    # run nothing. run-live.sh and rig-edits.sh export it themselves; these
    # checks call the binary directly and must too.
    # B by default, but a caller that names a stick keeps it: F4 fills A
    # and must run against A, and an unconditional export here beat its
    # prefix -- so F4 tested the stick that was never filled.
    export SEABASS_LIVE_STICK="$stick"
    # local, or this loop overwrites check()'s own $name through bash's
    # dynamic scoping, and the summary then lists a bundle under its last
    # sub-test ("LivePages::test_03" for R2-R5) -- round 4 read that way.
    local name
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
        live_test "$B" LivePages::test_01_statisticsLoads LivePages::test_02_performanceLoads \
                  LivePages::test_03_manageBackupsListsTheReferences \
        && unchanged_catalogs "$B"
}

# F5: leaving with unsaved changes, both ways out.
quit_with_changes() {
    live_test "$B" LiveQuit::test_01_discardLeavesTheStickAlone LiveQuit::test_02_saveThenLeaveWritesEverything \
        && unchanged_catalogs "$B"
}

# F4: fill the stick to within a few MB, try to save, then put the space
# back whatever happened -- the filler is removed even on a failure.
free_kb_of() { /bin/df -kP "$1" | awk 'NR==2 {print $4}'; }

fill_and_run() {  # <leave KB> <full test name> <records may appear: 0|1> <keep the filler for the next pass: 0|1>
    local leave_kb="$1" test="$2" records_may_appear="$3" keep_filler="$4"
    # Stick A, deliberately: exFAT has no fallocate, so the filler is
    # written for real, and A's spare gigabytes cost minutes where B's cost
    # the better part of an hour for exactly the same proof.
    local filler="$A/RIG-FILLER.bin"
    local free_kb; free_kb=$(free_kb_of "$A")
    # Under a quarter megabyte, and measured rather than assumed: the
    # backup this save writes is about 460 KB (the analysis file plus
    # exportLibrary.db), so a megabyte of slack let the save succeed and
    # F4 proved the opposite of its name. Below the backup's own size the
    # save has to refuse, which is the outcome this check exists for.
    local size_kb=$((free_kb - leave_kb))
    local rc=0
    if [ -f "$filler" ] && [ "$free_kb" -lt "$leave_kb" ]; then
        # The filler from the pass before, shrunk to the new margin: exFAT
        # shrinks a file in place, where writing 13 GB again costs eleven
        # minutes over USB 2 for less than a megabyte of difference. (Seen
        # before the "too little to fill" guard below, which the kept
        # filler would trip: 256 KB free IS too little to fill from.)
        echo "shrinking the filler from the pass before: $free_kb KB free -> leaving about $leave_kb KB"
        truncate -s -$((leave_kb - free_kb))K "$filler" || true
        sync
    elif [ "$size_kb" -lt "$leave_kb" ]; then
        # What the stick actually has, not the margin: this is the one
        # path that gives up on F4, so it should not misdescribe why. A
        # filler left from a pass before goes too, or the restore that
        # follows finds a full stick.
        echo "$A has $free_kb KB free, too little to fill down to $leave_kb KB; F4 cannot be proven here"
        rm -f "$filler"
        sync
        return 1
    else
        echo "filling $A: $free_kb KB free -> leaving about $leave_kb KB"
        # Appending: a filler kept from a pass whose margin was larger
        # grows rather than being cut to nothing and written again.
        dd if=/dev/zero of="$filler" bs=1M count=$((size_kb / 1024)) oflag=append conv=notrunc status=none || true
        sync
    fi
    # bs=1M rounds the filler down by up to a megabyte, and the old limit
    # of four times the margin let that through: round 4 was left with
    # 928 KB, room enough for the save's 460 KB backup, so the save fitted
    # and F4 passed having tested a stick with space on it. Top up in
    # cluster-sized steps (exFAT on these sticks: 32 KiB) to the margin,
    # then measure again; the limit is one cluster of rounding, no more.
    local left_kb; left_kb=$(free_kb_of "$A")
    if [ "$left_kb" -gt "$leave_kb" ]; then
        dd if=/dev/zero of="$filler" bs=32K count=$(((left_kb - leave_kb) / 32)) \
            oflag=append conv=notrunc status=none || true
        sync
        left_kb=$(free_kb_of "$A")
    fi
    /bin/df -hP "$A" | tail -1
    echo "left on $A after the fill: $left_kb KB (target $leave_kb KB)"
    if [ "$left_kb" -gt $((leave_kb + 32)) ]; then
        # The fill did not take, so a save that fits proves nothing about a
        # full stick. Said out loud rather than passed over: dd's own error
        # is swallowed so the filler is always removed.
        echo "the stick still has $left_kb KB free: the fill failed, F4 cannot be proven"
        rm -f "$filler"
        sync
        return 1
    fi
    # A save refused for lack of space must leave no backup record behind:
    # the one it started, a truncated backup.zip with no manifest, used
    # to stay, and Manage Backups listed it as an empty entry. Records are
    # directories; the save's .write.lock beside them is a file and stays.
    local records_before; records_before=$(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    SEABASS_RIG_FULL_STICK=1 live_test "$A" "$test" || rc=1
    local records_after; records_after=$(find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    if [ "$records_may_appear" -eq 0 ] && [ "$records_after" -ne "$records_before" ]; then
        echo "the refused save left a backup record behind: $records_before -> $records_after entries in $A/Seabass/backups"
        find "$A/Seabass/backups" -mindepth 1 -maxdepth 1 -type d -newer "$filler" 2>/dev/null | head -3
        rc=1
    fi
    if [ "$keep_filler" -eq 1 ]; then
        echo "filler kept for the next pass; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free"
    else
        rm -f "$filler"
        sync
        echo "filler removed; $(/bin/df -hP "$A" | awk 'NR==2 {print $4}') free again"
    fi
    unchanged_catalogs "$A" || rc=1
    return $rc
}

# F4: fill to under a quarter megabyte and the save has to refuse -- the
# backup it writes first is about 460 KB (measured; the margin is below
# that on purpose, see fill_and_run).
full_stick() {
    local keep=0
    rig_wants F4-undo-on-a-nearly-full-stick && keep=1
    fill_and_run 256 LiveFullStick::test_saveOnAFullStickFailsCleanly 0 "$keep"
}

# Issue #27: the same stick filled to leave room for the save and not
# for the undo. The save needs about 1.3 MB: its backup record (460 KB),
# the checkpoint copies of the two files it writes (520 KB) and one
# temporary (330 KB); 1.2 MB fitted it once and refused it a cluster
# later, so 1.6 MB. The undo then finds about 1.1 MB and needs about
# 1.25 MB (the copy of what it overwrites, one temporary, the margin),
# so it is refused for space up front, saying so -- a refusal that
# blamed the backup is what #27 was. Records may appear: the save's own,
# and the undo's pre-restore copy if it ever gets that far.
full_stick_undo() {
    fill_and_run 1600 LiveFullStick::test_undoOnANearlyFullStick 1 0
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
    # tools/rig_fake_dj.cpp, built as "rekordbox": a copy of a system
    # binary is killed on macOS, which left this check proving nothing.
    "$build/rekordbox" 120 &
    local fake=$!
    sleep 1
    "$build/rig_backup" "$B" "$out/backups-fb/$b.zip" --expect-refused
    local rc=$?
    kill "$fake" 2>/dev/null
    wait "$fake" 2>/dev/null
    return $rc
}

# FB1, with FB3's seed files put on A first so FB3 has something to
# change: part of the check body, so RIG_ONLY filters the seeding with
# the backup and a seed failure is FB1's, not FB3's.
backup_a_seeded() {
    seed_fb3 && "$build/rig_backup" "$A" "$out/backups-fb/$a.zip"
}

seed_fb3() {
    mkdir -p "$A/RIG-FB3"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/change-me.bin"
    head -c 1048576 /dev/urandom > "$A/RIG-FB3/remove-me.bin"
    sync
}

change_fb3() {
    if [ ! -d "$A/RIG-FB3" ] || [ ! -f "$out/backups-fb/$a.zip" ]; then
        echo "FB3 changes FB1's seed files and increments FB1's archive: run FB1-FB2-backup-A with it"
        return 1
    fi
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
check FB1-FB2-backup-A backup_a_seeded
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
#
# But B is not at its reference here: FB8 has just restored A's archive
# onto it. W8 cleans up duplicates on B through both catalogs, and on a
# stick carrying A's library over B's files a group's survivor has no
# Device Library Plus row -- which Clean Up deliberately refuses, so W8
# failed every full round for a reason that has nothing to do with it.
# Its first full round (round 4) is where that showed.
check W8-B-at-reference-first "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B could not be put back before W8; stopping"; exit 1; }
check W8-delete-orphans delete_orphans
check F4-stick-fills-up full_stick
check F4-undo-on-a-nearly-full-stick full_stick_undo
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

set -f  # a name like F4-* must be reported as typed, not glob-expanded
for wanted in $(rig_only_names); do
    if [[ " $ran " != *" $wanted "* ]]; then
        echo "RIG_ONLY names '$wanted', which is no check of this rig"
        printf '%s\tFAIL\n' "$wanted" >> "$summary"
    fi
done
set +f
if [ ! -s "$summary" ]; then
    echo "no check ran"
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo
cat "$summary"
if grep -q "FAIL" "$summary"; then
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo "RIG RESULT: PASS"
