#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Release rig: record what a reference backup is, so every machine
# compares against the same thing.
#
#   tools/rig-record-references.sh [--protect] [--replace] [reference.zip...]
#
# With no arguments it takes the two the rig itself uses, RIG_REFERENCE_A
# and RIG_REFERENCE_B, and writes to RIG_REFERENCE_PRINTS
# ($HOME/Seabass/e2e/reference-fingerprints.txt by default).
#
# Checks S4 and X1 read that file and compare a reference's size,
# modification time and manifest checksum against the line recorded for
# it. Nothing has ever written the file: it has been made by hand on each
# machine, which is how three machines end up comparing against three
# different baselines and all reporting green. This writes it, in exactly
# the format references_unchanged() greps for, and then proves the line
# it wrote is one that check accepts.
#
# Run it once per new reference, on one machine, and copy the file to the
# others beside the archive it describes.
#
# Three refusals, because each of them would otherwise record a
# fingerprint that looks fine and means nothing:
#
#   - an archive with no SEABASS-MANIFEST.tsv in it. `unzip -p` prints
#     nothing for a missing entry and exits 0 on some builds, and the
#     sha256 of nothing is a perfectly good-looking constant that every
#     other empty archive would also match.
#   - a writable reference. S4 fails a round for one, so recording it
#     would record a state the rig rejects. --protect makes it read-only
#     and records it.
#   - a line that already exists and differs, without --replace. The
#     whole point of the file is noticing that a reference changed;
#     quietly re-baselining it is the one thing this must not do.
#
# The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL".
set -u

here="$(cd "$(dirname "$0")" && pwd)"
# Same preconditions as every other rig script: GNU stat, sha256sum and
# friends, with the same message when they are missing.
. "$here/rig-platform.sh"

protect=0
replace=0
refs=()
for arg in "$@"; do
    case "$arg" in
        --protect) protect=1 ;;
        --replace) replace=1 ;;
        -h|--help)
            sed -n '7,41p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        -*)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
        *) refs+=("$arg") ;;
    esac
done

if [ "${#refs[@]}" -eq 0 ]; then
    refs=("${RIG_REFERENCE_A:-$HOME/Seabass/e2e/backups/CORSAIR.zip}"
          "${RIG_REFERENCE_B:-$HOME/Seabass/e2e/backups/WHALESHARK2.zip}")
fi
prints="${RIG_REFERENCE_PRINTS:-$HOME/Seabass/e2e/reference-fingerprints.txt}"

failed=0

# The one place the format lives, character for character the line
# references_unchanged() builds to compare against. Anything else here is
# a fingerprint that can never match.
fingerprint_line() {  # <reference path> -> "<name> size=.. mtime=.. manifest_sha256=.."
    local ref="$1"
    local name; name="$(basename "$ref")"
    # Piped straight into sha256sum, never captured in a variable first:
    # $(...) strips trailing newlines, and a manifest hashed without its
    # last newline gives a different sum from the one S4 computes off the
    # same pipe. The first draft of this script did exactly that and
    # wrote a fingerprint that could never match -- caught by the
    # read-back below, which is why it is there.
    local bytes; bytes="$(unzip -p "$ref" SEABASS-MANIFEST.tsv 2>/dev/null | wc -c)"
    if [ "${bytes:-0}" -eq 0 ]; then
        return 1
    fi
    printf '%s size=%s mtime=%s manifest_sha256=%s\n' \
        "$name" \
        "$(stat -Lc %s "$ref")" \
        "$(stat -Lc %Y "$ref")" \
        "$(unzip -p "$ref" SEABASS-MANIFEST.tsv | sha256sum | cut -d' ' -f1)"
}

mkdir -p "$(dirname "$prints")"
touch "$prints"

# Two references with the same file name would both be matched by the
# grep S4 does, and the first line would answer for the second.
seen=""
for ref in "${refs[@]}"; do
    name="$(basename "$ref")"
    case " $seen " in
        *" $name "*)
            echo "two references are both called $name; S4 greps by name and could not tell them apart"
            failed=1
            continue
            ;;
    esac
    seen="$seen $name"

    echo "=== $ref"
    if [ ! -f "$ref" ]; then
        echo "   not a file"
        failed=1
        continue
    fi

    target="$(readlink -f "$ref")"
    if [ -w "$target" ]; then
        if [ "$protect" -eq 1 ]; then
            chmod a-w "$target" || { echo "   could not make it read-only"; failed=1; continue; }
            echo "   made read-only"
        else
            echo "   writable, and S4 fails a round for that. Re-run with --protect, or: chmod a-w $target"
            failed=1
            continue
        fi
    fi

    if ! line="$(fingerprint_line "$ref")"; then
        echo "   no SEABASS-MANIFEST.tsv inside it, so there is nothing to fingerprint"
        failed=1
        continue
    fi

    if existing="$(grep "^$name " "$prints")"; then
        if [ "$existing" = "$line" ]; then
            echo "   already recorded, unchanged"
            echo "   $line"
            continue
        fi
        if [ "$replace" -eq 0 ]; then
            echo "   a different line is already recorded for $name, and this is not a re-baseline:"
            echo "   recorded: $existing"
            echo "   now:      $line"
            echo "   pass --replace if the reference really was rebuilt"
            failed=1
            continue
        fi
        echo "   replacing:"
        echo "   was: $existing"
        # Rewrite without this name, then append: leaves every other
        # reference's line exactly as it was.
        tmp="$(mktemp)"
        grep -v "^$name " "$prints" > "$tmp" || true
        mv "$tmp" "$prints"
    fi
    printf '%s\n' "$line" >> "$prints"
    echo "   now: $line"
done

# Proving it rather than trusting it: the same comparison S4 makes, on
# what was just written. A fingerprint file this script wrote that S4
# would reject is worse than no file at all, because it fails in the
# middle of a round rather than here.
echo
echo "=== reading it back the way S4 does"
for ref in "${refs[@]}"; do
    [ -f "$ref" ] || continue
    name="$(basename "$ref")"
    recorded="$(grep "^$name " "$prints")" || { echo "$name: nothing recorded"; failed=1; continue; }
    now="$name size=$(stat -Lc %s "$ref") mtime=$(stat -Lc %Y "$ref") manifest_sha256=$(unzip -p "$ref" SEABASS-MANIFEST.tsv | sha256sum | cut -d' ' -f1)"
    if [ "$recorded" = "$now" ]; then
        echo "$name: matches"
    else
        echo "$name: DOES NOT MATCH what S4 would compute"
        echo "   recorded: $recorded"
        echo "   S4 sees:  $now"
        failed=1
    fi
    if [ -w "$(readlink -f "$ref")" ]; then
        echo "$name: still writable, which S4 fails on"
        failed=1
    fi
done

echo
echo "$prints:"
sed 's/^/   /' "$prints"
echo
if [ "$failed" -eq 0 ]; then
    echo "RIG RESULT: PASS"
else
    echo "RIG RESULT: FAIL"
fi
exit "$failed"
