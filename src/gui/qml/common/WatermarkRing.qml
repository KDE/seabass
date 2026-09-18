// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// The background watermark while a track plays: the track's ring, faint,
// in the corner where the cover art watermark sits -- the same square,
// bleeding off the corner the same way (see WatermarkLayer) -- and moving
// to the music like any other TrackRing. It shows what the player has
// loaded, on whatever page the app is on.
//
// `shows` is false where the ring cannot be drawn (TrackRing.available),
// and Main.qml then keeps the blurred cover art it always had.
//
// Like the rest of the watermark it sits over the content and takes no
// input: the ring's own click-to-fullscreen is switched off here, or a
// faint picture in the corner would swallow every click on the page
// under it.
Item {
    id: root
    required property var playbackController

    readonly property bool shows: root.playbackController.hasTrack === true && ring.available
    // A little stronger than the blurred art's 0.18: that is a soft wash
    // of colour, this is thin bars with the page showing between them.
    readonly property real shownOpacity: 0.24
    readonly property real bleed: 0.08

    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.rightMargin: -width * bleed
    anchors.bottomMargin: -height * bleed
    width: Math.min(parent.width, parent.height) * 0.75
    height: width
    opacity: root.shows ? root.shownOpacity : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 400; easing.type: Easing.InOutQuad } }

    TrackRing {
        id: ring
        objectName: "watermarkTrackRing"
        anchors.fill: parent
        interactive: false
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
        positionMs: root.playbackController.position || 0
        beatTimesMs: root.playbackController.beatTimesMs || []
        beatNumbers: root.playbackController.beatNumbers || []
    }
}
