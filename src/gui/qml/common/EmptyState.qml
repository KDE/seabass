// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// What an empty list says instead of rows: "nothing to sync", "no
// backups yet", "no track matches that search". A card centred in the
// list's space, tinted by its tone, icon beside the words -- it should
// read as the page's answer, not a faint line lost in the middle of a
// blank area, which is what each page had on its own.
//
// Not a popup: nothing to dismiss, no shadow, and it sits in the list's
// place. It places itself in its parent (the list's area): centred
// across, and down at the golden section -- the space above it to the
// space below as 1 : 1.618 -- which reads as the middle of the area
// where the true middle reads as low. It caps its own width so a wide
// page does not stretch it into a strip.
Rectangle {
    id: root

    property alias text: headline.text
    property alias detail: detailLabel.text
    property string iconName: "help-about"
    // "info" for an answer that is just how things are, "good" for one
    // that is good news (synced, nothing wrong).
    property string tone: "info"
    // A way out of the empty list, when there is one ("Clear All
    // Filters"); no button without it.
    property string actionText: ""
    signal actionTriggered()

    readonly property color toneColor: root.tone === "good" ? Theme.good : Theme.accent

    width: Math.min(Theme.scaled(480), parent ? parent.width - 2 * Theme.pageMargin : Theme.scaled(480))
    x: parent ? Math.round((parent.width - root.width) / 2) : 0
    y: parent ? Math.round(Math.max(0, parent.height - root.height) * (1 - 1 / 1.618)) : 0
    implicitHeight: content.implicitHeight + 2 * Theme.cardPadding
    height: implicitHeight
    radius: Theme.popupRadius
    color: Qt.tint(Theme.surface, Qt.rgba(root.toneColor.r, root.toneColor.g, root.toneColor.b, 0.08))
    border.width: 1
    border.color: Qt.rgba(root.toneColor.r, root.toneColor.g, root.toneColor.b, 0.45)

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: Theme.cardPadding
        spacing: Theme.cardPadding

        SeabassIcon {
            Layout.alignment: Qt.AlignVCenter
            iconName: root.iconName
            size: Theme.iconSizeSmall
            color: root.toneColor
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.tightSpacing
            Label {
                id: headline
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pointSize: Theme.fontMedium
            }
            Label {
                id: detailLabel
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                color: Theme.textMuted
            }
            Button {
                objectName: "emptyStateAction"
                visible: root.actionText.length > 0
                text: root.actionText
                onClicked: root.actionTriggered()
            }
        }
    }
}
