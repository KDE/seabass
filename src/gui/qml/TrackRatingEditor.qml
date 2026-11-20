// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A track's rating as five stars that can be clicked (#105): star N asks
// for N stars, and the star that is the rating already asks for none.
// Hovering previews the stars up to the pointer. Each star is a button
// of its own, reached with Tab and pressed with Space, so the row works
// without a mouse and a screen reader names each one.
RowLayout {
    id: root
    // 1 to 5, or -1 for none.
    property int value: -1
    property bool editable: true
    // The value is staged and not on the stick yet.
    property bool pending: false
    signal ratingPicked(int stars)

    property int hoveredStar: 0
    spacing: 0

    Repeater {
        model: 5
        delegate: AbstractButton {
            id: star
            required property int index
            readonly property int starNumber: index + 1
            readonly property bool previewing: root.hoveredStar > 0
            readonly property bool filled: previewing ? starNumber <= root.hoveredStar : starNumber <= root.value
            objectName: "ratingStar" + starNumber
            enabled: root.editable
            focusPolicy: Qt.TabFocus
            hoverEnabled: true
            padding: 1
            Accessible.name: root.value === starNumber ? starNumber + (starNumber === 1 ? " star, the rating now; press to clear it" : " stars, the rating now; press to clear it")
                                                       : "Rate " + starNumber + (starNumber === 1 ? " star" : " stars")
            onHoveredChanged: root.hoveredStar = hovered ? starNumber : (root.hoveredStar === starNumber ? 0 : root.hoveredStar)
            onClicked: root.ratingPicked(root.value === starNumber ? -1 : starNumber)
            contentItem: SeabassIcon {
                iconName: star.filled ? "rating" : "rating-unrated"
                size: Theme.scaled(16)
                color: star.previewing && star.filled ? Theme.warnIcon
                    : star.filled ? (root.pending ? Theme.warnText : Theme.warnIcon) : Theme.textMuted
            }
            background: Rectangle {
                color: "transparent"
                radius: 2
                border.width: star.visualFocus ? 1 : 0
                border.color: Theme.accent
            }
        }
    }
}
