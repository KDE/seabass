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
// and whether over grids set on the player), what the ticked tracks would
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
                    text: "Experimental: this replaces analysis a Denon player made with one converted from "
                        + "rekordbox, and the conversion has not yet been confirmed on a player."
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
                            : "rekordbox's waveform replaces the one the player drew"
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
