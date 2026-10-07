#!/bin/bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# stick_damage_audit's #57 check against a copy of the anonymized fixture,
# which came off a stick a dev build had synced: one OneLibrary row
# (content_id 392) holds pads B and E in its cue table that its analysis
# file does not. Sixteen rows whose table is a subset of their file (the
# file adds rekordbox's memory cue at 0:00) are not damage and must not be
# listed. The counts are pinned, so a check that flags too much fails as
# surely as one that flags nothing.
#
#   stick_damage_audit_test.sh <stick_damage_audit binary>
set -u
set -o pipefail

audit="${1:?usage: $0 <stick_damage_audit binary>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fixture="$here/fixtures/anonymized_library/rekordbox"

work="$(mktemp -d "${TMPDIR:-/tmp}/seabass-damage-audit.XXXXXX")" || exit 1
trap 'rm -rf "$work"' EXIT
# A copy: opening the fixture's database in place would drop its -shm/-wal.
cp -a "$fixture" "$work/PIONEER" || exit 1

out="$("$audit" "$work" 2>&1)" || { echo "$out"; echo "FAIL: the audit exited non-zero"; exit 1; }
summary="$(grep '^#57 ' <<<"$out")"
expected="#57 OneLibrary cue table against the analysis file: 1644 rows read; 1627 with an empty table, 16 within the file, 0 with no analysis file read (left alone), 0 with cue kinds Seabass does not understand (left alone), 1 hold cues the analysis file does not"
if [ "$summary" != "$expected" ]; then
    echo "$out"
    echo "FAIL: expected: $expected"
    echo "      got:      $summary"
    exit 1
fi
listed="$(grep -c '^  content_id ' <<<"$out")"
if [ "$listed" != 1 ] || ! grep -q '^  content_id 392 ' <<<"$out"; then
    echo "$out"
    echo "FAIL: expected content_id 392 and only it listed, got $listed rows"
    exit 1
fi
# Cues are named by position, the way Library Health names them.
if ! grep -qx '    cue table:         pad A at 0:00.247, pad C at 0:01.188, pad D at 0:01.657, memory cue at 0:04.399, pad B at 1:07.751, pad E at 2:15.251' <<<"$out" \
    || ! grep -qx '    only in the table: pad B at 1:07.751, pad E at 2:15.251' <<<"$out"; then
    echo "$out"
    echo "FAIL: content_id 392's cue table, or what only it holds, is not listed as read"
    exit 1
fi
echo "stick_damage_audit #57 on the fixture: OK"

# The tool only reads. exportLibrary.db with an unfinished save's journal
# beside it (a live header is enough) would be rolled back by a read, so
# #57 is refused, nothing is written, and the Engine check still runs.
journalled="$work/journalled"
mkdir -p "$journalled" || exit 1
cp -a "$fixture" "$journalled/PIONEER" || exit 1
cp -a "$here/fixtures/anonymized_library/engine" "$journalled/Engine Library" || exit 1
db="$journalled/PIONEER/rekordbox/exportLibrary.db"
printf '\xd9\xd5\x05\xf9\x20\xa1\x63\xd7' > "$db-journal"
head -c 504 /dev/zero >> "$db-journal"
before="$(cd "$journalled/PIONEER/rekordbox" && cksum exportLibrary.db* | sort)"
out="$("$audit" "$journalled" 2>&1)" || { echo "$out"; echo "FAIL: the audit exited non-zero over a journal"; exit 1; }
after="$(cd "$journalled/PIONEER/rekordbox" && cksum exportLibrary.db* | sort)"
if ! grep -q '^#57 not checked: exportLibrary.db holds an unfinished save' <<<"$out" || grep -q '^#57 OneLibrary' <<<"$out"; then
    echo "$out"
    echo "FAIL: #57 must refuse a database with a pending journal"
    exit 1
fi
if [ "$before" != "$after" ]; then
    echo "FAIL: the audit changed exportLibrary.db or its journal"
    exit 1
fi
if ! grep -q '^#56 Engine cues the player hides: ' <<<"$out"; then
    echo "$out"
    echo "FAIL: #56 must still run when #57 is refused"
    exit 1
fi
echo "stick_damage_audit refuses a pending journal and reads on: OK"
