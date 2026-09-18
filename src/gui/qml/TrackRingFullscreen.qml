// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import SeabassGui

// The playing track's ring, alone on the whole screen: something to have
// up while a track plays. Opened by a click on the ring in the track
// details pane; a click anywhere, or Escape, goes back. It has the
// player's controls, and the keys to go with them.
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

    // The keys, while this window is up. Left and right jump four beats
    // -- a bar -- of the track's own grid and land on the same place in
    // the beat, so a jump stays in time; up and down are the
    // previous and next track of the list the track was played from.
    readonly property int skipBeats: 4
    // Enter sets the cover spinning like a record, and stops it again.
    // Off until asked for.
    property bool spinning: false
    function keyPressed(key, isAutoRepeat) {
        switch (key) {
        case Qt.Key_Escape: root.close(); return true;
        case Qt.Key_Space: if (!isAutoRepeat) root.playbackController.togglePlay(); return true;
        case Qt.Key_Return:
        case Qt.Key_Enter: if (!isAutoRepeat) root.spinning = !root.spinning; return true;
        case Qt.Key_Left: root.playbackController.skipBeats(-root.skipBeats); return true;
        case Qt.Key_Right: root.playbackController.skipBeats(root.skipBeats); return true;
        // The keyboard's own media keys, where they arrive as keys.
        case Qt.Key_MediaPlay:
        case Qt.Key_MediaTogglePlayPause: if (!isAutoRepeat) root.playbackController.togglePlay(); return true;
        case Qt.Key_MediaPause: root.playbackController.pause(); return true;
        case Qt.Key_MediaStop: root.playbackController.stop(); return true;
        case Qt.Key_MediaPrevious: if (!isAutoRepeat) root.playbackController.previous(); return true;
        case Qt.Key_MediaNext: if (!isAutoRepeat) root.playbackController.next(); return true;
        // Held down, these would run through the whole list.
        case Qt.Key_Up: if (!isAutoRepeat) root.playbackController.previous(); return true;
        case Qt.Key_Down: if (!isAutoRepeat) root.playbackController.next(); return true;
        }
        return false;
    }

    // The controls show when the pointer moves and go again when it has
    // been still a while: this is a thing to look at, not to operate.
    property bool controlsShown: false
    function showControls() {
        root.controlsShown = true;
        hideControls.restart();
    }
    Timer {
        id: hideControls
        interval: 2500
        onTriggered: root.controlsShown = transport.hovered
    }
    onVisibleChanged: {
        if (root.visible) {
            root.showControls();
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) { event.accepted = root.keyPressed(event.key, event.isAutoRepeat); }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: root.showControls()
            onClicked: root.close()
        }

        PlayerTrackRing {
            id: ring
            objectName: "fullscreenRing"
            playbackController: root.playbackController
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Theme.pageMargin * 2
            width: Math.min(parent.width - Theme.pageMargin * 4,
                            parent.height - captions.height - Theme.iconSizeLarge - Theme.pageMargin * 6)
            height: width
            // One bar to a waveform column: at this size there is room.
            bars: Math.max(200, Math.min(400, waveformData.length))
            spinning: root.spinning
            // This window exists from the moment the pane does, shut. An
            // item in a window that is not showing still calls itself
            // visible, so the ring would run its clock sixty times a
            // second in a window nobody can see, for as long as anything
            // played.
            animated: root.visible
            onClicked: root.close()
        }

        // Previous, back, play, forward, next. Over everything else, so a
        // click on a button is not a click on the ring under it.
        Row {
            id: transport
            objectName: "fullscreenTransport"
            z: 1
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: captions.top
            anchors.bottomMargin: Theme.pageMargin
            spacing: Theme.pageMargin / 2
            opacity: root.controlsShown ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.shortTransitionDuration } }
            readonly property bool hovered: previousButton.hovered || backButton.hovered || playButton.hovered
                || forwardButton.hovered || nextButton.hovered

            component TransportButton: RoundButton {
                required property string iconName
                required property string what
                display: AbstractButton.IconOnly
                text: what
                icon.source: Theme.iconUrl(iconName)
                icon.color: enabled ? Theme.text : Theme.textMuted
                icon.width: Theme.iconSizeLarge / 2
                icon.height: Theme.iconSizeLarge / 2
                implicitWidth: Theme.iconSizeLarge
                implicitHeight: Theme.iconSizeLarge
                focusPolicy: Qt.NoFocus
                ToolTip.visible: hovered
                ToolTip.text: what
                onHoveredChanged: root.showControls()
            }
            TransportButton {
                id: previousButton
                objectName: "fullscreenPrevious"
                iconName: "media-skip-backward"
                what: "Previous track (Up)"
                enabled: root.playbackController.hasPrevious === true
                onClicked: root.playbackController.previous()
            }
            TransportButton {
                id: backButton
                objectName: "fullscreenBack"
                iconName: "media-seek-backward"
                what: "Back " + root.skipBeats + " beats (Left)"
                onClicked: root.playbackController.skipBeats(-root.skipBeats)
            }
            TransportButton {
                id: playButton
                objectName: "fullscreenPlay"
                iconName: root.playbackController.playing === true ? "media-playback-pause" : "media-playback-start"
                what: root.playbackController.playing === true ? "Pause (Space)" : "Play (Space)"
                onClicked: root.playbackController.togglePlay()
            }
            TransportButton {
                id: forwardButton
                objectName: "fullscreenForward"
                iconName: "media-seek-forward"
                what: "Forward " + root.skipBeats + " beats (Right)"
                onClicked: root.playbackController.skipBeats(root.skipBeats)
            }
            TransportButton {
                id: nextButton
                objectName: "fullscreenNext"
                iconName: "media-skip-forward"
                what: "Next track (Down)"
                enabled: root.playbackController.hasNext === true
                onClicked: root.playbackController.next()
            }
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
