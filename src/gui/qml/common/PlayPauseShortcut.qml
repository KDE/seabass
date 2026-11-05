// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
import QtQuick

// Space plays or pauses the loaded track, wherever the user is in the
// app, a dialog included. A shortcut in the window context is refused
// by Qt Quick Controls while a modal popup is open unless it lives
// inside that popup, and every SeabassDialog is modal: so the key went
// dead over every question, with the music playing on. The application
// context is not refused. The price: a dialog gives keyboard focus to
// its selected button, and Space on a focused button used to press it.
// Return still does, and it is the key the dialog's own wiring
// documents (SeabassDialog.wireFooterKeys).
Item {
    id: root

    required property var playbackController
    // False while typing into a text field: see Main.qml.
    property bool active: true

    visible: false

    Shortcut {
        objectName: "playPauseShortcut"
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: root.active && root.playbackController !== null && root.playbackController.hasTrack
        onActivated: root.playbackController.togglePlay()
    }
}
