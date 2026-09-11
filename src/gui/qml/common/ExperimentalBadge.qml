// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The small "EXPERIMENTAL" pill next to a page title -- see
// docs/experimental-features.md for what earns one and how a feature
// loses it again.
Rectangle {
    radius: 3
    color: Theme.warnBg
    border.color: Theme.warnBorder
    implicitWidth: experimentalLabel.implicitWidth + 8
    implicitHeight: experimentalLabel.implicitHeight + 4
    Label {
        id: experimentalLabel
        anchors.centerIn: parent
        text: "EXPERIMENTAL"
        font.pointSize: Theme.fontTiny
        font.bold: true
        color: Theme.warnText
    }
}
