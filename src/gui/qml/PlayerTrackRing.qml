// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// A TrackRing of whatever the player has loaded: the one place that says
// which of the player's properties feed which of the ring's. The details
// pane, the fullscreen window and the watermark each show this, and each
// had its own copy of these fifteen lines.
TrackRing {
    id: root
    required property var playbackController

    waveformData: root.playbackController.waveform || []
    cueData: root.playbackController.cues || []
    trackDurationMs: root.playbackController.duration || 0
    artworkSource: root.playbackController.artworkPath || ""
    progress: root.playbackController.duration > 0
        ? root.playbackController.position / root.playbackController.duration : 0
    playing: root.playbackController.playing === true
    positionMs: root.playbackController.position || 0
    beatTimesMs: root.playbackController.beatTimesMs || []
    beatNumbers: root.playbackController.beatNumbers || []
    liveLevels: root.playbackController.liveLevels === true
    liveLow: root.playbackController.levelLow || 0
    liveMid: root.playbackController.levelMid || 0
    liveHigh: root.playbackController.levelHigh || 0
    beatCount: root.playbackController.beatCount || 0
}
