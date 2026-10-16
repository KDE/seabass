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

    // A tick on a checked entry, and room for one on every checkable
    // entry, so a checked and an unchecked playlist line up.
    indicator: SeabassIcon {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.checkable
        opacity: control.checked ? 1 : 0
        iconName: "checkmark"
        size: Theme.iconSizeSmall * 0.6
        color: control.icon.color
    }

    // The mark of an entry that opens a submenu.
    arrow: SeabassIcon {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.subMenu
        iconName: "arrow-right"
        rotation: control.mirrored ? 180 : 0
        size: Theme.iconSizeSmall * 0.6
        color: control.icon.color
    }

    contentItem: Impl.IconLabel {
        readonly property real arrowPadding: control.subMenu && control.arrow ? control.arrow.width + control.spacing : 0
        readonly property real indicatorPadding: control.checkable && control.indicator
            ? control.indicator.width + control.spacing : 0
        leftPadding: !control.mirrored ? indicatorPadding : arrowPadding
        rightPadding: control.mirrored ? indicatorPadding : arrowPadding
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
