// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import SeabassGui

// A track two check boxes wide and one tall, rather than the style's own,
// which came out comically large next to everything around it on Device
// Profile.
T.Switch {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    spacing: 6

    indicator: Rectangle {
        readonly property real trackHeight: Theme.scaled(18)
        implicitWidth: trackHeight * 1.9
        implicitHeight: trackHeight
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked ? Theme.accent : "transparent"
        border.width: Math.max(1, Math.round(Theme.scaled(1.5)))
        border.color: control.checked || control.visualFocus || (control.hovered && control.enabled)
                      ? Theme.accent : Theme.textMuted
        opacity: control.enabled ? 1 : 0.5

        Rectangle {
            readonly property real inset: Math.max(2, Math.round(Theme.scaled(3)))
            width: parent.height - inset * 2
            height: width
            radius: width / 2
            y: inset
            x: control.checked ? parent.width - width - inset : inset
            color: control.checked ? Theme.accentInk : Theme.textMuted
            Behavior on x { NumberAnimation { duration: 80 } }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textMuted
        verticalAlignment: Text.AlignVCenter
    }
}
