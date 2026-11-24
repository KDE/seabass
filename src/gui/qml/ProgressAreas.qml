// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma Singleton

import QtQuick

// The places on screen where an operation is showing its progress, so a
// ConfirmationToast can stay clear of them (#122). A save's toast comes
// up as the page starts reading the stick again, and both used to be
// centred on the page: the toast sat on the bar of the very refresh it
// started, and for six seconds nobody could read how far it was.
//
// ProgressReport adds itself, which covers every bar drawn by it (the
// overlay's, the inline ones on the backup, clone, restore and preview
// pages); BusyOverlay adds its whole column, title included. An area
// counts only while it is visible.
//
// The toast follows an area's own x, y and size, not its ancestors': an
// area inside a Flickable or ScrollView that scrolls is not followed as
// it scrolls. No area sits in one today; one that does needs the toast
// to follow the view's contentY as well.
QtObject {
    id: areas

    property var items: []

    function add(item) {
        if (item && areas.items.indexOf(item) < 0) {
            areas.items = areas.items.concat([item]);
        }
    }

    function remove(item) {
        const at = areas.items.indexOf(item);
        if (at >= 0) {
            const rest = areas.items.slice();
            rest.splice(at, 1);
            areas.items = rest;
        }
    }
}
