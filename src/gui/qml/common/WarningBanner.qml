// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A warning that needs reading before going on: the amber banner the
// write warning already uses, with its icon, the text, and whatever
// buttons settle it after the text (children go at the row's end).
// Collapses to nothing while hidden, so a page can leave it in its
// layout unconditionally.
Rectangle {
    id: root
    property alias text: label.text
    default property alias trailing: contentRow.data
    Layout.fillWidth: true
    implicitHeight: visible ? contentRow.implicitHeight + 2 * Theme.tightSpacing : 0
    color: Theme.warnBg
    border.color: Theme.warnBorder
    radius: Theme.cornerRadius

    RowLayout {
        id: contentRow
        anchors.fill: parent
        anchors.margins: Theme.tightSpacing
        anchors.leftMargin: Theme.rowSpacing
        spacing: Theme.rowSpacing

        SeabassIcon {
            iconName: "dialog-warning"
            size: Theme.iconSizeSmall * 0.75
            color: Theme.warnIcon
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            id: label
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.warnText
        }
    }
}
