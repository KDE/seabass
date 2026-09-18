// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import SeabassGui

// The playing track's ring, alone on the whole screen: something to have
// up while a track plays. Opened by a click on the ring in the track
// details pane; a click anywhere, or Escape, goes back.
//
// A window of its own, fullscreen, over the screen the app is on. The
// app's own window is left exactly as it is -- its size, its place and
// whether it is maximized are the user's, and taking it fullscreen and
// back is a good way to lose them.
//
// It shows what the PLAYER has loaded, not what the pane was showing, so
// it follows the player to the next track, and it closes by itself when
// the player has nothing loaded any more.
Window {
    id: root
    objectName: "ringFullscreenWindow"
    required property var playbackController
    // The app's window, for the screen to open on.
    property var hostWindow: null

    readonly property bool hasTrack: root.playbackController.hasTrack === true
    onHasTrackChanged: {
        if (!root.hasTrack) {
            root.close();
        }
    }

    function open() {
        // Only a Window made in QML has a `screen` to read; without one
        // this opens on the primary screen, which is no reason not to open.
        if (root.hostWindow && root.hostWindow.screen) {
            root.screen = root.hostWindow.screen;
        }
        root.showFullScreen();
        root.requestActivate();
    }

    visible: false
    flags: Qt.Window | Qt.FramelessWindowHint
    // The screen's size of its own accord, too: making a window
    // fullscreen is a request to the window manager, and where there is
    // none to grant it the window would stay the size it was born.
    width: Screen.width
    height: Screen.height
    title: root.playbackController.title || "Seabass"
    color: Theme.background

    Shortcut {
        sequence: "Esc"
        onActivated: root.close()
    }

    Item {
        anchors.fill: parent
        MouseArea {
            anchors.fill: parent
            onClicked: root.close()
        }

        TrackRing {
            id: ring
            objectName: "fullscreenRing"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Theme.pageMargin * 2
            width: Math.min(parent.width - Theme.pageMargin * 4, parent.height - captions.height - Theme.pageMargin * 5)
            height: width
            // One bar to a waveform column: at this size there is room.
            bars: Math.max(200, Math.min(400, root.playbackController.waveform ? root.playbackController.waveform.length : 200))
            waveformData: root.playbackController.waveform || []
            cueData: root.playbackController.cues || []
            trackDurationMs: root.playbackController.duration || 0
            artworkSource: root.playbackController.artworkPath || ""
            progress: root.playbackController.duration > 0
                ? root.playbackController.position / root.playbackController.duration : 0
            playing: root.playbackController.playing === true
            liveLevels: root.playbackController.liveLevels === true
            liveLow: root.playbackController.levelLow || 0
            liveMid: root.playbackController.levelMid || 0
            liveHigh: root.playbackController.levelHigh || 0
            beatCount: root.playbackController.beatCount || 0
            onClicked: root.close()
        }

        Column {
            id: captions
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Theme.pageMargin * 2
            width: parent.width - Theme.pageMargin * 4
            spacing: Theme.pageMargin / 4
            Label {
                objectName: "fullscreenTitle"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: root.playbackController.title || ""
                color: Theme.text
                font.pointSize: Theme.fontXLarge
            }
            Label {
                objectName: "fullscreenArtist"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: root.playbackController.artist || ""
                color: Theme.textMuted
                font.pointSize: Theme.fontLarge
            }
        }
    }
}
