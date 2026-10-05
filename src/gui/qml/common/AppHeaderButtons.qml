// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The three buttons at the top right of the home: About, Preferences and
// Support. Also on those three pages themselves, where they are not in
// the way and let one move between them without going home first
// (Sebastian, 2026-09-28); a stick's tool pages keep their header for
// their own controls. The button for the page one is on is off, which
// says where one is.
RowLayout {
    id: root
    // "about", "preferences" or "support": the page this row sits on, or
    // empty on the home, where every button leads somewhere.
    property string current: ""
    property int iconSize: Math.round(Theme.scaled(22))
    spacing: 0

    signal aboutRequested()
    signal preferencesRequested()
    signal supportRequested()

    // The cascade on the way back home (see AppHeaderOverlay): each
    // button a beat after the one before, the first `firstDelay` in.
    property int firstDelay: 0
    readonly property bool entranceRunning: aboutSlot.running || preferencesSlot.running || supportSlot.running
    function playEntrance() {
        aboutSlot.play();
        preferencesSlot.play();
        supportSlot.play();
    }

    EntranceSlot {
        id: aboutSlot
        delay: root.firstDelay
        IconToolButton {
            objectName: "aboutButton"
            iconName: "help-about"
            iconSize: root.iconSize
            text: "About Seabass"
            enabled: root.current !== "about"
            ToolTip.visible: hovered
            ToolTip.text: "About Seabass"
            onClicked: root.aboutRequested()
        }
    }
    EntranceSlot {
        id: preferencesSlot
        delay: root.firstDelay + 90
        IconToolButton {
            objectName: "preferencesButton"
            iconName: "configure"
            iconSize: root.iconSize
            text: "Preferences"
            enabled: root.current !== "preferences"
            ToolTip.visible: hovered
            ToolTip.text: "Preferences"
            onClicked: root.preferencesRequested()
        }
    }
    EntranceSlot {
        id: supportSlot
        delay: root.firstDelay + 180
        burstColor: "#aa0000"
        ToolButton {
            id: donateButton
            objectName: "donateButton"
            contentItem: HeartIcon {
                objectName: "donateHeart"
                iconSize: root.iconSize
                color: "#aa0000"
                opacity: donateButton.enabled ? 1.0 : 0.5
            }
            display: AbstractButton.IconOnly
            text: "Support Seabass"
            enabled: root.current !== "support"
            ToolTip.visible: hovered
            ToolTip.text: "Support Seabass"
            onClicked: root.supportRequested()

            Heartbeat {
                target: donateButton
                period: 6230
            }
        }
    }
}
