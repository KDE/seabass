// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A dedicated, discoverable entry point for LibraryConsistencyController's
// junk-cue cleanup (see domain::JunkCueFinder's own doc comment: a memory
// cue sitting at 0:00 is almost always an accidental leftover from
// analysis or import). This already existed as a secondary section on
// Library Health, whose own top-level billing is about missing-file
// repair -- easy to miss if you're looking for "clean up stray cues"
// specifically, per real user feedback. That section stays exactly where
// it was on Library Health too (a legitimate contextual shortcut, not a
// replaced one) -- this page is the "reachable through the main nav
// structure" home for the same feature, filed under Maintain
// where someone would actually think to look for it.
// Reuses LibraryConsistencyController rather than a new controller, at
// its CuesOnly depth: the catalogs and their cues, and none of Library
// Health's other checks. Those read every track's file on the stick (the
// sample-rate audit opens each one whose Engine row lacks a rate), and
// running them here made a stray-cue scan take minutes on a full USB
// stick for results this page never shows.
Page {
    id: root
    required property string stickLabel
    // The page it was opened from, for the breadcrumb: empty from the
    // home's Maintain card, "Sync Cue Points" from its stray-cue link.
    property string hubLabel: ""
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController
    // Plays a row's track from its cover, as Browse's rows do. Optional:
    // a page built without one (tests) shows no play key.
    property var playbackController: null

    LibraryConsistencyController {
        id: ownController
        scanDepth: LibraryConsistencyController.CuesOnly
    }
    // The page's own controller, unless a test hands in a stand-in to step
    // through a scan's phases (tst_JunkCuePage). Read when the page is
    // built, like EditSessionHost's registry.
    property QtObject controllerForTesting: null
    readonly property QtObject consistencyController: root.controllerForTesting !== null
        ? root.controllerForTesting : ownController

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "library-health"
        // The page does one thing, so the button says it. "Save" is the
        // default for a page that edits several kinds of thing; here the
        // only staged change is a stray cue being removed.
        //
        // The label is about this page, not about the session. Library
        // Health shares the "library-health" feature name, and it stages
        // missing-file repairs and orphan deletions as well as stray-cue
        // removals. Anything left unsaved there is still in this session,
        // so a button saying "Clean Up" writes those too, and it is live
        // on arrival even though nothing was staged here. The pending
        // list and the tooltip still name every change; the label does
        // not. Library Health's own save keeps saying "Save", because
        // that page really does stage several kinds of change.
        saveLabel: "Clean Up"
        // A save that landed whole is said over the list (savedToast
        // below), so it opens no summary; anything short of that still does.
        quietCleanSaves: true
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // "" scopes the scan to the whole library (every catalog present),
    // same empty-means-all convention every other picker in this app
    // uses (see SyncPage.qml's own selectedPlaylistName).
    property string selectedPlaylistName: ""

    // "All tracks" first (no meaningful single count across up to three
    // independent catalogs, so left blank rather than showing a
    // misleading sum), then the union of playlist names across whichever
    // catalogs are present -- built from consistencyController's own
    // unfiltered scan, so this list doesn't shrink once a playlist is
    // selected.
    readonly property var playlistPickerModel: [{name: "All tracks", count: ""}].concat(
        consistencyController.playlistNames.map((n) =>
            ({name: n, count: consistencyController.playlistTrackCounts[n] ?? 0})))

    function formatLabel(format) {
        if (format === "engine") return "Engine OS";
        if (format === "onelibrary") return "OneLibrary";
        return "DeviceLibrary";
    }
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    // The row's key, BPM and length, worded as Browse words them.
    function formatBpm(bpm) {
        if (bpm <= 0) {
            return "--";
        }
        const oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }
    function formatDuration(ms) {
        const total = Math.round(ms / 1000);
        const m = Math.floor(total / 60);
        const s = total % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    // Opens on the whole library, every time.
    //
    // It used to open on the playlist last picked anywhere in the app,
    // which made this page and Library Health -- the same check, the same
    // controller, the same rule -- report different numbers, with nothing
    // on either page to say why. A filter chosen on the Sync page days
    // ago silently narrowing "how much is wrong with this stick" is not a
    // scope anyone asked for. The picker below still narrows it, and
    // still remembers a pick for the pages that want one.
    Component.onCompleted: {
        root.selectedPlaylistName = "";
        consistencyController.scan(root.rekordboxPath, root.enginePath, "");
    }

    // A remembered playlist this library does not have would scan
    // nothing. The picker lists the whole library's playlists whatever
    // the scope, so once a scan is done a missing one falls back to all
    // tracks -- without forgetting it, since the next stick may have it.
    // Checked a turn later, not from inside the controller's own
    // busyChanged. A cancel stays on this page, so it is checked then too.
    function dropMissingPlaylist() {
        // Not after a scan that did not read everything either, stopped
        // or failed: its playlists are not the library's. A stop would be
        // undone by the rescan; a failure leaves the pick possibly living
        // in the very catalog that could not be read (a stick pulled, a
        // locked database), and rescanning the whole library on a stick
        // that just failed would throw the user's choice away for nothing.
        if (consistencyController.busy || root.resultIncomplete || root.selectedPlaylistName.length === 0
            || consistencyController.playlistNames.indexOf(root.selectedPlaylistName) >= 0) {
            return;
        }
        root.selectedPlaylistName = "";
        consistencyController.scan(root.rekordboxPath, root.enginePath, root.selectedPlaylistName);
    }

    // The last scan did not look at everything: it was stopped from the
    // overlay, or a catalog could not be read (the stick pulled mid-scan
    // lands here). Whatever it found stays listed, but the page must not
    // present it as the whole answer, and above all must not say "no
    // cues" about a library it never finished reading.
    //
    // Taken when the scan ends, not read live off errorMessage: that also
    // carries a staging error said long after a scan that read everything,
    // which is no reason to call its count partial.
    property bool scanStopped: false
    property bool scanFailed: false
    readonly property bool resultIncomplete: root.scanStopped || root.scanFailed

    function rescan() {
        consistencyController.scan(root.rekordboxPath, root.enginePath, root.selectedPlaylistName);
    }

    Connections {
        target: consistencyController
        function onBusyChanged() {
            if (consistencyController.busy) {
                root.scanStopped = false;
                root.scanFailed = false;
            } else {
                root.scanFailed = consistencyController.errorMessage.length > 0;
                Qt.callLater(root.dropMissingPlaylist);
            }
        }
        // Stays on this page, unlike Library Health's check pages: this
        // one scans for itself, so what is left is a list the user can
        // still act on or scan again, and the picker beside it.
        function onScanCancelled() { root.scanStopped = true; }
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                stickLabel: root.stickLabel
                middleLabel: root.hubLabel
                title: "Clean Up Stray Cues"
                backEnabled: !consistencyController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Playlist:"
                color: Theme.textMuted
            }
            PlaylistPickerCombo {
                objectName: "playlistPicker"
                Layout.minimumWidth: 140
                // The controller ignores a scan asked for while one runs,
                // so a pick made now would show a playlist the list below
                // is not about.
                enabled: !consistencyController.busy
                model: root.playlistPickerModel
                currentIndex: {
                    if (root.selectedPlaylistName.length === 0) {
                        return 0;
                    }
                    for (var i = 1; i < root.playlistPickerModel.length; i++) {
                        if (root.playlistPickerModel[i].name === root.selectedPlaylistName) {
                            return i;
                        }
                    }
                    return 0;
                }
                ToolTip.visible: hovered
                ToolTip.text: "Scope Clean Up Stray Cues to one playlist instead of the whole library"
                onPlaylistPicked: (index, modelData) => {
                    root.selectedPlaylistName = index === 0 ? "" : modelData.name;
                    root.appSettingsController.lastPlaylistName = root.selectedPlaylistName;
                    consistencyController.scan(root.rekordboxPath, root.enginePath, root.selectedPlaylistName);
                }
            }
        }
    }

    // Remove and Stage Removing All ask nothing first: they stage, and a
    // staged removal is undone with the row's own Unstage until Clean Up
    // writes it. The questions they used to ask were the write's, asked
    // at the wrong moment (one of them even said "permanently removes"
    // about a staging), and Clean Up still backs up before it writes.
    //
    // A save that landed whole, said over the list for a moment, in the
    // summary's own words ("12 cues removed"). The list is empty by
    // then: every row of a written track goes with the save.
    ConfirmationToast {
        id: savedToast
        objectName: "savedToast"
        parent: junkCueListView
        anchors.centerIn: parent
    }
    Connections {
        target: editHost.session
        ignoreUnknownSignals: true
        function onSaveFinished(summary) {
            Qt.callLater(() => root.showSaved(summary));
        }
    }
    function showSaved(summary) {
        const whole = summary && !(summary.error && summary.error.length > 0)
            && !(summary.warning && summary.warning.length > 0) && summary.cancelled !== true
            && !(summary.skipped > 0) && summary.written !== undefined && summary.written === summary.total;
        if (!whole || !(summary.written > 0)) {
            return;
        }
        savedToast.show(summary.written + " " + summary.unit + " " + summary.verb);
    }

    MessageDialog {
        id: confirmIgnoreAllJunkCuesDialog
        severity: SeabassDialog.Question
        title: "Ignore all cues at 0:00"
        headline: "Dismisses every cue at 0:00 currently listed, just for this view."
        detailText: "Nothing is written, they'll show up again the next time you scan."
        acceptText: "Ignore All"
        onAccepted: consistencyController.ignoreAllJunkCues()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: consistencyController.errorMessage.length > 0
            text: consistencyController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: consistencyController.statusMessage.length > 0
            text: consistencyController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        WarningBanner {
            objectName: "junkCueScanStopped"
            visible: root.scanStopped
            text: "The scan was stopped before it finished, so this list is incomplete."
            Button {
                text: "Scan Again"
                enabled: !consistencyController.busy
                onClicked: root.rescan()
            }
        }

        Rectangle {
            visible: junkCueListView.count > 0
            Layout.fillWidth: true
            Layout.bottomMargin: 12
            radius: Theme.cornerRadius
            color: Theme.surface
            border.color: Theme.borderSubtle
            implicitHeight: summaryRow.implicitHeight + 20

            RowLayout {
                id: summaryRow
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12
                Label {
                    objectName: "junkCueSummary"
                    // The scope belongs beside the count. Narrowed to a
                    // playlist, this page answers a different question
                    // from Library Health's card, and a number that does
                    // not say so reads as a disagreement about the same
                    // one.
                    text: junkCueListView.count + " cue(s) sitting at 0:00, likely accidental"
                        + (root.selectedPlaylistName.length > 0
                            ? ", in " + root.selectedPlaylistName + " only" : ", across the whole library")
                        + (root.resultIncomplete ? " (so far: not every catalog was read)" : "")
                    font.bold: true
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Button {
                    objectName: "stageAllButton"
                    text: "Stage Removing All"
                    // Nothing left to stage once they all are: the rows
                    // stay listed with their own Unstage buttons, so the
                    // list being long is no reason for this to be live.
                    enabled: !consistencyController.busy && !consistencyController.writing
                        && consistencyController.unstagedJunkCueCount > 0
                    ToolTip.visible: hovered
                    ToolTip.text: consistencyController.unstagedJunkCueCount === 0
                        ? "Every one of them is staged already. Press Clean Up to write it."
                        : "Stage removing every cue at 0:00 listed, in all catalogs."
                    onClicked: consistencyController.removeAllJunkCues()
                }
                Button {
                    text: "Ignore All"
                    enabled: !consistencyController.busy
                    onClicked: confirmIgnoreAllJunkCuesDialog.open()
                }
            }
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: junkCueListView
            objectName: "junkCueListView"
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: consistencyController.junkCues
            spacing: 4
            ScrollBar.vertical: BigScrollBar {}

            // A track row as Browse draws one (ScanPage.qml's
            // trackDelegate): the cover with its play key, the title over
            // the artist, key, BPM and length, the playing row outlined
            // with its waveform behind it. After those, what this page is
            // about: why the row is here, which catalog, and the row's own
            // Remove, Unstage and Ignore.
            delegate: Rectangle {
                id: junkDelegate
                objectName: "junkCueRow"
                width: ListView.view.width
                // Taller only for the playing row, so its waveform is
                // legible rather than a sliver.
                height: junkDelegate.isPlaying ? 64 : 56

                required property int index
                required property string format
                required property string title
                required property string artist
                required property var track
                required property bool staged
                // Required, like the roles above it: a plain property
                // of the same name is NOT filled in from the model when
                // a delegate declares required ones, so it would sit
                // empty and the line would never appear.
                required property string reason

                readonly property bool isPlaying: root.playbackController !== null
                    && root.playbackController.hasTrack
                    && root.playbackController.currentFormat === junkDelegate.format
                    && root.playbackController.currentSourceId === (junkDelegate.track.sourceId || "")
                readonly property string streamingSource: junkDelegate.track.streamingSource || ""

                color: junkDelegate.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd
                radius: Theme.cornerRadius
                border.color: junkDelegate.isPlaying ? Theme.accent : "transparent"
                border.width: junkDelegate.isPlaying ? 2 : 0

                // The playing row's waveform, faded, behind its content.
                Loader {
                    active: junkDelegate.isPlaying
                    anchors.fill: parent
                    anchors.margins: 2
                    sourceComponent: WaveformView {
                        opacity: 0.35
                        waveformData: root.playbackController.waveform
                        format: root.playbackController.currentFormat
                        cueData: root.playbackController.cues
                        trackDurationMs: root.playbackController.duration
                        progress: root.playbackController.duration > 0
                            ? root.playbackController.position / root.playbackController.duration : 0
                    }
                }
                HoverHandler { id: rowHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.tightSpacing
                    anchors.rightMargin: Theme.tightSpacing
                    spacing: Theme.rowSpacing

                    Rectangle {
                        objectName: "rowCell_artwork"
                        Layout.preferredWidth: Theme.iconSizeNormal
                        Layout.preferredHeight: Theme.iconSizeNormal
                        color: Theme.surface
                        radius: Theme.coverRadius(Math.min(width, height))
                        ArtworkImage {
                            objectName: "rowArtwork"
                            anchors.fill: parent
                            source: junkDelegate.track.artworkPath || ""
                        }
                        // Play the row's track from its cover: shown while
                        // the row is hovered, and while it is the track
                        // playing, as a pause key then. Not for a streaming
                        // track, which has no file to play.
                        ToolButton {
                            id: coverPlay
                            objectName: "rowPlayButton"
                            anchors.fill: parent
                            // Every side, not `padding`: a style that sets
                            // a side itself wins over it.
                            leftPadding: 0
                            rightPadding: 0
                            topPadding: 0
                            bottomPadding: 0
                            display: AbstractButton.IconOnly
                            text: junkDelegate.isPlaying && root.playbackController.playing ? "Pause" : "Play"
                            visible: root.playbackController !== null
                                && (junkDelegate.track.filePath || "").length > 0
                                && junkDelegate.streamingSource.length === 0
                                && (rowHover.hovered || coverPlay.hovered || junkDelegate.isPlaying)
                            contentItem: Item {
                                SeabassIcon {
                                    anchors.centerIn: parent
                                    iconName: junkDelegate.isPlaying && root.playbackController.playing
                                        ? "media-playback-pause" : "media-playback-start"
                                    size: Theme.iconSizeSmall * 0.75
                                    color: "white"
                                }
                            }
                            background: Rectangle {
                                radius: Theme.coverRadius(Math.min(width, height))
                                color: "black"
                                opacity: coverPlay.hovered ? 0.55 : 0.4
                            }
                            ToolTip.visible: hovered
                            ToolTip.text: junkDelegate.isPlaying
                                ? (root.playbackController.playing ? "Pause" : "Play")
                                : "Play " + root.formatLabel(junkDelegate.format) + "'s copy of the track"
                            onClicked: {
                                if (junkDelegate.isPlaying) {
                                    root.playbackController.togglePlay();
                                    return;
                                }
                                const t = junkDelegate.track;
                                root.playbackController.load(junkDelegate.format, root.pathForFormat(junkDelegate.format),
                                    t.sourceId, t.filePath, t.title, t.artist, t.artworkPath, t.cues);
                            }
                        }
                    }

                    ColumnLayout {
                        objectName: "rowCell_title"
                        Layout.fillWidth: true
                        spacing: 1
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                text: junkDelegate.title
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            // Plain text, no logo (see AboutPage.qml's
                            // trademark note): where a file-less row
                            // comes from, and why its cover has no play key.
                            Rectangle {
                                visible: junkDelegate.streamingSource.length > 0
                                radius: 3
                                color: Theme.groupBackground
                                border.color: Theme.borderSubtle
                                implicitWidth: streamingLabel.implicitWidth + 8
                                implicitHeight: streamingLabel.implicitHeight + 4
                                Label {
                                    id: streamingLabel
                                    anchors.centerIn: parent
                                    text: junkDelegate.streamingSource
                                    font.pointSize: Theme.fontTiny
                                    font.bold: true
                                    color: Theme.textMuted
                                }
                            }
                        }
                        Label {
                            text: junkDelegate.artist
                            color: Theme.textMuted
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    KeyBadge {
                        objectName: "rowCell_key"
                        keyName: junkDelegate.track.key || ""
                        notation: root.appSettingsController.keyNotation
                        Layout.preferredWidth: Theme.scaled(50)
                    }
                    Label {
                        objectName: "rowCell_bpm"
                        text: root.formatBpm(junkDelegate.track.bpm || 0)
                        Layout.preferredWidth: 50
                    }
                    Label {
                        objectName: "rowCell_duration"
                        text: root.formatDuration(junkDelegate.track.durationMs || 0)
                        Layout.preferredWidth: 60
                    }

                    // Why this row is here. Two checks feed this list: a
                    // cue at the very start of the track, and one of a
                    // crowd of hot cues in its first two seconds.
                    // Describing the second as the first would be wrong,
                    // and every row here is an offer to delete somebody's
                    // cue.
                    Label {
                        objectName: "junkCueReason"
                        visible: junkDelegate.reason.length > 0
                        text: junkDelegate.reason
                        elide: Text.ElideRight
                        Layout.preferredWidth: Theme.scaled(220)
                        font.pointSize: Theme.fontSmall
                        color: Theme.textMuted
                        ToolTip.visible: reasonHover.hovered && truncated
                        ToolTip.text: junkDelegate.reason
                        HoverHandler { id: reasonHover }
                    }
                    Rectangle {
                        radius: 3
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        implicitWidth: junkFormatLabelText.implicitWidth + 8
                        implicitHeight: junkFormatLabelText.implicitHeight + 4
                        Label {
                            id: junkFormatLabelText
                            anchors.centerIn: parent
                            text: root.formatLabel(junkDelegate.format)
                            font.pointSize: Theme.fontTiny
                            font.bold: true
                            color: Theme.textMuted
                        }
                    }
                    StatusBadge {
                        visible: junkDelegate.staged
                        label: "Staged"
                        badgeColor: Theme.warnText
                        tooltipText: "Not on the stick yet: press Clean Up."
                    }
                    // Stages, at once: every stray cue of this track, which
                    // flips every row of it. Unstage takes it back out.
                    Button {
                        objectName: "rowRemoveButton"
                        text: junkDelegate.staged ? "Unstage" : "Remove"
                        enabled: !consistencyController.busy && !consistencyController.writing
                        ToolTip.visible: hovered
                        ToolTip.text: junkDelegate.staged
                            ? "Staged for removal, not on the stick yet: press Clean Up. Click to take it back out."
                            : "Stage removing every cue this track has sitting at 0:00; Clean Up writes it, backed up first."
                        onClicked: {
                            if (junkDelegate.staged) {
                                consistencyController.unstageJunkCue(junkDelegate.index);
                            } else {
                                consistencyController.removeJunkCue(junkDelegate.index);
                            }
                        }
                    }
                    Button {
                        objectName: "rowIgnoreButton"
                        text: "Ignore"
                        enabled: !consistencyController.busy
                        onClicked: consistencyController.ignoreJunkCue(junkDelegate.index)
                    }
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 24
            Layout.bottomMargin: 12
            objectName: "junkCueNoneFound"
            // Only after a scan that read everything. Zero from a scan
            // that was stopped, or that could not read a catalog, is not
            // a finding.
            visible: junkCueListView.count === 0 && !consistencyController.busy && !root.resultIncomplete
            text: "No cues are sitting at 0:00."
            color: Theme.textMuted
        }
    }

    // The scan reads every catalog on the stick, off the UI thread, and on
    // a full stick that takes a while: say so over the whole content
    // area, the way Duplicates and Library Health do, rather than with a
    // spinner in the corner of the header. The header stays live, so the
    // breadcrumb still leaves (see the controller's destructor for what
    // becomes of the scan then).
    BusyOverlay {
        objectName: "junkCueBusyOverlay"
        anchors.fill: parent
        busy: consistencyController.busy
        current: consistencyController.scanCurrent
        total: consistencyController.scanTotal
        unitName: "tracks"
        label: consistencyController.scanningFormat.length > 0
            ? "Looking for stray cues in " + root.formatLabel(consistencyController.scanningFormat) + "..."
            : "Looking for stray cues..."
        // Which step of that catalog's read, counted by the bar above it:
        // each step reports its own total, so the bar counts rather than
        // sweeps while a stick is read.
        currentItem: consistencyController.scanPhase
        cancellable: consistencyController.scanCancellable
        onCancelRequested: consistencyController.cancelScan()
    }
}
