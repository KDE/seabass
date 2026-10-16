// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// Scrolls a list while something is dragged over it and the pointer is
// held near its top or bottom edge: a playlist below the sidebar's last
// row, or a place below the track list's, is otherwise out of reach for
// the drag, which cannot let go to scroll.
//
// Steady: `speed` pixels a second at the zone's inner line, up to twice
// that at the very edge, stepped by a timer, so the list moves the same
// however the pointer jiggles. Stops when the pointer leaves the zone,
// the drag ends, or the list reaches its end.
QtObject {
    id: scroller

    // The list to scroll.
    property Flickable view: null
    // A drag is over the view, the pointer at pointerY in its coordinates.
    property bool active: false
    property real pointerY: 0
    // How deep the zone along each edge is, and how fast it scrolls.
    property real edge: 24
    property real speed: 300

    readonly property real minContentY: view ? view.originY - view.topMargin : 0
    readonly property real maxContentY: view
        ? Math.max(minContentY, view.originY + view.contentHeight + view.bottomMargin - view.height) : 0
    // -1 up, 1 down, 0 still: in a zone, and not at that end already.
    readonly property int direction: !active || !view ? 0
        : pointerY < edge && view.contentY > minContentY ? -1
        : pointerY > view.height - edge && view.contentY < maxContentY ? 1
        : 0

    function step() {
        if (scroller.direction === 0) {
            return;
        }
        const depth = scroller.direction < 0 ? scroller.edge - scroller.pointerY
                                             : scroller.pointerY - (scroller.view.height - scroller.edge);
        const rate = scroller.speed * (1 + Math.max(0, Math.min(1, depth / scroller.edge)));
        const next = scroller.view.contentY + scroller.direction * rate * ticker.interval / 1000;
        scroller.view.contentY = Math.max(scroller.minContentY, Math.min(scroller.maxContentY, next));
    }

    property Timer ticker: Timer {
        interval: 16
        repeat: true
        running: scroller.direction !== 0
        onTriggered: scroller.step()
    }
}
