// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Clean Up Recordings: the sets DJ hardware recorded onto this stick
// (Engine OS into Sessions/, Pioneer and AlphaTheta decks into their REC
// folders), listed with a checkbox each, and deleted. Nothing is copied
// first: they are usually long since saved elsewhere. No library refers
// to these files; see RecordingsController for why no full stick backup
// is taken first.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    // A RecordingsController; a QtObject with the same shape in the tests.
    required property var controller
    // The app's player, which plays a recording as a plain file.
    required property var playbackController

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
    // "1:07:13" or "7:13"; "" when unknown.
    function duration(seconds) {
        if (seconds === undefined || seconds < 0) return "";
        const total = Math.round(seconds);
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const two = (v) => (v < 10 ? "0" : "") + v;
        return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
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
                middleLabel: "Housekeeping"
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
        onAccepted: {
            root.releasePlayerFromDeletedRecordings();
            root.controller.deleteSelected();
        }
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
                enabled: !root.controller.busy && root.selectedCount > 0
                onClicked: confirmDeleteDialog.open()
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

                readonly property string details: {
                    const parts = [row.folderName + " (" + root.sourceLabel(row.source) + ")"];
                    if (row.modified && !isNaN(row.modified.getTime())) {
                        parts.push(row.modified.toLocaleString(Qt.locale(), Locale.ShortFormat));
                    }
                    const length = root.duration(row.durationSeconds);
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
