#!/bin/sh
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Switch which rekordbox library is the live one, by swapping the folder
# rekordbox looks in.
#
# Why not rekordbox's own setting: Preferences -> Advanced -> Database ->
# "Database management" offers DRIVES, not folders. The feature exists to put
# the Master Database on an external drive ("With Master Database stored in a
# single external drive as well as tracks, you can manage your collection
# easily among multiple computers"), so it cannot point at a second folder on
# the internal disk. What it can do is keep looking where it always looks --
# so this renames libraries into and out of that one place, and rekordbox
# needs no setting changed at all.
#
# Usage:
#   rekordbox-switch.sh                 # which library is live, and what else exists
#   rekordbox-switch.sh <name>          # make <name> live
#   rekordbox-switch.sh --dry-run <name>
#
# A library is a directory next to the live one, named "rekordbox-<name>".

set -eu

BASE="$HOME/Library/Pioneer"
LIVE="$BASE/rekordbox"
DRY=""

if [ "${1-}" = "--dry-run" ]; then
    DRY="yes"
    shift
fi

# rekordbox rewrites its database on quit. Switching underneath a running
# instance is how a library gets half-written, so this refuses rather than
# racing it.
if pgrep -x rekordbox >/dev/null 2>&1; then
    echo "rekordbox is running -- quit it first." >&2
    exit 1
fi

current_label() {
    if [ -f "$LIVE/.library-name" ]; then
        cat "$LIVE/.library-name"
    else
        echo "original"
    fi
}

if [ $# -eq 0 ]; then
    if [ -d "$LIVE" ]; then
        echo "live: $(current_label)  ($(du -sh "$LIVE" 2>/dev/null | cut -f1))"
    else
        echo "live: none -- $LIVE does not exist"
    fi
    echo "available:"
    for dir in "$BASE"/rekordbox-*; do
        [ -d "$dir" ] || continue
        name=${dir##*/rekordbox-}
        echo "  $name  ($(du -sh "$dir" 2>/dev/null | cut -f1))"
    done
    exit 0
fi

WANTED="$1"
TARGET="$BASE/rekordbox-$WANTED"

if [ ! -d "$TARGET" ]; then
    echo "no such library: $TARGET" >&2
    echo "make one with: mkdir \"$TARGET\"   (rekordbox fills it on first launch)" >&2
    exit 1
fi

CURRENT=$(current_label)
if [ "$CURRENT" = "$WANTED" ]; then
    echo "$WANTED is already live"
    exit 0
fi

PARKED="$BASE/rekordbox-$CURRENT"
if [ -d "$LIVE" ] && [ -e "$PARKED" ] && [ "$PARKED" != "$TARGET" ]; then
    echo "cannot park the live library: $PARKED already exists" >&2
    exit 1
fi

if [ -n "$DRY" ]; then
    echo "would move $LIVE -> $PARKED"
    echo "would move $TARGET -> $LIVE"
    exit 0
fi

# Two renames on one filesystem: nothing is copied and nothing is deleted, so
# an interrupted switch leaves both libraries intact, just possibly parked.
if [ -d "$LIVE" ]; then
    echo "$CURRENT" > "$LIVE/.library-name"
    mv "$LIVE" "$PARKED"
fi
mv "$TARGET" "$LIVE"
echo "$WANTED" > "$LIVE/.library-name"

echo "live: $WANTED"
echo "parked: $CURRENT"
echo "Launch rekordbox; it looks in $LIVE as it always does."
