// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import QtQuick.Controls.impl as Impl
import SeabassGui

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 6
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

    // Nothing at rest: a tool button sits on a header or inside a row,
    // and only answers to the pointer.
    background: Rectangle {
        radius: Theme.cornerRadius
        visible: control.down || control.hovered || control.checked
        color: (control.down || control.checked) ? Theme.rowPressed : Theme.rowHover
    }
}
