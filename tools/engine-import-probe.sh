#!/bin/bash
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# What a Denon player's "update the Rekordbox library" prompt does to the
# Engine side of a stick, measured: the driver around engine_import_probe.
# The protocol, and what is measured versus guessed, is in
# docs/engine-import-prompt.md.
#
#   engine-import-probe.sh plant   <stick root> <work dir>
#       snapshot the stick as it is (pre-plant.tar), plant the matrix
#       (cases.tsv), record it (before.tsv), snapshot again (planted.tar)
#
#   (insert the stick into the player, accept the import, eject it)
#
#   engine-import-probe.sh record  <stick root> <work dir>
#       record the stick after the import (after.tsv) and snapshot it
#       (after-import.tar)
#
#   engine-import-probe.sh compare <work dir>
#       compare before.tsv and after.tsv per planted case (report.txt)
#
#   engine-import-probe.sh restore <stick root> <work dir>
#       put pre-plant.tar back, for the next round
#
# PROBE names the engine_import_probe binary when it is not on PATH, e.g.
# PROBE=~/Seabass/builds/master/engine_import_probe. COVER, when set, is
# the image case k falls back to when the stick has neither a repairable
# imported cover nor rekordbox art for a track (see the doc).
#
# Snapshots are tar files of the stick's "Engine Library" and "PIONEER"
# folders (and "Seabass probe", where the plant puts its Engine-only
# track), taken outside the stick in <work dir>. They are the raw
# evidence: the records are derived from them, and a question nobody
# thought of yet can still be asked of them later.

set -u

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=rig-platform.sh
. "$here/rig-platform.sh"

fail() { echo "error: $*" >&2; exit 2; }

probe_binary() {
    if [ -n "${PROBE:-}" ]; then
        [ -x "$PROBE" ] || fail "PROBE=$PROBE is not an executable"
        printf '%s\n' "$PROBE"
    elif command -v engine_import_probe >/dev/null 2>&1; then
        command -v engine_import_probe
    else
        fail "engine_import_probe not found: set PROBE to the binary in your build directory"
    fi
}

check_stick() {  # <stick root>
    [ -d "$1" ] || fail "$1 is not a directory"
    is_protected_label "$(stick_label "$1")" && fail "$1 is a protected stick; this probe never writes it"
    [ -f "$1/Engine Library/Database2/m.db" ] || fail "$1 has no Engine Library/Database2/m.db"
    [ -f "$1/PIONEER/rekordbox/export.pdb" ] || fail "$1 has no PIONEER/rekordbox/export.pdb"
}

snapshot() {  # <stick root> <tar file>
    local parts=("Engine Library" "PIONEER")
    [ -d "$1/Seabass probe" ] && parts+=("Seabass probe")
    tar -C "$1" -cf "$2" "${parts[@]}" || fail "could not snapshot $1 into $2"
    echo "snapshot: $2 ($(du -h "$2" | cut -f1))"
}

[ $# -ge 2 ] || { sed -n '7,30p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
step="$1"

case "$step" in
plant)
    [ $# -eq 3 ] || fail "usage: $0 plant <stick root> <work dir>"
    stick="$2"; work="$3"
    check_stick "$stick"
    probe="$(probe_binary)"
    [ -e "$work/cases.tsv" ] && fail "$work already holds a planted round; use a new work dir"
    mkdir -p "$work" || fail "could not create $work"
    snapshot "$stick" "$work/pre-plant.tar"
    "$probe" --plant "$stick" --out "$work/cases.tsv" ${COVER:+--cover "$COVER"} | tee "$work/plant.log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail "planting failed; the stick can be put back with: $0 restore '$stick' '$work'"
    "$probe" --record "$stick" --out "$work/before.tsv" | tee "$work/record-before.log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail "recording the planted stick failed"
    sync
    snapshot "$stick" "$work/planted.tar"
    echo
    echo "Planted. Now: eject the stick, insert it into the player, accept the import prompt,"
    echo "wait for it to finish, eject it from the player's menu, then:"
    echo "  $0 record '$stick' '$work'"
    ;;
record)
    [ $# -eq 3 ] || fail "usage: $0 record <stick root> <work dir>"
    stick="$2"; work="$3"
    check_stick "$stick"
    probe="$(probe_binary)"
    [ -f "$work/before.tsv" ] || fail "$work has no before.tsv: plant first"
    [ -e "$work/after.tsv" ] && fail "$work already has after.tsv; move it aside to record again"
    # The snapshot first: it is the evidence, and recording reads through
    # libdjinterop, which recovers a journal the player may have left.
    snapshot "$stick" "$work/after-import.tar"
    "$probe" --record "$stick" --out "$work/after.tsv" | tee "$work/record-after.log"
    [ "${PIPESTATUS[0]}" -eq 0 ] || fail "recording failed"
    echo "next: $0 compare '$work'"
    ;;
compare)
    [ $# -eq 2 ] || fail "usage: $0 compare <work dir>"
    work="$2"
    probe="$(probe_binary)"
    for f in cases.tsv before.tsv after.tsv; do
        [ -f "$work/$f" ] || fail "$work has no $f"
    done
    "$probe" --compare "$work/before.tsv" "$work/after.tsv" --cases "$work/cases.tsv" | tee "$work/report.txt"
    exit "${PIPESTATUS[0]}"
    ;;
restore)
    [ $# -eq 3 ] || fail "usage: $0 restore <stick root> <work dir>"
    stick="$2"; work="$3"
    check_stick "$stick"
    [ -f "$work/pre-plant.tar" ] || fail "$work has no pre-plant.tar"
    rm -rf "$stick/Engine Library" "$stick/PIONEER" "$stick/Seabass probe" || fail "could not clear $stick"
    tar -C "$stick" -xf "$work/pre-plant.tar" || fail "could not unpack pre-plant.tar onto $stick"
    sync
    echo "restored $stick from $work/pre-plant.tar"
    ;;
*)
    fail "unknown step: $step (plant, record, compare or restore)"
    ;;
esac
