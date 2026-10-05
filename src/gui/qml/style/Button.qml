// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl as Impl
import SeabassGui

T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 8
    leftPadding: 12
    rightPadding: 12
    spacing: 6

    icon.width: Theme.iconSizeSmall * 0.6
    icon.height: Theme.iconSizeSmall * 0.6
    icon.color: control.enabled ? Theme.text : Theme.textMuted

    contentItem: Impl.IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textMuted
    }

    // A flat button is a hover target, not a box: the app uses it for
    // icon-only actions in headers and rows, where a resting background
    // would draw a second rectangle over whatever it sits on.
    background: Rectangle {
        implicitHeight: Theme.compactControlHeight
        radius: Theme.cornerRadius
        visible: !control.flat || control.down || control.hovered
        color: control.down
               ? Theme.rowPressed
               : (control.hovered ? Theme.rowHover : (control.flat ? "transparent" : Theme.surface))
        border.color: control.flat ? "transparent" : Theme.border
        border.width: control.flat ? 0 : 1
    }
}
