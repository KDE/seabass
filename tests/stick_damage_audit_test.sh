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
