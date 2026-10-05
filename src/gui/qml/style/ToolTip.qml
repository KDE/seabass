// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Templates as T
import SeabassGui

// Below its item, not over it. A tooltip centred on what it describes
// hides the thing you are pointing at, which on a dense page -- the
// Camelot wheel above all -- covers the very segment being asked about.
// Below and clear of it by a few pixels keeps both readable.
//
// Opaque, and said explicitly: a Popup's background is whatever it is
// given, and the one here was see-through, so the text landed on top of
// the page behind it.
T.ToolTip {
    id: control

    // A template computes no size of its own. Without these the popup was
    // 0 x 0: its background, sized to the popup, vanished, while the text
    // still drew at its full natural width over whatever lay beneath and
    // out past the window's edge. Both reports -- no background, and
    // tooltips running off the window -- were this.
    //
    // Capped, so a long tip wraps into a readable block rather than one
    // line as wide as the text; the popup's margins then keep the block
    // inside the window.
    readonly property real maximumWidth: Theme.scaled(360)
    implicitWidth: Math.min(maximumWidth,
                            Math.max(implicitBackgroundWidth + leftInset + rightInset,
                                     contentItem.implicitWidth + leftPadding + rightPadding))
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentItem.implicitHeight + topPadding + bottomPadding)

    x: parent ? Math.round((parent.width - implicitWidth) / 2) : 0
    y: parent ? parent.height + 6 : 0

    margins: 6
    padding: 6
    leftPadding: 8
    rightPadding: 8

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    enter: Transition { NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; easing.type: Easing.OutQuad; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; easing.type: Easing.InQuad; duration: 120 } }

    contentItem: Text {
        width: control.availableWidth
        text: control.text
        font: control.font
        color: Theme.text
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignLeft
    }

    background: Rectangle {
        radius: Theme.cornerRadius
        // Not Theme.surface: a tooltip sits over cards that are already
        // that colour, and needs to read as a layer above them.
        color: Theme.rowPressed
        border.color: Theme.border
        border.width: 1
    }
}
