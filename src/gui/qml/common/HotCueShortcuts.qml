// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// Keys 1 to 8, on the number row and the keypad, jump to hot cue 1 to 8 of
// the track that is playing, the way the pads on a player do. The track
// keeps playing, or stays paused.
//
// Main.qml holds one of these for the whole window, next to the Space
// shortcut and with the same rule: while a text field has focus, a digit
// is typing, and these stay out of its way. The fullscreen ring has its
// own key handler and does the same there.
// An Item rather than an Instantiator: a Shortcut finds its window through
// the Item it sits in, and an Instantiator's objects sit in none.
Item {
    id: root

    required property var playbackController
    // False while typing into a text field: see Main.qml.
    property bool active: true

    visible: false

    Repeater {
        model: 8
        delegate: Item {
            required property int index
            Shortcut {
                readonly property int number: index + 1
                // The keypad's digits match these too: Qt tries a keypad key
                // without its keypad modifier when nothing claims it as is.
                sequence: String(number)
                enabled: root.active && root.playbackController !== null && root.playbackController.hasTrack
                onActivated: root.playbackController.jumpToHotCue(number)
            }
        }
    }
}
