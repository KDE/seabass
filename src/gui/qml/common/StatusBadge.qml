// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A small colored pill for a row's status ("CONFLICT", "REPAIRABLE",
// "(fixable)", ...), with the full explanation moved into a hover
// tooltip instead of always-visible text -- keeps delegates scannable
// at a glance while the detail is still one hover away. Works on any
// Item (not just Controls) via HoverHandler, since ToolTip's attached
// properties are available everywhere but need a hover source.
Rectangle {
    id: badge

    required property string label
    required property color badgeColor
    property string tooltipText: ""
    // A bundled Breeze icon in front of the label ("dialog-warning" for a
    // caution), in the badge's colour, or none.
    property string iconName: ""

    radius: 3
    border.color: badge.badgeColor
    color: Qt.rgba(badge.badgeColor.r, badge.badgeColor.g, badge.badgeColor.b, 0.15)
    implicitWidth: badgeContent.implicitWidth + 12
    implicitHeight: badgeContent.implicitHeight + 6

    Row {
        id: badgeContent
        anchors.centerIn: parent
        spacing: 4
        SeabassIcon {
            objectName: "badgeIcon"
            visible: badge.iconName.length > 0
            iconName: badge.iconName
            size: badgeLabel.implicitHeight
            color: badge.badgeColor
        }
        Label {
            id: badgeLabel
            text: badge.label
            font.bold: true
            font.pointSize: Theme.fontTiny
            color: badge.badgeColor
        }
    }

    HoverHandler { id: hoverHandler }
    ToolTip.visible: hoverHandler.hovered && badge.tooltipText.length > 0
    ToolTip.text: badge.tooltipText
    ToolTip.delay: 300
}
