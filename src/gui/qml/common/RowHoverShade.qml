// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// The hover/press shading on a list row, faded in and out rather than
// snapped on: the pointer crossing a dense list used to flash a hard
// edge from row to row.
//
// It fades its own opacity, not the row's colour. A list reuses one
// delegate for many tracks and rewrites that colour as rows scroll, sort
// or refilter, so a Behavior there would animate those rewrites too and
// smear one track's shading into the next's.
//
// Declared before a row's content, so it shades the row rather than
// covering what is written on it. Both colours are opaque, the same ones
// the rows themselves use, so a fully faded-in shade looks exactly like
// the colour it replaced.
Rectangle {
    id: root
    property bool hovered: false
    property bool pressed: false

    anchors.fill: parent
    color: root.pressed ? Theme.rowPressed : Theme.rowHover
    opacity: root.hovered || root.pressed ? 1 : 0
    // Nothing to draw, and nothing to hit-test, once it is gone. It
    // counts as there from the moment it starts fading in, though: the
    // animated opacity is still 0 on that first frame.
    visible: root.opacity > 0 || root.hovered || root.pressed
    Behavior on opacity { NumberAnimation { duration: Theme.shortTransitionDuration } }
}
