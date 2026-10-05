// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// One section of a page: a heading, and its content below it on the
// page's own left line. No box. Cards are for a set of peers -- the
// health checks, the backups, the home screen's actions -- not for
// wrapping a page's sections one by one.
//
// Sections used to be whatever was at hand: a GroupBox on one page, a
// Frame on the next, a bare heading on a third. GroupBox and Frame are
// drawn by the platform's style -- a bordered see-through box with the
// title above it on Linux, a filled card on macOS, something else under
// Fluent on Windows -- and each indented its content by the style's own
// padding, so no two pages shared a left edge. This is still a GroupBox
// underneath, so content goes in exactly as it did, but the heading and
// the (absent) background are drawn here.
GroupBox {
    id: control

    // Every side named: styles set leftPadding and the like on top of
    // `padding`, and those would win over it.
    padding: 0
    leftPadding: 0
    rightPadding: 0
    bottomPadding: 0
    topPadding: control.title.length > 0 ? sectionHeading.implicitHeight + Theme.rowSpacing : 0

    label: Subtitle {
        id: sectionHeading
        x: 0
        y: 0
        width: control.width
        visible: control.title.length > 0
        text: control.title
        color: Theme.text
        wrapMode: Text.WordWrap
    }

    background: Item {}
}
