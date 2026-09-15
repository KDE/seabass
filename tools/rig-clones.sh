#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig: Backup USB Stick between two TEST sticks -- checks C1 to C5.
#
#   tools/rig-clones.sh <stick A> <stick B> <empty backup dir> <A's reference archive> <B's reference archive>
#   SEABASS_BUILD_DIR=~/builds/seabass ...   a build outside <repo>/build
#
# A keeps its library; B becomes a backup stick of it. Both are written:
#
#   setup  B's catalogs are moved aside (B has no library, its audio stays
#          as stray files) and a stray file is dropped on it
#   C5     Create Backup USB Stick from A onto B, cancelled during the
#          backup stage: B untouched, partial archive kept
#   C1     the same again, resuming: B holds A's library, the stray file
#          survives, both sticks read as current
#   C2     a cue added on A: B is offered the update; the update runs;
#          both current again
#   C3     a cue added on B: A is offered the update
#   C4     another cue on A: changes on both, the divergence warning is
#          raised and the update still offered
#   end    both sticks are restored exactly from their reference archives,
#          whatever happened above
#
# The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL".
set -u

A="${1:?stick A}"
B="${2:?stick B}"
backups="${3:?an empty backup directory}"
referenceA="${4:?A's reference archive}"
referenceB="${5:?B's reference archive}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
build="${SEABASS_BUILD_DIR:-$root/build}"
export QT_QPA_PLATFORM=offscreen
a="$(basename "$A")"
b="$(basename "$B")"
failed=0

step() {  # name, command...
    local name="$1"; shift
    echo "=== $name"
    if "$@"; then
        echo "--- $name: pass"
    else
        echo "--- $name: FAIL"
        failed=1
    fi
}

keep_cue() {  # stick, position ms
    SEABASS_LIVE_STICK="$1" SEABASS_RIG_KEEP_CUE_MS="$2" \
        "$build/seabass_qml_tests" -input "$root/tests/qml-live" LiveEditMode::test_11_rigKeepCue
}

advise() {  # expectations...
    local args=()
    for e in "$@"; do args+=(--expect "$e"); done
    "$build/rig_advise" "$backups" "$A" "$B" "${args[@]}"
}

stray_survives() {
    [ -f "$B/RIG-STRAY.txt" ] && echo "RIG-STRAY.txt is still on $b"
}

mkdir -p "$backups"
if [ -n "$(ls -A "$backups")" ]; then
    echo "$backups is not empty"; echo "RIG RESULT: FAIL"; exit 1
fi

echo "=== setup: $b without a library, with a stray file"
mkdir -p "$B/RIG-ASIDE"
for d in PIONEER "Engine Library" Seabass; do
    [ -e "$B/$d" ] && mv "$B/$d" "$B/RIG-ASIDE/"
done
echo "a stray file the backup stick must keep" > "$B/RIG-STRAY.txt"
sync

step "C1a: $b is offered a copy of $a" advise "$b=NoBackups,clone=$a"
step "C5: create, cancelled during the backup" "$build/rig_clone" "$A" "$B" "$backups" --cancel-at 10
step "C1: create, resuming" "$build/rig_clone" "$A" "$B" "$backups"
step "C1: the stray file survives" stray_survives
step "C1: both read as current" advise "$a=Current,update=none" "$b=Current,update=none"

step "C2: a change on $a" keep_cue "$A" 30000
step "C2: $b is offered the update" advise "$b=Current,update=$a"
step "C2: update" "$build/rig_clone" "$A" "$B" "$backups"
step "C2: both current again" advise "$a=Current,update=none" "$b=Current,update=none"

step "C3: a change on $b" keep_cue "$B" 45000
step "C3: $a is offered the update" advise "$a=Current,update=$b"

step "C4: another change on $a" keep_cue "$A" 60000
step "C4: diverged, update still offered" advise "$b=Outdated,update=$a,diverged"

echo "=== end: restoring both sticks"
rm -rf "$B/RIG-ASIDE" "$B/RIG-STRAY.txt"
step "restore $a" "$build/rig_restore" "$referenceA" "$A" --execute
step "restore $b" "$build/rig_restore" "$referenceB" "$B" --execute

echo "RIG RESULT: $([ $failed = 0 ] && echo PASS || echo FAIL)"
exit $failed
