// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A row of 5 stars for showing (and, when editable, changing) a track's
// rating: hovering previews a value -- stars up to the cursor turn
// warnIcon gold -- and clicking locks that value in, emitting
// ratingPicked. value <= 0 renders as all-empty stars ("unrated").
// Read-only (no hover/click reaction) when editable is false.
RowLayout {
    id: root
    property int value: 0
    property bool editable: true
    signal ratingPicked(int rating)

    property int hoveredStar: 0
    // So a caller can attach its own ToolTip the same way it would to any
    // other control (ToolTip.visible: hovered), without needing a
    // HoverHandler of its own.
    readonly property bool hovered: root.hoveredStar > 0
    spacing: 1

    Repeater {
        model: 5
        delegate: Label {
            required property int index
            readonly property int starNumber: index + 1
            readonly property bool previewing: root.hoveredStar > 0
            readonly property bool filled: root.previewing
                ? starNumber <= root.hoveredStar : starNumber <= root.value

            text: filled ? "★" : "☆"
            font.pointSize: Theme.fontSmall
            color: previewing && filled ? Theme.warnIcon : (filled ? Theme.text : Theme.textMuted)

            MouseArea {
                anchors.fill: parent
                enabled: root.editable
                hoverEnabled: true
                cursorShape: root.editable ? Qt.PointingHandCursor : Qt.ArrowCursor
                onEntered: root.hoveredStar = starNumber
                onExited: root.hoveredStar = 0
                onClicked: root.ratingPicked(starNumber)
            }
        }
    }
}
