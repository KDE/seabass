#!/usr/bin/env bash

# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#
# Regenerates tests/fixtures/anonymized_library from a real USB stick.
#
# The committed fixture was exported by Seabass 0.6, before the anonymizer
# compacted what it scrubbed, and still holds real data that no reader can
# see. Replacing it needs the originating hardware, which is why this is a
# script you run with a stick plugged in rather than part of the suite.
#
# It writes nothing to the stick. It mounts it read-only if it has to
# mount it at all, and every write goes to a scratch directory.
#
# Usage:
#   tools/regenerate-test-fixture.sh [STICK_MOUNT] [BUILD_DIR]
#
# Defaults: the stick is found by label, the build is ../builds/master.

set -euo pipefail

STICK="${1:-}"
BUILD="${2:-$(cd "$(dirname "$0")/.." && pwd)/../builds/master}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

if [ -z "$STICK" ]; then
    for candidate in /media/"$USER"/*; do
        if [ -d "$candidate/PIONEER" ] || [ -d "$candidate/Engine Library" ]; then
            STICK="$candidate"
            break
        fi
    done
fi
if [ -z "$STICK" ] || [ ! -d "$STICK" ]; then
    echo "No stick found. Plug one in, or pass its mount point:" >&2
    echo "  $0 /media/$USER/WHALESHARK2" >&2
    echo >&2
    echo "Unmounted sticks show up in: lsblk -o NAME,LABEL,FSTYPE,SIZE,MOUNTPOINT" >&2
    echo "Mount one read-only with: udisksctl mount -b /dev/sdX1 -o ro" >&2
    exit 1
fi

CLI="$BUILD/seabass-cli"
if [ ! -x "$CLI" ]; then
    echo "No seabass-cli at $CLI -- build it first:" >&2
    echo "  cmake --build $BUILD --target seabass-cli" >&2
    exit 1
fi

echo "Stick:  $STICK"
if findmnt -no OPTIONS "$STICK" 2>/dev/null | tr ',' '\n' | grep -qx rw; then
    echo "        WARNING: mounted read-write. Nothing here writes to it, but"
    echo "        read-only is the safer way to run this."
fi

ARGS=()
[ -d "$STICK/PIONEER" ]       && ARGS+=(--rekordbox "$STICK/PIONEER")
[ -d "$STICK/Engine Library" ] && ARGS+=(--engine "$STICK/Engine Library")
if [ ${#ARGS[@]} -eq 0 ]; then
    echo "Neither PIONEER/ nor 'Engine Library/' is on $STICK." >&2
    exit 1
fi

echo "Output: $OUT"
echo
if "$CLI" anonymize "${ARGS[@]}" --out "$OUT" 2>&1 | grep -vE '^warning: track id='; then
    :
fi

if [ ! -f "$OUT/MANIFEST.txt" ]; then
    echo
    echo "REFUSED -- the export still held real data, so nothing was written."
    echo "That is the gate working. The detail above names the file and what"
    echo "was found in it; see anonymization_byte_sweep.hpp for why the"
    echo "reader-based checks cannot see it."
    exit 1
fi

echo
echo "Export verified clean. To adopt it as the committed fixture:"
echo
echo "  rm -rf $REPO/tests/fixtures/anonymized_library"
echo "  cp -r $OUT $REPO/tests/fixtures/anonymized_library"
echo "  cd $REPO && ctest --test-dir $BUILD"
echo
echo "Then, because the old one is public on both hosts and a replacement"
echo "commit does not remove it from history, decide whether the old blobs"
echo "need stripping from history too."
echo
echo "Afterwards the known-dirty exceptions come out: knownDirtyFixtureFiles"
echo "in tests/anonymization_verifier_test.cpp, and WILL_FAIL on"
echo "anonymized_fixture_is_clean_test in CMakeLists.txt. Both are written"
echo "to fail once the fixture is clean, so the suite will tell you."
