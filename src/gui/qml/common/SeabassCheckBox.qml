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

    // Some, not all: a playlist's box when only part of it is ticked. Drawn
    // as the ticked box with a bar for its mark.
    readonly property bool partial: control.checkState === Qt.PartiallyChecked

    // Between the box and its words, like RadioButton's.
    spacing: Theme.tightSpacing

    // The words, drawn here too. KDE's style sets them off by its own
    // indicator's pixel metric plus the spacing, and for any other
    // indicator by the indicator's width alone: no gap, so every label sat
    // hard against the box drawn above.
    contentItem: Label {
        objectName: "seabassCheckLabel"
        // The box's room, and the gap only with words to keep apart: some
        // styles size the control by this item alone, so a box without
        // words still needs its own width here.
        readonly property real indicatorRoom: control.indicator
            ? control.indicator.width + (control.text.length > 0 ? control.spacing : 0) : 0
        leftPadding: control.mirrored ? 0 : indicatorRoom
        rightPadding: control.mirrored ? indicatorRoom : 0
        visible: control.text.length > 0
        text: control.text
        font: control.font
        opacity: control.enabled ? 1 : 0.6
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }

    indicator: Rectangle {
        objectName: "seabassCheckIndicator"
        readonly property real side: Theme.scaled(18)
        implicitWidth: side
        implicitHeight: side
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.scaled(3)
        color: control.checked || control.partial ? Theme.accent : "transparent"
        border.width: Math.max(1, Math.round(Theme.scaled(1.5)))
        border.color: control.checked || control.partial || control.visualFocus || (control.hovered && control.enabled)
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
        Rectangle {
            objectName: "seabassPartialMark"
            anchors.centerIn: parent
            visible: control.partial
            width: Math.round(parent.side * 0.55)
            height: Math.max(2, Math.round(Theme.scaled(2)))
            radius: height / 2
            color: Theme.accentInk
        }
    }
}
