// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl as Impl
import SeabassGui

T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             Theme.compactControlHeight)
    padding: 8
    leftPadding: 12
    rightPadding: 12
    spacing: 6

    icon.width: Theme.iconSizeSmall * 0.6
    icon.height: Theme.iconSizeSmall * 0.6
    icon.color: control.enabled ? Theme.text : Theme.textMuted

    contentItem: Impl.IconLabel {
        readonly property real arrowPadding: control.subMenu && control.arrow ? control.arrow.width + control.spacing : 0
        leftPadding: !control.mirrored ? 0 : arrowPadding
        rightPadding: control.mirrored ? 0 : arrowPadding
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: Qt.AlignLeft | Qt.AlignVCenter
        icon: control.icon
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textMuted
    }

    background: Rectangle {
        color: control.highlighted || control.hovered ? Theme.rowHover : "transparent"
    }
}
