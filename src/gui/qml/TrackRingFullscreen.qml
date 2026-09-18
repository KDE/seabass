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
// It shows what the PLAYER has loaded, not what the pane was showing, so
// it follows the player to the next track, and it closes by itself when
// the player has nothing loaded any more.
Popup {
    id: root
    required property var playbackController

    readonly property bool hasTrack: root.playbackController.hasTrack === true
    onHasTrackChanged: {
        if (!root.hasTrack) {
            root.close();
        }
    }

    // What the window was before -- windowed or maximized -- to go back to.
    property int visibilityBefore: Window.Windowed
    readonly property var hostWindow: root.parent ? root.parent.Window.window : null

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    modal: true
    closePolicy: Popup.CloseOnEscape
    background: Rectangle { color: Theme.background }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.shortTransitionDuration } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.shortTransitionDuration } }

    onAboutToShow: {
        if (root.hostWindow) {
            root.visibilityBefore = root.hostWindow.visibility;
            root.hostWindow.visibility = Window.FullScreen;
        }
    }
    onAboutToHide: {
        if (root.hostWindow && root.hostWindow.visibility === Window.FullScreen) {
            root.hostWindow.visibility = root.visibilityBefore;
        }
    }

    contentItem: Item {
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
