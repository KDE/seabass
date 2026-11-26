// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui

// Clean Up Recordings: the sets DJ hardware recorded onto this stick
// (Engine OS into Sessions/, Pioneer and AlphaTheta decks into their REC
// folders), listed with a checkbox each, and deleted. Nothing is copied
// first: they are usually long since saved elsewhere. No library refers
// to these files; see RecordingsController for why no full stick backup
// is taken first.
//
// Keep this recording (experimental): a recording becomes a FLAC with
// tags and a cover in a folder on this computer, through the form below
// and a KeepRecordingController, beside the page in the passive progress
// strip. The recording on the stick stays as it is. The strip stays while
// a job runs or waits, even when Experimental is switched off meanwhile,
// and lists what ended until it is closed. The delete confirmation names
// every ticked recording a keep was asked for and did not end kept.
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
    // Keep this recording is experimental (docs/experimental-features.md).
    property bool keepEnabled: false

    readonly property bool keepOffered: root.keepEnabled && root.keepController !== null
        && root.keepController.available === true
    // A recording is being kept or waits to be: nothing is deleted
    // meanwhile, so no recording goes from under its job.
    readonly property bool keepWorking: root.keepController !== null
        && (root.keepController.busy === true || (root.keepController.queued ?? []).length > 0)
    readonly property int keepRevision: root.keepController !== null ? root.keepController.revision : 0

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
    // "1:07:13" or "7:13"; "" when unknown. Cut to whole seconds as a
    // clock shows them, never rounded up: the C++ texts (the notes of a
    // kept job, the reasons) count the same way.
    function duration(seconds) {
        if (seconds === undefined || seconds < 0) return "";
        const total = Math.floor(seconds);
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const two = (v) => (v < 10 ? "0" : "") + v;
        return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
    }
    // For a header that declares more than the file holds, when both
    // lengths print the same: how much is missing, in words. "" otherwise.
    function missingNote(declaredSeconds, heldSeconds, missingBytes) {
        if (missingBytes === undefined || missingBytes < 0 || declaredSeconds < 0
            || root.duration(declaredSeconds) !== root.duration(heldSeconds)) {
            return "";
        }
        return missingBytes === 1 ? qsTr("1 byte less than the header declares")
            : qsTr("%1 bytes less than the header declares").arg(missingBytes);
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

    // Opens the form for a row of the list.
    function openKeepForm(row) {
        keepDialog.openFor(row.path, row.fileName, row.modified, row.unfinished, row.heldSeconds,
                           row.declaredSeconds, row.declaredUnusable, row.missingBytes);
    }

    // What the strip says, for the job in hand or the ones that ended.
    readonly property var keepSteps: [
        {key: "encoding", label: qsTr("Encoding")},
        {key: "checking", label: qsTr("Checking")},
        {key: "tags", label: qsTr("Tags and cover")},
        {key: "placing", label: qsTr("Saving")},
    ]
    readonly property var keepFinishedJobs: root.keepController !== null ? (root.keepController.finishedJobs ?? []) : []
    readonly property var keepFinished: root.keepFinishedJobs.length > 0
        ? root.keepFinishedJobs[root.keepFinishedJobs.length - 1] : ({})
    readonly property int keepNotKeptCount: root.keepFinishedJobs.filter(job => job.state !== "kept").length
    readonly property bool keepShowsFinished: !root.keepWorking && root.keepFinishedJobs.length > 0
    // A line of the strip's panel for a job that ended, other than one
    // dropped (those are named together).
    function keepEndedLine(job) {
        if (job.state === "kept") {
            return qsTr("Kept: %1 as %2 in %3").arg(job.recording).arg(job.fileName).arg(job.folder);
        }
        if (job.state === "cancelled") {
            return (job.message ?? "").length > 0 ? qsTr("Stopped: %1: %2").arg(job.recording).arg(job.message)
                : qsTr("Stopped: %1, nothing of it was kept").arg(job.recording);
        }
        return qsTr("Not kept: %1: %2").arg(job.recording).arg(job.message ?? "");
    }
    function keepEndedLines() {
        const lines = root.keepFinishedJobs.filter(job => job.state !== "dropped").map(job => root.keepEndedLine(job));
        const dropped = root.keepFinishedJobs.filter(job => job.state === "dropped").map(job => job.recording);
        if (dropped.length > 0) {
            lines.push(qsTr("Not kept, stopped before it started: %n recording(s): %1", "", dropped.length)
                .arg(dropped.join(", ")));
        }
        return lines;
    }
    function keepStepIndex(step) {
        // The free space check is over before the encoder starts: it is
        // part of the first step.
        if (step === "space") return 0;
        return root.keepSteps.findIndex(s => s.key === step);
    }
    function percent(fraction) {
        return Math.floor(Math.max(0, Math.min(1, fraction)) * 100);
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
        // What Keep made of the ticked recordings in this session, read
        // as the dialog opens: {notKept: [names], kept: [{recording,
        // fileName, folder}]}.
        property var keepNotes: ({notKept: [], kept: [], keptGone: []})
        function openWithNotes() {
            confirmDeleteDialog.keepNotes = root.keepController !== null
                ? root.keepController.keepNotesFor(root.controller.selectedPaths())
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
                : qsTr("Not kept on this computer, though you asked for it: %1.")
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
                : qsTr("Kept earlier, but that file is no longer there as it was kept: %1.")
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
                : qsTr("Kept on this computer: %1.").arg(confirmDeleteDialog.keepNotes.kept
                    .map(k => qsTr("%1 as %2 in %3").arg(k.recording).arg(k.fileName).arg(k.folder)).join("; "))
        }
    }

    // ---- Keep this recording: the form ----
    SeabassDialog {
        id: keepDialog
        objectName: "keepDialog"
        severity: SeabassDialog.Question
        showBadge: false
        width: Math.min(root.width - 2 * Theme.pageMargin, Theme.scaled(640))
        title: qsTr("Keep This Recording")
        headline: qsTr("%1 becomes a FLAC file with tags and a cover on this computer.").arg(keepDialog.recording)
        detailText: qsTr("The recording on the stick stays as it is.")

        property string wavPath: ""
        property string recording: ""
        // The recording's header was not completed: the audio the file
        // holds is kept only when the person ticks the box for it. The
        // header's own length is -1 when it declares none.
        property bool unfinished: false
        property double heldSeconds: -1
        property double declaredSeconds: -1
        // The header declares a length no recording has; how many bytes
        // less the file holds than the header declares (-1 when not cut
        // short).
        property bool declaredUnusable: false
        property double missingBytes: -1
        // Shown once the person has tried to start, or has typed.
        property bool tried: false
        readonly property var keep: root.keepController
        readonly property string fileName: keep ? keep.fileNameFor(artistField.text, titleField.text, folderField.text) : ""
        readonly property string titleProblem: titleField.text.trim().length === 0 ? qsTr("The track name is needed.") : ""
        readonly property var textProblems: keep
            ? [keep.textProblem("artist", artistField.text), keep.textProblem("title", titleField.text),
               keep.textProblem("album", albumField.text), keep.textProblem("genre", genreField.text)] : []
        readonly property string yearProblem: keep ? keep.yearProblem(yearField.text) : ""
        readonly property string folderProblem: keep ? keep.folderProblem(folderField.text) : ""
        readonly property string targetProblem: keep && titleProblem.length === 0 && folderProblem.length === 0
            ? (root.keepRevision, keep.targetProblem(folderField.text, artistField.text, titleField.text)) : ""
        readonly property string coverProblem: keep && keep.coverError.length > 0 ? keep.coverError : ""
        readonly property var problems: [titleProblem].concat(textProblems)
            .concat([yearProblem, folderProblem, targetProblem, coverProblem])
            .filter(p => p.length > 0)
        readonly property bool ready: problems.length === 0 && keep !== null && keep.coverPreparing !== true
            && (!keepDialog.unfinished || keepUnfinishedBox.checked)

        function openFor(path, name, recorded, unfinished, heldSeconds, declaredSeconds, declaredUnusable,
                         missingBytes) {
            const suggested = root.keepController.suggestionsFor(name, recorded);
            keepDialog.wavPath = path;
            keepDialog.recording = name;
            keepDialog.unfinished = unfinished === true;
            keepDialog.heldSeconds = heldSeconds ?? -1;
            keepDialog.declaredSeconds = declaredSeconds ?? -1;
            keepDialog.declaredUnusable = declaredUnusable === true;
            keepDialog.missingBytes = missingBytes ?? -1;
            keepUnfinishedBox.checked = false;
            keepDialog.tried = false;
            artistField.text = suggested.artist;
            titleField.text = suggested.title;
            albumField.text = suggested.album;
            genreField.text = suggested.genre;
            yearField.text = suggested.year > 0 ? String(suggested.year) : "";
            folderField.text = suggested.folder;
            root.keepController.pickCover("");
            keepDialog.open();
        }
        function start() {
            keepDialog.tried = true;
            if (!keepDialog.ready) {
                return;
            }
            const taken = root.keepController.keep({
                wavPath: keepDialog.wavPath,
                recording: keepDialog.recording,
                artist: artistField.text,
                title: titleField.text,
                album: albumField.text,
                genre: genreField.text,
                year: yearField.text,
                folder: folderField.text,
                keepUnfinished: keepDialog.unfinished && keepUnfinishedBox.checked,
            });
            if (taken) {
                keepDialog.close();
            }
        }

        // Later than SeabassDialog's own onOpened, which hands focus to a
        // footer button: the form is what is being asked for.
        onOpened: Qt.callLater(() => {
            keepDialog.markFooterDefault(keepStartButton);
            artistField.forceActiveFocus();
        })

        footer: DialogButtonBox {
            Button {
                id: keepStartButton
                objectName: "keepStartButton"
                text: qsTr("Keep Recording")
                enabled: keepDialog.ready
                // Not AcceptRole, which would close the form whatever
                // keep() answers: start() closes it once the job is taken.
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: keepDialog.start()
            }
            Button {
                objectName: "keepCancelButton"
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        ExperimentalBadge {}

        // A recording whose header was not completed: said first, both
        // lengths where there are two, and kept only with the box ticked.
        // The cause is given as what usually leads there, not as fact.
        ColumnLayout {
            objectName: "keepUnfinishedNotice"
            Layout.fillWidth: true
            visible: keepDialog.unfinished
            spacing: Theme.tightSpacing
            Label {
                objectName: "keepUnfinishedLabel"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.danger
                readonly property string missing: root.missingNote(keepDialog.declaredSeconds, keepDialog.heldSeconds,
                                                                   keepDialog.missingBytes)
                text: keepDialog.declaredUnusable
                    ? qsTr("The header of this recording was not completed: the header's length is not usable, and "
                        + "the file holds %1 of audio. This happens when a player is switched off or a stick is "
                        + "pulled while recording. The recording on the stick is not changed.")
                        .arg(root.duration(keepDialog.heldSeconds))
                    : keepDialog.declaredSeconds >= 0 && missing.length > 0
                    ? qsTr("The header of this recording was not completed: the header declares %1 of audio, the "
                        + "file holds %2 (%3). This happens when a player is switched off or a stick is pulled "
                        + "while recording. The recording on the stick is not changed.")
                        .arg(root.duration(keepDialog.declaredSeconds)).arg(root.duration(keepDialog.heldSeconds))
                        .arg(missing)
                    : keepDialog.declaredSeconds >= 0
                    ? qsTr("The header of this recording was not completed: the header declares %1 of audio, the "
                        + "file holds %2. This happens when a player is switched off or a stick is pulled while "
                        + "recording. The recording on the stick is not changed.")
                        .arg(root.duration(keepDialog.declaredSeconds)).arg(root.duration(keepDialog.heldSeconds))
                    : qsTr("The header of this recording was not completed: it does not say how long the audio "
                        + "is. The file holds %1 of audio. This happens when a player is switched off or a stick "
                        + "is pulled while recording. The recording on the stick is not changed.")
                        .arg(root.duration(keepDialog.heldSeconds))
            }
            SeabassCheckBox {
                id: keepUnfinishedBox
                objectName: "keepUnfinishedBox"
                text: qsTr("Keep the audio the file holds")
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.rowSpacing
            rowSpacing: Theme.tightSpacing

            Label { text: qsTr("Artist") }
            TextField {
                id: artistField
                objectName: "keepArtistField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Track name") }
            TextField {
                id: titleField
                objectName: "keepTitleField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Album") }
            TextField {
                id: albumField
                objectName: "keepAlbumField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Genre") }
            TextField {
                id: genreField
                objectName: "keepGenreField"
                Layout.fillWidth: true
                onAccepted: keepDialog.start()
            }
            Label { text: qsTr("Year") }
            TextField {
                id: yearField
                objectName: "keepYearField"
                Layout.preferredWidth: Theme.scaled(90)
                maximumLength: 4
                inputMethodHints: Qt.ImhDigitsOnly
                onAccepted: keepDialog.start()
            }
            Label {
                Layout.alignment: Qt.AlignTop
                text: qsTr("Cover")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Rectangle {
                    objectName: "keepCoverFrame"
                    Layout.preferredWidth: Theme.scaled(72)
                    Layout.preferredHeight: Theme.scaled(72)
                    color: Theme.surface
                    border.color: Theme.borderSubtle
                    border.width: 1
                    radius: Theme.cornerRadius
                    Image {
                        objectName: "keepCoverPreview"
                        anchors.fill: parent
                        anchors.margins: 1
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: keepDialog.keep ? keepDialog.keep.coverPreview : ""
                        visible: source.toString().length > 0
                    }
                    SeabassIcon {
                        anchors.centerIn: parent
                        visible: !keepDialog.keep || keepDialog.keep.coverPreview.length === 0
                        iconName: "media-record"
                        size: Theme.iconSizeSmall
                        color: Theme.textMuted
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.tightSpacing
                    Label {
                        objectName: "keepCoverLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: keepDialog.coverProblem.length > 0 ? Theme.danger : Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: {
                            const keep = keepDialog.keep;
                            if (!keep || keep.coverPath.length === 0) return qsTr("No cover");
                            if (keep.coverPreparing) return qsTr("Preparing the cover...");
                            if (keep.coverError.length > 0) return keep.coverError;
                            return qsTr("Embedded as %1 × %2 pixels").arg(keep.coverWidth).arg(keep.coverHeight);
                        }
                    }
                    RowLayout {
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "keepCoverButton"
                            text: qsTr("Choose Picture…")
                            icon.source: Theme.iconUrl("folder-open")
                            onClicked: coverFileDialog.open()
                        }
                        Button {
                            objectName: "keepNoCoverButton"
                            text: qsTr("No Cover")
                            enabled: keepDialog.keep !== null && keepDialog.keep.coverPath.length > 0
                            onClicked: keepDialog.keep.pickCover("")
                        }
                    }
                }
            }
            Label { text: qsTr("Folder") }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                TextField {
                    id: folderField
                    objectName: "keepFolderField"
                    Layout.fillWidth: true
                    onAccepted: keepDialog.start()
                }
                Button {
                    objectName: "keepFolderButton"
                    text: qsTr("Choose…")
                    onClicked: keepFolderDialog.open()
                }
            }
        }
        Label {
            objectName: "keepFileNameLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("Saved as %1").arg(keepDialog.fileName)
        }
        Label {
            objectName: "keepProblemsLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: {
                const lines = keepDialog.problems.slice();
                const refused = keepDialog.keep ? keepDialog.keep.errorMessage : "";
                if (keepDialog.tried && refused.length > 0 && lines.indexOf(refused) < 0) {
                    lines.push(refused);
                }
                return lines.join("\n");
            }
        }
    }
    FileDialog {
        id: coverFileDialog
        title: qsTr("Choose a picture for the cover")
        nameFilters: [qsTr("Pictures (*.jpg *.jpeg *.png *.webp *.bmp *.gif *.tif *.tiff)")]
        onAccepted: root.keepController.pickCover(selectedFile.toString())
    }
    FolderDialog {
        id: keepFolderDialog
        title: qsTr("Choose the folder for the FLAC file")
        currentFolder: root.keepController ? root.keepController.folderUrl(folderField.text) : ""
        onAccepted: folderField.text = root.keepController.localPath(selectedFolder.toString())
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

                readonly property var kept: root.keepController !== null
                    ? (root.keepRevision, root.keepController.outcomeFor(row.path)) : ({})
                readonly property string keptState: row.kept.state ?? ""
                readonly property string keptLine: {
                    switch (row.keptState) {
                    case "queued": return qsTr("Waits to be kept as %1").arg(row.kept.fileName);
                    case "running": return qsTr("Being kept as %1").arg(row.kept.fileName);
                    case "kept": return qsTr("Kept as %1 in %2").arg(row.kept.fileName).arg(row.kept.folder);
                    case "failed": return qsTr("Not kept: %1").arg(row.kept.message);
                    case "cancelled": return (row.kept.message ?? "").length > 0
                        ? qsTr("Not kept, stopped: %1").arg(row.kept.message)
                        : qsTr("Not kept: stopped while it was being kept");
                    case "dropped": return qsTr("Not kept: stopped before it started");
                    default: return "";
                    }
                }

                readonly property string details: {
                    const parts = [row.folderName + " (" + root.sourceLabel(row.source) + ")"];
                    if (row.modified && !isNaN(row.modified.getTime())) {
                        parts.push(row.modified.toLocaleString(Qt.locale(), Locale.ShortFormat));
                    }
                    if (row.holdsNoAudio) {
                        parts.push(qsTr("holds no audio"));
                        return parts.join(" · ");
                    }
                    if (row.unfinished) {
                        const missing = root.missingNote(row.declaredSeconds, row.heldSeconds, row.missingBytes);
                        parts.push(row.declaredUnusable
                            ? qsTr("header not completed: its length is not usable, the file holds %1")
                                .arg(root.duration(row.heldSeconds))
                            : row.declaredSeconds >= 0 && missing.length > 0
                            ? qsTr("header not completed: it declares %1, the file holds %2 (%3)")
                                .arg(root.duration(row.declaredSeconds)).arg(root.duration(row.heldSeconds))
                                .arg(missing)
                            : row.declaredSeconds >= 0
                            ? qsTr("header not completed: it declares %1, the file holds %2")
                                .arg(root.duration(row.declaredSeconds)).arg(root.duration(row.heldSeconds))
                            : qsTr("header not completed, the file holds %1 of audio")
                                .arg(root.duration(row.heldSeconds)));
                        return parts.join(" · ");
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
                        Label {
                            objectName: "keptLabel"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: row.keptLine
                            elide: Text.ElideMiddle
                            color: row.keptState === "failed" || row.keptState === "cancelled"
                                || row.keptState === "dropped" ? Theme.danger
                                : row.keptState === "kept" ? Theme.good : Theme.textMuted
                            font.pointSize: Theme.fontSmall
                        }
                    }
                    Button {
                        objectName: "showKeptButton"
                        visible: row.keptState === "kept"
                        text: qsTr("Show in Folder")
                        icon.source: Theme.iconUrl("folder-open")
                        icon.color: Theme.text
                        onClicked: root.keepController.showInFolder(row.kept.flacPath)
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
                        visible: root.keepOffered && root.keepController.canKeep(row.path) && !row.holdsNoAudio
                        text: qsTr("Keep…")
                        icon.source: Theme.iconUrl("document-save")
                        icon.color: enabled ? Theme.text : Theme.textMuted
                        enabled: !root.controller.working && row.keptState !== "queued" && row.keptState !== "running"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Keep this recording on this computer, as a FLAC file with tags and a cover")
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

        // Keep this recording, beside the page: the job in hand, the
        // ones that wait, and the end of the last one.
        PassiveProgress {
            id: keepProgress
            objectName: "keepProgress"
            Layout.fillWidth: true
            readonly property var keep: root.keepController
            readonly property bool finishedShown: root.keepShowsFinished
            readonly property var last: root.keepFinished
            readonly property string state: last.state ?? ""
            readonly property int stepIndex: keep && keep.busy ? root.keepStepIndex(keep.progressStep) : -1
            readonly property int waiting: keep ? (keep.queued ?? []).length : 0
            // Not tied to Experimental: work under way is shown, and can be
            // cancelled, until it has ended, and what ended until closed.
            running: keep !== null && (root.keepWorking || finishedShown)
            done: finishedShown ? (root.keepNotKeptCount === 0 ? 1000 : 0) : Math.round((keep?.progress ?? 0) * 1000)
            total: 1000
            cancellable: keep !== null && keep.busy === true && keep.cancelRequested !== true
            summary: {
                if (!keep) return "";
                if (finishedShown) {
                    const jobs = root.keepFinishedJobs;
                    if (jobs.length > 1) {
                        return root.keepNotKeptCount === 0 ? qsTr("Kept %n recording(s)", "", jobs.length)
                            : qsTr("%1 of %2 recordings not kept").arg(root.keepNotKeptCount).arg(jobs.length);
                    }
                    const left = (last.leftBehind ?? "").length > 0;
                    if (state === "kept") {
                        return qsTr("Kept %1 as %2").arg(last.recording).arg(last.fileName);
                    }
                    if (state === "cancelled") {
                        return left ? qsTr("Stopped: %1 was not kept, and a file was left in %2").arg(last.recording)
                                          .arg(last.folder)
                            : qsTr("Stopped: nothing of %1 was kept").arg(last.recording);
                    }
                    if (state === "dropped") {
                        return qsTr("%1 was not kept: stopped before it started").arg(last.recording);
                    }
                    return left ? qsTr("%1 was not kept, and a file was left in %2").arg(last.recording).arg(last.folder)
                        : qsTr("%1 was not kept").arg(last.recording);
                }
                if (keep.cancelRequested) {
                    return qsTr("Stopping: keeping %1").arg(keep.currentRecording);
                }
                const step = root.keepSteps[Math.max(0, stepIndex)].label;
                let line = qsTr("Keeping %1: %2, %3%").arg(keep.currentRecording).arg(step.toLowerCase())
                    .arg(root.percent(keep.progress));
                const extras = [];
                if (waiting > 0) {
                    extras.push(qsTr("%n more waiting", "", waiting));
                }
                if (root.keepNotKeptCount > 0) {
                    extras.push(qsTr("%n not kept", "", root.keepNotKeptCount));
                }
                if (extras.length > 0) {
                    line += " (" + extras.join(", ") + ")";
                }
                return line;
            }
            steps: finishedShown ? [] : root.keepSteps.map((s, index) => ({
                label: s.label,
                count: index === keepProgress.stepIndex ? qsTr("%1%").arg(root.percent(keep.progress)) : "",
                state: index < keepProgress.stepIndex ? "done" : index === keepProgress.stepIndex ? "now" : "later",
            }))
            activity: {
                if (!keep) return [];
                if (finishedShown) {
                    return root.keepEndedLines();
                }
                const lines = keep.busy ? [qsTr("Writing %1 in %2").arg(keep.currentFileName).arg(keep.currentFolder)] : [];
                for (const job of (keep.queued ?? [])) {
                    lines.push(qsTr("Waiting: %1, as %2").arg(job.recording).arg(job.fileName));
                }
                return lines.concat(root.keepEndedLines());
            }
            // A kept job's notes (an unfinished recording and how much was
            // kept, leftovers swept); a failure's message before them.
            note: finishedShown ? [state === "kept" ? "" : (last.message ?? "")].concat(last.notes ?? [])
                    .filter(line => line.length > 0).join("\n")
                : qsTr("The recording on the stick is only read. Leaving this page stops the work.")
            actions: !finishedShown ? []
                : state === "kept" ? [{key: "show", text: qsTr("Show in Folder")}, {key: "close", text: qsTr("Close")}]
                : [{key: "close", text: qsTr("Close")}]
            onCancelRequested: keep.cancel()
            onActionTriggered: (key) => {
                if (key === "show") {
                    keep.showInFolder(last.flacPath);
                } else {
                    keep.dismissFinished();
                }
            }
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
