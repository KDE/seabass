#!/bin/sh
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

# Copies the Breeze icons Seabass draws into src/gui/qml/icons/breeze/.
#
# They are bundled rather than looked up in the running icon theme so the
# app looks the same on Linux, Windows and macOS: a theme lookup only
# finds anything where a theme is installed, and gives whatever that
# theme draws. Every icon is the 22 px monochrome action-style one, and
# the app tints it at render time (common/SeabassIcon.qml), so the files
# are copied byte for byte.
#
# The list below is the provenance record: upstream path inside
# breeze-icons' icons/ directory. REUSE covers the copies through
# .reuse/dep5 (LGPL-3.0-or-later). To add an icon, add its line here,
# run this, and list the file in src/gui/CMakeLists.txt.
#
# Usage: tools/update-breeze-icons.sh [breeze icons dir]
#   default: /usr/share/icons/breeze (a distribution's breeze-icon-theme);
#   a breeze-icons checkout's icons/ directory works the same.

set -eu

src="${1:-/usr/share/icons/breeze}"
dest="$(dirname "$0")/../src/gui/qml/icons/breeze"
mkdir -p "$dest"

while read -r path; do
    case "$path" in ''|'#'*) continue ;; esac
    if [ ! -e "$src/$path" ]; then
        echo "missing in $src: $path" >&2
        exit 1
    fi
    # Breeze aliases many names with symlinks; copy what they point at.
    cp "$(realpath "$src/$path")" "$dest/$(basename "$path")"
done <<'LIST'
# First page: header and home menu
actions/22/application-menu.svg
actions/22/help-about.svg
actions/22/configure.svg
actions/22/backup.svg
actions/22/view-list-details.svg
places/22/folder-open.svg
# First page: stick rows
devices/22/drive-removable-media-usb-pendrive.svg
devices/22/media-flash-sd-mmc.svg
places/22/folder.svg
actions/22/media-eject.svg
actions/22/window-close.svg
# First page and hub cards
actions/22/view-media-track.svg
actions/22/edit-clear-all.svg
actions/22/kt-check-data.svg
actions/22/office-chart-bar.svg
actions/22/speedometer.svg
actions/22/document-save.svg
actions/22/document-import.svg
places/22/server-database.svg
actions/22/exchange-positions.svg
actions/22/view-media-equalizer.svg
actions/22/edit-delete-shred.svg
actions/22/edit-copy.svg
actions/22/document-revert.svg
actions/22/bookmarks.svg
actions/22/edit-duplicate.svg
actions/22/edit-delete.svg
actions/22/draw-eraser.svg
actions/22/archive-insert.svg
actions/22/view-refresh.svg
actions/22/deep-history.svg
# Buttons and marks on the section pages
actions/22/go-home.svg
actions/22/edit-clear.svg
actions/22/edit-undo.svg
actions/22/view-sort-ascending.svg
actions/22/view-sort-descending.svg
actions/22/sidebar-collapse-left.svg
actions/22/sidebar-expand-left.svg
actions/22/media-playback-start.svg
actions/22/link.svg
actions/22/edit-find.svg
actions/22/go-up.svg
actions/22/go-down.svg
actions/22/go-previous.svg
actions/22/go-next.svg
actions/22/arrow-down.svg
actions/22/arrow-right.svg
actions/22/checkmark.svg
status/22/dialog-warning.svg
LIST
