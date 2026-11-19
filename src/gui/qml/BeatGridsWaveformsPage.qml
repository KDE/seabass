// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

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
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    property var appSettingsController: null
    property var playbackController: null

    // Overridable so a test can hand in its own; the app never sets it.
    property var controller: realController
    // The stick is mounted read-only (its filesystem needs checking).
    // Overridable for a test, which has no read-only stick to hand.
    property bool stickReadOnly: root.controller.stickReadOnly

    BeatGridsController {
        id: realController
        objectName: "beatGridsController"
    }

    Component.onCompleted: root.controller.load(root.rekordboxPath, root.enginePath)

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
    // An Undo Last Save is said in the summary's own words.
    Connections {
        target: editHost.session
        ignoreUnknownSignals: true
        function onSaveFinished(summary) {
            if (summary && summary.unit === "undo steps" && !(summary.error && summary.error.length > 0)
                    && summary.written > 0) {
                savedToast.show(summary.written + " " + summary.unit + " " + summary.verb);
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

                // What to copy. The boxes themselves on the page's left line.
                Flow {
                    objectName: "optionsRow"
                    Layout.fillWidth: true
                    visible: root.usable
                    spacing: Theme.rowSpacing
                    SeabassCheckBox {
                        objectName: "gridsCheckBox"
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
                        ToolTip.text: root.staged ? "Take these copies back out of the changes to save"
                            : "Stage a copy for every ticked track that would change; Save writes them"
                        onClicked: root.staged ? root.controller.unstage() : root.controller.stage()
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
                Layout.preferredWidth: Theme.scaled(240)
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
                        : "Not in the Engine library: nothing to copy to"
                    note: trackRow.model.note
                    noteColor: root.toneColor(trackRow.model.noteTone)
                    onSelectionToggled: selected => root.controller.setTicked(trackRow.rekordboxId, selected)

                    detailItems: [
                        Label {
                            objectName: "pendingLabel"
                            visible: trackRow.pending
                            text: "Checking"
                            color: Theme.textMuted
                            font.pointSize: Theme.fontTiny
                            font.italic: true
                        },
                        StateMark {
                            objectName: "gridMark"
                            visible: !trackRow.pending && trackRow.model.gridState.length > 0
                            caption: "Beat grid"
                            value: trackRow.model.gridState
                            tone: trackRow.model.gridTone
                            tip: trackRow.model.gridTip
                        },
                        StateMark {
                            objectName: "waveformMark"
                            visible: !trackRow.pending && trackRow.model.waveformState.length > 0
                            caption: "Waveform"
                            value: trackRow.model.waveformState
                            tone: trackRow.model.waveformTone
                            tip: trackRow.model.waveformTip
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
    // copying does to it.
    component StateMark: Row {
        id: mark
        property string caption: ""
        property string value: ""
        property string tone: ""
        property string tip: ""
        spacing: Theme.tightSpacing / 2
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: mark.caption
            color: Theme.textMuted
            font.pointSize: Theme.fontTiny
        }
        StatusBadge {
            objectName: "stateBadge"
            anchors.verticalCenter: parent.verticalCenter
            label: mark.value
            badgeColor: root.toneColor(mark.tone)
            tooltipText: mark.tip
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
