// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Every check box in Seabass, with an indicator Seabass draws itself.
//
// The platform's own was invisible on Linux. KDE's style paints an
// unticked box in the colour scheme's Button colours: filled with the
// button background and outlined a fifth of the way from there to the
// text. Kelp's button background is within a shade of the rows these
// boxes sit on, so the box was the row's own colour with an outline at
// 1.8:1, and on a real Plasma session, with its colour scheme loaded, not
// even that. A scheme that fixed it would have repainted every button.
//
// So the box is drawn from Theme: an outline in textMuted, which clears
// 3:1 on every row colour in both themes (tst_SeabassCheckBox measures
// it), and ticked, the accent with a dark mark on it. It looks the same
// under every style, which it never did before either.
CheckBox {
    id: control

    indicator: Rectangle {
        objectName: "seabassCheckIndicator"
        readonly property real side: Theme.scaled(18)
        implicitWidth: side
        implicitHeight: side
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.scaled(3)
        color: control.checked ? Theme.accent : "transparent"
        border.width: Math.max(1, Math.round(Theme.scaled(1.5)))
        border.color: control.checked || control.visualFocus || (control.hovered && control.enabled)
            ? Theme.accent : Theme.textMuted
        opacity: control.enabled ? 1 : 0.5

        SeabassIcon {
            objectName: "seabassCheckMark"
            anchors.centerIn: parent
            visible: control.checked
            iconName: "checkmark"
            size: parent.side * 0.8
            // Theme's ink for anything drawn on the accent: dark in either
            // theme, since the accent does not change with it, and a light
            // mark on it reads at barely 2:1.
            color: Theme.accentInk
        }
    }
}
