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
export QT_QPA_PLATFORM=offscreen

mkdir -p "$out"
summary="$out/summary.tsv"
: > "$summary"
a="$(basename "$A")"
b="$(basename "$B")"

# check NAME COMMAND... -- runs it into NAME.log; PASS when it exits 0.
check() {
    local name="$1"; shift
    echo "$(date +%T) === $name"
    if "$@" > "$out/$name.log" 2>&1; then
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
    grep -F "$1/" "$out/catalog-baseline.txt" | sha256sum -c
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
check S4-references-unchanged references_unchanged

# ---- restores onto the test sticks -----------------------------------
check B1-restore-A "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not at its reference; stopping"; exit 1; }
check B3-restore-B "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not at its reference; stopping"; exit 1; }
{ catalogs "$A"; catalogs "$B"; } > "$out/catalog-baseline.txt"

# ---- reads -----------------------------------------------------------
check R1-R3-read-A "$build/rig_read" "$A" "$refA"
check R1-R3-read-B "$build/rig_read" "$B" "$refB"
check R4-sync-dry-run cli_sync_dry_run
check R6-catalogs-after-reads bash -c "grep -F '$A/' '$out/catalog-baseline.txt' | sha256sum -c && grep -F '$B/' '$out/catalog-baseline.txt' | sha256sum -c"

# ---- edits -----------------------------------------------------------
check W1-W3-W4-F1-F2-F3-live live_edit_mode
check W2-W5-W6-edits "$root/tools/rig-edits.sh" "$B" "$out/catalog-baseline.txt"
check W7-metadata-between-sticks metadata_between_sticks

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
check FB-restore-B-from-reference "$build/rig_restore" "$refB" "$B" --execute || { echo "stick B is not back at its reference; stopping"; exit 1; }
check FB-restore-A-from-reference "$build/rig_restore" "$refA" "$A" --execute || { echo "stick A is not back at its reference; stopping"; exit 1; }

# ---- Backup USB Stick between the two --------------------------------
mkdir -p "$out/backups-clone"
check C1-C5-backup-usb-stick "$root/tools/rig-clones.sh" "$A" "$B" "$out/backups-clone" "$refA" "$refB"

# ---- both sticks back where they started -----------------------------
check X2-A-at-reference "$build/rig_restore" "$refA" "$A"
check X2-B-at-reference "$build/rig_restore" "$refB" "$B"
check X1-references-unchanged references_unchanged

echo
cat "$summary"
if grep -q "FAIL" "$summary"; then
    echo "RIG RESULT: FAIL"
    exit 1
fi
echo "RIG RESULT: PASS"
