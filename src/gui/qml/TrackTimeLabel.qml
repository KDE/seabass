// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The loaded track's time, "1:01 / 4:00", or the time left, "-2:59 /
// 4:00": a click, or Space or Return with the focus on it, switches. The
// player bar and the zoomed waveform popup show one each, and both follow
// the one setting (AppSettingsController.showRemainingTime) so they always
// agree; without a settings controller (a test, a preview) it switches
// on its own.
AbstractButton {
    id: root

    property real positionMs: 0
    property real durationMs: 0
    property var settings: null
    property bool localRemaining: false
    readonly property bool remaining: root.settings ? root.settings.showRemainingTime === true : root.localRemaining

    readonly property string shownText: (root.remaining
        ? "-" + Theme.trackTime(Math.max(0, root.durationMs - root.positionMs))
        : Theme.trackTime(root.positionMs)) + " / " + Theme.trackTime(root.durationMs)

    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    padding: 0
    text: root.shownText
    Accessible.name: root.remaining ? "Time left, " + root.shownText + ". Show the time played"
                                    : "Time played, " + root.shownText + ". Show the time left"
    ToolTip.visible: root.hovered
    ToolTip.text: root.remaining ? "Show the time played" : "Show the time left"

    contentItem: Label {
        objectName: "trackTimeText"
        text: root.shownText
        color: root.hovered || root.visualFocus ? Theme.text : Theme.textMuted
        horizontalAlignment: Text.AlignRight
    }
    background: Rectangle {
        color: "transparent"
        border.width: root.visualFocus ? 1 : 0
        border.color: Theme.accent
        radius: Theme.scaled(3)
    }

    onClicked: {
        if (root.settings) {
            root.settings.showRemainingTime = !root.remaining;
        } else {
            root.localRemaining = !root.localRemaining;
        }
    }
}
