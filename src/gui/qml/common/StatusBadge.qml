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
    // A badge that is also a way somewhere: the pointer says so, and a tap
    // emits clicked().
    property bool clickable: false
    // The label did not fit the width the badge was given and is elided.
    readonly property bool truncated: badgeLabel.truncated
    signal clicked()

    radius: 3
    border.color: badge.badgeColor
    color: Qt.rgba(badge.badgeColor.r, badge.badgeColor.g, badge.badgeColor.b, 0.15)
    // Off the label's own implicit width, not the Row's, so a badge set
    // narrower than its words (a row with no room for them) elides them
    // rather than running over whatever is beside it.
    implicitWidth: (badgeIcon.visible ? badgeIcon.width + badgeContent.spacing : 0) + badgeLabel.implicitWidth + 12
    implicitHeight: badgeContent.implicitHeight + 6

    Row {
        id: badgeContent
        anchors.centerIn: parent
        spacing: 4
        SeabassIcon {
            id: badgeIcon
            objectName: "badgeIcon"
            visible: badge.iconName.length > 0
            iconName: badge.iconName
            size: badgeLabel.implicitHeight
            color: badge.badgeColor
        }
        Label {
            id: badgeLabel
            text: badge.label
            width: Math.min(implicitWidth, Math.max(0, badge.width - 12
                - (badgeIcon.visible ? badgeIcon.width + badgeContent.spacing : 0)))
            elide: Text.ElideRight
            font.bold: true
            font.pointSize: Theme.fontTiny
            color: badge.badgeColor
        }
    }

    HoverHandler {
        id: hoverHandler
        cursorShape: badge.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler {
        enabled: badge.clickable
        onTapped: badge.clicked()
    }
    ToolTip.visible: hoverHandler.hovered && badge.tooltipText.length > 0
    ToolTip.text: badge.tooltipText
    ToolTip.delay: 300
}
