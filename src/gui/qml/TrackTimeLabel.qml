// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The loaded track's time, "1:01 / 4:00", or the time left, "-2:59 /
// 4:00": a click, or Return or Enter with the focus on it, switches.
// Space stays the app's play key (PlayPauseShortcut, which takes it
// before any focused control). The
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

    // Whole seconds, as shown: the time left is the length shown less the
    // time played shown, so the two always add up. Without a length there
    // is no time left to show, only the time played.
    readonly property int shownLength: Math.floor(Math.max(0, root.durationMs) / 1000)
    readonly property int shownPlayed: Math.min(root.shownLength > 0 ? root.shownLength : Infinity,
                                                Math.floor(Math.max(0, root.positionMs) / 1000))
    readonly property bool showsLeft: root.remaining && root.durationMs > 0
    readonly property string shownText: (root.showsLeft
        ? "-" + Theme.trackTime((root.shownLength - root.shownPlayed) * 1000)
        : Theme.trackTime(root.shownPlayed * 1000)) + " / " + Theme.trackTime(root.durationMs)

    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    padding: 0
    text: root.shownText
    Accessible.name: root.showsLeft ? "Time left, " + root.shownText : "Time played, " + root.shownText
    Accessible.description: root.remaining ? "Press Return to show the time played"
                                           : "Press Return to show the time left"
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

    Keys.onReturnPressed: root.clicked()
    Keys.onEnterPressed: root.clicked()

    onClicked: {
        if (root.settings) {
            root.settings.showRemainingTime = !root.remaining;
        } else {
            root.localRemaining = !root.localRemaining;
        }
    }
}
