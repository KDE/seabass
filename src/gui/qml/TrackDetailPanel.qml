// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The track details column on the Library page: a resizable, closable
// SplitView pane to the right of the track list (the same idiom as the
// Matching panel), opened by clicking a row. Holds what used to be the
// track info popup: title / artist, the waveform with its cue markers
// (click to add a cue, drag to add a loop), the add-cue form, and the
// playlists the track is in. A proper redesign of the track details is
// still to come; this is the popup's content given a home that does not
// block the list. showFor() is populated from a track delegate; the
// waveform is read on demand right there (never during the bulk scan
// that fills the list), same pattern playbackController.load() uses.
Pane {
    id: panel
    required property var addCueController
    required property var playbackController
    property string format: ""
    property string libraryPath: ""
    // Adding cues is switched off for now: one property rather than
    // deleting the UI, so turning it back on is a one-line change and the
    // code stays under the compiler and the tests meanwhile. Covers the
    // whole feature -- the waveform's click-to-add, the staging form, and
    // the hint that tells you to click.
    property bool addCueEnabled: false
    // Camelot or traditional, from AppSettingsController. Threaded
    // through by the host rather than read from Theme here, the same way
    // every other KeyBadge call site does it, so the panel stays
    // testable without a running AppSettingsController.
    property string keyNotation: "camelot"

    signal closeRequested()
    // The row to open instead, by sourceId. The panel does not know how
    // the list is sorted or filtered, so it names the track and lets
    // ScanPage find and select it.
    signal jumpToTrackRequested(string sourceId)
    // Supplied by ScanPage so the artist list can be built. Optional:
    // without it the section simply does not appear, which is what the
    // page-instantiation test hands it.
    property var scanController: null
    // The page's track list is refreshed after a cue is added, so it is
    // not showing stale cue counts.
    signal rescanRequested()

    padding: 0

    function formatDuration(seconds) {
        var total = Math.round(seconds);
        var m = Math.floor(total / 60);
        var s = total % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    property string trackSourceId: ""
    property string trackTitle: ""
    property string trackArtist: ""
    property var trackCues: []
    property double trackDurationMs: 0
    property var trackPlaylistNames: []
    property string trackStreamingSource: ""
    property string trackArtworkPath: ""
    property string trackFilePath: ""
    property int trackRating: -1      // -1 is "unrated", distinct from 0 stars
    property double trackBpm: 0
    property string trackKey: ""
    property int trackBitrate: 0      // 0 means this format records none
    property string trackComment: ""
    property string trackAlbum: ""
    property int trackPlayCount: 0
    // -1 means no pending Add-Cue form; set by clicking the
    // waveform below.
    property real pendingPositionMs: -1
    // -1 means the pending add is a plain cue; a real value
    // (from dragging out a range) means it's a loop, and
    // pendingPositionMs is the loop-in.
    property real pendingLoopEndMs: -1

    function showFor(delegate) {
        panel.trackSourceId = delegate.sourceId;
        panel.trackTitle = delegate.title;
        panel.trackArtist = delegate.artist;
        panel.trackCues = delegate.cues;
        panel.trackDurationMs = delegate.durationSeconds * 1000;
        panel.trackPlaylistNames = delegate.playlistNames;
        panel.trackStreamingSource = delegate.streamingSource;
        // Read with a fallback rather than straight off the delegate.
        // showFor() is handed whatever row the caller has, and a row from
        // a model without these roles would otherwise assign undefined
        // and throw. Every default here is also the value that hides the
        // field, so a caller that cannot supply one shows nothing rather
        // than a wrong number.
        function value(name, fallback) {
            return delegate[name] !== undefined && delegate[name] !== null ? delegate[name] : fallback;
        }
        panel.trackArtworkPath = value("artworkPath", "");
        panel.trackFilePath = value("filePath", "");
        panel.trackRating = value("rating", -1);
        panel.trackBpm = value("bpm", 0);
        panel.trackKey = value("key", "");
        panel.trackBitrate = value("bitrate", 0);
        panel.trackComment = value("comment", "");
        panel.trackAlbum = value("album", "");
        panel.trackPlayCount = value("playCount", 0);
        panel.pendingPositionMs = -1;
        panel.pendingLoopEndMs = -1;
        waveformView.waveformData = playbackController.waveformFor(
            panel.format, panel.libraryPath, delegate.sourceId);
        waveformView.format = panel.format;
        // Last, once every field above is set.
        panel.refreshArtistTracks();
    }

    // Other tracks credited to the same artist. Recomputed when the shown
    // track changes rather than bound to a function call, so it is not
    // re-run on every unrelated property change in the panel.
    property var artistTracks: []
    // Said out loud when a jump cannot land: the artist list searches the
    // whole library while the list behind it may be showing one playlist
    // or a search, so the track really can be absent from the view.
    property string jumpMissMessage: ""
    function reportJumpMiss() {
        panel.jumpMissMessage = "That track is not in the current view. Clear the playlist or search filter to reach it.";
    }

    function refreshArtistTracks() {
        if (panel.scanController === null || typeof panel.scanController.tracksByArtist !== "function"
                || panel.trackArtist.length === 0) {
            panel.artistTracks = [];
            return;
        }
        panel.artistTracks = panel.scanController.tracksByArtist(panel.trackArtist, panel.trackSourceId);
    }
    // One handler: QML allows a signal only one, and both of these have
    // to happen when the shown track changes.
    // Only the message here. The artist list is refreshed at the END of
    // showFor() instead: trackSourceId is assigned first, so a refresh
    // driven off its change ran while trackArtist still held the
    // PREVIOUS track's name, and the panel listed that artist's tracks
    // under this one's heading.
    onTrackSourceIdChanged: panel.jumpMissMessage = ""

    // Cues staged for this track and not on the stick yet (see
    // AddCueController: adding stages, the floating Save writes).
    readonly property var pendingCues: {
        var revision = panel.addCueController.pendingRevision;  // re-evaluate when it bumps
        if (typeof panel.addCueController.pendingCuesFor !== "function" || panel.trackSourceId.length === 0) {
            return [];
        }
        return panel.addCueController.pendingCuesFor(panel.trackSourceId);
    }

    // Once a cue is staged, the panel's own trackCues (a snapshot from
    // when the track was shown) is patched locally so the new marker
    // shows up on the waveform immediately; the page's track list is
    // refreshed only once a save has actually written something, so it
    // never shows cue counts the stick does not have yet.
    Connections {
        // A plain JS stand-in (tests) has no signals and is not a QObject.
        target: ("objectName" in panel.addCueController) ? panel.addCueController : null
        ignoreUnknownSignals: true
        function onCuesSaved() {
            panel.rescanRequested();
        }
        function onStatusMessageChanged() {
            if (addCueController.statusMessage.length === 0) {
                return;
            }
            var isLoopAdded = panel.pendingLoopEndMs >= 0;
            var isHot = isLoopAdded || cueKindCombo.currentIndex === 0;
            var cues = panel.trackCues.slice();
            if (isHot) {
                cues = cues.filter((c) => !(c.kind === "hot" && c.hotCueNumber === hotCueNumberSpin.value));
            }
            cues.push({
                kind: isHot ? "hot" : "memory",
                hotCueNumber: isHot ? hotCueNumberSpin.value : 0,
                positionMs: panel.pendingPositionMs,
                isLoop: isLoopAdded,
                loopEndMs: isLoopAdded ? panel.pendingLoopEndMs : 0,
                color: isLoopAdded ? "#3daee9" : (isHot ? "#ffcc00" : "#00a5e3"),
                comment: cueCommentField.text,
            });
            panel.trackCues = cues;
            panel.pendingPositionMs = -1;
            panel.pendingLoopEndMs = -1;
            cueCommentField.text = "";
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 8
            Label {
                text: "Track"
                font.family: Theme.titleFamily
                font.weight: Font.Bold
                font.pointSize: Theme.fontMedium
                Layout.fillWidth: true
            }
            ToolButton {
                objectName: "closeTrackPanelButton"
                text: "✕"
                ToolTip.visible: hovered
                ToolTip.text: "Close track details"
                onClicked: panel.closeRequested()
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.borderSubtle }

        PageScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 12

    ColumnLayout {
        width: parent.width
        spacing: 10


        // Title, artist and the facts in one column, with the artwork
        // beside all of it and pinned to the top. Previously the artwork
        // sat in a row with the facts alone, so its top edge lined up
        // with "Length" rather than with the track's own title.
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: 10

            ColumnLayout {
                spacing: 1
                Label {
                    text: panel.trackTitle
                    font.bold: true
                    font.pointSize: Theme.fontMedium
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                Label {
                    text: panel.trackArtist
                    color: Theme.textMuted
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
                // Under the artist rather than in the facts grid: an
                // album is part of how a record is named, not a
                // measurement of it.
                Label {
                    visible: panel.trackAlbum.length > 0
                    text: panel.trackAlbum
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }

                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 4

                    // Labels right-aligned against their values, both a size
                    // down from body text: this is reference data you glance
                    // at, not prose, and at body size six rows of it shouted
                    // over the track's own title.
                    component FactLabel: Label {
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        horizontalAlignment: Text.AlignRight
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    }
                    // Numbers in the tabular face, so the column of them
                    // lines up digit for digit.
                    component FactValue: Label {
                        font.family: Theme.dataFamily
                        font.pointSize: Theme.fontSmall
                        Layout.alignment: Qt.AlignVCenter
                    }

                    FactLabel { visible: panel.trackDurationMs > 0; text: "Length" }
                    FactValue {
                        visible: panel.trackDurationMs > 0
                        // m:ss, not Theme.humanDuration(): that rounds to
                        // "~6 min", the right answer for how long an
                        // operation takes and the wrong one for a piece of
                        // music. A DJ reads a track length to the second.
                        text: {
                            var total = Math.round(panel.trackDurationMs / 1000);
                            var minutes = Math.floor(total / 60);
                            var seconds = total % 60;
                            return minutes + ":" + (seconds < 10 ? "0" : "") + seconds;
                        }
                    }

                    FactLabel { visible: panel.trackBpm > 0; text: "BPM" }
                    FactValue { visible: panel.trackBpm > 0; text: panel.trackBpm.toFixed(1) }

                    FactLabel { visible: panel.trackKey.length > 0; text: "Key" }
                    KeyBadge {
                        visible: panel.trackKey.length > 0
                        keyName: panel.trackKey
                        notation: panel.keyNotation
                    }

                    FactLabel { visible: panel.trackBitrate > 0; text: "Bitrate" }
                    RowLayout {
                        visible: panel.trackBitrate > 0
                        spacing: 3
                        Layout.alignment: Qt.AlignVCenter
                        Label {
                            text: panel.trackBitrate
                            font.family: Theme.dataFamily
                            font.pointSize: Theme.fontSmall
                        }
                        // The unit dimmer than the number: the number is what
                        // is being compared between two copies of a track,
                        // the unit is the same every time.
                        Label {
                            text: "kbps"
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                        }
                    }

                    // Unrated and zero stars are different answers, and both
                    // formats store them the same way at the file level, so
                    // the distinction only survives if it is shown.
                    FactLabel { visible: panel.trackRating >= 0; text: "Rating" }
                    Label {
                        visible: panel.trackRating >= 0
                        font.pointSize: Theme.fontSmall
                        Layout.alignment: Qt.AlignVCenter
                        text: panel.trackRating > 0 ? "\u2605".repeat(panel.trackRating)
                                                     + "\u2606".repeat(5 - panel.trackRating)
                                                    : "not rated above zero"
                        color: panel.trackRating > 0 ? Theme.warnIcon : Theme.textMuted
                    }

                    FactLabel { visible: panel.trackPlayCount > 0; text: "Plays" }
                    FactValue { visible: panel.trackPlayCount > 0; text: panel.trackPlayCount }
                }
            }

            Item {
                id: artwork
                // 1.6x the original 96: big enough to recognise a sleeve
                // at a glance, which is what artwork is for here.
                Layout.preferredWidth: 154
                Layout.preferredHeight: 154
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
                visible: panel.trackArtworkPath.length > 0

                // Only a track with a local file can play: a streaming
                // row's path names a cache on another machine, so it gets
                // no play button rather than one that fails.
                readonly property bool playable: panel.trackFilePath.length > 0
                    && panel.trackStreamingSource.length === 0

                Image {
                    anchors.fill: parent
                    // Assigned straight through: ArtworkPathRole already
                    // comes back as a file:// URL (toLocalFileUrl in
                    // scan_controller.cpp), so prefixing it again gave
                    // "file://file:///..." and an image that silently did
                    // not load.
                    source: panel.trackArtworkPath
                    fillMode: Image.PreserveAspectCrop
                    // Decoded at twice the display size, so it stays
                    // sharp on a hidpi screen without holding a
                    // full-resolution sleeve in memory.
                    sourceSize.width: 308
                    sourceSize.height: 308
                    // Synchronous: one small image, and loading it in the
                    // background made it pop in a frame or two after the
                    // rest of the panel. It also made the panel
                    // unscreenshotable, since a grab right after
                    // showFor() caught it before the image arrived.
                    asynchronous: false
                    smooth: true
                }

                // Fades rather than snaps: the sleeve is the thing being
                // looked at, and a control appearing over it instantly
                // reads as the image itself changing.
                Rectangle {
                    anchors.fill: parent
                    color: "#80000000"
                    opacity: artworkHover.hovered && artwork.playable ? 1 : 0
                    visible: opacity > 0
                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    Label {
                        anchors.centerIn: parent
                        text: "\u25b6"
                        color: "white"
                        font.pointSize: Theme.fontLarge * 1.6
                    }
                }

                HoverHandler { id: artworkHover }

                MouseArea {
                    anchors.fill: parent
                    enabled: artwork.playable
                    cursorShape: artwork.playable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: panel.playbackController.load(panel.format, panel.libraryPath,
                        panel.trackSourceId, panel.trackFilePath, panel.trackTitle, panel.trackArtist,
                        panel.trackArtworkPath, panel.trackCues)
                }
            }
        }

        // The DJ's own words about the track, so it wraps rather than
        // being squeezed into the grid above.
        ColumnLayout {
            visible: panel.trackComment.length > 0
            Layout.fillWidth: true
            spacing: 2
            Label { text: "Comment"; color: Theme.textMuted }
            Label {
                text: panel.trackComment
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }


        Label {
            visible: panel.trackStreamingSource.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Streaming source: " + panel.trackStreamingSource
                + " - no local file; playback, merging, and adding cues aren't available for this track."
            color: Theme.textMuted
        }

        WaveformView {
            id: waveformView
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            cueData: panel.trackCues
            trackDurationMs: panel.trackDurationMs
            progress: -1
            // Off with the rest of the feature: without this the waveform
            // still invites a click that now leads nowhere, which is
            // worse than a waveform that is plainly read-only.
            cueEditable: panel.addCueEnabled
            onPositionClicked: (ms) => {
                if (!panel.addCueEnabled || panel.trackStreamingSource.length > 0) {
                    return;
                }
                panel.pendingPositionMs = ms;
                panel.pendingLoopEndMs = -1;
            }
            onLoopRangeSelected: (startMs, endMs) => {
                if (!panel.addCueEnabled || panel.trackStreamingSource.length > 0) {
                    return;
                }
                panel.pendingPositionMs = startMs;
                panel.pendingLoopEndMs = endMs;
                // Loops always need a hot slot -- see the
                // combo's own visibility below.
                cueKindCombo.currentIndex = 0;
            }
        }

        Label {
            visible: panel.addCueEnabled && panel.pendingPositionMs < 0
                && panel.trackStreamingSource.length === 0
            text: "Click the waveform above to add a cue there, or drag to add a loop."
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // Position-only for now, no beatgrid snap, see
        // AddCueController's own class comment for why.
        ColumnLayout {
            id: addCueForm
            visible: panel.addCueEnabled && panel.pendingPositionMs >= 0
            Layout.fillWidth: true
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    font.bold: true
                    text: panel.pendingLoopEndMs >= 0
                        ? "Add loop " + panel.formatDuration(panel.pendingPositionMs / 1000)
                            + " to " + panel.formatDuration(panel.pendingLoopEndMs / 1000)
                        : "Add cue at " + panel.formatDuration(panel.pendingPositionMs / 1000)
                }
                Item { Layout.fillWidth: true }
                // Loops always occupy a hot slot -- same
                // constraint the hardware itself has, no
                // memory-loop equivalent exists to offer here.
                Label { text: "Kind:"; visible: panel.pendingLoopEndMs < 0 }
                ComboBox {
                    id: cueKindCombo
                    model: ["Hot", "Memory"]
                    Layout.preferredWidth: 110
                    visible: panel.pendingLoopEndMs < 0
                }
                Label {
                    text: "Slot:"
                    visible: panel.pendingLoopEndMs >= 0 || cueKindCombo.currentIndex === 0
                }
                SpinBox {
                    id: hotCueNumberSpin
                    visible: panel.pendingLoopEndMs >= 0 || cueKindCombo.currentIndex === 0
                    from: 1
                    to: 8
                    value: 1
                }
            }
            TextField {
                id: cueCommentField
                Layout.fillWidth: true
                placeholderText: "Comment (optional)"
            }
            Label {
                // AddCueController refuses this too (belt and
                // suspenders), but saying so up front skips a
                // pointless rescan-then-fail round trip -- see
                // AddCueController's own class comment for why
                // rekordbox/OneLibrary loop writes are refused
                // rather than silently downgraded to a point.
                visible: panel.pendingLoopEndMs >= 0 && panel.format !== "engine"
                text: "Hot loops can only be added to Engine tracks right now."
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                visible: addCueController.errorMessage.length > 0
                text: addCueController.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                visible: addCueController.statusMessage.length > 0
                text: addCueController.statusMessage
                color: Theme.good
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                BusyIndicator {
                    visible: addCueController.busy
                    running: visible
                    implicitWidth: 20
                    implicitHeight: 20
                }
                Label {
                    visible: addCueController.busy
                    text: "Adding..."
                    color: Theme.textMuted
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Cancel"
                    enabled: !addCueController.busy
                    onClicked: {
                        panel.pendingPositionMs = -1;
                        panel.pendingLoopEndMs = -1;
                    }
                }
                Button {
                    text: panel.pendingLoopEndMs >= 0 ? "Add Loop" : "Add Cue"
                    enabled: !addCueController.busy
                        && !(panel.pendingLoopEndMs >= 0 && panel.format !== "engine")
                    onClicked: {
                        var isLoop = panel.pendingLoopEndMs >= 0;
                        var isHot = isLoop || cueKindCombo.currentIndex === 0;
                        addCueController.addCue(panel.format, panel.libraryPath,
                            panel.trackSourceId, panel.pendingPositionMs,
                            isHot ? "hot" : "memory", hotCueNumberSpin.value,
                            isLoop ? "#3daee9" : (isHot ? "#ffcc00" : "#00a5e3"), cueCommentField.text,
                            isLoop, isLoop ? panel.pendingLoopEndMs : 0, panel.trackTitle);
                    }
                }
            }
        }

        // What this track has staged and not saved yet.
        ColumnLayout {
            visible: panel.pendingCues.length > 0
            Layout.fillWidth: true
            spacing: 4
            Label {
                text: "Unsaved on this track"
                font.bold: true
                color: Theme.warnText
                font.pointSize: Theme.fontSmall
            }
            Repeater {
                model: panel.pendingCues
                delegate: RowLayout {
                    id: pendingRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        color: Theme.text
                        text: (pendingRow.modelData.isLoop ? "Hot loop " + pendingRow.modelData.hotCueNumber
                            : pendingRow.modelData.kind === "hot" ? "Hot cue " + pendingRow.modelData.hotCueNumber
                            : "Memory cue")
                            + " at " + panel.formatDuration(pendingRow.modelData.positionMs / 1000)
                    }
                    ToolButton {
                        text: "Undo"
                        enabled: !addCueController.writing
                        onClicked: addCueController.unstage(pendingRow.modelData.changeId)
                    }
                }
            }
        }


        Label {
            text: "Playlists"
            font.bold: true
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: panel.trackPlaylistNames.length > 0
                ? panel.trackPlaylistNames.join("\n")
                : "Not in any playlist"
        }

        ColumnLayout {
            visible: panel.artistTracks.length > 0
            Layout.fillWidth: true
            spacing: 4

            Label {
                text: panel.artistTracks.length === 1
                    ? "1 more track by " + panel.trackArtist
                    : panel.artistTracks.length + " more tracks by " + panel.trackArtist
                color: Theme.textMuted
                Layout.fillWidth: true
                elide: Text.ElideRight
            }

            Label {
                visible: panel.jumpMissMessage.length > 0
                text: panel.jumpMissMessage
                color: Theme.warnText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: panel.artistTracks
                delegate: ItemDelegate {
                    required property var modelData
                    Layout.fillWidth: true
                    padding: 6
                    onClicked: panel.jumpToTrackRequested(modelData.sourceId)
                    ToolTip.visible: hovered
                    ToolTip.text: "Open this track"
                    contentItem: RowLayout {
                        spacing: 8
                        Label {
                            text: modelData.title
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: modelData.key.length > 0
                            text: modelData.key
                            color: Theme.textMuted
                            font.family: Theme.dataFamily
                        }
                        Label {
                            visible: modelData.durationSeconds > 0
                            text: Theme.humanDuration(modelData.durationSeconds)
                            color: Theme.textMuted
                            font.family: Theme.dataFamily
                        }
                    }
                }
            }
        }
    }
        }
    }
}
