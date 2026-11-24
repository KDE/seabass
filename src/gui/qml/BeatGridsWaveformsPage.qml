// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui
import "common/SaveSummary.js" as SaveSummary

// Beat Grids & Waveforms (Sync): rekordbox's beat grids and waveforms
// copied into the stick's Engine library, for the playlists and tracks the
// user ticks. Years of work go into rekordbox grids, and a Denon player's
// own analysis is not always as good; replacing it is the point.
//
// Top to bottom: what the page does, what to copy (beat grids, waveforms,
// and whether over grids set on the player and waveforms it drew), what the ticked tracks would
// get and the action that stages it; then rekordbox's playlists, each with
// a box that ticks all of it, beside the tracks of the one shown, in its
// order, each with a box of its own. Nothing is ticked at first.
//
// A track's row says what copying would do to its grid and to its
// waveform. That is worked out by a preview (BeatGridsController), which
// writes nothing, for the tracks shown and the ticked ones only: a row
// says "Checking" until its answer comes. "Stage Selected" stages one
// change per ticked track that would change; Save writes them, backed up
// first, with the usual floating Save, Cancel and summary.
//
// The way back, as two secondary actions under it: the player's own
// analysis of a row is kept on the stick before a copy replaces it, and
// "Restore Engine's Analysis" puts it back; "Let the Player Analyse Again"
// has the player analyse the ticked rows anew. Each stages one change per
// track into the same session. A track row says when its analysis is kept.
//
// Experimental (docs/experimental-features.md): its card is behind the
// setting, and the page says so under its title.
//
// A rekordbox track with no Engine row cannot be ticked; when the list
// shows such tracks the page says how many and asks for the libraries to
// be synced first, with a link to Sync after Rekordbox Export.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    property var appSettingsController: null
    property var playbackController: null

    // Overridable so a test can hand in its own; the app never sets it.
    property var controller: realController

    // The link in the sentence about tracks not in the Engine library.
    signal rekordboxExportSyncRequested(string stickLabel, string rekordboxPath, string enginePath)
    // The stick is mounted read-only (its filesystem needs checking).
    // Overridable for a test, which has no read-only stick to hand.
    property bool stickReadOnly: root.controller.stickReadOnly

    BeatGridsController {
        id: realController
        objectName: "beatGridsController"
    }

    Component.onCompleted: root.controller.load(root.rekordboxPath, root.enginePath)
    // Back in front after another page saved to this stick (the link below
    // goes to Sync after Rekordbox Export, which adds Engine rows): the
    // libraries are read again, the ticks that still apply kept.
    StackView.onActivated: root.controller.reloadIfStale()

    EditSessionHost {
        id: editHost
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        // A save that landed whole is said with the toast below, in this
        // page's words; anything short of that gets the summary dialog.
        quietCleanSaves: true
        feature: "beat-grids"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    ConfirmationToast {
        id: savedToast
        objectName: "savedToast"
        parent: root
        z: 1100
        anchors.centerIn: parent
    }
    Connections {
        target: root.controller
        function onSaveCounted(whole) {
            if (whole) {
                savedToast.show(root.controller.lastSaveText);
            }
        }
        // A cancelled read takes the user back to where they came from.
        function onLoadCancelled() { root.StackView.view.pop(); }
    }
    // An Undo Last Save says what every page says after one (#98): the
    // session marks the summary (undo: true), and the words are
    // SaveSummary's.
    Connections {
        target: editHost.session
        ignoreUnknownSignals: true
        function onSaveFinished(summary) {
            if (SaveSummary.isUndo(summary) && !(summary.error && summary.error.length > 0)
                    && summary.written > 0) {
                savedToast.show(SaveSummary.undoneText());
            }
        }
    }

    // Read, and an Engine library Seabass writes to.
    readonly property bool usable: root.controller.loaded && !root.controller.engineTooOld
    // What is ticked, and what to copy, can change: not while a batch is
    // staged (it was worked out with them), saved, or another page edits.
    readonly property bool ticksEnabled: root.usable && !root.stickReadOnly && root.controller.stagedCount === 0
        && !root.controller.writing && !editHost.blockedByOtherPage
    readonly property bool staged: root.controller.stagedCount > 0

    function formatBpm(bpm) {
        const oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }
    function toneColor(tone) {
        switch (tone) {
        case "good": return Theme.good;
        case "warn": return Theme.warnText;
        case "danger": return Theme.danger;
        default: return Theme.textMuted;
        }
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin and
        // nothing else (see SyncPage.qml's header for why).
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }
        implicitHeight: headerLayout.implicitHeight + Theme.headerBottomPadding

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.sectionSpacing

            RowLayout {
                objectName: "crumbRow"
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.crumbGap - headerLayout.spacing
                spacing: Theme.rowSpacing
                BackBreadcrumb {
                    objectName: "breadcrumb"
                    stack: root.StackView.view
                    stickLabel: root.stickLabel
                    title: "Beat Grids & Waveforms"
                    backEnabled: !root.controller.writing
                    onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                    onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
                }
                // Experimental (docs/experimental-features.md): the card
                // that opens this page is behind the setting.
                ExperimentalBadge {
                    Layout.alignment: Qt.AlignVCenter
                }
                Item { Layout.fillWidth: true }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.tightSpacing

                // What this page does, in two sentences.
                Label {
                    objectName: "introLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Copies rekordbox's beat grids and waveforms into this stick's Engine library, for the "
                        + "tracks you tick. A track the player has not analysed yet then loads without analysis; "
                        + "on one it has, Engine's own grid stays beside the copy, so the player can go back to it."
                }
                Label {
                    objectName: "experimentalLabel"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.conflictText
                    // What is replaced and what is not, as the boxes below
                    // have it, and the way back.
                    text: "Experimental: the beat grid and the waveform are converted from rekordbox's, and "
                        + "the conversion has been checked on a player with test tracks only. The grid goes "
                        + "beside Engine's own; a grid locked or changed on the player, and a waveform the "
                        + "player drew, are replaced only when you tick their box. Seabass keeps the "
                        + "player's own analysis on the stick first, so Restore Engine's Analysis can put it back."
                }

                // What to copy. The boxes themselves on the page's left line.
                // A box that does not fit beside the others goes onto a line
                // of its own, and one wider than the page wraps its words.
                Flow {
                    id: optionsRow
                    objectName: "optionsRow"
                    Layout.fillWidth: true
                    visible: root.usable
                    spacing: Theme.rowSpacing
                    SeabassCheckBox {
                        objectName: "gridsCheckBox"
                        width: Math.min(implicitWidth, optionsRow.width)
                        leftPadding: 0
                        text: "Beat grids"
                        checked: root.controller.grids
                        enabled: root.ticksEnabled
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: root.staged ? "Unstage to change what is copied"
                            : "rekordbox's beat grid becomes the one the player plays by"
                        onToggled: {
                            root.controller.grids = checked;
                            checked = Qt.binding(() => root.controller.grids);
                        }
                    }
                    SeabassCheckBox {
                        objectName: "waveformsCheckBox"
                        width: Math.min(implicitWidth, optionsRow.width)
                        text: "Waveforms"
                        checked: root.controller.waveforms
                        enabled: root.ticksEnabled
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: root.staged ? "Unstage to change what is copied"
                            : "rekordbox's waveform, for a track the player has drawn none of"
                        onToggled: {
                            root.controller.waveforms = checked;
                            checked = Qt.binding(() => root.controller.waveforms);
                        }
                    }
                    SeabassCheckBox {
                        objectName: "replaceEditsCheckBox"
                        width: Math.min(implicitWidth, optionsRow.width)
                        text: "Also replace grids locked or changed on the player"
                        checked: root.controller.replaceEngineEdits
                        enabled: root.ticksEnabled
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: root.staged ? "Unstage to change what is copied"
                            : "Off, a grid the DJ locked or moved on the player is left as it is"
                        onToggled: {
                            root.controller.replaceEngineEdits = checked;
                            checked = Qt.binding(() => root.controller.replaceEngineEdits);
                        }
                    }
                    SeabassCheckBox {
                        objectName: "replaceWaveformsCheckBox"
                        width: Math.min(implicitWidth, optionsRow.width)
                        text: "Also replace waveforms the player drew"
                        checked: root.controller.replacePlayerWaveforms
                        // Only for a copy of the waveforms.
                        enabled: root.ticksEnabled && root.controller.waveforms
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: root.staged ? "Unstage to change what is copied"
                            : "Off, a waveform the player drew is left as it is"
                        onToggled: {
                            root.controller.replacePlayerWaveforms = checked;
                            checked = Qt.binding(() => root.controller.replacePlayerWaveforms);
                        }
                    }
                }

                // What the ticked tracks would get, and the action.
                RowLayout {
                    objectName: "summaryRow"
                    Layout.fillWidth: true
                    visible: root.usable
                    spacing: Theme.rowSpacing
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 2
                        Label {
                            objectName: "summaryLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.controller.summaryText
                        }
                        Label {
                            objectName: "leftAloneLabel"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: root.controller.leftAloneText
                        }
                    }
                    Label {
                        objectName: "stagedNote"
                        visible: root.staged
                        text: "staged, not saved yet"
                        color: Theme.warnText
                    }
                    Button {
                        objectName: "undoLastSaveButton"
                        text: "Undo Last Save"
                        visible: editHost.session !== null && editHost.session.canUndo === true
                        enabled: !editHost.writing && !editHost.dirty
                        ToolTip.visible: hovered
                        ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                        onClicked: editHost.session.undoLastSave()
                    }
                    // Every row the list shows ticked, as the shown
                    // playlist's box (or All tracks') ticks them; and every
                    // tick taken off. Each is off where it would change
                    // nothing, and while a batch is staged, as the row
                    // boxes are.
                    Button {
                        objectName: "selectAllButton"
                        text: "Select All"
                        // Asked again when the ticks move and when the
                        // library is read again (playlistNames' signal),
                        // which can change what can be ticked.
                        enabled: root.ticksEnabled && root.controller.tickRevision >= 0
                            && root.controller.playlistNames !== undefined
                            && root.controller.tickablePlaylistTracks(root.controller.shownPlaylist) > 0
                            && root.controller.playlistCheckState(root.controller.shownPlaylist) !== Qt.Checked
                        ToolTip.visible: hovered
                        ToolTip.text: root.controller.shownPlaylist.length > 0
                            ? "Tick every track of this playlist" : "Tick every track"
                        onClicked: root.controller.setPlaylistTicked(root.controller.shownPlaylist, true)
                    }
                    Button {
                        objectName: "selectNoneButton"
                        text: "Select None"
                        enabled: root.ticksEnabled && root.controller.tickedCount > 0
                        ToolTip.visible: hovered
                        ToolTip.text: "Untick every track, in every playlist"
                        onClicked: root.controller.setPlaylistTicked("", false)
                    }
                    Button {
                        objectName: "stageButton"
                        highlighted: !root.staged
                        text: root.staged ? "Unstage" : "Stage Selected"
                        enabled: root.staged ? !root.controller.writing
                            : root.controller.canStage && !root.stickReadOnly && !editHost.blockedByOtherPage
                        ToolTip.visible: hovered
                        ToolTip.text: root.staged ? "Take these changes back out of the changes to save"
                            : "Stage a copy for every ticked track that would change; Save writes them"
                        onClicked: root.staged ? root.controller.unstage() : root.controller.stage()
                    }
                }

                // The way back from a copy, for the ticked tracks: two
                // secondary actions, on the summary's left line.
                RowLayout {
                    objectName: "engineAnalysisRow"
                    Layout.fillWidth: true
                    visible: root.usable && root.controller.tickedCount > 0
                    spacing: Theme.rowSpacing
                    Label {
                        objectName: "engineAnalysisLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: root.controller.engineAnalysisText
                    }
                    Button {
                        objectName: "restoreButton"
                        text: "Restore Engine's Analysis"
                        enabled: !root.staged && root.controller.canRestore && !root.stickReadOnly
                            && !editHost.blockedByOtherPage
                        ToolTip.visible: hovered
                        ToolTip.text: "Stage putting back the player's own analysis, kept on the stick when a "
                            + "copy replaced it, for every ticked track that has it; Save writes it"
                        onClicked: root.controller.stageRestore()
                    }
                    Button {
                        objectName: "letPlayerAnalyseButton"
                        text: "Let the Player Analyse Again"
                        enabled: !root.staged && root.controller.canLetPlayerAnalyse && !root.stickReadOnly
                            && !editHost.blockedByOtherPage
                        ToolTip.visible: hovered
                        ToolTip.text: "Stage marking every ticked track the player has analysed so that it "
                            + "analyses it anew when it is next loaded; Save writes it"
                        onClicked: root.controller.stageLetPlayerAnalyse()
                    }
                }
            }
        }
    }

    ColumnLayout {
        id: body
        objectName: "pageBody"
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        Label {
            objectName: "errorLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: root.controller.errorMessage
        }
        Label {
            objectName: "engineTooOldLabel"
            Layout.fillWidth: true
            visible: root.controller.engineTooOld
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: "This Engine library was made by Engine 1.x, whose beat grids and waveforms Seabass does not write. "
                + "Once Engine DJ has updated the library, they can be copied here."
        }
        Label {
            objectName: "readOnlyLabel"
            Layout.fillWidth: true
            visible: root.usable && root.stickReadOnly
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: "This stick is mounted read-only: its filesystem needs checking, which Library Health can do. "
                + "Until then nothing can be copied onto it."
        }
        Label {
            objectName: "ledgerWarningLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: root.controller.ledgerWarning
        }
        // Tracks listed that have no Engine row: sync first.
        Label {
            id: notInEngineLabel
            objectName: "notInEngineLabel"
            Layout.fillWidth: true
            visible: root.usable && root.controller.notInEngineShown > 0
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            textFormat: Text.StyledText
            linkColor: Theme.accent
            text: root.controller.notInEngineText
            onLinkActivated: editHost.requestLeave(() => root.rekordboxExportSyncRequested(
                root.stickLabel, root.rekordboxPath, root.enginePath))
            HoverHandler {
                cursorShape: notInEngineLabel.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }
        Label {
            objectName: "nothingMatchedLabel"
            Layout.fillWidth: true
            visible: root.usable && root.controller.matchedCount === 0
            wrapMode: Text.WordWrap
            text: "No track is in both libraries, so there is nothing to copy."
        }
        Label {
            objectName: "noPlaylistsLabel"
            Layout.fillWidth: true
            visible: root.usable && root.controller.playlistNames.length === 0
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "This rekordbox export has no playlists. All tracks lists every track in it."
        }

        // The one bar of a preview batch: the tracks being checked.
        ProgressReport {
            objectName: "previewProgress"
            Layout.fillWidth: true
            visible: root.controller.previewBusy && root.controller.previewTotal > 0
            unitsDone: root.controller.previewCurrent
            unitsTotal: root.controller.previewTotal
            unitName: "tracks"
            currentItem: "Checking what copying would change"
        }

        RowLayout {
            objectName: "panes"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.usable
            spacing: Theme.rowSpacing

            PlaylistListView {
                id: playlistList
                objectName: "playlistList"
                // Narrower on a narrow page, down to a floor that still
                // shows a playlist's name: the tracks beside it need the
                // room more, their state marks most of all (on macOS, where
                // Theme.scaled() is 1.3 and the small fonts are not small,
                // the full width left the title column 86 px at 720).
                Layout.preferredWidth: Math.min(Theme.scaled(240), Math.max(Theme.scaled(160), body.width * 0.3))
                Layout.fillHeight: true
                scanController: root.controller
                summariesEnabled: false
                shownPlaylist: root.controller.shownPlaylist
                selectedIndex: root.controller.shownPlaylist.length === 0 ? 0
                    : root.controller.playlistNames.indexOf(root.controller.shownPlaylist) + 1
                keyNotation: root.appSettingsController ? root.appSettingsController.keyNotation : "camelot"
                checkable: true
                checksEnabled: root.ticksEnabled
                checkRevision: root.controller.tickRevision
                checkStateOf: name => root.controller.playlistCheckState(name)
                checkEnabledFor: name => root.controller.tickablePlaylistTracks(name) > 0
                onPlaylistPicked: (index, name) => root.controller.shownPlaylist = index === 0 ? "" : name
                onPlaylistCheckToggled: (name, checked) => root.controller.setPlaylistTicked(name, checked)
            }

            ListView {
                id: trackList
                objectName: "trackList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: root.controller.tracks
                bottomMargin: editHost.saveClearance
                spacing: 0
                interactive: contentHeight > height
                ScrollBar.vertical: BigScrollBar {}

                delegate: MetadataTrackDelegate {
                    id: trackRow
                    // The roles by name would shadow the delegate's own
                    // title and artist, so the row reads them off `model`.
                    required property var model
                    readonly property string rekordboxId: trackRow.model.rekordboxId
                    readonly property bool matched: trackRow.model.matched
                    readonly property bool pending: trackRow.model.pending
                    objectName: "trackRow_" + rekordboxId
                    // BigScrollBar overlays the list: the row stops short of it.
                    width: trackList.width - Theme.scaled(10)
                    title: trackRow.model.title
                    artist: trackRow.model.artist
                    artworkUrl: trackRow.model.artworkUrl.length > 0 ? trackRow.model.artworkUrl
                                                                     : trackRow.model.fallbackArtworkUrl
                    selectable: trackRow.matched && root.ticksEnabled
                    reserveSelectionSpace: true
                    selected: trackRow.model.ticked
                    selectTooltip: trackRow.matched ? "Copy this track's beat grid and waveform"
                        : "Not in the Engine library yet: sync the libraries first"
                    note: trackRow.model.note
                    noteColor: root.toneColor(trackRow.model.noteTone)
                    onSelectionToggled: selected => root.controller.setTicked(trackRow.rekordboxId, selected)

                    // rekordbox's analysis of the track, for its small
                    // waveform: asked for while the row is built, let go
                    // when it goes, read off the GUI thread (see
                    // requestRowAnalysis()). `analysisId` is the track
                    // `analysis` is of: an answer for any other track is
                    // never taken.
                    property var analysis: ({ready: false})
                    property string analysisId: ""
                    property string askedFor: ""
                    function takeAnalysis() {
                        trackRow.analysisId = trackRow.rekordboxId;
                        trackRow.analysis = root.controller.rowAnalysis(trackRow.rekordboxId);
                    }
                    function ask() {
                        if (trackRow.askedFor !== trackRow.rekordboxId) {
                            if (trackRow.askedFor.length > 0) {
                                root.controller.releaseRowAnalysis(trackRow.askedFor);
                            }
                            trackRow.askedFor = trackRow.rekordboxId;
                        }
                        root.controller.requestRowAnalysis(trackRow.rekordboxId);
                        trackRow.takeAnalysis();
                    }
                    onRekordboxIdChanged: {
                        trackRow.ask();
                        trackRow.askEngine();
                    }
                    // Built and holding what was already read: from now on
                    // an answer arriving blends in (rowWaveform).
                    property bool built: false
                    Component.onCompleted: {
                        trackRow.ask();
                        trackRow.askEngine();
                        trackRow.built = true;
                    }
                    Component.onDestruction: {
                        if (trackRow.askedFor.length > 0) {
                            root.controller.releaseRowAnalysis(trackRow.askedFor);
                        }
                        if (trackRow.engineAskedFor.length > 0) {
                            root.controller.releaseEngineAnalysis(trackRow.engineAskedFor);
                        }
                    }
                    Connections {
                        target: root.controller
                        function onRowAnalysisReady(id) {
                            if (id === trackRow.rekordboxId) {
                                trackRow.takeAnalysis();
                            }
                        }
                        function onRowAnalysesStale() { trackRow.ask(); }
                    }

                    // Opened by a click on the row: rekordbox's analysis
                    // over the Engine library's, on one time axis, each
                    // with its grid, and what differs between the grids in
                    // words. One row open at a time. Engine's side is read
                    // only for the open row (requestEngineAnalysis()).
                    expanded: root.openTrackId.length > 0 && root.openTrackId === trackRow.rekordboxId
                    onExpandToggled: root.openTrackId = trackRow.expanded ? "" : trackRow.rekordboxId
                    property var engineSide: ({ready: false})
                    property string engineAskedFor: ""
                    function askEngine() {
                        const want = trackRow.expanded ? trackRow.rekordboxId : "";
                        if (trackRow.engineAskedFor !== want && trackRow.engineAskedFor.length > 0) {
                            root.controller.releaseEngineAnalysis(trackRow.engineAskedFor);
                        }
                        trackRow.engineAskedFor = want;
                        if (want.length > 0) {
                            root.controller.requestEngineAnalysis(want);
                        }
                        trackRow.engineSide = want.length > 0 ? root.controller.engineAnalysis(want) : {ready: false};
                    }
                    onExpandedChanged: trackRow.askEngine()
                    // A delegate built for the open row (scrolled back into
                    // view, or the list rebuilt after a read) starts
                    // expanded, and onExpandedChanged does not run for the
                    // value it starts with; one reused for another track
                    // asks for that one's.
                    // (askEngine() is called from the row's own
                    // onRekordboxIdChanged and Component.onCompleted above,
                    // and let go in its Component.onDestruction.)
                    Connections {
                        target: root.controller
                        function onEngineAnalysisReady(id) {
                            if (id === trackRow.engineAskedFor) {
                                trackRow.engineSide = root.controller.engineAnalysis(id);
                            }
                        }
                        function onRowAnalysesStale() {
                            if (trackRow.expanded) {
                                trackRow.askEngine();
                            }
                        }
                    }
                    // rekordbox's grid in figures, on Engine's timeline: once
                    // both sides are in (the shift comes with Engine's).
                    readonly property var rekordboxGrid: trackRow.analysis.ready === true
                        && trackRow.engineSide.ready === true
                        ? root.controller.rowGridFacts(trackRow.analysisId) : ({ready: false})

                    expandedItems: [
                        ColumnLayout {
                            id: comparison
                            objectName: "analysisComparison"
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing
                            readonly property real laneHeight: Math.round(Theme.scaled(56))

                            Label {
                                objectName: "comparisonSummary"
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                color: Theme.text
                                text: root.comparisonText(trackRow.matched, trackRow.rekordboxGrid, trackRow.engineSide)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: "rekordbox" + root.gridFactsText(trackRow.rekordboxGrid)
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                            WaveformView {
                                id: rekordboxLane
                                objectName: "comparisonRekordbox"
                                Layout.fillWidth: true
                                Layout.preferredHeight: comparison.laneHeight
                                gridStyle: "lines"
                                beatsSelected: true
                                // On Engine's timeline, as Engine's lane: an
                                // MP3's waveform and grid moved by its lead-in
                                // (rowGridFacts().shiftMs), so a grid copied
                                // across lines up with the copy.
                                readonly property bool ready: trackRow.rekordboxGrid.ready === true
                                readonly property int columnCount: rekordboxLane.ready && trackRow.analysis.columns
                                    ? trackRow.analysis.columns.byteLength / 3 : 0
                                columnBytes: rekordboxLane.ready ? trackRow.analysis.columns : null
                                trackDurationMs: rekordboxLane.ready ? trackRow.analysis.durationMs : 0
                                columnMs: rekordboxLane.columnCount > 0
                                    ? trackRow.analysis.durationMs / rekordboxLane.columnCount : 0
                                columnStartMs: rekordboxLane.ready ? trackRow.rekordboxGrid.shiftMs || 0 : 0
                                beatData: rekordboxLane.ready && rekordboxLane.width > 0
                                    ? root.controller.comparisonBeatTicks(trackRow.analysisId, rekordboxLane.width) : []
                                // Drawn once both sides are in (the shift comes
                                // with Engine's): until then it is reading,
                                // not missing.
                                missingText: trackRow.analysis.ready !== true || trackRow.engineSide.ready !== true
                                    ? "Reading..." : "rekordbox has no analysis of this track"
                                // A click opens rekordbox's waveform zoomed,
                                // as the row's small one does.
                                readonly property bool opensZoom: rekordboxLane.ready && rekordboxLane.hasWaveform
                                    && trackRow.analysis.playable === true && rekordboxLane.trackDurationMs > 0
                                    && root.playbackController !== null
                                pointerShape: rekordboxLane.opensZoom ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onPositionClicked: {
                                    if (rekordboxLane.opensZoom) {
                                        root.openZoom(trackRow.analysisId, "rekordbox");
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: "Engine" + root.gridFactsText(trackRow.engineSide)
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                            WaveformView {
                                id: engineLane
                                objectName: "comparisonEngine"
                                Layout.fillWidth: true
                                Layout.preferredHeight: comparison.laneHeight
                                gridStyle: "lines"
                                beatsSelected: true
                                // Drawn as Engine draws it, as the zoomed
                                // popup does: Engine's colours, the bands as
                                // one outline each, at full strength. The
                                // grid's lines are the same in every scheme.
                                colorScheme: "engine"
                                unplayedOpacity: 1
                                readonly property bool ready: trackRow.engineSide.ready === true
                                columnBytes: engineLane.ready ? trackRow.engineSide.columns : null
                                trackDurationMs: engineLane.ready ? trackRow.engineSide.durationMs : 0
                                beatData: engineLane.ready && engineLane.width > 0
                                    ? root.controller.engineBeatTicks(trackRow.engineAskedFor, engineLane.width) : []
                                missingText: !engineLane.ready ? "Reading..."
                                    : trackRow.engineSide.readFailed === true ? "Engine's analysis could not be read"
                                    : trackRow.engineSide.hasEngineTrack !== true ? "Not in the Engine library"
                                    : "Engine has no waveform for this track"
                                // A click opens Engine's own copy of the track
                                // zoomed: its waveform, grid and cues.
                                readonly property var zoomTrack: engineLane.ready && engineLane.hasWaveform
                                    ? root.controller.engineZoomTrack(trackRow.engineAskedFor) : ({})
                                readonly property bool opensZoom: trackRow.engineSide.playable === true
                                    && engineLane.zoomTrack.filePath !== undefined && engineLane.trackDurationMs > 0
                                    && root.playbackController !== null
                                pointerShape: engineLane.opensZoom ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onPositionClicked: {
                                    if (engineLane.opensZoom) {
                                        root.openZoom(trackRow.engineAskedFor, "engine");
                                    }
                                }
                            }
                        }
                    ]

                    // Two lines, the same on every row: the grid's state over
                    // the waveform's, each badge after a caption column as
                    // wide as the wider caption, so the badges line up from
                    // row to row whatever they say. A row with no state yet
                    // (Checking) or none at all (not in Engine) keeps them,
                    // and the note under them is always one line.
                    noteOnOneLine: true
                    detailItems: [
                        Column {
                            id: stateMarks
                            objectName: "stateMarks"
                            // The title column's width: a badge with more to
                            // say than that elides, never reaching the tempo.
                            // The Flow fills that column and its width does not
                            // follow the badges back: one width change of the
                            // page moves it once (test_stateMarksSettleInOnePass).
                            readonly property real room: parent ? parent.width : 0
                            spacing: Theme.tightSpacing / 2
                            StateMark {
                                objectName: "gridMark"
                                maxWidth: stateMarks.room
                                caption: "Beat grid"
                                pending: trackRow.pending
                                value: trackRow.model.gridState
                                tone: trackRow.model.gridTone
                                tip: trackRow.model.gridTip
                            }
                            StateMark {
                                objectName: "waveformMark"
                                maxWidth: stateMarks.room
                                caption: "Waveform"
                                pending: trackRow.pending
                                value: trackRow.model.waveformState
                                tone: trackRow.model.waveformTone
                                tip: trackRow.model.waveformTip
                            }
                        }
                    ]
                    actionItems: [
                        // rekordbox's waveform and beat grid, what copying
                        // would put on the player: as wide on every row
                        // whether it has one or not, so the columns beside
                        // it line up, and lower than the text beside it, so
                        // the row is no taller. A click opens it zoomed in,
                        // the track loaded into the player, where there is
                        // a file to play.
                        Item {
                            objectName: "rowWaveformSlot"
                            visible: root.rowWaveformWidth > 0
                            Layout.preferredWidth: root.rowWaveformWidth
                            Layout.preferredHeight: root.rowWaveformHeight
                            Layout.alignment: Qt.AlignVCenter
                            WaveformView {
                                id: rowWaveform
                                objectName: "rowSourceWaveform"
                                anchors.fill: parent
                                readonly property string trackId: trackRow.analysisId
                                readonly property bool shown: trackRow.analysis.ready === true && rowWaveform.hasWaveform
                                visible: rowWaveform.shown
                                // Blended in when its read comes back, so a
                                // page of rows filling in does not flicker.
                                // A row built with its answer already there
                                // (scrolled back to) shows it at once: the
                                // fade is armed only once the row is built
                                // and has taken what was there.
                                opacity: rowWaveform.shown ? 1 : 0
                                Behavior on opacity {
                                    enabled: trackRow.built
                                    NumberAnimation {
                                        duration: Theme.arrivalTransitionDuration
                                        easing.type: Easing.OutCubic
                                    }
                                }
                                gridStyle: "lines"
                                columnBytes: trackRow.analysis.ready === true ? trackRow.analysis.columns : null
                                trackDurationMs: trackRow.analysis.ready === true ? trackRow.analysis.durationMs : 0
                                beatsSelected: true
                                beatData: trackRow.analysis.ready === true
                                    ? root.controller.rowBeatTicks(trackRow.analysisId, rowWaveform.width) : []
                                readonly property bool opensZoom: rowWaveform.visible
                                    && trackRow.analysis.playable === true && rowWaveform.trackDurationMs > 0
                                    && root.playbackController !== null
                                pointerShape: rowWaveform.opensZoom ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onPositionClicked: {
                                    if (rowWaveform.opensZoom) {
                                        root.openZoom(trackRow.analysisId, "both");
                                    }
                                }
                            }
                        },
                        Label {
                            objectName: "bpmLabel"
                            visible: trackRow.model.bpm > 0
                            text: root.formatBpm(trackRow.model.bpm) + " BPM"
                            color: Theme.textMuted
                            font.family: Theme.dataFamily
                            font.pointSize: Theme.fontSmall
                        },
                        KeyBadge {
                            objectName: "keyBadge"
                            visible: trackRow.model.keyName.length > 0
                            keyName: trackRow.model.keyName
                            notation: root.appSettingsController ? root.appSettingsController.keyNotation : "camelot"
                        }
                    ]
                }
            }
        }
    }

    // A part's state on a row: what it is, and the badge saying what
    // copying does to it. As tall as a badge even without one, and its
    // caption as wide as the wider of the two, so the badge's x is the same
    // on every row.
    component StateMark: Row {
        id: mark
        property string caption: ""
        property bool pending: false
        property string value: ""
        property string tone: ""
        property string tip: ""
        // The most the mark may take; its badge elides to stay inside it.
        property real maxWidth: Infinity
        readonly property bool shown: mark.pending || mark.value.length > 0
        height: badgeMetric.implicitHeight
        spacing: Theme.tightSpacing
        Label {
            objectName: "stateCaption"
            anchors.verticalCenter: parent.verticalCenter
            width: root.stateCaptionWidth
            visible: mark.shown
            text: mark.caption
            color: Theme.textMuted
            font.pointSize: Theme.fontTiny
        }
        Label {
            objectName: "pendingLabel"
            anchors.verticalCenter: parent.verticalCenter
            visible: mark.pending
            text: "Checking"
            color: Theme.textMuted
            font.pointSize: Theme.fontTiny
            font.italic: true
        }
        StatusBadge {
            objectName: "stateBadge"
            anchors.verticalCenter: parent.verticalCenter
            visible: !mark.pending && mark.value.length > 0
            width: Math.min(implicitWidth, Math.max(0, mark.maxWidth - root.stateCaptionWidth - mark.spacing))
            label: mark.value
            badgeColor: root.toneColor(mark.tone)
            // Elided, the badge's words are said in full on hover.
            tooltipText: truncated ? mark.value + (mark.tip.length > 0 ? "\n" + mark.tip : "") : mark.tip
        }
    }
    // What StateMark measures itself by: never shown.
    Label {
        id: gridCaptionMetric
        visible: false
        text: "Beat grid"
        font.pointSize: Theme.fontTiny
    }
    Label {
        id: waveformCaptionMetric
        visible: false
        text: "Waveform"
        font.pointSize: Theme.fontTiny
    }
    StatusBadge {
        id: badgeMetric
        visible: false
        label: "New"
        badgeColor: Theme.good
    }
    readonly property real stateCaptionWidth: Math.ceil(Math.max(gridCaptionMetric.implicitWidth,
                                                                 waveformCaptionMetric.implicitWidth))

    // A row's waveform: up to rowWaveformMaximumWidth, narrower on a
    // narrow page and none at all where the title column would have too
    // little left (the badges under the title need it more). Taken from
    // the page's width, not the list's: it is then known before the rows
    // are laid out again, and the title column beside it moves once per
    // change of width (see StateMark's room). As tall as the expanded
    // waveform of a metadata row, below the height of the row's text.
    readonly property real rowWaveformMaximumWidth: Theme.scaled(160)
    readonly property real rowWaveformWidth: {
        const room = Math.round(Math.min(root.rowWaveformMaximumWidth, (root.width - Theme.scaled(640)) / 2));
        return room >= Theme.scaled(64) ? room : 0;
    }
    readonly property real rowWaveformHeight: Math.round(Theme.scaled(40))

    // The row opened for comparing, by rekordbox id; empty for none.
    property string openTrackId: ""

    // A grid's figures after a lane's name: its tempo and where its first
    // downbeat is.
    function gridFactsText(facts) {
        if (!facts || facts.ready !== true) {
            return "";
        }
        if (facts.hasGrid !== true) {
            return ", no beat grid";
        }
        return ", " + facts.bpm.toFixed(2) + " BPM, first downbeat at "
            + (facts.firstDownbeatMs / 1000).toFixed(3) + " s";
    }

    // What differs between the two grids, in one sentence. The offset is
    // taken within one beat of rekordbox's grid (grids a whole number of
    // beats apart put the same lines in the same places); a downbeat on
    // another beat of the bar is said separately.
    function comparisonText(matched, rekordbox, engine) {
        if (!matched) {
            return "Not in the Engine library yet: there is nothing to compare with.";
        }
        if (!rekordbox || rekordbox.ready !== true || !engine || engine.ready !== true) {
            return "Reading both analyses...";
        }
        if (engine.readFailed === true) {
            return "Engine's analysis of this track could not be read. Is the stick still there?";
        }
        if (engine.hasEngineTrack !== true) {
            return "Not in the Engine library yet: there is nothing to compare with.";
        }
        if (rekordbox.hasGrid !== true && engine.hasGrid !== true) {
            return "Neither library has a beat grid for this track.";
        }
        if (rekordbox.hasGrid !== true) {
            return "rekordbox has no beat grid for this track; Engine has one.";
        }
        if (engine.hasGrid !== true) {
            return "Engine has no beat grid for this track yet.";
        }
        const parts = [];
        const tempo = engine.bpm - rekordbox.bpm;
        if (Math.abs(tempo) >= 0.005) {
            parts.push("Engine's tempo is " + Math.abs(tempo).toFixed(2) + " BPM "
                       + (tempo > 0 ? "faster" : "slower"));
        }
        if (engine.shiftUnknown === true) {
            // Without the lead-in the two timelines cannot be lined up:
            // the tempo still compares, the places of the beats do not.
            parts.push("where the beats fall is not compared: this MP3's lead-in could not be read");
            const said = parts.join("; ");
            return said.charAt(0).toUpperCase() + said.slice(1) + ".";
        }
        const beatMs = 60000 / rekordbox.bpm;
        const apart = engine.firstDownbeatMs - rekordbox.firstDownbeatMs;
        let phase = apart - Math.round(apart / beatMs) * beatMs;
        if (Math.abs(phase) >= 1) {
            parts.push("Engine's beats fall " + Math.round(Math.abs(phase)) + " ms "
                       + (phase > 0 ? "later" : "earlier"));
        }
        const beatsApart = Math.round(apart / beatMs);
        if (((beatsApart % 4) + 4) % 4 !== 0) {
            parts.push("Engine's bars start on another beat");
        }
        if (parts.length === 0) {
            return "The two beat grids agree.";
        }
        return parts.join("; ") + ".";
    }

    // One zoomed waveform for the page, opened from a row's waveform.
    // It has both sides of the track (#131): opened from a comparison
    // lane it shows that side, from the row's own waveform both, one
    // above the other; its box switches between them.
    WaveformZoomPopup {
        id: zoomPopup
        playbackController: root.playbackController
        settings: root.appSettingsController
        sides: root.zoomSides
        // Closed: a read of its detail under way stops.
        onClosed: {
            root.controller.releaseZoomDetail(root.zoomId);
            root.zoomId = "";
            zoomPopup.wantBoth = false;
        }
        onWantBothChanged: root.askZoomDetail()
        onLoadedSideChanged: root.askZoomDetail()
    }
    // The track the popup shows, by rekordbox id; empty while it is shut.
    property string zoomId: ""
    // Counts up as either side's read of that track comes in.
    property int zoomRevision: 0
    // Both sides as the popup takes them (WaveformZoomPopup.sides), on
    // Engine's timeline as the comparison lanes are: rekordbox's moved by
    // its lead-in (rowGridFacts().shiftMs). Each at the detail its own
    // zoomed view draws once read (zoomDetail()), the rows' overview until
    // then; the popup draws the side the player has from the player's.
    readonly property var zoomSides: {
        root.zoomRevision;
        const id = root.zoomId;
        if (id.length === 0) {
            return [];
        }
        const c = root.controller;
        const rekordbox = c.rowAnalysis(id);
        const engine = c.engineAnalysis(id);
        const facts = c.rowGridFacts(id);
        const engineTrack = c.engineZoomTrack(id);
        const detail = c.zoomDetail(id);
        const detailed = (columns) => detail.ready === true && !!columns && columns.byteLength >= 3;
        const columnsOf = (side) => side.ready === true && side.columns && side.columns.byteLength >= 3
            ? side.columns : null;
        const spread = (side) => {
            const columns = columnsOf(side);
            return columns && side.durationMs > 0 ? side.durationMs / (columns.byteLength / 3) : 0;
        };
        return [{
            label: "rekordbox",
            scheme: "vivid",
            track: c.zoomTrack(id),
            available: rekordbox.ready === true && rekordbox.playable === true,
            detailed: detailed(detail.rekordboxColumns),
            columns: detailed(detail.rekordboxColumns) ? detail.rekordboxColumns : columnsOf(rekordbox),
            msPerColumn: detailed(detail.rekordboxColumns)
                ? (detail.rekordboxMsPerColumn > 0 ? detail.rekordboxMsPerColumn
                   : rekordbox.durationMs > 0 ? rekordbox.durationMs / (detail.rekordboxColumns.byteLength / 3) : 0)
                : spread(rekordbox),
            startMs: facts.ready === true ? facts.shiftMs || 0 : 0,
            ticksAt: (durationMs, widthPx) => c.comparisonBeatTicks(id, widthPx),
        }, {
            label: "Engine",
            scheme: "engine",
            track: engineTrack,
            available: engine.ready === true && engine.hasEngineTrack === true && engine.playable === true
                && engineTrack.filePath !== undefined,
            detailed: detailed(detail.engineColumns),
            columns: detailed(detail.engineColumns) ? detail.engineColumns : columnsOf(engine),
            msPerColumn: detailed(detail.engineColumns) ? detail.engineMsPerColumn : spread(engine),
            startMs: 0,
            ticksAt: (durationMs, widthPx) => c.engineBeatTicks(id, widthPx),
        }];
    }
    // What was read for the popup may be out of date: asked for again.
    function askZoomAgain() {
        if (root.zoomId.length > 0) {
            root.controller.requestRowAnalysis(root.zoomId);
            root.controller.requestEngineAnalysis(root.zoomId);
            root.askZoomDetail();
            ++root.zoomRevision;
        }
    }
    // Both's detail, for the side the player has not loaded: the loaded
    // one is drawn from the player's own (WaveformZoomPopup.lanes), and an
    // Engine file is not decoded twice. With Both left for one side, or
    // the player gone on to another track, a read under way stops and
    // none starts for a track no longer shown.
    function askZoomDetail() {
        if (root.zoomId.length === 0) {
            return;
        }
        if (zoomPopup.wantBoth && zoomPopup.loadedSide >= 0) {
            root.controller.requestZoomDetail(root.zoomId, zoomPopup.loadedSide !== 0, zoomPopup.loadedSide !== 1);
        } else {
            root.controller.releaseZoomDetail(root.zoomId);
        }
    }
    // Opens the popup on a track: "rekordbox" or "engine" for that side's
    // copy, "both" for the two stacked (rekordbox's copy in the player).
    // Both sides are asked for, so the box can offer each.
    function openZoom(id, view) {
        if (root.zoomId !== id) {
            root.controller.requestRowAnalysis(id);
            root.controller.requestEngineAnalysis(id);
            root.zoomId = id;
        }
        ++root.zoomRevision;
        // Loaded first, so Both asks only for the side the player has not.
        zoomPopup.openFor(view === "engine" ? root.controller.engineZoomTrack(id) : root.controller.zoomTrack(id));
        zoomPopup.wantBoth = view === "both";
        root.askZoomDetail();
    }
    Connections {
        target: root.controller
        function onRowAnalysisReady(id) {
            if (id === root.zoomId) {
                ++root.zoomRevision;
            }
        }
        function onEngineAnalysisReady(id) {
            if (id === root.zoomId) {
                ++root.zoomRevision;
            }
        }
        function onZoomDetailReady(id) {
            if (id === root.zoomId) {
                ++root.zoomRevision;
            }
        }
        function onRowAnalysesStale() {
            root.askZoomAgain();
        }
        function onLibraryChanged() {
            root.askZoomAgain();
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: root.controller.loading
        current: root.controller.loadCurrent
        total: root.controller.loadTotal
        label: root.controller.loadLabel.length > 0 ? root.controller.loadLabel : "Reading the libraries..."
        cancellable: true
        onCancelRequested: root.controller.cancelLoad()
    }
}
