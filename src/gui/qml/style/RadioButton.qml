// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import SeabassGui

// Sized and coloured to match common/SeabassCheckBox: the same
// Theme.scaled(18) box, the same accent fill and 1.5 px border, round
// instead of square. Left to the style it came out half again as big as
// the check box beside it, which is the kind of mismatch that reads as
// carelessness on a form where both appear.
T.RadioButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    spacing: 6

    indicator: Rectangle {
        readonly property real side: Theme.scaled(18)
        implicitWidth: side
        implicitHeight: side
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: side / 2
        color: "transparent"
        border.width: Math.max(1, Math.round(Theme.scaled(1.5)))
        border.color: control.checked || control.visualFocus || (control.hovered && control.enabled)
                      ? Theme.accent : Theme.textMuted
        opacity: control.enabled ? 1 : 0.5

        Rectangle {
            width: parent.side * 0.5
            height: width
            anchors.centerIn: parent
            radius: width / 2
            color: Theme.accent
            visible: control.checked
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textMuted
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }
}
