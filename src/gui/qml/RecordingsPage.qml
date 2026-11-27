// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui
import "RecordingSave.js" as RecordingSave

// Clean Up Recordings: the sets DJ hardware recorded onto this stick
// (Engine OS into Sessions/, Pioneer and AlphaTheta decks into their REC
// folders), listed with a checkbox each, and deleted. Nothing is copied
// first: they are usually long since saved elsewhere. No library refers
// to these files; see RecordingsController for why no full stick backup
// is taken first.
//
// Save as FLAC (experimental): a recording becomes a FLAC with tags and a
// cover in a folder on this computer, through KeepRecordingForm and a
// KeepRecordingController, beside the page in KeepRecordingProgress (Browse
// opens the same form and shows the same strip for its Own Recordings).
// The recording on the stick stays as it is. The controller lives with the
// window: the work goes on when the page is left, and the strip shows it
// again when the page is opened again. The controller is the app's one,
// for every stick: the page asks it about this stick only (its root,
// controller.stickRoot), and says in one line when a recording of another
// stick is being saved, so that a job asked for here is seen to wait. The
// strip stays while a job of this stick runs or waits, even when
// Experimental is switched off meanwhile, and lists what ended until it is
// closed. The delete confirmation names every ticked recording a save was
// asked for and did not end saved.
//
// With Experimental off the page is the released one: no Save as FLAC
// button, and the rows say nothing of a WAV's header (the controller does
// not read it) and give the length rounded to the second.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    // A RecordingsController; a QtObject with the same shape in the tests.
    required property var controller
    // The app's player, which plays a recording as a plain file.
    required property var playbackController
    // A KeepRecordingController; a QtObject with the same shape in the
    // tests. Null: nothing is offered to be kept.
    property var keepController: null
    // Save as FLAC is experimental (docs/experimental-features.md).
    property bool keepEnabled: false
    // A recording to show: scrolled to and marked once listed (Browse's
    // "Show on the Recordings page"); "" for none.
    property string markedPath: ""

    readonly property bool keepOffered: root.keepEnabled && keepForm.available
    readonly property int keepRevision: root.keepController !== null ? root.keepController.revision : 0
    // The stick's root, as the keep controller is asked about it.
    readonly property string stickRoot: root.controller.stickRoot ?? ""
    // A recording of this stick is being saved or waits to be: nothing is
    // deleted meanwhile, so no recording goes from under its job.
    readonly property bool keepWorking: keepProgress.keepWorking
    // The job in hand is another stick's.
    readonly property bool keepRunsElsewhere: root.keepController !== null
        && (root.keepRevision, root.keepController.runsElsewhere(root.stickRoot)) === true

    readonly property bool working: root.controller.working === true
    readonly property int recordingCount: root.controller.recordingCount
    readonly property int selectedCount: root.controller.selectedCount

    function sourceLabel(source) {
        if (source === "engine") return "Engine OS";
        if (source === "pioneer") return "Pioneer";
        if (source === "alphatheta") return "AlphaTheta";
        return source;
    }
    function plural(n, one, many) {
        return n + " " + (n === 1 ? one : many);
    }
    // Sessions/ is Engine OS's, the REC folders a Pioneer or AlphaTheta
    // deck's: the play key takes that deck's form.
    function formatFor(source) {
        return source === "engine" ? "engine" : "rekordbox";
    }
    function playRecording(row) {
        const format = root.formatFor(row.source);
        root.playbackController.loadFile(format, format === "engine" ? root.enginePath : root.rekordboxPath,
            row.path, row.fileName, row.folderName + " (" + root.sourceLabel(row.source) + ")");
    }
    // A recording that is playing cannot be deleted on Windows and keeps
    // its space on every system until the player lets go of it, so the
    // player lets go first when it holds one of those about to go.
    function releasePlayerFromDeletedRecordings() {
        const player = root.playbackController;
        if (!player || !player.hasTrack || !player.currentSourceId.startsWith("recording:")) {
            return;
        }
        const playing = player.currentSourceId.substring("recording:".length);
        if (root.controller.selectedPaths().indexOf(playing) >= 0) {
            player.stop();
        }
    }

    readonly property string totalLine: {
        if (!root.controller.listed) return "";
        let line = Theme.humanBytes(root.controller.totalBytes) + " in "
            + root.plural(root.recordingCount, "recording", "recordings");
        if (root.controller.stickTotalBytes > 0) {
            line += ", stick has " + Theme.humanBytes(root.controller.stickFreeBytes) + " free";
        }
        return line;
    }
    readonly property string leftAloneLine: {
        const items = root.controller.leftAlone || [];
        if (items.length === 0) return "";
        const names = items.slice(0, 4).map(item => item.fileName + " in " + item.folderName);
        const more = items.length > 4 ? " and " + (items.length - 4) + " more" : "";
        return "Left alone, not recordings: " + names.join(", ") + more + ".";
    }

    Component.onCompleted: root.controller.load(root.stickLabel, root.rekordboxPath, root.enginePath)

    // The recording Browse asked to show, scrolled to once listed, and
    // only once: a listing again (after a delete, a save) leaves the view
    // where the person has it. The mark stays on its row.
    property bool markedShown: false
    function showMarked() {
        if (root.markedShown || root.markedPath.length === 0 || !root.controller.listed) {
            return;
        }
        const index = root.controller.rowOf(root.markedPath);
        if (index >= 0) {
            list.positionViewAtIndex(index, ListView.Center);
            root.markedShown = true;
        }
    }
    Connections {
        target: root.controller
        function onListingChanged() { Qt.callLater(root.showMarked); }
    }

    // Opens the form for a row of the list.
    function openKeepForm(row) {
        keepForm.openFor(row.path, row.fileName, row.modified, row.unfinished, row.heldSeconds,
                           row.declaredSeconds, row.declaredUnusable, row.missingBytes);
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else; see PendingDeletionsPage.qml.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                stickLabel: root.stickLabel
                                title: "Clean Up Recordings"
                backEnabled: !root.working
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
        }
    }

    MessageDialog {
        id: confirmDeleteDialog
        objectName: "confirmDeleteDialog"
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete " + root.plural(root.selectedCount, "Recording", "Recordings") + "?"
        headline: "This deletes " + root.plural(root.selectedCount, "recording", "recordings") + " from "
            + root.stickLabel + " and frees " + Theme.humanBytes(root.controller.selectedBytes) + "."
        detailText: "They will be gone for good."
        acceptText: "Delete"
        // What Save as FLAC made of the ticked recordings in this session, read
        // as the dialog opens: {notKept: [names], kept: [{recording,
        // fileName, folder}]}.
        property var keepNotes: ({notKept: [], kept: [], keptGone: []})
        function openWithNotes() {
            confirmDeleteDialog.keepNotes = root.keepController !== null
                ? root.keepController.keepNotesFor(root.stickRoot, root.controller.selectedPaths())
                : ({notKept: [], kept: [], keptGone: []});
            confirmDeleteDialog.open();
        }
        onAccepted: {
            root.releasePlayerFromDeletedRecordings();
            root.controller.deleteSelected();
        }

        Label {
            objectName: "notKeptLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            font.bold: true
            text: confirmDeleteDialog.keepNotes.notKept.length === 0 ? ""
                : qsTr("Not saved as FLAC, though you asked for it: %1.")
                    .arg(confirmDeleteDialog.keepNotes.notKept.join(", "))
        }
        Label {
            objectName: "keptGoneLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            font.bold: true
            text: (confirmDeleteDialog.keepNotes.keptGone ?? []).length === 0 ? ""
                : qsTr("Saved as FLAC earlier, but that file is no longer there as it was saved: %1.")
                    .arg(confirmDeleteDialog.keepNotes.keptGone
                        .map(k => qsTr("%1 as %2 in %3").arg(k.recording).arg(k.fileName).arg(k.folder)).join("; "))
        }
        Label {
            objectName: "keptNotesLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: confirmDeleteDialog.keepNotes.kept.length === 0 ? ""
                : qsTr("Saved as FLAC on this computer: %1.").arg(confirmDeleteDialog.keepNotes.kept
                    .map(k => qsTr("%1 as %2 in %3").arg(k.recording).arg(k.fileName).arg(k.folder)).join("; "))
        }
    }

    // ---- Save as FLAC: the form, Browse's too ----
    KeepRecordingForm {
        id: keepForm
        keepController: root.keepController
    }

    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: EditSessionRegistry.removeLock(lockedDialog.libraryId)
    }
    OperationSummaryDialog {
        id: summaryDialog
        objectName: "summaryDialog"
    }
    Connections {
        target: root.controller
        function onFinished(summary) { summaryDialog.show(summary); }
        function onLockRefused(holder) {
            lockedDialog.openFor(EditSessionRegistry.libraryIdForPath(
                root.enginePath.length > 0 ? root.enginePath : root.rekordboxPath), holder);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        Label {
            objectName: "introLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "Sets your players recorded onto this stick. None of your libraries uses them, so they "
                + "can go without touching a track. Only audio files in the recording folders are listed, "
                + "and nothing is copied before they are deleted."
        }
        Label {
            objectName: "errorLabel"
            visible: root.controller.errorMessage.length > 0
            text: root.controller.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "unreadableLabel"
            visible: (root.controller.unreadableFolders || []).length > 0
            text: "Could not read all of: " + (root.controller.unreadableFolders || []).join(", ")
                + ". Recordings in there may be missing from this list."
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                objectName: "totalLabel"
                text: root.totalLine
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Label {
                objectName: "selectedLabel"
                visible: root.recordingCount > 0
                text: root.selectedCount + " selected (" + Theme.humanBytes(root.controller.selectedBytes) + ")"
                color: Theme.textMuted
            }
            Button {
                objectName: "selectAllButton"
                text: "Select All"
                enabled: !root.controller.busy && root.recordingCount > 0
                onClicked: root.controller.setAllIncluded(true)
            }
            Button {
                objectName: "selectNoneButton"
                text: "Deselect All"
                enabled: !root.controller.busy && root.recordingCount > 0
                onClicked: root.controller.setAllIncluded(false)
            }
            Button {
                objectName: "deleteButton"
                text: "Delete Selected Recordings"
                enabled: !root.controller.busy && root.selectedCount > 0 && !root.keepWorking
                onClicked: confirmDeleteDialog.openWithNotes()
            }
        }

        // What deleting the ticked recordings buys, against the stick's
        // real capacity: the same bar Clean Up Duplicates shows.
        Rectangle {
            objectName: "spaceBarFrame"
            Layout.fillWidth: true
            visible: list.count > 0 && spaceBar.known
            implicitHeight: spaceBar.implicitHeight + 2 * Theme.cardPadding
            color: Theme.surface
            border.color: Theme.borderSubtle
            border.width: 1
            radius: Theme.cornerRadius

            SpaceReclaimBar {
                id: spaceBar
                objectName: "spaceBar"
                anchors.fill: parent
                anchors.margins: Theme.cardPadding
                totalBytes: root.controller.stickTotalBytes
                freeBytes: root.controller.stickFreeBytes
                reclaimBytes: root.controller.selectedBytes
                reclaimableBytes: root.controller.totalBytes
                unitPlural: "recordings"
                unitSingular: "recording"
            }
        }

        ListView {
            id: list
            objectName: "recordingsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            interactive: contentHeight > height
            clip: true
            model: root.controller.recordings
            spacing: Theme.tightSpacing
            ScrollBar.vertical: BigScrollBar {}

            delegate: ItemDelegate {
                id: row
                objectName: "recordingRow" + row.index
                width: ListView.view.width
                hoverEnabled: false
                leftPadding: 0
                rightPadding: 0

                required property int index
                required property string path
                required property string fileName
                required property string folderName
                required property string source
                required property double sizeBytes
                required property var modified
                required property double durationSeconds
                required property bool included
                required property bool unfinished
                required property double heldSeconds
                required property double declaredSeconds
                required property bool declaredUnusable
                required property double missingBytes
                required property bool holdsNoAudio

                // Browse's "Show on the Recordings page" came for this one.
                readonly property bool marked: root.markedPath.length > 0 && row.path === root.markedPath
                Rectangle {
                    objectName: "markedFrame"
                    anchors.fill: parent
                    visible: row.marked
                    color: "transparent"
                    radius: Theme.cornerRadius
                    border.width: 2
                    border.color: Theme.accent
                }

                readonly property string details: {
                    const parts = [row.folderName + " (" + root.sourceLabel(row.source) + ")"];
                    if (row.modified && !isNaN(row.modified.getTime())) {
                        parts.push(row.modified.toLocaleString(Qt.locale(), Locale.ShortFormat));
                    }
                    // What the header says is Save as FLAC's
                    // (experimental): the released page says nothing of it.
                    if (root.keepEnabled && row.holdsNoAudio) {
                        parts.push(qsTr("holds no audio"));
                        return parts.join(" · ");
                    }
                    if (root.keepEnabled && row.unfinished) {
                        const missing = RecordingSave.missingNote(row.declaredSeconds, row.heldSeconds, row.missingBytes);
                        parts.push(row.declaredUnusable
                            ? qsTr("header not completed: its length is not usable, the file holds %1")
                                .arg(RecordingSave.duration(row.heldSeconds))
                            : row.declaredSeconds >= 0 && missing.length > 0
                            ? qsTr("header not completed: it declares %1, the file holds %2 (%3)")
                                .arg(RecordingSave.duration(row.declaredSeconds)).arg(RecordingSave.duration(row.heldSeconds))
                                .arg(missing)
                            : row.declaredSeconds >= 0
                            ? qsTr("header not completed: it declares %1, the file holds %2")
                                .arg(RecordingSave.duration(row.declaredSeconds)).arg(RecordingSave.duration(row.heldSeconds))
                            : qsTr("header not completed, the file holds %1 of audio")
                                .arg(RecordingSave.duration(row.heldSeconds)));
                        return parts.join(" · ");
                    }
                    const length = RecordingSave.duration(row.durationSeconds, !root.keepEnabled);
                    if (length.length > 0) {
                        parts.push(length);
                    }
                    return parts.join(" · ");
                }

                contentItem: RowLayout {
                    spacing: Theme.rowSpacing
                    SeabassCheckBox {
                        objectName: "includeBox"
                        checked: row.included
                        enabled: !root.controller.busy
                        onToggled: root.controller.setIncluded(row.index, checked)
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Label {
                            objectName: "fileNameLabel"
                            Layout.fillWidth: true
                            text: row.fileName
                            elide: Text.ElideMiddle
                            font.bold: true
                        }
                        Label {
                            objectName: "detailsLabel"
                            Layout.fillWidth: true
                            text: row.details
                            elide: Text.ElideRight
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                        }
                        RecordingSaveState {
                            id: saveState
                            Layout.fillWidth: true
                            keepController: root.keepController
                            stickRoot: root.stickRoot
                            path: row.path
                        }
                    }
                    Button {
                        objectName: "showKeptButton"
                        visible: saveState.keptState === "kept"
                        text: qsTr("Show in Folder")
                        icon.source: Theme.iconUrl("folder-open")
                        icon.color: Theme.text
                        onClicked: root.keepController.showInFolder(saveState.kept.flacPath)
                    }
                    Label {
                        objectName: "sizeLabel"
                        text: Theme.humanBytes(row.sizeBytes)
                        color: Theme.textMuted
                        font.family: Theme.dataFamily
                        horizontalAlignment: Text.AlignRight
                    }
                    // The play button the Library Health rows have.
                    Button {
                        objectName: "playButton"
                        text: "Play"
                        icon.source: Theme.iconUrl("media-playback-start")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        enabled: !root.controller.busy
                        ToolTip.visible: hovered
                        ToolTip.text: "Play this recording"
                        onClicked: root.playRecording(row)
                    }
                    Button {
                        objectName: "keepButton"
                        visible: root.keepEnabled && keepForm.offers(row.path, row.holdsNoAudio)
                        text: qsTr("Save as FLAC...")
                        icon.source: Theme.iconUrl("document-save")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        enabled: !root.controller.working && !saveState.inProgress
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Save this recording on this computer as a FLAC file with tags and a cover")
                        onClicked: root.openKeepForm(row)
                    }
                }
            }

            EmptyState {
                objectName: "emptyLabel"
                visible: list.count === 0 && root.controller.listed && !root.controller.busy
                iconName: "media-record"
                text: "No recordings on this stick."
            }
        }

        Label {
            objectName: "leftAloneLabel"
            visible: root.leftAloneLine.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: root.leftAloneLine
        }

        // Another stick's recording is being saved: this stick's jobs wait
        // for it, one job at a time.
        Label {
            objectName: "otherStickKeepLabel"
            Layout.fillWidth: true
            visible: root.keepRunsElsewhere && (root.keepOffered || root.keepWorking)
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("A recording from another stick is being saved as FLAC. What you save from this stick "
                + "waits until that has ended.")
        }

        // Save as FLAC, beside the page: this stick's job in hand, its
        // ones that wait, and the end of the last one.
        KeepRecordingProgress {
            id: keepProgress
            objectName: "keepProgress"
            Layout.fillWidth: true
            keepController: root.keepController
            stickRoot: root.stickRoot
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: root.controller.busy === true
        label: root.controller.listing ? "Looking for recordings..."
            : "Deleting recordings. Do not remove your USB stick."
        current: root.controller.filesDone
        total: root.working ? root.controller.filesTotal : 0
        unitName: "recordings"
        currentItem: root.working ? root.controller.currentItem : ""
        cancellable: root.working && !root.controller.cancelRequested
        onCancelRequested: root.controller.cancel()
    }
}
