// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// One playlist row: name (elided) + track count, right-aligned in a
// slightly darker tone than the name -- same alternating-shading/hover/
// press/accent-border-when-current idiom trackListView's own delegate
// uses. Shared by PlaylistListView.qml (the left Pane / off-canvas
// Drawer) and the "This Playlist" ComboBox's own popup delegate in
// MatchingPage.qml, so both render identically instead of
// drifting apart.
Rectangle {
    id: root
    required property int index
    required property string name
    property int count: 0
    property bool isCurrent: false
    signal picked()

    width: ListView.view ? ListView.view.width : implicitWidth
    height: 32

    color: mouseArea.pressed ? Theme.rowPressed
        : mouseArea.containsMouse ? Theme.rowHover
        : (root.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
    border.color: root.isCurrent ? Theme.accent : "transparent"
    border.width: root.isCurrent ? 2 : 0
    radius: root.isCurrent ? 4 : 0

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.picked()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 6
        Label {
            text: root.name
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Label {
            text: root.count
            color: Qt.darker(Theme.textMuted, 1.3)
            font.pointSize: Theme.fontSmall
            horizontalAlignment: Text.AlignRight
        }
    }
}
