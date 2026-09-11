// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One track's investigation view -- format badge, title, an optional
// action button, waveform with cue markers, cue-fallback notice, and an
// optional hint -- shared by every page that lets someone compare one
// catalog's copy of a track against another's before deciding what to do
// with it: DuplicatesPage's per-copy conflict view and SyncPage's plan
// list and cross-source conflict cards. Pulled out after these three
// diverged into three near-identical hand-rolled Frames, one of which
// (Sync's new cross-source conflicts) was the only one showing which
// library an option even came from -- this makes the format badge a
// standard part of the shared shape, not an extra to remember. CleanupPage
// and LibraryConsistencyPage's own resolve UI are deliberately NOT built
// on this: neither shows a waveform or lets you compare cues at all --
// they're a different kind of decision (bitrate/size/an already-computed
// survivor), not this one.
Frame {
    id: root

    // trackToMap()-shaped (src/gui/*.cpp): side/format, sourceId, title,
    // artist, filePath, artworkPath, durationMs, cues. Deliberately no
    // waveform field -- that's fetched on demand below, not precomputed
    // by the controller, see the WaveformView binding's own comment for
    // why that matters. Also reads track.playlists when showPlaylists is
    // true (DuplicatesPage only -- Sync tracks don't carry playlist
    // membership).
    required property var track
    property string formatLabelText: ""
    // Optional detail shown on hover instead of crammed into
    // formatLabelText itself (e.g. SyncPage's per-track cue summary) --
    // empty just means the badge has no tooltip.
    property string formatLabelTooltip: ""
    property bool showPlaylists: false

    // Empty actionButtonText hides the button entirely (SyncPage's plan
    // list has no per-track action, just Play).
    property string actionButtonText: ""
    property string actionButtonTooltip: ""
    property bool actionButtonEnabled: true
    signal actionTriggered()

    property string hintText: ""

    // Play wiring, and the waveform fetch below: playbackController's
    // own signatures need the catalog path (not carried on the track
    // itself), so callers pass it separately rather than this component
    // guessing at pathForFormat().
    property var playbackController: null
    property string playbackPath: ""

    // Passed straight through to the inner WaveformView -- see its own
    // doc comment. -1 (default): every cue renders identically, today's
    // behavior for every existing call site.
    property real highlightCuePositionMs: -1

    // Optional status pill (e.g. "KEEPING"/"REMOVING" on CleanupPage,
    // which copy of a duplicate group survives) -- empty hides it.
    // Deliberately styled distinctly from the neutral format badge above
    // (colored background/border, not just muted text) so it actually
    // stands out rather than reading as another label. Every other
    // TrackWaveformCard call site (Sync's plan list, Sync's cross-source
    // conflicts, DuplicatesPage's conflict view) leaves this unset.
    property string statusBadgeText: ""
    property color statusBadgeBg: Theme.groupBackground
    property color statusBadgeBorder: Theme.borderSubtle
    property color statusBadgeTextColor: Theme.text

    Layout.fillWidth: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: root.track.title + " - " + root.track.artist
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Rectangle {
                visible: root.formatLabelText.length > 0
                radius: 3
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                implicitWidth: formatBadgeLabel.implicitWidth + 8
                implicitHeight: formatBadgeLabel.implicitHeight + 4
                Label {
                    id: formatBadgeLabel
                    anchors.centerIn: parent
                    text: root.formatLabelText
                    font.pointSize: Theme.fontSmall
                    color: Theme.textMuted
                }

                HoverHandler { id: formatBadgeHover }
                ToolTip.visible: formatBadgeHover.hovered && root.formatLabelTooltip.length > 0
                ToolTip.text: root.formatLabelTooltip
                ToolTip.delay: 300
            }
            Rectangle {
                visible: root.statusBadgeText.length > 0
                radius: 3
                color: root.statusBadgeBg
                border.color: root.statusBadgeBorder
                border.width: 1
                implicitWidth: statusBadgeLabel.implicitWidth + 10
                implicitHeight: statusBadgeLabel.implicitHeight + 6
                Label {
                    id: statusBadgeLabel
                    anchors.centerIn: parent
                    text: root.statusBadgeText
                    font.pointSize: Theme.fontSmall
                    font.bold: true
                    color: root.statusBadgeTextColor
                }
            }
            Button {
                text: root.actionButtonText
                visible: root.actionButtonText.length > 0
                enabled: root.actionButtonEnabled
                ToolTip.visible: root.actionButtonTooltip.length > 0 && hovered
                ToolTip.text: root.actionButtonTooltip
                onClicked: root.actionTriggered()
            }
            Button {
                text: "▶ Play"
                enabled: root.track.filePath.length > 0 && root.playbackController !== null
                ToolTip.visible: hovered
                ToolTip.text: "Play this copy of the track"
                onClicked: root.playbackController.load(root.track.side, root.playbackPath, root.track.sourceId,
                    root.track.filePath, root.track.title, root.track.artist, root.track.artworkPath, root.track.cues)
            }
        }
        Label {
            visible: root.showPlaylists
            text: (root.track.playlists && root.track.playlists.length > 0)
                ? "Playlists: " + root.track.playlists.join(", ")
                : "Playlists: (none)"
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        WaveformView {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            // Fetched on demand, not carried on `track` -- see this
            // component's own header comment. Only the rows a ListView
            // actually renders ever pay for this file read at all.
            waveformData: root.playbackController
                ? root.playbackController.waveformFor(root.track.side, root.playbackPath, root.track.sourceId)
                : []
            format: root.track.side
            cueData: root.track.cues
            trackDurationMs: root.track.durationMs
            highlightCuePositionMs: root.highlightCuePositionMs
        }
        CueFallbackNotice {
            cues: root.track.cues
            durationMs: root.track.durationMs
        }
        Label {
            visible: root.hintText.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: root.hintText
        }
    }
}
