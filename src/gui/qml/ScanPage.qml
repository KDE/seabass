// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Templates as T
import SeabassGui
import "common/SaveSummary.js" as SaveSummary

Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController
    required property var appSettingsController

    // Fired when a row is clicked anywhere except its own artwork (which
    // plays directly instead, see trackDelegate's own MouseAreas).
    // Carries the live scanController instance itself, not just an id --
    // Main.qml's handler pushes the detail page with it directly, so
    // that page's own prev/next lookups (trackAt()/trackCount()) read
    // the exact same already-scanned, currently filtered/sorted list
    // this page is showing, instead of re-scanning from scratch.
    signal trackDetailRequested(var scanController, int trackIndex, string format, string libraryPath)
    // "Open Sync" in the dialog about a track Engine does not have yet:
    // Sync after Rekordbox Export brings it there.
    signal openRekordboxExportSyncRequested()
    // "Show on the Recordings page" on a recording of Own Recordings: Clean
    // Up Recordings of this stick, at the recording at `path`.
    signal recordingsPageRequested(string path)

    // The window's KeepRecordingController, which Save as FLAC on a
    // recording of Own Recordings runs through, as the Recordings page's
    // does; null: nothing is offered to save.
    property var keepController: null

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    // OneLibrary lives alongside export.pdb under the same PIONEER root
    // (see ScanController::hasOneLibrary()'s own doc comment). It's a
    // third view onto the DeviceLibrary side of the stick, not a
    // separately-stored DetectedStick path, so it needs hasRekordbox
    // (the path it reads from) rather than a path of its own.
    readonly property bool hasOneLibrary: root.hasRekordbox && scanController.hasOneLibrary(root.rekordboxPath)

    // Deliberately a plain property, not bound to appSettingsController.
    // preferredFormat (unlike every other page's FormatToggle). That
    // setting is a shared, persisted, binary Rekordbox/Engine choice used
    // by Clean Up, Sync, and Local Cue Backup, none of which understand a
    // third "onelibrary" value. Library-source browsing here is scoped to
    // just this page and doesn't persist across restarts.
    property string format: root.hasRekordbox ? "rekordbox" : "engine"
    // A plain function, not a cached property: QML evaluates onFormatChanged
    // before a *dependent* property like a cached "path" has re-settled, so
    // reading a cached path here could see the previous format's value.
    // A function call is always evaluated fresh against the current format.
    function currentPath() {
        // "onelibrary" reads from the same PIONEER root as "rekordbox" -
        // falls into this branch already, no separate case needed.
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }

    ScanController {
        id: scanController
        objectName: "scanController"
        // Off, the stick's recordings are not even listed.
        recordingsEnabled: root.ownRecordingsEnabled
        recordingsPlaylist: root.ownRecordingsEnabled ? root.ownRecordingsPlaylist : ""
        // playlistNames only becomes available once the (async) scan
        // finishes, so restoring the last-selected playlist has to wait
        // for this rather than happening at Component.onCompleted
        // alongside rescan() below.
        onPlaylistNamesChanged: {
            root.restoreSelectedPlaylist();
            // The playlist menu's Restore original analysis is right the
            // first time it opens.
            root.originalAnalysisController.warmKeptCounts(root.rekordboxPath, root.enginePath);
        }
    }

    // Backs the "Merge with..." picker below, reuses CleanupController
    // wholesale (planManualMerge()/apply()) rather than a bespoke write
    // path, since a manual two-track merge is mechanically identical to
    // an auto-detected group's cleanup once the group exists.
    CleanupController {
        id: mergeController
    }

    // Backs the "click the waveform to add a cue" form in the track
    // details column (TrackDetailPanel).
    AddCueController {
        id: addCueController
    }

    // A track's rating and comment, edited in the track details (#105),
    // staged into the same session and written by the same Save into
    // every catalog on the stick that lists the file.
    property var trackEditController: TrackEditController {}
    Connections {
        target: ("objectName" in root.trackEditController) ? root.trackEditController : null
        ignoreUnknownSignals: true
        function onTracksSaved() {
            root.rescan();
        }
    }

    // Playlist editing (out of the experimental setting since 2026-11-19):
    // creating and deleting a playlist, adding a track to one and taking it
    // out, staged into the same session as an added cue, written by the
    // same Save into every library on the stick that has the playlist.

    // The playlist the list is showing, by name: set where the selection
    // is made, since the row number is the FILTERED list's when the
    // playlist filter is in use.
    property string shownPlaylist: ""

    // Own Recordings (Experimental, as the rest of Own Recordings): a
    // fixed row under "All tracks" while the stick holds a recording the
    // players left on it or a playlist "Own Recordings" (or an import
    // waits to make it). The row is that playlist: it lists the
    // playlist's tracks as any playlist, and after them the recordings
    // (ScanController.showRecordings()); the playlist is not listed a
    // second time. "Add to the library" on a recording saved as FLAC
    // imports the FLAC into it, made in every library that lacks it by
    // the same save. Off, the sidebar and
    // the list are as they were without it, the playlist an ordinary one.
    readonly property bool ownRecordingsEnabled: root.appSettingsController.experimentalFeaturesEnabled === true
    readonly property string ownRecordingsPlaylist: "Own Recordings"
    readonly property bool ownRecordingsPlaylistOnStick: scanController.playlistNames.indexOf(root.ownRecordingsPlaylist) >= 0
    readonly property bool ownRecordingsListed: root.ownRecordingsEnabled && (scanController.recordingCount > 0
        || root.ownRecordingsPlaylistOnStick || root.newPlaylistNames.indexOf(root.ownRecordingsPlaylist) >= 0)
    // The row shown is Own Recordings: shownPlaylist is its playlist then,
    // which every question about the playlist shown (a drop at a place,
    // "Remove from", a move) asks of, made or not yet. Kept across a new
    // read of the stick while the row is there (restoreSelectedPlaylist());
    // gone with it.
    property bool showingRecordings: false
    onOwnRecordingsListedChanged: {
        if (!root.ownRecordingsListed && root.showingRecordings) {
            root.restoreSelectedPlaylist();
        } else if (root.ownRecordingsListed && !root.showingRecordings
                   && root.shownPlaylist === root.ownRecordingsPlaylist) {
            // The playlist shown is the row now.
            root.showOwnRecordings();
        }
    }
    // What waits for Save about the row's playlist, which the row's menu
    // takes back and the ways into it heed: a delete of the stick
    // playlist (staged with Experimental off, say), and its making by a
    // track dragged onto the row or by the import that waits.
    readonly property bool ownRecordingsPendingDelete: root.playlistEditController.pendingRevision >= 0
        && root.playlistEditController.isPlaylistStaged(root.ownRecordingsPlaylist) === true
    readonly property bool ownRecordingsMadeByDrag: root.playlistEditController.pendingRevision >= 0
        && root.playlistEditController.isNewPlaylistStaged(root.ownRecordingsPlaylist) === true
    readonly property bool ownRecordingsMadeByImport: root.trackImportController.stagedPlaylistIsNew === true
        && root.trackImportController.stagedPlaylist === root.ownRecordingsPlaylist
    // A rename staged from or to the row's playlist's name (with
    // Experimental off, say), as {from, to}; null for none. Save would
    // leave the playlist under two names if anything went into it now.
    readonly property var ownRecordingsRename: {
        if (root.playlistEditController.pendingRevision < 0 || !("pendingRenames" in root.playlistEditController)) {
            return null;
        }
        const renames = root.playlistEditController.pendingRenames;
        const name = root.ownRecordingsPlaylist;
        if (renames[name] !== undefined) {
            return {from: name, to: renames[name]};
        }
        const from = Object.keys(renames).find(key => renames[key] === name);
        return from !== undefined ? {from: from, to: name} : null;
    }
    // Why nothing can be added to the row's playlist now, "" when it can:
    // PlaylistEditController::addToPlaylistMakingItWhereMissing's rule, in
    // its words, for the import too.
    function ownRecordingsRefusal() {
        if (root.ownRecordingsPendingDelete) {
            return qsTr("\"%1\" is to be deleted when you Save. Keep it first (the row's menu), then add to it.")
                .arg(root.ownRecordingsPlaylist);
        }
        if (root.ownRecordingsRename !== null) {
            return qsTr("\"%1\" is to be renamed \"%2\" when you Save. Keep its name first (the row's menu), then "
                        + "add to it.").arg(root.ownRecordingsRename.from).arg(root.ownRecordingsRename.to);
        }
        return "";
    }
    // Every way a track of the library goes into a playlist from Browse
    // (the drag card, the track menu's Add to): the row's playlist by its
    // one rule, made by the same save wherever a library lacks it and the
    // track then in it in every library; any other as it always was.
    // False when refused at once (said already).
    function addTrackToPlaylist(playlist, filePath, title, toIndex, moveFrom) {
        if (playlist === root.ownRecordingsPlaylist && root.ownRecordingsEnabled) {
            const refusal = root.ownRecordingsRefusal();
            // Made by the import that waits: the add would find it only
            // where it is already, and be left out of the others.
            const why = refusal.length > 0 ? refusal : root.ownRecordingsMadeByImport
                ? qsTr("\"%1\" is made by the import waiting for Save. Save first, then add tracks to it.")
                    .arg(playlist)
                : "";
            if (why.length > 0) {
                playlistEditMessage.show(why, true, qsTr("Not added"));
                return false;
            }
            return root.playlistEditController.addToPlaylistMakingItWhereMissing(
                root.rekordboxPath, root.enginePath, playlist, filePath, title, toIndex, moveFrom);
        }
        return root.playlistEditController.addToPlaylist(root.rekordboxPath, root.enginePath, playlist, filePath, title,
                                                         toIndex, moveFrom);
    }
    readonly property var sidebarFixedRows: [{kind: "all", name: "All tracks"}]
        .concat(root.ownRecordingsListed
                ? [{kind: "recordings", name: "Own Recordings", playlist: root.ownRecordingsPlaylist}] : [])
    // What a recording's row is, and why it offers none of a track's edits.
    readonly property string recordingRowText: qsTr("A recording a player left on the stick. It is not in the "
                                                    + "library, so it has no cues, tags or analysis, and it goes "
                                                    + "into no playlist.")
    // ...and of one whose FLAC the library has: the WAV is a second copy.
    function recordingInLibraryText(title) {
        return qsTr("A recording a player left on the stick. Saved as FLAC, it is in the library as \"%1\", so "
                    + "this WAV is a second copy of it.").arg(title);
    }
    property var playlistEditController: PlaylistEditController {}
    Connections {
        target: ("objectName" in root.playlistEditController) ? root.playlistEditController : null
        ignoreUnknownSignals: true
        function onPlaylistsSaved() {
            root.rescan();
        }
        function onErrorMessageChanged() {
            if (root.playlistEditController.errorMessage.length > 0) {
                playlistEditMessage.show(root.playlistEditController.errorMessage, true);
                // Refused: nothing is coming to take the held place.
                root.settled();
            }
        }
        // The edit asked for at a place is staged: the list shows it there
        // (the page's pending* bindings answer the same signal), and the
        // held place can close. After them, not before.
        function onPendingChanged() {
            if (root.settleGap >= 0) {
                Qt.callLater(root.settled);
            }
        }
    }

    // A move or an add asked for at a place in the list is staged off the
    // GUI thread (PlaylistEditController looks the playlist up first), so
    // the list shows it a moment after the release. Meanwhile the place
    // stays open, the faded copy in it (dropGap.heldIndex), so the rows
    // never go back to their old order before sliding to the new one.
    // Closed when the staging is shown, refused, or after a second at most.
    property int settleGap: -1
    property var settleTrack: null
    function settle(gapIndex) {
        root.settleGap = gapIndex;
        root.settleTrack = trackDrag;
        settleTimeout.restart();
    }
    function settled() {
        settleTimeout.stop();
        root.settleGap = -1;
        root.settleTrack = null;
    }
    Timer {
        id: settleTimeout
        interval: 1000
        onTriggered: root.settled()
    }
    MessagePopup {
        id: playlistEditMessage
        objectName: "playlistEditMessage"
    }

    // Track import (experimental): audio files and folders from the
    // computer, dropped on a playlist, on the track list or on the empty
    // part of the playlist list, or picked from a playlist's menu. Staged
    // into the same session; Save copies them to the stick and lists them
    // in every library on it.
    readonly property bool trackImportEnabled: root.appSettingsController.experimentalFeaturesEnabled === true
    // Switched off, it stages nothing new; what waits stays staged.
    property var originalAnalysisController: OriginalAnalysisController {}
    property var trackImportController: TrackImportController {}
    // The switch, for whichever controller the page has: one that is not
    // told stages nothing.
    Binding {
        target: root.trackImportController
        property: "experimentalEnabled"
        value: root.trackImportEnabled
        when: root.trackImportController !== null && ("experimentalEnabled" in root.trackImportController)
    }
    Connections {
        target: ("objectName" in root.trackImportController) ? root.trackImportController : null
        ignoreUnknownSignals: true
        function onTracksImported() {
            root.rescan();
        }
        function onErrorMessageChanged() {
            if (root.trackImportController.errorMessage.length > 0) {
                // Headed as what did not happen: Analyse again imports nothing.
                playlistEditMessage.show(root.trackImportController.errorMessage, true,
                                         root.trackImportController.errorSource === "reanalysis"
                                             ? qsTr("Not analysed again") : qsTr("Not imported"));
            }
        }
        function onResultReady() {
            importResultDialog.outcome = root.trackImportController.lastResult;
            importResultDialog.open();
        }
        function onReanalysisReady() {
            reanalysisResultDialog.outcome = root.trackImportController.lastReanalysis;
            reanalysisResultDialog.open();
        }
    }
    // Escape stops the import (or Analyse again) being prepared, as Escape
    // backs out of anything not yet committed (Clean Up's staging):
    // nothing is staged of it. Only an Escape nothing else takes: whatever
    // has the keyboard and does something with Escape itself (the drag
    // card asking what a drop should do, the comment being edited, the
    // open strip, a menu or a dialog over the page) keeps it, and so does
    // a text field, which is for typing. Whatever comes to have an Escape
    // of its own later keeps it too, with nothing to add here:
    //
    // - With the keyboard on the page, the key goes to the item that has
    //   it, and on up through the items it is in to the page, which
    //   takes it (Keys below) only when none of them did.
    // - With the keyboard on none of the page's items (on the window
    //   itself, after a click on a row, or on the player bar), there is
    //   nothing on the page to take the key: the window's shortcut does.
    //   A menu or a dialog has the keyboard in the overlay, which keeps
    //   its own keys, so the shortcut stays off then.
    readonly property Item keyboardItem: root.Window.activeFocusItem
    readonly property bool typingInField: root.keyboardItem !== null && root.keyboardItem.hasOwnProperty("cursorPosition")
    function within(item, ancestor) {
        for (let at = item; at !== null && ancestor !== null; at = at.parent) {
            if (at === ancestor) {
                return true;
            }
        }
        return false;
    }
    readonly property bool importCancellableByEscape: root.trackImportController.busy === true && root.visible
        && (root.StackView.view === null || root.StackView.status === StackView.Active)
    Keys.onEscapePressed: (event) => {
        event.accepted = root.importCancellableByEscape && !root.typingInField;
        if (event.accepted) {
            root.trackImportController.cancel();
        }
    }
    Shortcut {
        objectName: "cancelImportShortcut"
        sequences: [StandardKey.Cancel]
        enabled: root.importCancellableByEscape && !root.typingInField
            && !root.within(root.keyboardItem, root) && !root.within(root.keyboardItem, root.Overlay.overlay)
        onActivated: root.trackImportController.cancel()
    }
    // `files` into `playlist` ("" for none), which the save makes when
    // `create`; atIndex below 0 for its end.
    function importFiles(files, playlist, create, atIndex) {
        // Own Recordings' playlist, whichever way the files came: made by
        // the same save in every library that lacks it, the files then in
        // it in all of them (TrackImportController::
        // importMakingPlaylistWhereMissing()). An import that waits for it
        // is joined, the files added last first.
        if (playlist === root.ownRecordingsPlaylist && root.ownRecordingsEnabled) {
            const refusal = root.ownRecordingsRefusal();
            if (refusal.length > 0) {
                playlistEditMessage.show(refusal, true, qsTr("Not imported"));
                return;
            }
            // Made with the tracks dragged onto the row: an import is
            // checked against the stick as it is, so it cannot join them.
            if (root.ownRecordingsMadeByDrag) {
                playlistEditMessage.show(qsTr("\"%1\" waits to be made by Save, with the tracks dragged onto it. Save "
                                              + "first, then add to it.").arg(playlist), true, qsTr("Not imported"));
                return;
            }
            root.trackImportController.importMakingPlaylistWhereMissing(root.rekordboxPath, root.enginePath, files,
                                                                        playlist, atIndex);
            return;
        }
        // More for the playlist an import that waits will make: the
        // controller joins the two.
        const joins = playlist.length > 0 && root.trackImportController.stagedPlaylistIsNew === true
            && root.trackImportController.stagedPlaylist === playlist;
        if (joins) {
            root.trackImportController.importFiles(root.rekordboxPath, root.enginePath, files, playlist, true, -1);
            return;
        }
        if (playlist.length > 0 && !create && root.newPlaylistNames.indexOf(playlist) >= 0) {
            playlistEditMessage.show(qsTr("\"%1\" is not on the stick yet. Save first, then import into it.").arg(playlist),
                                     true, qsTr("Not imported"));
            return;
        }
        root.trackImportController.importFiles(root.rekordboxPath, root.enginePath, files, playlist, create, atIndex);
    }
    // The dropped files go into a playlist still to be named.
    function importIntoNewPlaylist(files) {
        importPlaylistDialog.files = files;
        importPlaylistDialog.open();
    }
    FileDialog {
        id: importFileDialog
        objectName: "importFileDialog"
        property string playlist: ""
        title: qsTr("Import tracks")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Audio files (%1)").arg(root.trackImportController.fileDialogFilter()), qsTr("All files (*)")]
        onAccepted: root.importFiles(selectedFiles, importFileDialog.playlist, false, -1)
    }
    FolderDialog {
        id: importFolderDialog
        objectName: "importFolderDialog"
        property string playlist: ""
        title: qsTr("Import a folder of tracks")
        onAccepted: root.importFiles([selectedFolder], importFolderDialog.playlist, false, -1)
    }
    SeabassDialog {
        id: importPlaylistDialog
        objectName: "importPlaylistDialog"
        contentHeightFollowsContent: true
        property var files: []
        severity: SeabassDialog.Question
        title: qsTr("Import into a new playlist")
        headline: qsTr("The tracks go into a new playlist, made in every library on this stick.")
        detailText: qsTr("Nothing is copied until you Save.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: importPlaylistNameField.text = root.trackImportController.suggestedPlaylistName(files)
        onOpened: Qt.callLater(() => {
            importPlaylistDialog.markFooterDefault(importPlaylistDialog.standardButton(Dialog.Ok));
            importPlaylistNameField.forceActiveFocus();
            importPlaylistNameField.selectAll();
        })
        Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(
            () => importPlaylistNameField.text.trim().length > 0)
        TextField {
            id: importPlaylistNameField
            objectName: "importPlaylistNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Playlist name")
            onAccepted: {
                if (text.trim().length > 0) {
                    importPlaylistDialog.accept();
                }
            }
        }
        onAccepted: root.importFiles(importPlaylistDialog.files, importPlaylistNameField.text.trim(), true, -1)
    }
    // What an import came to: what waits for Save, and every file left
    // out with its reason.
    SeabassDialog {
        id: importResultDialog
        objectName: "importResultDialog"
        contentHeightFollowsContent: true
        property var outcome: ({})
        readonly property int staged: outcome.staged ?? 0
        readonly property var notImported: outcome.notImported ?? []
        readonly property int possibleDuplicates: outcome.possibleDuplicates ?? 0
        readonly property string playlist: outcome.playlist ?? ""
        // Left out because they are on the stick and where they were asked
        // to be: nothing went wrong with those, there was nothing to do.
        readonly property int onStick: notImported.filter(left => left.onStick === true).length
        readonly property bool allOnStick: notImported.length > 0 && onStick === notImported.length
        severity: staged + (outcome.addedToPlaylist ?? 0) > 0 || allOnStick ? SeabassDialog.Info : SeabassDialog.Warning
        title: qsTr("Import tracks")
        readonly property int added: outcome.addedToPlaylist ?? 0
        headline: staged === 0 && added > 0 ? qsTr("%n track(s) of the stick ready to go into \"%1\".", "", added).arg(playlist)
            : staged === 0 && allOnStick ? qsTr("Nothing to import: %n file(s) on the stick already.", "", onStick)
            : staged === 0 && onStick > 0
                ? qsTr("%n file(s) on the stick already.", "", onStick) + " "
                    + qsTr("%n other file(s) not imported.", "", notImported.length - onStick)
            : staged === 0 ? qsTr("None of these files can be imported.")
            : playlist.length === 0 ? qsTr("%n track(s) ready to import.", "", staged)
            : (outcome.playlistIsNew ? qsTr("%n track(s) ready to import into the new playlist \"%1\".", "", staged)
                                    : qsTr("%n track(s) ready to import into \"%1\".", "", staged)).arg(playlist)
        detailText: (staged > 0
                ? qsTr("Save copies them to the stick (%1 MB) and lists them in every library on it.")
                        .arg(Math.ceil(outcome.megabytes ?? 0)) + " "
                    + ((outcome.analysed ?? 0) === 0 ? qsTr("They have no beat grid or waveform yet.")
                        : (outcome.withGrid ?? 0) === staged
                            ? qsTr("Seabass analysed them with its own analysis, which is experimental: each has a beat grid and waveforms.")
                            : qsTr("Seabass analysed %1 of them with its own analysis, which is experimental: %2 with a beat grid.")
                                .arg(outcome.analysed).arg(outcome.withGrid ?? 0))
                : "")
            + ((outcome.followedGrids ?? []).length > 0
                ? "\n\n" + qsTr("The tempo of %n of them moves, so their grid follows it.", "",
                                 outcome.followedGrids.length) : "")
            + (staged > 0 && added > 0
                ? "\n\n" + qsTr("%n more are on the stick already and go into the playlist too.", "", added) : "")
            + ((outcome.note ?? "").length > 0 ? (staged + added > 0 ? "\n\n" : "") + outcome.note : "")
        // The files left out: their number, and which ones on request.
        property bool showNotImported: false
        onAboutToShow: showNotImported = false
        RowLayout {
            visible: importResultDialog.notImported.length > 0
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                objectName: "importNotImportedHeading"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("%n file(s) left out.", "", importResultDialog.notImported.length)
            }
            Button {
                objectName: "importNotImportedToggle"
                flat: true
                text: importResultDialog.showNotImported ? qsTr("Hide them") : qsTr("Show them")
                onClicked: importResultDialog.showNotImported = !importResultDialog.showNotImported
            }
        }
        ScrollView {
            objectName: "importNotImportedScroll"
            visible: importResultDialog.notImported.length > 0 && importResultDialog.showNotImported
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(notImportedList.contentHeight, 200)
            clip: true
            ListView {
                id: notImportedList
                objectName: "importNotImportedList"
                model: importResultDialog.notImported
                delegate: Label {
                    required property var modelData
                    width: ListView.view.width
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: modelData.name + ": " + modelData.why
                }
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "importDuplicatesButton"
                // Not offered once Experimental is switched off: the
                // import it starts would be refused.
                visible: importResultDialog.possibleDuplicates > 0 && root.trackImportEnabled
                text: qsTr("Import possible duplicates too")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    importResultDialog.close();
                    root.trackImportController.importPossibleDuplicates();
                }
            }
            Button {
                objectName: "importTakeBackButton"
                visible: importResultDialog.staged + importResultDialog.added > 0
                text: qsTr("Take back")
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    root.trackImportController.dropStaged();
                    importResultDialog.close();
                }
            }
            Button {
                objectName: "importResultOkButton"
                text: qsTr("OK")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
    }

    // What Analyse again came to: how many tracks wait for Save with a new
    // analysis, and how many were left as they are (counted, not named).
    // Cue points that will move are said in counts, apart: those that
    // follow the new grid on their own (every move of their track under
    // the threshold) and those that wait for the person's choice, so a
    // change to the DJ's cue points is never made unsaid.
    SeabassDialog {
        id: reanalysisResultDialog
        objectName: "reanalysisResultDialog"
        contentHeightFollowsContent: true
        property var outcome: ({})
        readonly property int analysed: outcome.analysed ?? 0
        readonly property int leftAlone: outcome.leftAlone ?? 0
        readonly property string playlist: outcome.playlist ?? ""
        readonly property int cuesToMove: outcome.cuesToMove ?? 0
        readonly property int autoCueTracks: outcome.autoCueTracks ?? 0
        readonly property int autoCues: outcome.autoCues ?? 0
        readonly property int askedCueTracks: outcome.askedCueTracks ?? 0
        readonly property int askedCues: outcome.askedCues ?? 0
        readonly property int thresholdMs: outcome.cueFollowThresholdMs ?? 0
        readonly property int gridChangedTooMuch: outcome.gridChangedTooMuch ?? 0
        severity: analysed > 0 ? SeabassDialog.Info : SeabassDialog.Warning
        title: qsTr("Analyse again")
        headline: analysed > 0 ? qsTr("%n track(s) analysed again.", "", analysed)
            : qsTr("No track of \"%1\" could be analysed again.").arg(playlist)
        detailText: (leftAlone > 0 ? qsTr("%n track(s) left as they are.", "", leftAlone) : "")
            + (analysed > 0 ? (leftAlone > 0 ? " " : "") + qsTr("Nothing is written until you save.") : "")
            + (autoCues > 0
                ? "\n\n" + qsTr("%n cue point(s) or loop(s) will follow the new grid on their own when you save, "
                                 + "as they move by less than %1 ms (tracks: %2).", "", autoCues)
                    .arg(thresholdMs).arg(autoCueTracks)
                : "")
            + (askedCues > 0
                ? "\n\n" + (thresholdMs > 0
                    ? qsTr("On %n track(s) cue points or loops would move by %1 ms or more. They stay where they "
                           + "are unless you choose to move them (cue points: %2).", "", askedCueTracks)
                        .arg(thresholdMs).arg(askedCues)
                    : qsTr("On %n track(s) cue points or loops sat on a beat of the old grid, which the new one has "
                           + "elsewhere. They stay where they are unless you choose to move them (cue points: %1).",
                           "", askedCueTracks).arg(askedCues))
                : "")
            + (gridChangedTooMuch > 0
                ? "\n\n" + qsTr("On %n track(s) the grid changed too much to carry cue points; they stay where they are.",
                                 "", gridChangedTooMuch) : "")
        footer: DialogButtonBox {
            Button {
                objectName: "reanalysisReviewCuesButton"
                visible: reanalysisResultDialog.cuesToMove > 0
                text: qsTr("Review the cue points...")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    reanalysisResultDialog.close();
                    cueReviewDialog.open();
                }
            }
            Button {
                objectName: "reanalysisTakeBackButton"
                visible: reanalysisResultDialog.analysed > 0
                text: qsTr("Take back")
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    root.trackImportController.dropReanalysis(reanalysisResultDialog.playlist);
                    reanalysisResultDialog.close();
                }
            }
            Button {
                objectName: "reanalysisResultOkButton"
                text: qsTr("OK")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
    }

    RestoreOriginalAnalysisDialog {
        controller: root.originalAnalysisController
    }

    // Analyse again's cue review: each track whose cues sat on the old
    // grid, each such cue where it is and where the new grid would take
    // it, and a switch per track. Nothing moves for a track whose switch
    // is off. The tracks whose every move is under the threshold come
    // last, in a section of their own by the size of their moves; their
    // switches started on, and say what each will do now. A threshold set
    // here applies only when "Apply to all" is pressed, so turning the
    // spin box moves nothing, and it leaves alone a switch the person set:
    // the line under the buttons says how many kept such a choice. Move
    // all and Move none are the person's choice for every track. While a
    // save writes, nothing here can be changed.
    SeabassDialog {
        id: cueReviewDialog
        objectName: "cueReviewDialog"
        contentHeightFollowsContent: true
        readonly property var tracks: root.trackImportController.cueReview ?? []
        readonly property var tooDifferent: root.trackImportController.cueReviewTooDifferent ?? []
        readonly property int thresholdMs: root.trackImportController.cueFollowThresholdMs ?? 0
        // Under the threshold is what the controller compares: the
        // largest move, not rounded.
        function onTheirOwn(track) {
            return (track.largestMoveMs ?? 0) < cueReviewDialog.thresholdMs;
        }
        readonly property var askedTracks: tracks.filter(track => !onTheirOwn(track))
        readonly property var ownTracks: tracks.filter(track => onTheirOwn(track))
        // Closed until asked for: each of its cues has a picture, and a
        // long playlist can have many tracks in it.
        property bool ownTracksOpen: false
        // How many tracks the last Apply to all left as the person set
        // them, against the threshold; 0 once anything else is chosen.
        property int keptByHand: 0
        readonly property bool locked: root.trackImportController.saveWriting ?? false
        onAboutToShow: {
            ownTracksOpen = false;
            keptByHand = 0;
            thresholdSpin.value = thresholdMs;
        }
        severity: SeabassDialog.Question
        title: qsTr("Cue points on the old grid")
        headline: qsTr("Move cue points onto the new grid?")
        detailText: qsTr("These cue points and loops sat on a beat of the old grid. Switched on for a track, its cue "
                         + "points move to the same beat of the new grid when you save. Nothing moves for a track whose "
                         + "switch is off.")
        width: Math.min(root.width - 2 * Theme.pageMargin, Theme.scaled(900))
        Label {
            objectName: "cueReviewLegend"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("In each picture: dashed grey lines are the old grid, solid lines the new grid. The open marker "
                       + "labelled old is where the cue is now, the filled marker labelled new where it would go.")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                objectName: "cueReviewThresholdLabel"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Move cue points on their own when they move by less than")
            }
            SpinBox {
                id: thresholdSpin
                objectName: "cueReviewThresholdSpin"
                editable: true
                from: 0
                to: 100
                stepSize: 1
                value: cueReviewDialog.thresholdMs
                textFromValue: (value, locale) => qsTr("%1 ms").arg(value)
                valueFromText: (text, locale) => {
                    const ms = parseInt(text, 10);
                    return isNaN(ms) ? thresholdSpin.value : ms;
                }
            }
            Button {
                objectName: "cueReviewApplyThreshold"
                text: qsTr("Apply to all")
                enabled: !cueReviewDialog.locked
                onClicked: {
                    const kept = root.trackImportController.applyCueThreshold(thresholdSpin.value);
                    cueReviewDialog.keptByHand = kept > 0 ? kept : 0;
                }
            }
            Button {
                objectName: "cueReviewAllOn"
                text: qsTr("Move all")
                enabled: !cueReviewDialog.locked
                onClicked: {
                    cueReviewDialog.keptByHand = 0;
                    root.trackImportController.setMoveCuesForAll(true);
                }
            }
            Button {
                objectName: "cueReviewAllOff"
                text: qsTr("Move none")
                enabled: !cueReviewDialog.locked
                onClicked: {
                    cueReviewDialog.keptByHand = 0;
                    root.trackImportController.setMoveCuesForAll(false);
                }
            }
        }
        Label {
            objectName: "cueReviewKeptByHand"
            visible: cueReviewDialog.keptByHand > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: qsTr("%n track(s) kept the choice you made by hand.", "", cueReviewDialog.keptByHand)
        }
        // One track of the review: its title, by how much its cues move,
        // its switch, and each cue with both times and a picture.
        Component {
            id: cueReviewTrackDelegate
            ColumnLayout {
                id: reviewTrack
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.tightSpacing
                Label {
                    objectName: "cueReviewTrackTitle"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.bold: true
                    text: reviewTrack.modelData.title
                }
                Label {
                    objectName: "cueReviewTrackShift"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    readonly property real oldBpm: reviewTrack.modelData.oldBpm ?? 0
                    readonly property real newBpm: reviewTrack.modelData.newBpm ?? 0
                    // Whole ms, rounded down: a move under the threshold
                    // never reads as the threshold itself.
                    text: qsTr("The cue points move by up to %1 ms.")
                            .arg(Math.floor(reviewTrack.modelData.largestMoveMs ?? 0))
                        + (oldBpm > 0 && Math.abs(oldBpm - newBpm) >= 0.01
                            ? " " + qsTr("Tempo %1 BPM before, %2 BPM now.").arg(oldBpm.toFixed(2))
                                .arg(newBpm.toFixed(2)) : "")
                }
                Switch {
                    id: moveCuesSwitch
                    objectName: "moveCuesSwitch"
                    text: qsTr("Move these cue points onto the new grid")
                    // As the controller has it now, not as the list was
                    // made: a section made later (the closed one opened,
                    // the threshold applied) shows the choices since.
                    checked: root.trackImportController.movesCues(reviewTrack.modelData.id)
                    enabled: !cueReviewDialog.locked
                    onToggled: root.trackImportController.setMoveCues(reviewTrack.modelData.id, checked)
                    // Only this switch follows a choice: the list and its
                    // pictures stay as they are, and so does where it is
                    // scrolled to.
                    Connections {
                        target: root.trackImportController
                        ignoreUnknownSignals: true
                        function onMoveCuesChanged(changeId, move) {
                            if (changeId === reviewTrack.modelData.id) {
                                moveCuesSwitch.checked = move;
                            }
                        }
                    }
                }
                Repeater {
                    model: reviewTrack.modelData.rows
                    delegate: ColumnLayout {
                        id: reviewRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.tightSpacing / 2
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing
                            Rectangle {
                                visible: (reviewRow.modelData.color ?? "").length > 0
                                implicitWidth: Theme.scaled(10)
                                implicitHeight: Theme.scaled(10)
                                radius: Theme.scaled(2)
                                color: visible ? reviewRow.modelData.color : "transparent"
                            }
                            Label {
                                objectName: "cueReviewRowLabel"
                                text: reviewRow.modelData.label
                                    + ((reviewRow.modelData.name ?? "").length > 0 ? ", " + reviewRow.modelData.name : "")
                            }
                            Label {
                                objectName: "cueReviewRowTimes"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                color: Theme.textMuted
                                readonly property int diff: reviewRow.modelData.diffMs
                                text: qsTr("%1 now, %2 on the new grid (%3 ms)").arg(reviewRow.modelData.oldText)
                                        .arg(reviewRow.modelData.newText).arg((diff > 0 ? "+" : "") + diff)
                                    + (reviewRow.modelData.isLoop
                                        ? ", " + qsTr("ends %1 now, %2 on the new grid").arg(reviewRow.modelData.oldEndText)
                                            .arg(reviewRow.modelData.newEndText) : "")
                            }
                        }
                        ReanalysisCueStrip {
                            objectName: "cueReviewStrip"
                            Layout.fillWidth: true
                            strip: reviewRow.modelData.strip
                        }
                    }
                }
                Label {
                    objectName: "cueReviewOffBeat"
                    visible: (reviewTrack.modelData.offBeat ?? 0) > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: qsTr("%n cue point(s) not on a beat stay where they are.", "", reviewTrack.modelData.offBeat ?? 0)
                }
            }
        }
        ScrollView {
            objectName: "cueReviewScroll"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(cueReviewColumn.implicitHeight, root.height * 0.55)
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                id: cueReviewColumn
                width: parent.width
                spacing: Theme.rowSpacing * 2
                // The tracks by the size of their moves, not by their
                // switches: each switch says what its track will do.
                Label {
                    objectName: "cueReviewAskedHeading"
                    visible: cueReviewDialog.thresholdMs > 0 && cueReviewDialog.askedTracks.length > 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.bold: true
                    text: qsTr("Moves of %1 ms or more: %n track(s)", "", cueReviewDialog.askedTracks.length)
                        .arg(cueReviewDialog.thresholdMs)
                }
                Repeater {
                    objectName: "cueReviewTracks"
                    model: cueReviewDialog.askedTracks
                    delegate: cueReviewTrackDelegate
                }
                ColumnLayout {
                    visible: cueReviewDialog.ownTracks.length > 0
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing * 2
                    AbstractButton {
                        id: ownTracksHeading
                        objectName: "cueReviewOwnHeading"
                        Layout.fillWidth: true
                        // Opened from the keyboard as any button is.
                        focusPolicy: Qt.StrongFocus
                        onClicked: cueReviewDialog.ownTracksOpen = !cueReviewDialog.ownTracksOpen
                        text: qsTr("Moves of less than %1 ms: %n track(s)", "", cueReviewDialog.ownTracks.length)
                            .arg(cueReviewDialog.thresholdMs)
                        Accessible.name: text
                        contentItem: RowLayout {
                            spacing: Theme.tightSpacing
                            SeabassIcon {
                                iconName: cueReviewDialog.ownTracksOpen ? "arrow-down" : "arrow-right"
                                size: Theme.iconSizeSmall * 0.5
                                color: Theme.textMuted
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                font.bold: true
                                text: ownTracksHeading.text
                            }
                        }
                    }
                    Repeater {
                        objectName: "cueReviewOwnTracks"
                        // Made only when opened.
                        model: cueReviewDialog.ownTracksOpen ? cueReviewDialog.ownTracks : []
                        delegate: cueReviewTrackDelegate
                    }
                }
                ColumnLayout {
                    visible: cueReviewDialog.tooDifferent.length > 0
                    Layout.fillWidth: true
                    spacing: Theme.tightSpacing
                    Label {
                        objectName: "cueReviewTooDifferentHeading"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("On these tracks the grid changed too much to carry cue points; they stay where they are.")
                    }
                    Label {
                        objectName: "cueReviewTooDifferentList"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: cueReviewDialog.tooDifferent.join("\n")
                    }
                }
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "cueReviewDoneButton"
                text: qsTr("Done")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
    }

    Menu {
        id: playlistMenu
        objectName: "playlistMenu"
        property string playlist: ""
        readonly property bool staged: root.playlistEditController.pendingRevision >= 0
            && playlist.length > 0 && root.playlistEditController.isPlaylistStaged(playlist)
        // Staged to go with its tracks ("Delete playlist and its tracks").
        readonly property bool tracksStaged: staged && ("isTracksDeleteStaged" in root.playlistEditController)
            && root.playlistEditController.isTracksDeleteStaged(playlist)
        // One staged to be created: the menu takes the create back.
        readonly property bool isNew: root.playlistEditController.pendingRevision >= 0
            && playlist.length > 0 && root.playlistEditController.isNewPlaylistStaged(playlist)
        // Staged to be renamed: the name the stick has, which Keep gives back.
        readonly property string oldName: root.playlistEditController.pendingRevision >= 0 && playlist.length > 0
            && ("pendingRenames" in root.playlistEditController)
            ? root.oldPlaylistName(playlist) : ""
        // Tracks waiting to be imported into this playlist.
        readonly property int importing: root.trackImportController.pendingRevision >= 0 && playlist.length > 0
            ? root.trackImportController.trackCountChange(playlist) : 0
        // Tracks of this playlist waiting for Save with a new analysis.
        readonly property int reanalysing: root.trackImportController.pendingRevision >= 0 && playlist.length > 0
            && ("reanalysisCount" in root.trackImportController)
            ? root.trackImportController.reanalysisCount(playlist) : 0
        // Each of the items up to the separator is enabled only while it
        // is shown: the keys of a menu pass over a disabled item, not over
        // one that is merely hidden, and Return would trigger it.
        MenuItem {
            id: importTracksItem
            objectName: "importTracksItem"
            visible: root.trackImportEnabled && !playlistMenu.isNew && !playlistMenu.staged
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Import tracks...")
            onTriggered: {
                importFileDialog.playlist = playlistMenu.playlist;
                importFileDialog.open();
            }
        }
        MenuItem {
            id: importFolderItem
            objectName: "importFolderItem"
            visible: root.trackImportEnabled && !playlistMenu.isNew && !playlistMenu.staged
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Import a folder...")
            onTriggered: {
                importFolderDialog.playlist = playlistMenu.playlist;
                importFolderDialog.open();
            }
        }
        MenuItem {
            id: takeBackImportItem
            objectName: "takeBackImportItem"
            visible: playlistMenu.importing > 0
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Don't import the %n track(s)", "", playlistMenu.importing)
            onTriggered: root.trackImportController.dropStaged()
        }
        MenuItem {
            id: analyseAgainItem
            objectName: "playlistAnalyseAgainItem"
            visible: root.trackImportEnabled && !playlistMenu.isNew && !playlistMenu.staged
            height: visible ? implicitHeight : 0
            enabled: visible && root.trackImportController.busy !== true
            text: qsTr("Analyse again")
            onTriggered: root.trackImportController.analyseAgain(root.rekordboxPath, root.enginePath, playlistMenu.playlist)
        }
        MenuItem {
            id: takeBackReanalysisItem
            objectName: "takeBackReanalysisItem"
            visible: playlistMenu.reanalysing > 0
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Don't analyse the %n track(s) again", "", playlistMenu.reanalysing)
            onTriggered: root.trackImportController.dropReanalysis(playlistMenu.playlist)
        }
        // The way back from Analyse again: offered whenever something is
        // kept for the playlist's tracks, also with the feature switched off.
        RestoreOriginalAnalysisMenuItem {
            id: restoreAnalysisItem
            // Only with something kept, the switch on or off: it is the
            // way back from Analyse again. Decided when the menu opens.
            visible: kept > 0 && !playlistMenu.isNew && !playlistMenu.staged
            height: visible ? implicitHeight : 0
            controller: root.originalAnalysisController
            rekordboxPath: root.rekordboxPath
            enginePath: root.enginePath
            playlist: playlistMenu.playlist
        }
        // Under the items above whenever one of them is shown.
        MenuSeparator {
            objectName: "playlistImportSeparator"
            visible: importTracksItem.visible || importFolderItem.visible || takeBackImportItem.visible
                || analyseAgainItem.visible || takeBackReanalysisItem.visible || restoreAnalysisItem.visible
            height: visible ? implicitHeight : 0
        }
        MenuItem {
            objectName: "renamePlaylistItem"
            text: qsTr("Rename playlist...")
            onTriggered: {
                renamePlaylistDialog.playlist = playlistMenu.playlist;
                renamePlaylistDialog.open();
            }
        }
        MenuItem {
            objectName: "keepPlaylistNameItem"
            visible: playlistMenu.oldName.length > 0
            height: visible ? implicitHeight : 0
            text: qsTr("Keep the name \"%1\"").arg(playlistMenu.oldName)
            onTriggered: root.playlistEditController.keepPlaylistName(playlistMenu.playlist)
        }
        // Delete asks nothing: it only marks the playlist, nothing is
        // written before Save, and this same item then takes it back
        // (with its tracks, when it was to take those along).
        MenuItem {
            objectName: "deletePlaylistItem"
            text: playlistMenu.isNew ? qsTr("Don't create this playlist")
                : playlistMenu.staged ? (playlistMenu.tracksStaged ? qsTr("Keep this playlist and its tracks")
                                                                   : qsTr("Keep this playlist"))
                : qsTr("Delete playlist")
            onTriggered: {
                if (playlistMenu.isNew) {
                    root.playlistEditController.dropNewPlaylist(playlistMenu.playlist);
                } else if (playlistMenu.staged) {
                    root.playlistEditController.keepPlaylist(playlistMenu.playlist);
                } else {
                    root.playlistEditController.deletePlaylist(root.rekordboxPath, root.enginePath, playlistMenu.playlist);
                }
            }
        }
        // The playlist and the tracks only it holds. This one does ask:
        // files go, and what goes is read from every library on the stick
        // first (deleteWithTracksDialog).
        MenuItem {
            objectName: "deletePlaylistWithTracksItem"
            visible: !playlistMenu.isNew && !playlistMenu.staged
                && ("planDeleteWithTracks" in root.playlistEditController)
            height: visible ? implicitHeight : 0
            text: qsTr("Delete playlist and its tracks...")
            onTriggered: root.playlistEditController.planDeleteWithTracks(root.rekordboxPath, root.enginePath,
                                                                          playlistMenu.playlist)
        }
    }

    // What deleting a playlist with its tracks comes to, as the controller
    // read it from every library on the stick (deleteWithTracksPlanned):
    // the libraries the playlist goes from, the tracks that go with it,
    // and the ones another playlist keeps. OK stages it; nothing is
    // written before Save.
    SeabassDialog {
        id: deleteWithTracksDialog
        objectName: "deleteWithTracksDialog"
        property string playlist: ""
        property var libraries: []
        property int going: 0
        property int staying: 0
        property int withoutFile: 0
        property int megabytes: 0
        function names(list) {
            return list.length <= 1 ? list.join("") : list.slice(0, -1).join(", ") + " and " + list[list.length - 1];
        }
        severity: SeabassDialog.Warning
        title: qsTr("Delete playlist and its tracks")
        messageObjectName: "deleteWithTracksHeadline"
        detailObjectName: "deleteWithTracksDetail"
        headline: going === 0 ? qsTr("Delete \"%1\"? None of its tracks would go with it.").arg(playlist)
            : staying > 0 ? qsTr("Delete \"%1\" and %2 of its %3 tracks from this stick?").arg(playlist).arg(going).arg(going + staying)
            : going === 1 ? qsTr("Delete \"%1\" and its track from this stick?").arg(playlist)
            : qsTr("Delete \"%1\" and its %2 tracks from this stick?").arg(playlist).arg(going)
        detailText: {
            const lines = [qsTr("The playlist is deleted from %1.").arg(names(libraries))];
            if (going > 0) {
                const size = megabytes > 0 ? qsTr(" (%1 MB)").arg(megabytes) : "";
                lines.push(going === 1
                    ? qsTr("Its track is taken out of every library on the stick, and the file%1 is listed for Free Up Space to delete.").arg(size)
                    : qsTr("%1 tracks are taken out of every library on the stick, and their files%2 are listed for Free Up Space to delete.")
                        .arg(going).arg(size));
            }
            if (staying > 0) {
                lines.push(staying === 1 ? qsTr("1 track stays on the stick: another playlist holds it.")
                                         : qsTr("%1 tracks stay on the stick: other playlists hold them.").arg(staying));
            }
            if (withoutFile > 0) {
                lines.push(withoutFile === 1 ? qsTr("1 streaming track has no file here to delete.")
                                             : qsTr("%1 streaming tracks have no file here to delete.").arg(withoutFile));
            }
            lines.push(qsTr("Nothing changes until you Save."));
            return lines.join(" ");
        }
        standardButtons: Dialog.Ok | Dialog.Cancel
        // With the number agreed to: more than that by the time it is
        // staged, and the controller refuses and says so.
        onAccepted: root.playlistEditController.deletePlaylistWithTracks(root.rekordboxPath, root.enginePath,
                                                                         deleteWithTracksDialog.playlist,
                                                                         deleteWithTracksDialog.going)
    }

    // The playlist dialogs wear SeabassDialog's frame, which is what
    // insets their buttons from the edge: a plain Dialog left them flush
    // against the frame's right and bottom border.
    //
    // A new name for a playlist (its last segment), given in every library
    // on the stick on Save.
    SeabassDialog {
        id: renamePlaylistDialog
        objectName: "renamePlaylistDialog"
        // Assigned, as SeabassDialog's selectFooterButton does; set here
        // because OK is still disabled when the name is empty, and that
        // helper passes over a disabled button.
        property string playlist: ""
        severity: SeabassDialog.Question
        title: qsTr("Rename playlist")
        headline: qsTr("It is renamed in every library on this stick.")
        detailText: qsTr("Nothing changes until you Save.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: renamePlaylistNameField.text = playlist.substring(playlist.lastIndexOf("/") + 1)
        // Later than SeabassDialog's own onOpened, which hands focus to a
        // footer button: here the name is what is being asked for. OK is
        // marked as the default, because Return in the field is OK.
        onOpened: Qt.callLater(() => {
            renamePlaylistDialog.markFooterDefault(renamePlaylistDialog.standardButton(Dialog.Ok));
            renamePlaylistNameField.forceActiveFocus();
            renamePlaylistNameField.selectAll();
        })
        Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(
            () => renamePlaylistNameField.text.trim().length > 0)
        TextField {
            id: renamePlaylistNameField
            objectName: "renamePlaylistNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Playlist name")
            onAccepted: {
                if (text.trim().length > 0) {
                    renamePlaylistDialog.accept();
                }
            }
        }
        onAccepted: root.playlistEditController.renamePlaylist(root.rekordboxPath, root.enginePath,
                                                               renamePlaylistDialog.playlist, renamePlaylistNameField.text)
    }

    // The name the stick has for a playlist listed under a staged new one,
    // or "" when none is staged for it.
    function oldPlaylistName(shown) {
        const renames = root.playlistEditController.pendingRenames;
        for (const from in renames) {
            if (renames[from] === shown) {
                return from;
            }
        }
        return "";
    }

    // An add to a playlist some library on the stick has without the
    // track (rekordbox exported it, Engine has not been synced since): the
    // add would leave that library alone and its player show the playlist
    // short, so it waits for an answer. Nothing is staged meanwhile.
    SeabassDialog {
        id: missingLibrariesDialog
        objectName: "missingLibrariesDialog"
        property string playlist: ""
        property string filePath: ""
        property string trackTitle: ""
        property var missing: []
        property var added: []
        readonly property bool engineMissing: missing.indexOf("Engine") >= 0
        readonly property bool pioneerMissing: missing.indexOf("rekordbox") >= 0 || missing.indexOf("OneLibrary") >= 0
        severity: SeabassDialog.Warning
        title: qsTr("Not in every library")
        headline: missingLibrariesDialog.text
        messageObjectName: "missingLibrariesText"
        closePolicy: Popup.CloseOnEscape
        function names(list) {
            return list.length <= 1 ? list.join("") : list.slice(0, -1).join(", ") + " and " + list[list.length - 1];
        }
        readonly property string text: {
            const players = engineMissing && pioneerMissing ? qsTr("Denon and Pioneer players")
                : engineMissing ? qsTr("a Denon player") : qsTr("a Pioneer player");
            const lines = [qsTr("\"%1\" is not in the %2 %3 on this stick, so it would be added to \"%4\" in %5 only, "
                                + "and %6 would not see it there.")
                               .arg(trackTitle).arg(names(missing)).arg(missing.length === 1 ? qsTr("library") : qsTr("libraries"))
                               .arg(playlist).arg(names(added)).arg(players)];
            if (engineMissing) {
                lines.push(qsTr("Run Sync after Rekordbox Export first to bring the track into Engine."));
            }
            if (pioneerMissing) {
                lines.push(qsTr("Export the track to this stick from rekordbox first to bring it into %1.")
                           .arg(names(missing.filter(n => n !== "Engine"))));
            }
            return lines.join(" ");
        }
        // No default marked: SeabassDialog takes the last, Don't add.
        footer: DialogButtonBox {
            Button {
                objectName: "openSyncButton"
                visible: missingLibrariesDialog.engineMissing
                text: qsTr("Open Sync")
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: {
                    missingLibrariesDialog.drop();
                    root.openRekordboxExportSyncRequested();
                }
            }
            Button {
                objectName: "addAnywayButton"
                text: qsTr("Add anyway")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                objectName: "dontAddButton"
                text: qsTr("Don't add")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        // Answered once: whichever closes it first says what happens.
        property bool answered: false
        onAboutToShow: answered = false
        function drop() {
            if (!answered) {
                answered = true;
                root.playlistEditController.dropAddition(playlist, filePath);
            }
            close();
        }
        onAccepted: {
            if (!answered) {
                answered = true;
                root.playlistEditController.addAnyway(playlist, filePath);
            }
        }
        onRejected: drop()
    }
    Connections {
        target: root.playlistEditController
        ignoreUnknownSignals: true
        function onAdditionNeedsLibraries(playlist, filePath, title, missing, toIndex, added) {
            missingLibrariesDialog.playlist = playlist;
            missingLibrariesDialog.filePath = filePath;
            missingLibrariesDialog.trackTitle = title;
            missingLibrariesDialog.missing = missing;
            missingLibrariesDialog.added = added ?? [];
            missingLibrariesDialog.open();
        }
        function onDeleteWithTracksPlanned(plan) {
            deleteWithTracksDialog.playlist = plan.playlist;
            deleteWithTracksDialog.libraries = plan.libraries ?? [];
            deleteWithTracksDialog.going = plan.going ?? 0;
            deleteWithTracksDialog.staying = plan.staying ?? 0;
            deleteWithTracksDialog.withoutFile = plan.withoutFile ?? 0;
            deleteWithTracksDialog.megabytes = plan.megabytes ?? 0;
            deleteWithTracksDialog.open();
        }
    }

    // Playlist edits a crash, a kill or a power cut left staged (Seabass
    // keeps them on this machine while they are, see StagedEditsStore):
    // offered back once the page has its stick and its session. Not while
    // anything is staged into the session or a save is writing: the
    // question waits until the session is clean (the Connections below),
    // so another page's batch saved from here does not leave this one to
    // be overwritten by the first new edit. Nothing is staged until it is
    // answered.
    property bool leftOverEditsChecked: false
    function offerLeftOverEdits() {
        const controller = root.playlistEditController;
        const session = editHost.session;
        if (session === null || typeof controller.leftOverEdits !== "function") {
            return;
        }
        if (session.dirty || session.writing) {
            return;
        }
        const offer = controller.leftOverEdits(root.rekordboxPath, root.enginePath);
        if (offer.count > 0) {
            leftOverEditsDialog.count = offer.count;
            leftOverEditsDialog.stick = offer.stickLabel.length > 0 ? offer.stickLabel : root.stickLabel;
            leftOverEditsDialog.open();
        }
        root.leftOverEditsChecked = true;
    }
    Connections {
        target: editHost.session
        ignoreUnknownSignals: true
        // dirty notifies through pendingChanged, writing through stateChanged.
        function onPendingChanged() {
            if (!root.leftOverEditsChecked) {
                root.offerLeftOverEdits();
            }
        }
        function onStateChanged() {
            if (!root.leftOverEditsChecked) {
                root.offerLeftOverEdits();
            }
        }
    }
    SeabassDialog {
        id: leftOverEditsDialog
        objectName: "leftOverEditsDialog"
        property int count: 0
        property string stick: ""
        severity: SeabassDialog.Question
        title: qsTr("Unsaved playlist edits")
        headline: leftOverEditsDialog.text
        messageObjectName: "leftOverEditsText"
        // Answered, never dismissed: Discard drops a batch that may have
        // taken half an hour, and Keep stages it.
        closePolicy: Popup.NoAutoClose
        readonly property string text: (count === 1
            ? qsTr("Seabass last ran with 1 playlist edit staged for %1 and had not saved it. Keep editing it?").arg(stick)
            : qsTr("Seabass last ran with %1 playlist edits staged for %2 and had not saved them. Keep editing them?")
                .arg(count).arg(stick))
        // Keep is the default (highlighted, so SeabassDialog focuses it):
        // the key that answers takes it.
        footer: DialogButtonBox {
            Button {
                objectName: "keepLeftOverEditsButton"
                text: qsTr("Keep")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                objectName: "discardLeftOverEditsButton"
                text: qsTr("Discard")
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    root.playlistEditController.discardLeftOverEdits(root.rekordboxPath, root.enginePath);
                    leftOverEditsDialog.close();
                }
            }
        }
        onAccepted: root.playlistEditController.keepLeftOverEdits(root.rekordboxPath, root.enginePath)
    }

    // The list's last row: a name for a new, empty playlist, made in every
    // library on the stick on Save.
    SeabassDialog {
        id: createPlaylistDialog
        objectName: "createPlaylistDialog"
        // Assigned, as SeabassDialog's selectFooterButton does; set here
        // because OK is still disabled when the name is empty, and that
        // helper passes over a disabled button.
        severity: SeabassDialog.Question
        title: qsTr("Create new playlist")
        headline: qsTr("It is made in every library on this stick.")
        detailText: qsTr("Nothing changes until you Save.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: newPlaylistNameField.text = ""
        // Later than SeabassDialog's own onOpened, as in the rename dialog.
        onOpened: Qt.callLater(() => {
            createPlaylistDialog.markFooterDefault(createPlaylistDialog.standardButton(Dialog.Ok));
            newPlaylistNameField.forceActiveFocus();
        })
        Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(
            () => newPlaylistNameField.text.trim().length > 0)
        TextField {
            id: newPlaylistNameField
            objectName: "newPlaylistNameField"
            Layout.fillWidth: true
            placeholderText: qsTr("Playlist name")
            onAccepted: {
                if (text.trim().length > 0) {
                    createPlaylistDialog.accept();
                }
            }
        }
        onAccepted: root.playlistEditController.createPlaylist(root.rekordboxPath, root.enginePath,
                                                               newPlaylistNameField.text)
    }

    function openCreatePlaylistDialog() {
        createPlaylistDialog.open();
    }

    Menu {
        id: trackMenu
        objectName: "trackMenu"
        // The row it was opened for: its delegate carries filePath, title,
        // streamingSource and what Merge and Find need.
        property var row: null
        onClosed: row = null
        readonly property string filePath: row ? row.filePath : ""
        readonly property bool streaming: row ? row.streamingSource.length > 0 : false
        // A row of the track import's staged batch: a new track (not on
        // the stick yet) or the stick's own, put into the playlist by it.
        readonly property string importChangeId: row && row.importChangeId ? row.importChangeId : ""
        readonly property bool pendingImport: row !== null && row.pendingImport === true
        // Only for a track in the playlist shown: a Matching result opens
        // this menu too, and need not be in it. A row of the import's
        // batch is taken back by its own entry below.
        readonly property bool canRemove: root.shownPlaylist.length > 0 && filePath.length > 0
            && importChangeId.length === 0
            && (!row || !row.playlistNames || row.playlistNames.indexOf(root.shownPlaylist) >= 0)
        readonly property bool staged: canRemove && root.playlistEditController.pendingRevision >= 0
            && root.playlistEditController.isRemovalStaged(root.shownPlaylist, filePath)
        // In the playlist shown only because it is staged to be added.
        readonly property bool pendingAddition: canRemove && row !== null && row.pendingAddition === true
        // At a place in the playlist shown that a staged move gives it.
        readonly property bool pendingMove: canRemove && row !== null && row.pendingMove === true
        // Adding to a playlist: the playlists this stick
        // has and the ones staged to be created, but not the one shown,
        // which already holds the track.
        readonly property bool canAdd: filePath.length > 0 && !streaming && !pendingImport
        readonly property var addTargets: canAdd
            ? root.addTargetsFor(scanController.playlistNames, root.newPlaylistNames,
                                 row && row.playlistNames ? row.playlistNames : [], root.shownPlaylist) : []
        // The playlist a track was last added to, when this stick has it.
        readonly property string lastAdd: ("lastAddPlaylist" in root.playlistEditController)
            && addTargets.indexOf(root.playlistEditController.lastAddPlaylist) >= 0
            ? root.playlistEditController.lastAddPlaylist : ""
        function addedTo(playlist) {
            return root.playlistEditController.pendingRevision >= 0
                && root.playlistEditController.isAdditionStaged(playlist, filePath);
        }
        function toggleAdd(playlist) {
            if (addedTo(playlist)) {
                root.playlistEditController.keepOutOfPlaylist(playlist, filePath);
            } else {
                root.addTrackToPlaylist(playlist, filePath, row.title, -1, "");
            }
        }
        // The submenu only while it has somewhere to add to.
        onAboutToShow: {
            const shown = trackMenu.indexOfMenu(addToMenu) >= 0;
            if (canAdd && addTargets.length > 0 && !shown) {
                trackMenu.addMenu(addToMenu);
            } else if (!(canAdd && addTargets.length > 0) && shown) {
                trackMenu.removeMenu(addToMenu);
            }
        }
        function indexOfMenu(menu) {
            for (let i = 0; i < count; ++i) {
                const item = itemAt(i);
                if (item && item.subMenu === menu) {
                    return i;
                }
            }
            return -1;
        }
        MenuItem {
            objectName: "mergeTrackItem"
            enabled: root.format !== "onelibrary" && !trackMenu.streaming && !trackMenu.pendingImport
            text: trackMenu.streaming ? qsTr("Merge with another track... (streaming track, no local file)")
                : trackMenu.pendingImport ? qsTr("Merge with another track... (after saving)")
                : root.format === "onelibrary" ? qsTr("Merge with another track... (not on OneLibrary yet)")
                : qsTr("Merge with another track...")
            onTriggered: mergePickerPopup.showFor(trackMenu.row)
        }
        MenuItem {
            objectName: "findMatchingItem"
            enabled: !trackMenu.pendingImport
            text: trackMenu.pendingImport ? qsTr("Find matching tracks (after saving)") : qsTr("Find matching tracks")
            onTriggered: root.toggleAnchor(trackMenu.row)
        }
        // A new track's playlists are the import's: "Add to" waits for
        // the save, said rather than left out.
        MenuItem {
            objectName: "addToAfterSaveItem"
            visible: trackMenu.pendingImport
            height: visible ? implicitHeight : 0
            enabled: false
            text: qsTr("Add to... (after saving)")
        }
        MenuSeparator {
            visible: trackMenu.canRemove || dontImportItem.visible
            height: visible ? implicitHeight : 0
        }
        // Takes this one row out of the import's batch; the last row takes
        // the batch back whole.
        MenuItem {
            id: dontImportItem
            objectName: "dontImportItem"
            // Not for a row the last save wrote, shown until the stick is
            // read again: nothing of it waits any more.
            visible: trackMenu.importChangeId.length > 0
                && (root.stagedImport.rows ?? []).some(row => row.changeId === trackMenu.importChangeId)
            height: visible ? implicitHeight : 0
            text: trackMenu.row ? root.dontImportText(trackMenu.row) : ""
            onTriggered: root.trackImportController.dropStagedTrack(trackMenu.importChangeId)
        }
        MenuItem {
            objectName: "removeFromPlaylistItem"
            visible: trackMenu.canRemove
            height: visible ? implicitHeight : 0
            text: trackMenu.pendingAddition ? qsTr("Don't add to \"%1\"").arg(root.shownPlaylist)
                : trackMenu.staged ? qsTr("Keep in \"%1\"").arg(root.shownPlaylist)
                : qsTr("Remove from \"%1\"").arg(root.shownPlaylist)
            onTriggered: {
                if (trackMenu.pendingAddition) {
                    root.playlistEditController.keepOutOfPlaylist(root.shownPlaylist, trackMenu.filePath);
                } else if (trackMenu.staged) {
                    root.playlistEditController.keepInPlaylist(root.shownPlaylist, trackMenu.filePath);
                } else {
                    root.playlistEditController.removeFromPlaylist(root.rekordboxPath, root.enginePath, root.shownPlaylist,
                                                                   trackMenu.filePath, trackMenu.row.title);
                }
            }
        }
        MenuItem {
            objectName: "keepPositionItem"
            visible: trackMenu.pendingMove
            height: visible ? implicitHeight : 0
            text: qsTr("Keep at its place in \"%1\"").arg(root.shownPlaylist)
            onTriggered: root.playlistEditController.keepPositionInPlaylist(root.shownPlaylist, trackMenu.filePath)
        }
        MenuSeparator {
            visible: trackMenu.canAdd && !trackMenu.canRemove
            height: visible ? implicitHeight : 0
        }
        MenuItem {
            objectName: "addToLastPlaylistItem"
            visible: trackMenu.lastAdd.length > 0
            height: visible ? implicitHeight : 0
            text: trackMenu.addedTo(trackMenu.lastAdd) ? qsTr("Don't add to \"%1\"").arg(trackMenu.lastAdd)
                                                       : qsTr("Add to \"%1\"").arg(trackMenu.lastAdd)
            onTriggered: trackMenu.toggleAdd(trackMenu.lastAdd)
        }
    }

    // The row menu's last entry: every playlist to add the track to, a
    // tick on the ones it is staged into (picking one again takes it back).
    //
    // The entries are kept, one per playlist name, and the list is
    // brought in line with the targets by moving, adding and taking out
    // entries, never by making it afresh. An Instantiator over the
    // targets made every entry anew on every change and destroyed the
    // old ones, and under org.kde.breeze and org.kde.desktop the menu's
    // own ListView keeps the entries it has scrolled past and does not
    // hear that they went: the next wheel read a destroyed one (the
    // 2026-10-17 Browse crash, the shape 31d3c5d6 ended for the Matching
    // list; tst_ScanPageStorm's Add To round found this one under those
    // styles, where Basic, the storm lane's style, never holds an entry).
    // An entry whose playlist is gone from the targets is taken out of
    // the menu and kept, hidden, for as long as the page lives: a few
    // items at most, and nothing a list still holds is ever destroyed.
    Menu {
        id: addToMenu
        objectName: "addToMenu"
        title: qsTr("Add to...")

        // playlist name -> its MenuItem: in the menu (entries) or taken
        // out of it and kept hidden (parked), to go back in when its
        // playlist is a target again.
        //
        // Maps, not plain objects: every close of the row menu moves every
        // name from one to the other and every open moves it back, and a
        // plain object given and stripped of the same names over and over
        // runs into a Qt 6.11.1 bug (QTBUG-147153, fixed in 6.11.2): at
        // the 255th such change the object's class is rebuilt, and a name
        // added again is written at its old slot, past the object's end,
        // over whatever the JS heap holds there. The 2026-11-10 crash in
        // tst_ScanPageStorm's Add To round was the collector later freeing
        // a pointer that write had put in a neighbouring object.
        property var entries: new Map()
        property var parked: new Map()

        Component {
            id: addToEntryComponent
            MenuItem {
                property string playlist: ""
                objectName: "addTo_" + playlist
                text: playlist
                checkable: true
                checked: trackMenu.filePath.length > 0 && trackMenu.addedTo(playlist)
                onTriggered: {
                    trackMenu.toggleAdd(playlist);
                    // A click flips `checked` itself; the staging says.
                    checked = Qt.binding(() => trackMenu.filePath.length > 0 && trackMenu.addedTo(playlist));
                }
            }
        }

        function syncEntries() {
            const targets = trackMenu.addTargets;
            const wanted = new Set(targets);
            // Out: the entries whose playlist is no longer a target.
            // takeItem, never removeItem: removeItem destroys the item,
            // which is the one thing this must not do.
            for (const [name, item] of Array.from(addToMenu.entries)) {
                if (wanted.has(name)) {
                    continue;
                }
                addToMenu.takeItem(addToMenu.indexOfEntry(item));
                item.visible = false;
                addToMenu.parked.set(name, item);
                addToMenu.entries.delete(name);
            }
            // In and in order: each target's entry at the target's index.
            for (let i = 0; i < targets.length; ++i) {
                const name = targets[i];
                let item = addToMenu.entries.get(name);
                if (item === undefined) {
                    item = addToMenu.parked.get(name);
                    if (item !== undefined) {
                        addToMenu.parked.delete(name);
                        item.visible = true;
                    } else {
                        item = addToEntryComponent.createObject(addToMenu, {playlist: name});
                    }
                    addToMenu.entries.set(name, item);
                    addToMenu.insertItem(i, item);
                    continue;
                }
                const at = addToMenu.indexOfEntry(item);
                if (at !== i) {
                    addToMenu.moveItem(at, i);
                }
            }
        }
        function indexOfEntry(item) {
            for (let i = 0; i < addToMenu.count; ++i) {
                if (addToMenu.itemAt(i) === item) {
                    return i;
                }
            }
            return -1;
        }
        Connections {
            target: trackMenu
            function onAddTargetsChanged() { addToMenu.syncEntries(); }
        }
        Component.onCompleted: addToMenu.syncEntries()
    }

    // The playlists a track can be added to: every one but the playlist
    // shown when the track is in it (inPlaylists: the row's own
    // memberships). A Browse row of the playlist shown is in it; a
    // Matching result is found while looking at the playlist it is wanted
    // in, so for it the shown playlist is a target like any other. The
    // new playlists first: they are what is being put together, and a
    // stick's full list runs past the bottom of the window, where a last
    // entry is out of reach.
    function addTargetsFor(stickPlaylists, newPlaylists, inPlaylists, shown) {
        return newPlaylists.concat(stickPlaylists.filter(n => newPlaylists.indexOf(n) < 0))
            .filter(n => n !== shown || inPlaylists.indexOf(n) < 0);
    }

    readonly property var newPlaylistNames: (("newPlaylists" in root.playlistEditController)
            ? root.playlistEditController.newPlaylists : [])
        .concat(root.trackImportController.stagedPlaylistIsNew === true ? [root.trackImportController.stagedPlaylist] : [])
    // The tracks staged into a playlist show in it, greyed, until Save.
    readonly property var pendingAdditions: ("pendingAdditions" in root.playlistEditController)
        ? root.playlistEditController.pendingAdditions : ({})
    onPendingAdditionsChanged: scanController.setPendingEdits(root.pendingAdditions, root.pendingMoves)
    // The tracks staged to move within a playlist show at their new place,
    // greyed, until Save.
    readonly property var pendingMoves: ("pendingMoves" in root.playlistEditController)
        ? root.playlistEditController.pendingMoves : ({})
    onPendingMovesChanged: scanController.setPendingEdits(root.pendingAdditions, root.pendingMoves)
    // The track import's staged batch shows in the list until Save: its
    // new tracks as rows of their own, in "All tracks" and at their place
    // in the playlist, greyed and marked new; the stick's tracks it puts
    // into the playlist greyed there, as an add is. Read from the changes
    // the save will apply (TrackImportController::stagedImport).
    readonly property var stagedImport: ("stagedImport" in root.trackImportController)
        ? root.trackImportController.stagedImport : ({})
    onStagedImportChanged: scanController.setStagedImport(root.stagedImport)
    // The stick read again after a save (or a read that failed): the rows
    // the save wrote are the stick's own now, or gone with the rest.
    function releaseLandedRows() {
        if ((root.stagedImport.landed ?? []).length > 0 && ("releaseLanded" in root.trackImportController)) {
            root.trackImportController.releaseLanded();
        }
    }
    // "Don't import" for one row of the batch: what its menu says, by
    // what goes with it.
    function dontImportText(row) {
        const rows = (root.stagedImport.rows ?? []).length;
        const last = rows <= 1;
        const playlist = root.stagedImport.playlist ?? "";
        const madeToo = last && root.stagedImport.isNew === true && playlist.length > 0;
        if (row && row.pendingImport === true) {
            return madeToo ? qsTr("Don't import (the new playlist \"%1\" is not made either)").arg(playlist)
                           : qsTr("Don't import");
        }
        return madeToo ? qsTr("Don't add to \"%1\" (the new playlist is not made either)").arg(playlist)
                       : qsTr("Don't add to \"%1\"").arg(playlist);
    }
    // What a row not on the stick yet cannot do until Save.
    readonly property string afterSaveText: qsTr("Not on the stick yet. Playing, editing and dragging it are "
                                                 + "available after saving.")
    // A row the save wrote, until the stick has been read again.
    readonly property string awaitingReadText: qsTr("Saved to the stick. Playing, editing and dragging it are "
                                                    + "available once the stick has been read again.")
    // The details pane, for a row that stands for a track a library has
    // read: none for a row of the import's (a track not on the stick
    // yet, or one the save wrote before the stick was read again).
    function openDetails(row) {
        if (row.unreadable === true || row.recording === true) {
            return false;
        }
        trackDetailPanel.showFor(row);
        root.trackPanelOpen = true;
        return true;
    }
    // A playlist staged to be renamed is listed under its new name, its
    // tracks the scanned playlist's.
    readonly property var pendingRenames: ("pendingRenames" in root.playlistEditController)
        ? root.playlistEditController.pendingRenames : ({})
    onPendingRenamesChanged: scanController.setPlaylistAliases(root.pendingRenames)

    function openPlaylistMenu(name, row) {
        playlistMenu.playlist = name;
        playlistMenu.popup(row);
    }

    // The row's menu, at `at` (the row's menu button, or the row itself
    // for a right click).
    function openTrackMenu(row, at) {
        // A row the save wrote has nothing to offer until it is read.
        if (row.awaitingRead === true) {
            return;
        }
        // A recording none of a track's actions (recordingRowText): its own.
        if (row.recording === true) {
            recordingMenu.row = row;
            recordingMenu.popup(at);
            return;
        }
        trackMenu.row = row;
        trackMenu.popup(at);
    }

    // Own Recordings' row has no playlist menu (no rename, no delete):
    // only the taking back of what waits for Save about its playlist, in
    // the words a staged playlist's menu has for it. None: no menu.
    function openOwnRecordingsMenu(row) {
        if (root.ownRecordingsMadeByDrag || root.ownRecordingsPendingDelete || root.ownRecordingsRename !== null
                || root.trackImportController.trackCountChange(root.ownRecordingsPlaylist) > 0) {
            ownRecordingsMenu.popup(row);
        }
    }
    Menu {
        id: ownRecordingsMenu
        objectName: "ownRecordingsMenu"
        // Read when it opens, and while open as the staging changes.
        readonly property int importing: root.trackImportController.pendingRevision >= 0
            ? root.trackImportController.trackCountChange(root.ownRecordingsPlaylist) : 0
        MenuItem {
            objectName: "ownRecordingsDontCreateItem"
            visible: root.ownRecordingsMadeByDrag
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Don't create this playlist")
            onTriggered: root.playlistEditController.dropNewPlaylist(root.ownRecordingsPlaylist)
        }
        MenuItem {
            objectName: "ownRecordingsDontImportItem"
            visible: ownRecordingsMenu.importing > 0
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Don't import the %n track(s)", "", ownRecordingsMenu.importing)
            onTriggered: root.trackImportController.dropStaged()
        }
        MenuItem {
            objectName: "ownRecordingsKeepItem"
            visible: root.ownRecordingsPendingDelete
            enabled: visible
            height: visible ? implicitHeight : 0
            text: qsTr("Keep this playlist")
            onTriggered: root.playlistEditController.keepPlaylist(root.ownRecordingsPlaylist)
        }
        // As a renamed playlist's own menu says it; from either side.
        MenuItem {
            objectName: "ownRecordingsKeepNameItem"
            visible: root.ownRecordingsRename !== null
            enabled: visible
            height: visible ? implicitHeight : 0
            text: root.ownRecordingsRename !== null ? qsTr("Keep the name \"%1\"").arg(root.ownRecordingsRename.from) : ""
            onTriggered: root.playlistEditController.keepPlaylistName(root.ownRecordingsPlaylist)
        }
    }

    // A recording of Own Recordings: saved as FLAC with the Recordings
    // page's form, or shown there, where it is deleted. Nothing else.
    Menu {
        id: recordingMenu
        objectName: "recordingMenu"
        property var row: null
        onClosed: row = null
        readonly property string filePath: row ? row.filePath : ""
        MenuItem {
            objectName: "saveAsFlacItem"
            // The Recordings page's rule: nothing is saved from a stick
            // while recordings are deleted from it.
            readonly property bool deleting: root.recordingsBeingDeleted()
            text: deleting ? qsTr("Save as FLAC... (once the recordings being deleted are gone)") : qsTr("Save as FLAC...")
            enabled: recordingMenu.row !== null && !deleting
                && keepForm.offers(recordingMenu.filePath, recordingMenu.row.recordingHoldsNoAudio)
                && !root.recordingSaveInProgress(recordingMenu.filePath)
            onTriggered: root.openSaveAsFlac(recordingMenu.filePath)
        }
        // The FLAC Save as FLAC made, imported into Own Recordings'
        // playlist as any import (analysed, staged, written by Save); never
        // the WAV itself, which would be a second copy of up to 2 GB in the
        // music folder. Shown with the reason while it cannot be.
        MenuItem {
            objectName: "addToLibraryItem"
            readonly property string inLibraryAs: recordingMenu.row ? (recordingMenu.row.recordingInLibraryAs ?? "") : ""
            readonly property string flac: recordingMenu.row ? root.keptFlacFor(recordingMenu.filePath) : ""
            readonly property string saveState: recordingMenu.row && root.keepController !== null
                ? ((root.keepController.revision,
                    root.keepController.outcomeFor(scanController.recordingsRoot, recordingMenu.filePath)).state ?? "")
                : ""
            // That FLAC is in the import that waits for Save already.
            readonly property bool waits: flac.length > 0 && root.trackImportController.pendingRevision >= 0
                && (root.stagedImport.rows ?? []).some(staged => staged.sourceFile === flac)
            readonly property bool changed: saveState === "kept" && flac.length === 0 && root.keepController !== null
                && ("keptFlacChanged" in root.keepController)
                && (root.keepController.revision,
                    root.keepController.keptFlacChanged(scanController.recordingsRoot, recordingMenu.filePath)) === true
            enabled: flac.length > 0 && inLibraryAs.length === 0 && !waits && !root.ownRecordingsPendingDelete
                && root.ownRecordingsRename === null && !root.ownRecordingsMadeByDrag
            text: inLibraryAs.length > 0 ? qsTr("Add to the library (it is there as \"%1\")").arg(inLibraryAs)
                : waits ? qsTr("Add to the library (waits for Save)")
                : flac.length > 0 && root.ownRecordingsPendingDelete
                    ? qsTr("Add to the library (\"%1\" is to be deleted when you Save)").arg(root.ownRecordingsPlaylist)
                : flac.length > 0 && root.ownRecordingsRename !== null
                    ? qsTr("Add to the library (\"%1\" is to be renamed when you Save)").arg(root.ownRecordingsPlaylist)
                : flac.length > 0 && root.ownRecordingsMadeByDrag
                    ? qsTr("Add to the library (once Save has made \"%1\")").arg(root.ownRecordingsPlaylist)
                : flac.length > 0 ? qsTr("Add to the library")
                : saveState === "queued" || saveState === "running" ? qsTr("Add to the library (once saved as FLAC)")
                : changed ? qsTr("Add to the library (its FLAC is no longer the one that was saved: drop the FLAC "
                                 + "onto the row instead)")
                : saveState === "kept" ? qsTr("Add to the library (its FLAC is no longer where it was saved)")
                : qsTr("Add to the library (save it as FLAC first)")
            onTriggered: root.addRecordingToLibrary(recordingMenu.filePath)
        }
        MenuItem {
            objectName: "showOnRecordingsPageItem"
            text: qsTr("Show on the Recordings page")
            onTriggered: root.recordingsPageRequested(recordingMenu.filePath)
        }
    }
    KeepRecordingForm {
        id: keepForm
        objectName: "keepForm"
        keepController: root.keepController
        // The stick the form is for, which no page of Browse's hands on.
        stickRoot: scanController.recordingsRoot
        handsStickRoot: true
    }
    // Recordings are being deleted from this stick (the Recordings page).
    function recordingsBeingDeleted() {
        return root.keepController !== null && ("deletingOn" in root.keepController)
            && (root.keepController.revision, root.keepController.deletingOn(scanController.recordingsRoot)) === true;
    }
    // A save of the recording at `path` is asked for and not ended: not
    // asked for again (the Recordings page's rule).
    function recordingSaveInProgress(path) {
        if (root.keepController === null) {
            return false;
        }
        const state = (root.keepController.revision,
                       root.keepController.outcomeFor(scanController.recordingsRoot, path)).state ?? "";
        return state === "queued" || state === "running";
    }
    // The FLAC the recording at `path` was saved as, while it is there; "".
    function keptFlacFor(path) {
        if (root.keepController === null || !("keptFlacFor" in root.keepController)) {
            return "";
        }
        return (root.keepController.revision, root.keepController.keptFlacFor(scanController.recordingsRoot, path));
    }
    // At the top of the playlist: Own Recordings lists the newest first.
    function addRecordingToLibrary(path) {
        const flac = root.keptFlacFor(path);
        if (flac.length > 0) {
            root.importFiles([flac], root.ownRecordingsPlaylist, false, 0);
        }
    }
    function openSaveAsFlac(path) {
        const recording = scanController.recordingAt(path);
        if (recording.path === undefined) {
            return;
        }
        if (!keepForm.openFor(recording.path, recording.fileName, recording.modified, recording.unfinished,
                              recording.heldSeconds, recording.declaredSeconds, recording.declaredUnusable,
                              recording.missingBytes)) {
            playlistEditMessage.show(qsTr("It is not known which stick this recording is on. Read the stick again, "
                                          + "then save it as FLAC."), true, qsTr("Not saved"));
        }
    }

    // ---- Dragging a track to a playlist ----
    // The drag card Browse's rows and the Matching panel's results share.
    readonly property var trackDragCard: trackDrag
    // The page's half of the drag contract (TrackDragItem has the rest):
    // whether a track is being dragged, and which, as TrackDragItem.
    // asMap() gives it, null when none is.
    readonly property bool dragging: trackDrag.dragging
    property var draggedTrack: null
    onDraggingChanged: {
        root.draggedTrack = root.dragging ? trackDrag.asMap() : null;
        if (root.dragging) {
            // Where the track was picked up, kept for the drop: a playlist
            // held open on the way (spring-loading) is not where it came
            // from.
            root.dragSource = root.moveSourceFor(trackDrag);
            root.springOpened = "";
        }
    }
    // The playlist the dragged track was picked up in ("" when it came
    // from no playlist shown), and the one the drag has opened since.
    property string dragSource: ""
    property string springOpened: ""

    // A Move takes the track out of the playlist Browse shows, so it is
    // offered only for a track that is in it.
    function moveSourceFor(track) {
        return root.shownPlaylist.length > 0 && track.playlistNames
            && track.playlistNames.indexOf(root.shownPlaylist) >= 0 ? root.shownPlaylist : "";
    }
    // What a drop on `target` moves the track out of: where it was picked
    // up, unless that is the target itself.
    function dropSourceFor(target) {
        return root.dragSource.length > 0 && root.dragSource !== target ? root.dragSource : "";
    }

    // A dragged track held over a playlist opens it, as a click on the row
    // would, and the drag goes on in it.
    function springLoad(name) {
        const index = scanController.playlistNames.indexOf(name);
        root.selectPlaylist(index >= 0 ? index + 1 : -1, name);
        root.springOpened = name;
    }

    // Browse is read-only until a cue is added: the session is opened
    // here (no lock), the first staged cue takes the lock and enables
    // the floating Save.
    EditSessionHost {
        id: editHost
        objectName: "editHost"
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "addcue"
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

    // A save that landed whole, said over the list for a moment: what the
    // playlist edits did (the controller words those), or what the save
    // wrote, in its own unit.
    ConfirmationToast {
        id: savedToast
        objectName: "savedToast"
        parent: trackListView
        anchors.centerIn: parent
    }
    Connections {
        target: editHost.session
        ignoreUnknownSignals: true
        function onSaveFinished(summary) {
            // After the controller's own slot for this signal, which words
            // the edits: it attached to the session later than this page did.
            // The page may be gone by the time this runs: a save finishing
            // as the user leaves (the storm walks off mid-save), and root
            // is null then.
            Qt.callLater(() => { if (root) { root.showSaved(summary); } });
        }
    }
    function showSaved(summary) {
        const whole = summary && !(summary.error && summary.error.length > 0)
            && !(summary.warning && summary.warning.length > 0) && summary.cancelled !== true
            && !(summary.skipped > 0) && summary.written !== undefined && summary.written === summary.total;
        if (!whole || !(summary.written > 0)) {
            return;
        }
        const edits = ("lastSavedSummary" in root.playlistEditController)
            ? root.playlistEditController.lastSavedSummary : "";
        savedToast.show(SaveSummary.isUndo(summary) ? SaveSummary.undoneText()
            : edits.length > 0 ? edits : summary.written + " " + summary.unit + " " + summary.verb);
    }

    property int selectedPlaylistIndex: 0

    // Single chokepoint for changing which playlist Browse shows --
    // both PlaylistListView instances' onPlaylistPicked and
    // restoreSelectedPlaylist() below funnel through this, so filtering
    // and persisting the choice (AppSettingsController.
    // lastPlaylistName) can't drift out of sync with each other.
    // remember is false only for the fallback below, which must not
    // forget a playlist because this library happens not to have it.
    function selectPlaylist(index, name, remember) {
        // Own Recordings' playlist is that row (a spring-load, a restore).
        if (index !== 0 && name === root.ownRecordingsPlaylist && root.ownRecordingsListed) {
            root.showOwnRecordings();
            return;
        }
        const fromRecordings = root.showingRecordings;
        root.showingRecordings = false;
        root.selectedPlaylistIndex = index;
        // -1: a playlist staged to be created, listed below the others.
        root.shownPlaylist = index === 0 ? "" : name;
        scanController.filterByPlaylist(index === 0 ? "" : name);
        if (remember !== false) {
            root.appSettingsController.lastPlaylistName = index === 0 ? "" : name;
        }
        // The library's rows are the queue again.
        if (fromRecordings && !scanController.busy) {
            root.offerQueue();
        }
    }

    // Own Recordings shown: its playlist is the one shown, Matching's
    // target once the stick has it (the whole library until then), and the
    // playlist remembered for the next visit stays the one picked last.
    function showOwnRecordings() {
        const index = scanController.playlistNames.indexOf(root.ownRecordingsPlaylist);
        root.selectedPlaylistIndex = index >= 0 ? index + 1 : 0;
        root.shownPlaylist = root.ownRecordingsPlaylist;
        root.showingRecordings = true;
        scanController.showRecordings();
        root.offerQueue();
    }

    // Called once scanController.playlistNames is populated (see
    // onPlaylistNamesChanged above). Falls back to "All tracks" if the
    // last-selected name doesn't match any playlist on this stick --
    // e.g. it was deleted, or this is a different stick than last time --
    // and keeps remembering it, since the next stick may well have it.
    // A staged rename lists it under its new name, and a take-back under
    // its old one again: currentPlaylistName follows it there.
    function restoreSelectedPlaylist() {
        if (root.showingRecordings && root.ownRecordingsListed) {
            root.showOwnRecordings();
            return;
        }
        const wanted = scanController.currentPlaylistName(root.appSettingsController.lastPlaylistName);
        const idx = wanted.length > 0 ? scanController.playlistNames.indexOf(wanted) : -1;
        root.selectPlaylist(idx >= 0 ? idx + 1 : 0, wanted, false);
    }

    // ---- Matching (see docs/experimental-features.md for its history)
    // -- the playlist selection above is shared between the
    // always-on left Pane (when this is off) and the off-canvas Drawer +
    // the panel's own "This Playlist" chip (when it's on); the anchor
    // properties below track whichever Browse row's edit button was last
    // clicked, read by MatchingPage to find compatible tracks. ----
    // Matching graduated from experimental on 2026-09-17, so this page has
    // one layout: the playlists sidebar and the panel, no classic playlist
    // column behind a setting. Its PREVIEW badge came off on 2026-11-19: a
    // result is added to a playlist through Browse's row menu or a drag,
    // staged and saved like any playlist edit here.
    //
    // Closed by default -- only the edit button (or the panel's own close
    // button) toggles it, so arriving in Browse doesn't open it until a
    // track is actually being edited.
    property bool matchingPanelOpen: false
    // The track details column: opened by clicking a row, closed by its
    // own button. Sits between the track list and the Matching panel.
    property bool trackPanelOpen: false

    // This page's list is the player's queue: next, previous and what
    // plays when a track ends are the rows of the list as it is sorted
    // and filtered now. Offered whenever a scan has filled the list, so a
    // track played from the details pane has a queue too.
    // The recordings among them are no queue's rows: the player passes
    // over them (PlaybackController skips a row with `recording`).
    function offerQueue() {
        root.playbackController.setQueue(scanController.tracks, root.format, root.currentPath());
    }

    // When the player moves on by itself -- the next track, the previous,
    // the end of one -- and the pane was showing the track it left, the
    // pane goes with it. A pane turned to some other track is being read,
    // and stays.
    Connections {
        target: scanController
        function onBusyChanged() {
            if (!scanController.busy) {
                root.offerQueue();
            }
        }
    }
    Connections {
        target: root.playbackController
        function onAdvanced(previousSourceId) {
            var row = root.playbackController.currentQueueRow();
            // Not while a cue is being placed in the pane: following would
            // turn the pane to another track and the placement would be
            // gone without a word.
            var placing = trackDetailPanel.pendingPositionMs >= 0;
            if (row >= 0 && root.trackPanelOpen && !placing && trackDetailPanel.trackSourceId === previousSourceId
                    && trackDetailPanel.trackFormat === root.playbackController.currentFormat
                    && trackDetailPanel.trackLibraryPath === root.playbackController.currentLibraryPath
                    && root.playbackController.currentFormat === root.format
                    && root.playbackController.currentLibraryPath === root.currentPath()) {
                root.openDetails(scanController.tracks.trackAt(row));
            }
        }
    }

    // The loaded track's row, if it is this library's and in the list
    // as filtered now, hands the player the cues it holds.
    function handThePlayerItsCues() {
        const player = root.playbackController;
        if (!player.hasTrack || player.currentFormat !== root.format
                || player.currentLibraryPath !== root.currentPath()) {
            return;
        }
        const row = scanController.tracks.indexOfSourceId(player.currentSourceId);
        if (row >= 0) {
            player.takeCues(root.format, root.currentPath(), player.currentSourceId,
                scanController.tracks.trackAt(row).cues);
        }
    }

    // Plays a row's track and turns the details pane to it. The pane is
    // where the playing track is shown as a ring, so a track played from
    // the list while the pane was shut, or on another track, played with
    // nothing to show for it.
    function playRow(row) {
        if (row.unreadable === true) {
            return;
        }
        if (row.recording === true) {
            root.playRecording(row);
            return;
        }
        root.offerQueue();
        root.playbackController.load(root.format, root.currentPath(), row.sourceId, row.filePath, row.title,
            row.artist, row.artworkPath, row.cues);
        root.openDetails(row);
    }
    // A recording plays as the Recordings page plays one: a file no catalog
    // lists, with no queue (offerQueue()) and no details pane.
    function playRecording(row) {
        root.playbackController.loadFile(root.format, root.currentPath(), row.filePath, row.title,
            qsTr("Own Recordings"));
    }
    // A recording row's second line: when it was recorded, its size, and
    // whether the player finished its WAV or left one without audio.
    function recordingDetails(row) {
        const parts = [];
        if (row.recordedAt && !isNaN(row.recordedAt.getTime())) {
            parts.push(row.recordedAt.toLocaleString(Qt.locale(), Locale.ShortFormat));
        }
        parts.push(Theme.humanBytes(row.recordingSizeBytes));
        // As the Recordings page says it of the same file.
        if (row.recordingHoldsNoAudio) {
            parts.push(qsTr("holds no audio"));
        } else if (row.recordingUnfinished) {
            parts.push(qsTr("not finished by the player"));
        }
        return parts.join(" · ");
    }
    // Starts open, like the playlist column it replaced -- the header pill
    // collapses and expands it, unlike matchingPanelOpen above, which starts
    // collapsed.
    property bool playlistSidebarOpen: true
    // The playlist shown, by the name it is listed under: a new one staged
    // to be created (picked at index -1, not among the scan's names) and a
    // renamed one's new name included.
    readonly property string currentPlaylistLabel: root.showingRecordings ? "Own Recordings"
        : root.shownPlaylist.length > 0 ? root.shownPlaylist : "All tracks"

    // How much of Browse's own row content fits once the panel above has
    // taken its share of the window: 2 (comfortable) shows every column,
    // 1 (tight) drops Key/BPM/Time/Cues/Plays down to just artwork+title/
    // artist, 0 (very tight) drops the title/artist column too, leaving
    // only artwork -- with the full details one hover away, see that
    // Rectangle's own tooltip below -- rather than letting every column
    // get squeezed illegibly thin at once.
    //
    // Measured on the column the SplitView gives Browse, not on the list
    // inside it. The tier decides which headers show, the headers' row
    // sets the column's minimum width, and a ColumnLayout narrower than
    // its minimum lays its children out at the minimum: with every
    // header showing, the list came out 656 wide on a 388-wide column,
    // the tier stayed at 2, and the headers never went. Whether that
    // loop closed depended on the font -- not with macOS's, but with the
    // wider one the offscreen platform (and so the test suite) uses.
    //
    // The full tier's threshold counts the Plays column only while it is
    // shown (AppSettingsController.showPlayCount): without it the other
    // columns fit that much sooner, and the step to the full tier comes
    // where they do.
    readonly property bool showPlayCount: root.appSettingsController.showPlayCount === true
    readonly property real fullTierWidth: 620 - (root.showPlayCount ? 0 : root.playsColumnWidth + root.trackColumnSpacing)
    readonly property int browseTier: browseColumn.width >= root.fullTierWidth ? 2 : (browseColumn.width >= 340 ? 1 : 0)

    // The list's sort, held here so the column headers and the Sort by
    // combo show one state. A header click sorts by its column, ascending
    // for a new column and flipped for the sorted one; a third click, on a
    // column sorted descending, goes back to Playlist Order, the order the
    // tracks have in the playlist on the player (with All tracks, the
    // order the catalog lists them in). The combo picks
    // the field and keeps the direction. Playlist Order and Artist have
    // no column, so no header shows their direction: they always sort
    // ascending, and the direction the headers last showed is kept for
    // the next column sort rather than applied where nobody can see it.
    // Plays only while its column is shown.
    readonly property var sortOptions: [
        { text: "Playlist Order", value: "playlist" },
        { text: "Title", value: "title" },
        { text: "Artist", value: "artist" },
        { text: "Key", value: "key" },
        { text: "BPM", value: "bpm" },
        { text: "Duration", value: "duration" },
        { text: "Cues", value: "cues" },
        { text: "Plays", value: "plays" },
    ].filter(option => option.value !== "plays" || root.showPlayCount)
    readonly property var columnSortKeys: ["title", "key", "bpm", "duration", "cues", "plays"]
        .filter(key => key !== "plays" || root.showPlayCount)
    // A list sorted by a column that goes away is put back in Playlist
    // Order, rather than kept in an order nothing on screen explains.
    onShowPlayCountChanged: {
        if (!root.showPlayCount && root.sortField === "plays") {
            root.sortField = "playlist";
            root.sortAscending = true;
            root.applySort();
        }
    }
    property string sortField: "playlist"
    property bool sortAscending: true
    // What Playlist Order is for the list shown, as the headers' tooltips
    // name it.
    readonly property string unsortedOrderName: root.showingRecordings
        ? "the playlist's own order, then the newest recording first"
        : root.shownPlaylist.length > 0 ? "the playlist's own order" : "the library's own order"
    readonly property bool sortedByColumn: root.columnSortKeys.indexOf(root.sortField) >= 0

    function applySort() {
        scanController.setSort(root.sortField, root.sortedByColumn ? root.sortAscending : true);
    }
    function sortByColumn(key) {
        if (root.sortField === key && !root.sortAscending) {
            root.sortField = "playlist";
            root.sortAscending = true;
        } else if (root.sortField === key) {
            root.sortAscending = false;
        } else {
            root.sortField = key;
            root.sortAscending = true;
        }
        root.applySort();
    }
    function sortByField(key) {
        root.sortField = key;
        root.applySort();
    }

    property string anchorSourceId: ""
    property string anchorTitle: ""
    property string anchorArtist: ""
    property string anchorKey: ""
    property double anchorBpm: 0
    property string anchorArtworkPath: ""
    property string anchorFallbackArtworkPath: ""
    property var anchorPlaylistNames: []

    // Toggles: clicking edit on the row that's already the open panel's
    // anchor closes it again; clicking it on any other row (or opening
    // fresh) sets that row as the anchor and (re)opens the panel, which
    // then updates live since every anchor* property below is a plain
    // binding on MatchingPage's own required properties.
    function toggleAnchor(delegate) {
        if (root.matchingPanelOpen && delegate.sourceId === root.anchorSourceId) {
            root.matchingPanelOpen = false;
            return;
        }
        root.anchorSourceId = delegate.sourceId;
        root.anchorTitle = delegate.title;
        root.anchorArtist = delegate.artist;
        root.anchorKey = delegate.key;
        root.anchorBpm = delegate.bpm;
        root.anchorArtworkPath = delegate.artworkPath;
        root.anchorFallbackArtworkPath = delegate.fallbackArtworkPath;
        root.anchorPlaylistNames = delegate.playlistNames;
        root.matchingPanelOpen = true;
    }

    function rescan() {
        selectedPlaylistIndex = 0;
        root.shownPlaylist = "";
        // Until the scan is done the list still holds the OLD library's
        // rows. A queue offered now would put this library's name on
        // them, and a track ending meanwhile would load the old
        // library's file as one of this one's. No queue until then.
        root.playbackController.setQueue(null, "", "");
        scanController.scan(root.format, root.currentPath(), root.format === "engine" ? root.rekordboxPath : "");
    }

    Component.onCompleted: {
        scanController.setHideStreamingTracks(root.appSettingsController.hideStreamingTracks);
        rescan();
        // After EditSessionHost has opened the session.
        Qt.callLater(root.offerLeftOverEdits);
    }
    onFormatChanged: rescan()
    // The library the list is read from. Another one chosen (Engine to
    // Device Library, say) closes the details pane and Matching: each
    // shows a row of the list it was opened from, which is no longer on
    // screen, and that row's id is another track, or none, in the library
    // now shown.
    readonly property string listedLibrary: root.format + "\n" + root.currentPath()
    onListedLibraryChanged: {
        root.trackPanelOpen = false;
        root.matchingPanelOpen = false;
        root.anchorSourceId = "";
    }

    // Live-applies without a rescan, setHideStreamingTracks() just
    // re-runs the existing display-filter pipeline (same one search()
    // already uses) over tracks already in memory.
    Connections {
        target: root.appSettingsController
        function onHideStreamingTracksChanged() {
            scanController.setHideStreamingTracks(root.appSettingsController.hideStreamingTracks);
        }
    }

    function formatDuration(seconds) {
        var total = Math.round(seconds);
        var m = Math.floor(total / 60);
        var s = total % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    // The sortable columns' widths, one number each for the header and the
    // rows under it: the column's own minimum, or what its header needs for
    // its label and the sort arrow after it in the font in use, whichever
    // is more. They were bare pixel counts tuned to one platform's font
    // ("Plays is 60, not 50"), so on Windows, whose header font is wider,
    // "BPM" was cut short beside its arrow. The minimums stay the pixel
    // counts they were: where a header fits, nothing moves, and the
    // page's column tiers (browseTier) are laid out against them.
    //
    // Measured with TextMetrics in TableHeaderLabel's own font, not with
    // hidden labels: an Item among the Page's children, visible or not,
    // changes how the Page sizes its content, and the list came out wide
    // enough for the full column tier on a page too narrow for it.
    readonly property real sortIndicatorRoom: Theme.iconSizeSmall * 0.5 + Theme.scaled(1)  // see SortableTableHeader
    readonly property font sortHeaderFont: Qt.font({family: Theme.dataFamily, pointSize: Theme.tableHeaderSize,
                                                    letterSpacing: 0.6})  // see TableHeaderLabel
    function sortColumnWidth(measure, minimum) {
        return Math.ceil(Math.max(minimum, measure.advanceWidth + root.sortIndicatorRoom));
    }
    TextMetrics { id: keyHeaderMeasure; font: root.sortHeaderFont; text: "KEY" }
    TextMetrics { id: bpmHeaderMeasure; font: root.sortHeaderFont; text: "BPM" }
    TextMetrics { id: timeHeaderMeasure; font: root.sortHeaderFont; text: "TIME" }
    TextMetrics { id: cuesHeaderMeasure; font: root.sortHeaderFont; text: "CUES" }
    TextMetrics { id: playsHeaderMeasure; font: root.sortHeaderFont; text: "PLAYS" }
    // Key's minimum is the KeyBadge's own width, which scales where the
    // other columns' pixel counts do not; the badge reads this width too.
    readonly property real keyColumnWidth: sortColumnWidth(keyHeaderMeasure, Theme.scaled(50))
    readonly property real bpmColumnWidth: sortColumnWidth(bpmHeaderMeasure, 50)
    readonly property real timeColumnWidth: sortColumnWidth(timeHeaderMeasure, 60)
    readonly property real cuesColumnWidth: sortColumnWidth(cuesHeaderMeasure, 50)
    readonly property real playsColumnWidth: sortColumnWidth(playsHeaderMeasure, 60)

    // The rest of the track list's column geometry, read by the header row
    // and by every row under it, so the two cannot disagree. They used to
    // carry their own copies: the header kept room for two trailing
    // buttons after the rows had gone down to one menu button, so every
    // header from Title on stood 40 px left of its column.
    readonly property real trackRowInset: Theme.tightSpacing
    readonly property real trackColumnSpacing: Theme.rowSpacing
    readonly property real artworkColumnWidth: Theme.iconSizeNormal
    // The row's one trailing button, the track actions menu.
    readonly property real actionsColumnWidth: Theme.iconSizeSmall

    // One decimal place, but only when there actually is one, "128"
    // reads better than "128.0" for the (very common) case of a whole-
    // number BPM, while a genuinely fractional one (e.g. a half-time
    // edit) still keeps its precision instead of getting rounded away.
    function formatBpm(bpm) {
        if (bpm <= 0) {
            return "--";
        }
        var oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }

    // The track search in the header (searchField): asked for with its
    // icon, and open for as long as it holds text whoever put it there.
    property bool searchOpen: false
    readonly property bool searchExpanded: root.searchOpen || searchField.text.length > 0
    function openSearch() {
        root.searchOpen = true;
        searchField.forceActiveFocus();
    }
    // Closed, it narrows nothing: a search that could not be seen would
    // leave a short list with no reason shown.
    function closeSearch() {
        searchField.text = "";
        root.searchOpen = false;
    }

    // A filter field with its own clear button: the playlist sidebar has
    // one for playlist names and the track list one for its tracks, each
    // over what it narrows (#106). One text used to narrow both, so a
    // playlist's name typed to find it emptied the track list, and an
    // artist typed to find tracks hid the playlist being looked at.
    component FilterField: TextField {
        id: filterField
        property string clearText: "Clear filter"
        property string clearObjectName: ""
        // What a screen reader calls the field: the placeholder is only
        // its description.
        property string accessibleName: placeholderText
        Accessible.name: accessibleName
        rightPadding: filterClearButton.visible ? filterClearButton.width + Theme.tightSpacing : 0
        // As tall as the combo in the header, and on a ground Theme chose:
        // left to the style, a field sat 25 px taller than the combo and
        // square beside its rounded neighbours.
        implicitHeight: Theme.compactControlHeight
        color: Theme.text
        placeholderTextColor: Theme.textMuted
        leftPadding: Theme.tightSpacing
        background: Rectangle {
            radius: Theme.cornerRadius
            color: Theme.surface
            border.color: filterField.activeFocus ? Theme.accent : Theme.border
            border.width: 1
        }

        IconToolButton {
            id: filterClearButton
            objectName: filterField.clearObjectName
            visible: filterField.text.length > 0
            anchors.right: parent.right
            anchors.rightMargin: Theme.scaled(2)
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: Theme.iconSizeSmall + Theme.scaled(8)
            implicitHeight: Theme.iconSizeSmall + Theme.scaled(8)
            flat: true
            text: filterField.clearText
            iconName: "edit-clear"
            ToolTip.visible: hovered
            ToolTip.text: filterField.clearText
            // The field keeps the keyboard: cleared to type something else.
            onClicked: {
                filterField.forceActiveFocus();
                filterField.text = "";
            }
        }
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

        // A ColumnLayout child sized purely by anchors.fill doesn't feed its
        // own implicit size back up to the ToolBar, so without this the
        // ToolBar stays single-row tall and the second row of controls
        // renders past its bottom edge, overlapping the page content below.
        implicitHeight: headerLayout.implicitHeight + 20

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                // The gap a one-row header shows under its crumb (Theme.crumbGap),
                // kept here too, before this header's second row.
                Layout.bottomMargin: Theme.crumbGap - headerLayout.spacing
                BackBreadcrumb {
                    stack: root.StackView.view
                    middleLabel: root.stickLabel
                    title: "Library"
                    backEnabled: !editHost.writing
                    onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                    onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
                }
                Item { Layout.fillWidth: true }
                LibrarySourceToggle {
                    current: root.format
                    hasRekordbox: root.hasRekordbox
                    hasEngine: root.hasEngine
                    hasOneLibrary: root.hasOneLibrary
                    onSourceRequested: (value) => root.format = value
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ToolButton {
                    objectName: "playlistSidebarButton"
                    text: root.currentPlaylistLabel
                    icon.source: Theme.iconUrl(root.playlistSidebarOpen ? "sidebar-collapse-left" : "sidebar-expand-left")
                    icon.color: Theme.text
                    ToolTip.visible: hovered
                    ToolTip.text: root.playlistSidebarOpen ? "Collapse the playlist sidebar" : "Show the playlist sidebar"
                    onClicked: root.playlistSidebarOpen = !root.playlistSidebarOpen
                }
                // The track search, folded into its icon: a field over the
                // list cost every playlist a row, searched or not. Open
                // while it has the keyboard or holds text (root.
                // searchExpanded), so a narrowed list always shows what
                // narrows it. The sidebar's playlists have their own filter
                // (playlistFilterField).
                IconToolButton {
                    id: searchButton
                    objectName: "searchButton"
                    iconName: "edit-find"
                    text: root.searchExpanded ? "Close the search" : "Search tracks"
                    ToolTip.visible: hovered
                    ToolTip.text: text
                    // By Tab, not by a click: a click that took the
                    // keyboard from the empty field would close it, and
                    // the click itself then open it again.
                    focusPolicy: Qt.TabFocus
                    onClicked: root.searchExpanded ? root.closeSearch() : root.openSearch()
                }
                FilterField {
                    id: searchField
                    objectName: "searchField"
                    // Grows out of the icon and back into it. Shown from the
                    // moment it opens, so it can take the keyboard then, and
                    // until it has closed all the way.
                    readonly property real openWidth: Theme.scaled(260)
                    property real shownWidth: root.searchExpanded ? openWidth : 0
                    Behavior on shownWidth {
                        NumberAnimation { duration: Theme.shortTransitionDuration; easing.type: Easing.OutCubic }
                    }
                    visible: root.searchExpanded || shownWidth > 0
                    clip: true
                    Layout.preferredWidth: shownWidth
                    Layout.minimumWidth: 0
                    placeholderText: "Search title, artist, or album..."
                    accessibleName: "Search tracks"
                    clearText: "Clear search"
                    clearObjectName: "searchClearButton"
                    // Left empty, or emptied while the keyboard is
                    // elsewhere, it folds away again.
                    onTextChanged: {
                        scanController.search(text);
                        if (!activeFocus && text.length === 0) {
                            root.searchOpen = false;
                        }
                    }
                    onActiveFocusChanged: {
                        if (!activeFocus && text.length === 0) {
                            root.searchOpen = false;
                        }
                    }
                    Keys.onEscapePressed: root.closeSearch()
                }
                Item { Layout.fillWidth: true }
                Label { text: "Sort by" }
                // The only way to Artist, which has no column, and the one
                // that says when the list is in Playlist Order (a third
                // header click also goes back to it); the direction is the
                // column headers' (see root.sortField).
                //
                // The template, not the styled ComboBox: every part is drawn
                // below, and a style still runs its own code against the
                // parts it is handed. KDE's desktop ComboBox calls
                // positionToRectangle() on its contentItem, which a Label
                // does not have, and ScanPage failed to load under it.
                T.ComboBox {
                    id: sortCombo
                    objectName: "sortCombo"
                    // Wide enough for "Playlist Order" beside the
                    // indicator: the label is elided to whatever is left
                    // after it, and at 140 the longest entry came out as
                    // "Playlis...".
                    Layout.preferredWidth: Theme.compactComboWidth
                    implicitWidth: Theme.compactComboWidth
                    // As tall as the library picker above it.
                    implicitHeight: Theme.compactControlHeight
                    textRole: "text"
                    valueRole: "value"
                    model: root.sortOptions
                    currentIndex: root.sortOptions.findIndex(option => option.value === root.sortField)
                    onActivated: (index) => root.sortByField(root.sortOptions[index].value)

                    // Background, popup and rows drawn here rather than
                    // left to the active style, for the reason
                    // LibrarySourceToggle already records: what the style
                    // draws comes from the PLATFORM's palette, while every
                    // colour in this app comes from Theme, which is dark
                    // unless the user asked for the system one. Where the
                    // two disagree the text lands on a ground Theme never
                    // chose -- here the open list's entries came out
                    // unreadable, dark on dark. The other combo on this
                    // very row was fixed this way; this one had simply
                    // never been given the same treatment.
                    // The same arrow the library picker beside it uses,
                    // rather than the style's: Basic draws a pair of
                    // chevrons, so the two combos on one row pointed at
                    // different things.
                    indicator: SeabassIcon {
                        x: sortCombo.width - width - 8
                        y: Theme.snap(sortCombo.topPadding + (sortCombo.availableHeight - height) / 2)
                        iconName: "arrow-down"
                        size: Theme.iconSizeSmall * 0.5
                        color: Theme.textMuted
                    }
                    background: Rectangle {
                        radius: Theme.cornerRadius
                        color: sortCombo.hovered ? Theme.rowHover : Theme.surface
                        border.color: Theme.border
                        border.width: 1
                    }
                    contentItem: Label {
                        text: sortCombo.displayText
                        color: Theme.text
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        leftPadding: 8
                        rightPadding: sortCombo.indicator ? sortCombo.indicator.width + 4 : 8
                    }
                    popup: Popup {
                        y: sortCombo.height
                        width: sortCombo.width
                        implicitHeight: Math.min(contentItem.implicitHeight + 2, 320)
                        padding: 1
                        background: Rectangle {
                            radius: Theme.cornerRadius
                            color: Theme.surface
                            border.color: Theme.border
                            border.width: 1
                        }
                        contentItem: ListView {
                            clip: true
                            implicitHeight: contentHeight
                            model: sortCombo.delegateModel
                            currentIndex: sortCombo.highlightedIndex
                            ScrollIndicator.vertical: ScrollIndicator {}
                        }
                    }
                    delegate: ItemDelegate {
                        id: sortEntry
                        required property int index
                        required property var modelData
                        width: ListView.view ? ListView.view.width : sortCombo.width
                        height: Theme.compactControlHeight
                        highlighted: sortCombo.highlightedIndex === index
                        background: Rectangle {
                            color: sortEntry.highlighted
                                   ? Theme.rowPressed
                                   : (sortEntry.hovered ? Theme.rowHover : Theme.surface)
                        }
                        contentItem: Label {
                            text: sortEntry.modelData.text
                            color: Theme.text
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            leftPadding: 8
                        }
                    }
                }
            }
        }
    }

    // A failed read is said in a dialog, not in a line over the list.
    // Refused by the system (macOS, until Seabass may read removable
    // volumes): how to allow it, with the file underneath. Anything
    // else: the reader's own words. When this page never had a list,
    // closing it goes back the way every exit does (editHost), and only
    // while the page is on top: an empty page has nothing to offer. A
    // failure after a list was up (its cues, a rescan, another format)
    // leaves the page where it is.
    property bool everListed: false
    SeabassDialog {
        id: scanErrorDialog
        objectName: "scanErrorDialog"
        anchors.centerIn: Overlay.overlay
        severity: SeabassDialog.Error
        closePolicy: Popup.CloseOnEscape
        title: scanController.accessDenied ? "Not allowed to read this library" : "Cannot read this library"
        headline: scanController.accessDenied ? LibraryAccess.deniedHeadline + " " + LibraryAccess.deniedAdvice
                                              : scanController.errorMessage
        detailText: scanController.accessDenied ? scanController.errorMessage : ""
        footer: DialogButtonBox {
            Button {
                objectName: "openPrivacySettingsButton"
                visible: scanController.accessDenied && LibraryAccess.privacySettingsUrl.length > 0
                text: "Open Privacy Settings"
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                onClicked: Qt.openUrlExternally(LibraryAccess.privacySettingsUrl)
            }
            Button {
                objectName: "scanErrorOkButton"
                text: "OK"
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
        onClosed: {
            const stack = root.StackView.view;
            if (!root.everListed && stack && stack.currentItem === root) {
                editHost.requestLeave(() => stack.pop());
            }
        }
    }
    Connections {
        target: scanController
        function onErrorMessageChanged() {
            if (scanController.errorMessage.length > 0) {
                root.releaseLandedRows();
                scanErrorDialog.open();
            }
        }
        function onTracksPublished(cuesLanded) {
            root.everListed = true;
            if (!cuesLanded) {
                root.releaseLandedRows();
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        // The header's inset, so the list starts on the same left line
        // as the breadcrumb and the sidebar button above it.
        anchors.leftMargin: Theme.pageMargin
        anchors.rightMargin: Theme.pageMargin
        spacing: 0

        // Right pane (tracks) and the Matching panel share a
        // SplitView so their relative widths are user-resizable via a
        // drag handle -- previously a fixed root.width-derived split,
        // which is exactly what let the panel's own content end up wider
        // than what was actually available on a narrower window.
        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 4
                color: SplitHandle.pressed || SplitHandle.hovered ? Theme.accent : Theme.borderSubtle
            }

        // Playlists sidebar -- a real, resizable SplitView pane instead of
        // the off-canvas Drawer this replaced: collapsible via the header's
        // own pill button rather than an overlay you have to close before
        // doing anything else.
        Pane {
            objectName: "playlistSidebar"
            visible: root.playlistSidebarOpen
            SplitView.preferredWidth: 240
            SplitView.minimumWidth: 160
            padding: 0
            // The open import panel is wider than this pane, over the
            // tracks beside it.
            z: importProgress.expanded || keepProgress.expanded ? 1 : 0

            // Save as FLAC of this stick's recordings, as the Recordings
            // page shows it: above the import's strip when both run. With
            // Experimental off too, on purpose and as that page does: a
            // save under way can still be followed and cancelled, and what
            // ended closed; nothing new is offered (no row, no menu).
            KeepRecordingProgress {
                id: keepProgress
                objectName: "keepProgress"
                anchors.left: parent.left
                anchors.bottom: importProgress.visible ? importProgress.top : parent.bottom
                width: expanded ? Math.max(parent.width, Math.min(Theme.scaled(460), root.width - 2 * Theme.pageMargin))
                                : parent.width
                keepController: root.keepController
                stickRoot: scanController.recordingsRoot
                focusFallback: root
            }

            // The import being prepared, out of the way under the list.
            PassiveProgress {
                id: importProgress
                objectName: "importProgress"
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                // Open, it has the width to show a file's name whole,
                // however narrow the sidebar is.
                width: expanded ? Math.max(parent.width, Math.min(Theme.scaled(460), root.width - 2 * Theme.pageMargin))
                                : parent.width
                readonly property string step: root.trackImportController.progressStep ?? ""
                // Analyse again reads the stick and analyses; it has no files
                // to read in or plan to check.
                readonly property string reanalysing: root.trackImportController.reanalysingPlaylist ?? ""
                readonly property var stepKeys: reanalysing.length > 0 ? ["stick", "analysis"]
                                                                       : ["stick", "files", "check", "analysis"]
                readonly property int stepIndex: stepKeys.indexOf(step)
                running: root.trackImportController.busy === true
                // One bar for the whole run (progressFraction); the steps'
                // own counts are in the panel. Until the work is counted
                // (the stick read, the folders walked) how far it is cannot
                // be told: the busy look.
                done: root.trackImportController.progressFraction ?? 0
                total: root.trackImportController.progressPlanned === true ? 1 : 0
                // The keyboard back where it was when the run ends on it.
                focusFallback: root
                readonly property int stepDone: root.trackImportController.progressDone ?? 0
                readonly property int stepTotal: root.trackImportController.progressTotal ?? 0
                // Each step's count as it last had one, so the steps done
                // keep theirs in the panel. Read from the controller itself:
                // the step and its count change at once.
                property var stepTotals: ({})
                onStepTotalChanged: {
                    const now = root.trackImportController.progressStep ?? "";
                    const total = root.trackImportController.progressTotal ?? 0;
                    if (now.length > 0 && total > 0) {
                        const totals = Object.assign({}, importProgress.stepTotals);
                        totals[now] = total;
                        importProgress.stepTotals = totals;
                    }
                }
                onRunningChanged: {
                    if (!running) {
                        importProgress.stepTotals = ({});
                    }
                }
                readonly property string lastFile: root.trackImportController.progressFile ?? ""
                detail: step === "analysis" && lastFile.length > 0 ? qsTr("Last done: %1").arg(lastFile) : ""
                activity: (root.trackImportController.progressFiles ?? []).map(
                    file => step === "analysis" ? qsTr("Analysing %1").arg(file) : qsTr("Reading %1").arg(file))
                note: qsTr("Nothing is written until you save.")
                summary: reanalysing.length > 0
                    ? (step === "analysis" && stepTotal > 0
                        ? qsTr("Analysing the tracks of \"%1\" again: track %2 of %3").arg(reanalysing)
                            .arg(Math.min(stepTotal, stepDone + 1)).arg(stepTotal)
                        : qsTr("Analysing the tracks of \"%1\" again").arg(reanalysing))
                    : step === "files" && stepTotal > 0 ? qsTr("Import: reading file %1 of %2").arg(Math.min(stepTotal, stepDone + 1)).arg(stepTotal)
                    : step === "analysis" && stepTotal > 0
                        ? qsTr("Import: analysing track %1 of %2").arg(Math.min(stepTotal, stepDone + 1)).arg(stepTotal)
                    : step === "check" && stepTotal > 0
                        ? qsTr("Import: reading cover %1 of %2").arg(Math.min(stepTotal, stepDone + 1)).arg(stepTotal)
                    : step === "check" ? qsTr("Import: checking against the stick")
                    : qsTr("Import: reading the stick")
                steps: (reanalysing.length > 0 ? [qsTr("Read the stick"), qsTr("Analyse the tracks")]
                        : [qsTr("Read the stick"), qsTr("Read the files"), qsTr("Check against the stick"),
                           qsTr("Analyse the tracks")]).map((label, index) => ({
                            label: label,
                            count: index === importProgress.stepIndex && importProgress.stepTotal > 0
                                ? qsTr("%1 of %2 done").arg(importProgress.stepDone).arg(importProgress.stepTotal)
                                : index < importProgress.stepIndex && (importProgress.stepTotals[importProgress.stepKeys[index]] ?? 0) > 0
                                ? qsTr("%1 of %2 done").arg(importProgress.stepTotals[importProgress.stepKeys[index]])
                                    .arg(importProgress.stepTotals[importProgress.stepKeys[index]])
                                : "",
                            state: index < importProgress.stepIndex ? "done" : index === importProgress.stepIndex ? "now" : "later"
                        }))
                onCancelRequested: root.trackImportController.cancel()
            }
            ColumnLayout {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: keepProgress.visible ? keepProgress.top
                    : importProgress.visible ? importProgress.top : parent.bottom
                spacing: 0

                // Narrows the playlists below by name, and nothing else: the
                // track list keeps its own search (searchField).
                FilterField {
                    id: playlistFilterField
                    objectName: "playlistFilterField"
                    Layout.fillWidth: true
                    Layout.rightMargin: Theme.tightSpacing
                    Layout.topMargin: Theme.tightSpacing
                    Layout.bottomMargin: Theme.tightSpacing
                    placeholderText: "Filter playlists..."
                    accessibleName: "Filter playlists"
                    clearText: "Clear playlist filter"
                    clearObjectName: "playlistFilterClearButton"
                }

                PlaylistListView {
                    id: playlistSidebarList
                    objectName: "playlistList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    scanController: scanController
                    searchQuery: playlistFilterField.text
                    selectedIndex: root.selectedPlaylistIndex
                    onPlaylistPicked: (index, name) => root.selectPlaylist(index, name)
                    fixedRows: root.sidebarFixedRows
                    shownFixedRow: root.showingRecordings ? "recordings" : root.shownPlaylist.length === 0 ? "all" : ""
                    onFixedRowPicked: kind => {
                        if (kind === "recordings") {
                            root.showOwnRecordings();
                        }
                    }
                    isPendingDelete: name => root.playlistEditController.isPlaylistStaged(name)
                    isMadeWhereMissing: name => ("isMadeWhereMissing" in root.playlistEditController)
                        && root.playlistEditController.isMadeWhereMissing(name) === true
                    isRenamed: name => ("isRenameStaged" in root.playlistEditController)
                        && root.playlistEditController.isRenameStaged(name)
                    isEdited: name => (("isPlaylistEdited" in root.playlistEditController)
                            && root.playlistEditController.isPlaylistEdited(name))
                        || root.trackImportController.trackCountChange(name) > 0
                    // "All tracks" grows by the new tracks it lists.
                    trackCountChange: name => (("trackCountChange" in root.playlistEditController)
                            ? root.playlistEditController.trackCountChange(name) : 0)
                        + (name.length === 0 ? scanController.importNewTrackCount
                                             : root.trackImportController.trackCountChange(name))
                    pendingRevision: root.playlistEditController.pendingRevision + root.trackImportController.pendingRevision
                    fileDropsEnabled: root.trackImportEnabled
                    onFilesDropped: (name, urls) => root.importFiles(urls, name, false, -1)
                    onFilesDroppedForNew: urls => root.importIntoNewPlaylist(urls)
                    onPlaylistContextRequested: (name, row) => root.openPlaylistMenu(name, row)
                    onFixedRowContextRequested: (kind, row) => {
                        if (kind === "recordings") {
                            root.openOwnRecordingsMenu(row);
                        }
                    }
                    canCreate: true
                    newPlaylists: root.newPlaylistNames
                    shownPlaylist: root.shownPlaylist
                    keyNotation: root.appSettingsController.keyNotation
                    onNewPlaylistPicked: name => root.selectPlaylist(-1, name)
                    onCreateRequested: root.openCreatePlaylistDialog()
                    // A dragged track is dropped on a playlist row; the card
                    // then asks what to do (TrackDragItem).
                    trackDropsEnabled: true
                    // The playlist shown takes no drop of a track that is in it
                    // (a Browse row of it), and is a target like any other for
                    // one that is not (a Matching result).
                    droppedTrackPlaylists: root.draggedTrack && root.draggedTrack.playlistNames
                        ? root.draggedTrack.playlistNames : []
                    heldDropTarget: trackDrag.confirming ? trackDrag.targetPlaylist : ""
                    onTrackDropped: name => trackDrag.dropOn(name, root.dropSourceFor(name))
                    onSpringLoaded: name => root.springLoad(name)
                    // No summary over the rows while a track is dragged onto
                    // them, or while the card asks what the drop should do.
                    summariesEnabled: !root.dragging && !trackDrag.confirming

                    // The fixed rows stay at the top whatever the filter, so
                    // only they left means no playlist's name matched.
                    EmptyState {
                        objectName: "playlistFilterEmptyState"
                        // Over the list, not in its content, which is only as
                        // tall as its few rows.
                        parent: playlistSidebarList
                        visible: playlistFilterField.text.length > 0
                            && playlistSidebarList.count <= playlistSidebarList.fixedRowCount
                        iconName: "edit-find"
                        text: "No playlists match."
                    }
                }
            }
        }

        ColumnLayout {
            id: browseColumn
            SplitView.fillWidth: true
            spacing: 0

            RowLayout {
                // The track rows' column geometry (root.trackRowInset and
                // the widths beside it), read from the same properties as
                // the rows, so a header stands over its column.
                Layout.fillWidth: true
                Layout.leftMargin: root.trackRowInset
                Layout.rightMargin: root.trackRowInset
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: root.trackColumnSpacing
                Item { objectName: "headerCell_artwork"; Layout.preferredWidth: root.artworkColumnWidth }
                // Each header with a sort key sorts by it (see
                // root.sortByColumn); the artwork column and the trailing
                // action columns have none.
                SortableTableHeader {
                    objectName: "sortHeader_title"
                    label: "Title"
                    sortKey: "title"
                    sortField: root.sortField
                    sortAscending: root.sortAscending
                    unsortedName: root.unsortedOrderName
                    onSortRequested: (key) => root.sortByColumn(key)
                    Layout.fillWidth: true
                    visible: root.browseTier >= 1
                }
                // Out of the Title column's fill, so the columns after it
                // stay where the rows have them. Says why the Cues column
                // below is empty for now; the list itself is usable.
                Label {
                    objectName: "cuesPendingNote"
                    visible: scanController.cuesPending && root.browseTier >= 2
                    text: "Reading cues..."
                    color: Theme.textMuted
                    font.pointSize: Theme.fontTiny
                }
                // The widths are the row delegate's own, column for column.
                Repeater {
                    model: [
                        { label: "Key", key: "key", width: root.keyColumnWidth },
                        { label: "BPM", key: "bpm", width: root.bpmColumnWidth },
                        { label: "Time", key: "duration", width: root.timeColumnWidth },
                        { label: "Cues", key: "cues", width: root.cuesColumnWidth },
                        { label: "Plays", key: "plays", width: root.playsColumnWidth },
                    ].filter(column => column.key !== "plays" || root.showPlayCount)
                    delegate: SortableTableHeader {
                        required property var modelData
                        objectName: "sortHeader_" + modelData.key
                        label: modelData.label
                        sortKey: modelData.key
                        sortField: root.sortField
                        sortAscending: root.sortAscending
                        unsortedName: root.unsortedOrderName
                        onSortRequested: (key) => root.sortByColumn(key)
                        Layout.preferredWidth: modelData.width
                        visible: root.browseTier >= 2
                    }
                }
                // Title's fill, at the narrowest tier where Title goes: the
                // rows have the same, so the actions stay at the right edge
                // in both rather than wherever each layout leaves them.
                Item {
                    visible: root.browseTier === 0
                    Layout.fillWidth: true
                }
                // Over the rows' track actions button. Any other width here
                // and Title's fill takes up the difference, which moves
                // every column after it off its rows.
                Item {
                    objectName: "headerCell_actions"
                    Layout.preferredWidth: root.actionsColumnWidth
                }
            }

            ListView {
                // Room to scroll the last row clear of the Save overlay (bottom right).
                bottomMargin: 80
                id: trackListView
                objectName: "trackListView"
                // Not draggable when everything already fits.
                interactive: contentHeight > height
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                ScrollBar.vertical: BigScrollBar {}
                model: scanController.tracks

                // No move or displaced transitions on this list. They slid
                // the rows into a staged order (setPendingEdits moves and
                // inserts rows in place), and Qt 6.11.1 crashed in
                // QQuickItemView::animStopped, reading a delegate's y after
                // the delegate was gone, when a wheel flick ended while such
                // a transition and an in-place change overlapped (core of
                // 2026-10-17 02:12, no Seabass frame). The rows take their
                // new places at once; the gap's own Translate still animates.

                // A plain Rectangle, not an ItemDelegate. Overriding a
                // Material Control's `background:` property doesn't
                // reliably replace its own implicit chrome (see
                // StickListPage.qml's stick-card delegate for the same
                // fix and the fuller explanation); a bare Rectangle +
                // MouseArea sidesteps it entirely.
                delegate: Rectangle {
                    id: trackDelegate
                    width: ListView.view.width
                    // Taller only for the currently playing row, gives
                    // its inline waveform (below) room to actually be
                    // legible instead of a sliver.
                    height: trackDelegate.isPlaying ? 64 : 56
                    // Down a row while a dragged track's gap is open above it.
                    transform: Translate {
                        y: dropGap.shiftFor(trackDelegate.index)
                        Behavior on y { NumberAnimation { duration: Theme.shortTransitionDuration } }
                    }

                    required property int index
                    required property string sourceId
                    required property string title
                    required property string artist
                    required property double durationSeconds
                    required property int cueCount
                    required property int playCount
                    required property string filePath
                    required property string artworkPath
                    required property double bpm
                    required property string key
                    required property var cues
                    required property var playlistNames
                    required property string streamingSource
                    required property int rating
                    required property int bitrate
                    required property string comment
                    required property string album
                    required property string fallbackArtworkPath
                    required property bool pendingAddition
                    required property bool pendingMove
                    // A new track of the import's staged batch: not on the
                    // stick until Save.
                    required property bool pendingImport
                    required property string importChangeId
                    // A row the last save wrote, until the stick is read
                    // again; this or pendingImport: no track a library
                    // has read yet, nothing to play, open or edit.
                    required property bool awaitingRead
                    required property bool unreadable
                    // A recording the player left on the stick (Own
                    // Recordings): it plays, and that is all it does here.
                    required property bool recording
                    required property var recordedAt
                    required property double recordingSizeBytes
                    required property bool recordingUnfinished
                    required property bool recordingHoldsNoAudio
                    // The title of its FLAC in Own Recordings' playlist, ""
                    // for a recording no library has.
                    required property string recordingInLibraryAs

                    readonly property bool isPlaying: playbackController.hasTrack
                        && playbackController.currentFormat === root.format
                        && playbackController.currentSourceId === trackDelegate.sourceId

                    // Alternating row shading, makes it much easier to
                    // track a row across the wide, densely-columned list.
                    // Solid, muted colors rather than a translucent overlay,
                    // so the result doesn't depend on (and can't pick up an
                    // unexpected tint from) whatever's rendered underneath.
                    // Set straight away, never animated: the list rewrites
                    // it as rows scroll and sort, and the hover shading
                    // below is what fades.
                    color: trackDelegate.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd

                    // Hover and press, faded in and out over that colour.
                    RowHoverShade {
                        objectName: "rowHoverShade"
                        radius: trackDelegate.radius
                        hovered: rowHover.hovered
                        pressed: rowMouseArea.pressed
                    }
                    // Not rowMouseArea's own hover: under org.kde.desktop
                    // every Label carries a HoverHandler, and the
                    // MouseArea under the row's text never hears the
                    // pointer, so the row shaded only between its cells.
                    HoverHandler { id: rowHover }
                    // What a row not on the stick yet cannot do, and when it can.
                    readonly property bool waitTipShown: ToolTip.visible
                    ToolTip.visible: (trackDelegate.unreadable || trackDelegate.recording) && rowHover.hovered
                    ToolTip.text: trackDelegate.recording ? (trackDelegate.recordingInLibraryAs.length > 0
                            ? root.recordingInLibraryText(trackDelegate.recordingInLibraryAs) : root.recordingRowText)
                        : trackDelegate.awaitingRead ? root.awaitingReadText : root.afterSaveText
                    ToolTip.delay: 500

                    // Now-playing highlight, an accent-colored stripe,
                    // same idiom as most media players use for "this one."
                    // Rounded to match every other accent-bordered highlight
                    // in the app (BackupsPage's active-field outline,
                    // etc.), all radius: 4. This one was square.
                    border.color: trackDelegate.isPlaying ? Theme.accent : "transparent"
                    border.width: trackDelegate.isPlaying ? 2 : 0
                    // Rounded whether or not this is the playing row: the
                    // corner was conditional, so a row changed shape as it
                    // started playing and sat square against the rounded
                    // boxes above it the rest of the time.
                    radius: Theme.cornerRadius

                    // The currently playing row's own waveform, with cue
                    // markers and live progress, used as a faded
                    // full-row backdrop rather than a discrete column -
                    // declared before (so it renders behind) the row's
                    // real content, and deliberately NOT part of the
                    // RowLayout below, so it can never affect column
                    // widths/alignment the way an inline version did.
                    // Its own internal seek MouseArea is inert here
                    // (rowMouseArea below sits on top and claims every
                    // click first), purely decorative; the PlayerBar's
                    // own waveform is still the real interactive one.
                    //
                    // Made in the playing row only. Made in every row and
                    // hidden, each visible row held a Canvas and followed
                    // the player's position on every tick, and every list
                    // rebuild made them all again.
                    Loader {
                        active: trackDelegate.isPlaying
                        anchors.fill: parent
                        anchors.margins: 2
                        sourceComponent: WaveformView {
                            opacity: 0.35
                            // A backdrop behind a row's text: no ruler lane.
                            gridStyle: "lines"
                            waveformData: playbackController.waveform
                            look: playbackController.waveformLook
                            format: playbackController.currentFormat
                            cueData: playbackController.cues
                            trackDurationMs: playbackController.duration
                            progress: playbackController.duration > 0
                                ? playbackController.position / playbackController.duration : 0
                        }
                    }

                    // Opens the track details column -- playing now
                    // happens only via the artwork's own hover-play
                    // overlay above (which, being declared later/topmost
                    // for just that region, claims its own clicks
                    // first). Streaming tracks are fine here: the column
                    // still shows their cues and playlists, only Play
                    // itself (on the artwork) is unavailable for those.
                    // Press and move drags the track to a playlist (see
                    // TrackDragArea).
                    TrackDragArea {
                        id: rowMouseArea
                        objectName: "rowMouseArea"
                        anchors.fill: parent
                        trackDrag: root.trackDragCard
                        track: trackDelegate
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton) {
                                root.openTrackMenu(trackDelegate, trackDelegate);
                                return;
                            }
                            // Its details are the stick's, once a library
                            // has read it.
                            root.openDetails(trackDelegate);
                        }
                        onPressAndHold: root.openTrackMenu(trackDelegate, trackDelegate)
                    }

                    // The page's column geometry, which the header row
                    // above reads too.
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: root.trackRowInset
                        anchors.rightMargin: root.trackRowInset
                        spacing: root.trackColumnSpacing

                        // Play, direct from the row (PlayableCover, #133): the
                        // rest of the row (below) opens the track's details
                        // instead, so this is the one place that plays without
                        // an extra click. Shown while the pointer is on the
                        // cover, so the artwork itself isn't permanently
                        // obscured, and disabled for a track with no local
                        // file (a streaming one: see the tooltip below).
                        PlayableCover {
                            id: artworkRect
                            objectName: "rowCell_artwork"
                            Layout.preferredWidth: root.artworkColumnWidth
                            Layout.preferredHeight: Theme.iconSizeNormal
                            // An Engine row's own art first, the same song's
                            // rekordbox art when that does not load.
                            source: trackDelegate.artworkPath
                            fallbackSource: trackDelegate.fallbackArtworkPath
                            playbackController: root.playbackController
                            playSource: ({format: root.format, libraryPath: root.currentPath(),
                                          sourceId: trackDelegate.sourceId})
                            // Not read from the stick yet: there, and not to
                            // be pressed.
                            playFilePath: trackDelegate.streamingSource.length === 0 && !trackDelegate.unreadable
                                ? trackDelegate.filePath || "" : ""
                            unavailableText: trackDelegate.awaitingRead ? root.awaitingReadText
                                : trackDelegate.pendingImport ? root.afterSaveText
                                : "No audio file for this track on the stick"
                            onPlayRequested: root.playRow(trackDelegate)

                            // At the narrowest tier (see root.browseTier's own
                            // comment) every other column is squeezed out --
                            // this hover tooltip is the only way left to see
                            // title/artist/key/BPM/duration for this row.
                            // A track that cannot be played says why, as
                            // every cover's disabled key does.
                            ToolTip.visible: artworkRect.coverHovered
                                && (trackDelegate.streamingSource.length > 0 || !artworkRect.playKey.enabled
                                    || root.browseTier === 0)
                            ToolTip.text: trackDelegate.awaitingRead ? root.awaitingReadText
                                : trackDelegate.pendingImport ? root.afterSaveText
                                : trackDelegate.streamingSource.length > 0
                                ? "Streaming track (" + trackDelegate.streamingSource + ") - no local file, can't be played."
                                : !artworkRect.playKey.enabled ? artworkRect.unavailableText
                                : title + " - " + artist + "\n" + (key.length > 0 ? key : "--") + " · "
                                    + root.formatBpm(bpm) + " BPM · " + root.formatDuration(durationSeconds)
                            ToolTip.delay: 300

                            // Topmost over rowMouseArea below (declared
                            // later in the same Item, and scoped to just
                            // this Rectangle rather than the whole row) --
                            // claims clicks here for Play before they'd
                            // otherwise reach the row's own "open detail
                            // page" handler.
                            // A drag that starts on the cover drags the
                            // track as one from the rest of the row does.
                            TrackDragArea {
                                anchors.fill: parent
                                trackDrag: root.trackDragCard
                                track: trackDelegate
                                hoverEnabled: true
                                cursorShape: held ? Qt.ClosedHandCursor : trackDelegate.streamingSource.length === 0
                                    ? Qt.PointingHandCursor : Qt.ArrowCursor
                                // As the key under it does: the track
                                // loaded plays or pauses, any other plays.
                                // A track with no file opens its details,
                                // as a click on the rest of the row does;
                                // a streaming one, as before, nothing.
                                onClicked: {
                                    if (artworkRect.playKey.enabled) {
                                        artworkRect.press();
                                    } else if (trackDelegate.streamingSource.length === 0) {
                                        root.openDetails(trackDelegate);
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            objectName: "rowCell_title"
                            visible: root.browseTier >= 1
                            Layout.fillWidth: true
                            spacing: 1
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label {
                                    objectName: "rowTitle"
                                    text: title
                                    font.bold: true
                                    // Staged to be added to the playlist shown,
                                    // or moved within it, or imported.
                                    font.italic: trackDelegate.pendingAddition || trackDelegate.pendingMove
                                        || trackDelegate.pendingImport
                                    // Saved and waiting to be read: muted,
                                    // not staged.
                                    color: trackDelegate.pendingAddition || trackDelegate.pendingMove
                                        || trackDelegate.unreadable ? Theme.textMuted : Theme.text
                                    // Staged to come out of the playlist shown,
                                    // or to go from the stick with a playlist
                                    // deleted with its tracks, whichever list
                                    // shows it.
                                    font.strikeout: root.playlistEditController.pendingRevision >= 0
                                        && ((root.shownPlaylist.length > 0
                                             && root.playlistEditController.isRemovalStaged(root.shownPlaylist, filePath))
                                            || (("isTrackDeleteStaged" in root.playlistEditController)
                                                && root.playlistEditController.isTrackDeleteStaged(filePath)))
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                // Plain text, no logo. This project never
                                // reproduces a real brand's mark (see
                                // AboutPage.qml's trademark note). Just
                                // tells you where a file-less row comes
                                // from instead of it looking broken.
                                // Not on the stick yet: "new", as the playlist
                                // list marks a playlist staged to be made.
                                // Saved, and not yet read: said in a word.
                                Label {
                                    objectName: "awaitingReadLabel"
                                    visible: trackDelegate.awaitingRead
                                    text: qsTr("saved, reading the stick again")
                                    font.pointSize: Theme.fontTiny
                                    font.italic: true
                                    color: Theme.textMuted
                                }
                                Rectangle {
                                    objectName: "pendingImportBadge"
                                    visible: trackDelegate.pendingImport
                                    radius: 3
                                    color: Theme.groupBackground
                                    border.color: Theme.borderSubtle
                                    implicitWidth: pendingImportLabel.implicitWidth + 8
                                    implicitHeight: pendingImportLabel.implicitHeight + 4
                                    Label {
                                        id: pendingImportLabel
                                        anchors.centerIn: parent
                                        text: qsTr("new")
                                        font.pointSize: Theme.fontTiny
                                        font.bold: true
                                        font.italic: true
                                        color: Theme.textMuted
                                    }
                                }
                                // Whether the library has it: a WAV whose FLAC
                                // it has is a second copy now.
                                Rectangle {
                                    objectName: "recordingBadge"
                                    visible: trackDelegate.recording
                                    radius: 3
                                    color: Theme.groupBackground
                                    border.color: Theme.borderSubtle
                                    implicitWidth: recordingBadgeLabel.implicitWidth + 8
                                    implicitHeight: recordingBadgeLabel.implicitHeight + 4
                                    Label {
                                        id: recordingBadgeLabel
                                        anchors.centerIn: parent
                                        text: trackDelegate.recordingInLibraryAs.length > 0
                                            ? qsTr("In the library as FLAC") : qsTr("Not in the library")
                                        font.pointSize: Theme.fontTiny
                                        font.bold: true
                                        color: Theme.textMuted
                                    }
                                }
                                Rectangle {
                                    visible: trackDelegate.streamingSource.length > 0
                                    radius: 3
                                    color: Theme.groupBackground
                                    border.color: Theme.borderSubtle
                                    implicitWidth: streamingLabel.implicitWidth + 8
                                    implicitHeight: streamingLabel.implicitHeight + 4
                                    Label {
                                        id: streamingLabel
                                        anchors.centerIn: parent
                                        text: trackDelegate.streamingSource
                                        font.pointSize: Theme.fontTiny
                                        font.bold: true
                                        color: Theme.textMuted
                                    }
                                }
                            }
                            // A recording has no artist: when it was
                            // recorded, its size, and whether the player
                            // finished it, in its place.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.rowSpacing
                                Label {
                                    objectName: "rowSubtitle"
                                    text: trackDelegate.recording ? root.recordingDetails(trackDelegate) : artist
                                    color: Theme.textMuted
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                // A recording's Save as FLAC, as its row on
                                // the Recordings page says it; made only on
                                // a recording's row.
                                Loader {
                                    objectName: "recordingSaveStateLoader"
                                    active: trackDelegate.recording
                                    visible: active && item !== null && item.text.length > 0
                                    Layout.maximumWidth: implicitWidth
                                    Layout.fillWidth: true
                                    sourceComponent: RecordingSaveState {
                                        keepController: root.keepController
                                        stickRoot: scanController.recordingsRoot
                                        path: trackDelegate.filePath
                                    }
                                }
                            }
                        }

                        KeyBadge {
                            objectName: "rowCell_key"
                            visible: root.browseTier >= 2
                            keyName: key
                            notation: root.appSettingsController.keyNotation
                            Layout.preferredWidth: root.keyColumnWidth
                        }
                        Label {
                            objectName: "rowCell_bpm"
                            visible: root.browseTier >= 2
                            text: root.formatBpm(bpm)
                            Layout.preferredWidth: root.bpmColumnWidth
                        }
                        Label {
                            objectName: "rowCell_duration"
                            visible: root.browseTier >= 2
                            text: root.formatDuration(durationSeconds)
                            Layout.preferredWidth: root.timeColumnWidth
                        }
                        // Empty while the cue pass runs, rather than a 0
                        // that would read as "this track has no cues".
                        Label {
                            objectName: "cueCountLabel"
                            visible: root.browseTier >= 2
                            text: scanController.cuesPending || trackDelegate.recording ? "" : cueCount
                            Layout.preferredWidth: root.cuesColumnWidth
                        }
                        Label {
                            objectName: "rowCell_plays"
                            visible: root.browseTier >= 2 && root.showPlayCount
                            text: trackDelegate.recording ? "" : playCount >= 0 ? playCount : "--"
                            Layout.preferredWidth: root.playsColumnWidth
                        }
                        // The header row's spacer, for the same tier.
                        Item {
                            visible: root.browseTier === 0
                            Layout.fillWidth: true
                        }
                        // The row's actions in one menu: merging, finding
                        // matches, and (a playlist shown) taking the
                        // track out of that playlist. A right
                        // click on the row opens the same menu.
                        IconToolButton {
                            id: rowMenuButton
                            objectName: "trackRowMenuButton"
                            text: "Track actions"
                            iconName: "application-menu"
                            Layout.preferredWidth: root.actionsColumnWidth
                            enabled: !trackDelegate.awaitingRead
                            ToolTip.visible: hovered
                            ToolTip.text: trackDelegate.recording ? qsTr("Save as FLAC, show on the Recordings page")
                                : trackDelegate.importChangeId.length > 0 ? root.dontImportText(trackDelegate)
                                : "Merge, find matching tracks" + (root.shownPlaylist.length > 0
                                ? ", remove from this playlist" : "")
                            onClicked: root.openTrackMenu(trackDelegate, rowMenuButton)
                        }
                    }
                }

                // A search that matches nothing says so, and where: in the
                // playlist shown, or in the whole library.
                EmptyState {
                    objectName: "trackListEmptyState"
                    visible: trackListView.count === 0 && !scanController.busy
                    iconName: "edit-find"
                    text: searchField.text.length === 0 ? (root.showingRecordings ? "No recordings found." : "No tracks found.")
                        : root.showingRecordings ? "No recordings match."
                        : root.shownPlaylist.length > 0 ? "No tracks in this playlist match."
                        : "No tracks match."
                }

                // Dropping a track of the playlist shown between two rows
                // moves it there: the drag item carries the
                // row's roles (Drag.keys ["seabass/track"]). Over the list
                // itself, not its content, so they stay put as it scrolls.
                DropArea {
                    id: trackDropArea
                    objectName: "trackDropArea"
                    parent: trackListView
                    anchors.fill: parent
                    keys: ["seabass/track"]
                    onDropped: (drop) => {
                        if (dropGap.release()) {
                            drop.accept(Qt.MoveAction);
                        }
                    }
                }
                // Audio files from the computer dropped on the track
                // list (the track import): into the playlist shown, at the
                // row they are dropped before when the list is in the
                // playlist's own order, else at its end; into no playlist
                // on "All tracks". The list wears an outline meanwhile, and
                // a line marks the place.
                DropArea {
                    id: fileDropArea
                    objectName: "trackFileDropArea"
                    parent: trackListView
                    anchors.fill: parent
                    // On Own Recordings the files go into its playlist.
                    enabled: root.trackImportEnabled
                    keys: ["text/uri-list"]
                    // The place counts only where a row's place is its
                    // place in the playlist, and nothing staged moves it.
                    readonly property bool placed: root.shownPlaylist.length > 0 && dropGap.orderShown
                        && root.newPlaylistNames.indexOf(root.shownPlaylist) < 0
                        && (!root.showingRecordings || root.ownRecordingsPlaylistOnStick)
                    // The row the files would go before; the count for the end.
                    readonly property int targetIndex: {
                        if (!containsDrag || !placed) {
                            return -1;
                        }
                        const y = drag.y + trackListView.contentY;
                        const at = trackListView.indexAt(trackListView.width / 2, y);
                        if (at < 0 || at >= dropGap.rowCount) {
                            return dropGap.rowCount;
                        }
                        const row = trackListView.itemAtIndex(at);
                        return row !== null && y > row.y + row.height / 2 ? at + 1 : at;
                    }
                    readonly property real lineY: {
                        if (targetIndex < 0) {
                            return 0;
                        }
                        const last = targetIndex >= dropGap.rowCount;
                        const row = trackListView.itemAtIndex(last ? dropGap.rowCount - 1 : targetIndex);
                        return row === null ? 0 : row.y + (last ? row.height : 0) - trackListView.contentY;
                    }
                    onDropped: drop => {
                        if (!drop.hasUrls) {
                            return;
                        }
                        const to = fileDropArea.targetIndex;
                        drop.accept(Qt.CopyAction);
                        root.importFiles(drop.urls, root.shownPlaylist, false,
                                         to >= 0 && to < dropGap.rowCount ? to : -1);
                    }
                    Rectangle {
                        objectName: "trackFileDropOutline"
                        anchors.fill: parent
                        visible: fileDropArea.containsDrag
                        color: "transparent"
                        border.color: Theme.accent
                        border.width: 2
                        radius: Theme.cornerRadius
                    }
                    Rectangle {
                        objectName: "trackFileDropLine"
                        visible: fileDropArea.targetIndex >= 0
                        x: root.trackRowInset
                        width: parent.width - 2 * root.trackRowInset
                        y: Theme.snap(fileDropArea.lineY) - 1
                        height: 3
                        radius: 1
                        color: Theme.accent
                    }
                }
                PlaylistDropGap {
                    id: dropGap
                    objectName: "playlistDropGap"
                    parent: trackListView
                    anchors.fill: parent
                    draggedTrack: trackDropArea.containsDrag ? trackDropArea.drag.source : null
                    draggedIndex: draggedTrack && draggedTrack.sourceId !== undefined
                        ? scanController.tracks.indexOfSourceId(draggedTrack.sourceId) : -1
                    playlist: root.shownPlaylist
                    // The playlist whole, in its own order: sorted, searched
                    // or with rows hidden, a row's place is not its place in
                    // the playlist.
                    orderShown: root.sortField === "playlist" && root.sortAscending && searchField.text.length === 0
                        && !root.appSettingsController.hideStreamingTracks
                    hovering: trackDropArea.containsDrag
                    pointerY: trackDropArea.drag.y
                    contentY: trackListView.contentY
                    originY: trackListView.originY
                    // The playlist's rows: Own Recordings' recordings come
                    // after them and have no place in it.
                    rowCount: trackListView.count - (root.showingRecordings ? scanController.recordingRowsShown : 0)
                    // A track from elsewhere gets a place too; the card then
                    // asks, holding the gap open meanwhile.
                    takesOtherTracks: true
                    heldIndex: trackDrag.confirming && trackDrag.targetIndex >= 0 ? trackDrag.targetIndex : root.settleGap
                    heldTrack: trackDrag.confirming && trackDrag.targetIndex >= 0 ? trackDrag : root.settleTrack
                    onPlaceRequested: toIndex => trackDrag.dropOn(root.shownPlaylist, root.dropSourceFor(root.shownPlaylist),
                                                                  toIndex)
                    onMoveRequested: (filePath, title, toIndex) => {
                        root.settle(dropGap.releasedIndex);
                        root.playlistEditController.moveInPlaylist(root.rekordboxPath, root.enginePath,
                                                                   root.shownPlaylist, filePath, title, toIndex);
                    }
                }
            }

            // Step 1 of "Merge with...": search for the second track.
            // Deliberately searches ScanController's full unfiltered
            // m_allTracks (via findMergeCandidates()), never the page's
            // own filtered/sorted `tracks` model or scanController.search()
            // Typing here must not disturb whatever's shown on the page
            // underneath once this closes.
            PanelPopup {
                id: mergePickerPopup
                modal: true
                focus: true
                x: Theme.snap((root.width - width) / 2)
                y: Theme.snap((root.height - height) / 2)
                width: 480
                height: 420

                property string trackASourceId: ""
                property string trackATitle: ""
                property string trackAArtist: ""

                function showFor(delegate) {
                    mergePickerPopup.trackASourceId = delegate.sourceId;
                    mergePickerPopup.trackATitle = delegate.title;
                    mergePickerPopup.trackAArtist = delegate.artist;
                    mergeSearchField.text = "";
                    mergeCandidatesList.model = [];
                    mergePickerPopup.open();
                    mergeSearchField.forceActiveFocus();
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: "Merge “" + mergePickerPopup.trackATitle + " - " + mergePickerPopup.trackAArtist + "” with:"
                        font.bold: true
                    }
                    TextField {
                        id: mergeSearchField
                        Layout.fillWidth: true
                        placeholderText: "Search title or artist..."
                        onTextChanged: mergeCandidatesList.model =
                            scanController.findMergeCandidates(text, mergePickerPopup.trackASourceId)
                    }
                    ListView {
                        id: mergeCandidatesList
                        interactive: contentHeight > height
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: []
                        spacing: 2

                        ScrollBar.vertical: BigScrollBar {}

                        delegate: ItemDelegate {
                            width: ListView.view.width
                            required property var modelData

                            contentItem: ColumnLayout {
                                spacing: 1
                                Label {
                                    text: modelData.title + " - " + modelData.artist
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: modelData.filePath
                                    color: Theme.textMuted
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                    font.pointSize: Theme.fontSmall
                                }
                            }
                            onClicked: {
                                var a = { sourceId: mergePickerPopup.trackASourceId, title: mergePickerPopup.trackATitle,
                                    artist: mergePickerPopup.trackAArtist };
                                mergePickerPopup.close();
                                mergeReviewPopup.showFor(a, modelData);
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: mergeSearchField.text.length === 0
                            text: "Type to search for the track to merge with."
                            color: Theme.textMuted
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: mergeSearchField.text.length > 0 && mergeCandidatesList.count === 0
                            text: "No matches."
                            color: Theme.textMuted
                        }
                    }
                    Button {
                        text: "Cancel"
                        Layout.alignment: Qt.AlignRight
                        onClicked: mergePickerPopup.close()
                    }
                }
            }

            // Step 2: review the plan (survivor, cues to merge, playlist
            // note) before actually applying, exactly the same
            // DuplicateCleanupPlanner output and apply() path Clean Up
            // Duplicates uses for an auto-detected group, just seeded with
            // this one manually-chosen pair via planManualMerge().
            PanelPopup {
                id: mergeReviewPopup
                modal: true
                focus: true
                x: Theme.snap((root.width - width) / 2)
                y: Theme.snap((root.height - height) / 2)
                width: 520

                property string trackALabel: ""
                property string trackBLabel: ""

                function showFor(trackA, trackB) {
                    mergeReviewPopup.trackALabel = trackA.title + " - " + trackA.artist;
                    mergeReviewPopup.trackBLabel = trackB.title + " - " + trackB.artist;
                    mergeController.planManualMerge(root.format, root.currentPath(), trackA.sourceId, trackB.sourceId);
                    mergeReviewPopup.open();
                }

                onClosed: root.rescan()

                ColumnLayout {
                    width: parent.width
                    spacing: 10

                    PageTitle {
                        text: "Merge Tracks"
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: "“" + mergeReviewPopup.trackALabel + "” + “" + mergeReviewPopup.trackBLabel + "”"
                    }

                    RowLayout {
                        visible: mergeController.busy
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 8
                        SeabassBusyIndicator { running: mergeController.busy; implicitWidth: 24; implicitHeight: 24 }
                        Label { text: mergeController.writing ? "Merging..." : "Comparing tracks..."; color: Theme.textMuted }
                    }

                    Label {
                        visible: mergeController.errorMessage.length > 0
                        text: mergeController.errorMessage
                        color: Theme.danger
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: mergeController.statusMessage.length > 0
                        text: mergeController.statusMessage
                        color: Theme.good
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }

                    Repeater {
                        // statusMessage only ever gets set once apply()
                        // finishes, once it's non-empty, hide the plan
                        // preview even though apply() -> onWriteFinished()
                        // triggers its own trailing rescan() that briefly
                        // repopulates `plans` with the full auto-detected
                        // list (the same refresh CleanupPage.qml relies on
                        // after every apply, not worth special-casing
                        // away just for this dialog).
                        model: mergeController.statusMessage.length === 0
                            ? mergeController.plans : null

                        delegate: ColumnLayout {
                            id: planRow
                            Layout.fillWidth: true
                            spacing: 6

                            required property int index
                            required property var survivor
                            required property var toRemove
                            required property bool differs
                            required property string wastedBytesHuman
                            required property int newCueCount
                            required property bool included

                            RowLayout {
                                Layout.fillWidth: true
                                SeabassCheckBox {
                                    checked: planRow.included
                                    onToggled: mergeController.setIncluded(planRow.index, checked)
                                }
                                Label {
                                    text: "Keeps: " + planRow.survivor.title + " - " + planRow.survivor.artist
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                            Label {
                                text: planRow.wastedBytesHuman + " freed"
                                    + (planRow.newCueCount > 0 ? " - " + planRow.newCueCount + " cue(s) merged onto the survivor" : "")
                                color: Theme.textMuted
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: "Conserved: cues (merged, never lost) and playlist membership on both formats. "
                                    + "Every playlist the removed copy was in now points at the kept copy instead. "
                                    + "Not conserved yet: rating, color tag, genre and other tag fields."
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                            Label {
                                visible: planRow.differs
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                text: "These copies differ in quality and length. The higher-bitrate copy isn't the "
                                    + "longest one. Check the box above if you still want to merge them."
                                color: Theme.conflictText
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Button {
                            text: "Merge These Tracks"
                            visible: mergeController.statusMessage.length === 0
                            enabled: !mergeController.busy && mergeController.includedCount > 0
                            onClicked: mergeController.apply()
                        }
                        Button {
                            text: mergeController.statusMessage.length > 0 ? "Done" : "Cancel"
                            enabled: !mergeController.writing
                            onClicked: mergeReviewPopup.close()
                        }
                    }
                }
            }
        }

        TrackDetailPanel {
            id: trackDetailPanel
            objectName: "trackDetailPanel"
            visible: root.trackPanelOpen
            SplitView.preferredWidth: 460
            SplitView.minimumWidth: 360
            addCueController: addCueController
            playbackController: root.playbackController
            trackEditController: root.trackEditController
            rekordboxPath: root.rekordboxPath
            enginePath: root.enginePath
            format: root.format
            libraryPath: root.currentPath()
            scanController: scanController
            keyNotation: root.appSettingsController.keyNotation
            appSettingsController: root.appSettingsController
            onCloseRequested: root.trackPanelOpen = false
            onRescanRequested: root.rescan()
        }

        MatchingPage {
            id: matchingPage
            objectName: "matchingPage"
            visible: root.matchingPanelOpen
            SplitView.preferredWidth: Math.max(420, root.width * 0.38)
            SplitView.minimumWidth: 420
            scanController: scanController
            keyNotation: root.appSettingsController.keyNotation
            browseSelectedPlaylistIndex: root.selectedPlaylistIndex
            anchorSourceId: root.anchorSourceId
            anchorTitle: root.anchorTitle
            anchorArtist: root.anchorArtist
            anchorKey: root.anchorKey
            anchorBpm: root.anchorBpm
            anchorArtworkPath: root.anchorArtworkPath
            anchorFallbackArtworkPath: root.anchorFallbackArtworkPath
            anchorPlaylistNames: root.anchorPlaylistNames
            onCloseRequested: root.matchingPanelOpen = false
            onMenuRequested: (candidate, at) => root.openTrackMenu(candidate, at)
            trackDrag: root.trackDragCard
            // Just play it: the detail panel a Browse row opens shares
            // this side of the page with Matching, which stays.
            onPlayRequested: candidate => root.playbackController.load(root.format, root.currentPath(),
                candidate.sourceId, candidate.filePath, candidate.title, candidate.artist,
                candidate.artworkPath, candidate.cues)
        }
        } // SplitView
    }

    // A cancelled scan takes the user back to where they came from; the
    // next visit scans from scratch (nothing partial is ever kept).
    Connections {
        target: scanController
        function onScanCancelled() { root.StackView.view.pop(); }
        // The cues have landed in the rows (they update where they stand);
        // the details pane holds a copy of its track's, taken when it was
        // opened, so it is handed the real ones too. Only the cues: a cue
        // being placed there stays. So does the player, for a track
        // played before they landed: it takes them without stopping.
        function onTracksPublished(cuesLanded) {
            if (!cuesLanded) {
                return;
            }
            root.handThePlayerItsCues();
            if (!root.trackPanelOpen || trackDetailPanel.trackSourceId.length === 0) {
                return;
            }
            const row = scanController.tracks.indexOfSourceId(trackDetailPanel.trackSourceId);
            if (row >= 0) {
                trackDetailPanel.trackCues = scanController.tracks.trackAt(row).cues;
            }
        }
    }

    // While the drag card asks what a drop should do, a press anywhere
    // else lets it go and does nothing besides: the user is answering the
    // card, not carrying on with the page. Pressing the row again counts.
    MouseArea {
        objectName: "trackDropDismissArea"
        anchors.fill: parent
        z: trackDrag.z - 1
        visible: trackDrag.confirming
        acceptedButtons: Qt.AllButtons
        onPressed: trackDrag.close()
    }

    TrackDragItem {
        id: trackDrag
        objectName: "trackDragItem"
        keyNotation: root.appSettingsController.keyNotation
        // Over the track list the card and the gap are one thing: while
        // the pointer waits out the gap's delay the card says to hold; once
        // the gap is open the faded row in it ("Move track here") is where
        // the track lands, and the card stands back, smaller and dimmed,
        // as the pointer. Only for a place that moves it: the gaps either
        // side of its own row say nothing.
        // Over a playlist that takes the drop, the card says so (the row
        // wears the outline too); over a place in the list that would
        // move the track, the gap is the sign.
        // A playlist the drag opened says so until the gap takes over.
        hint: trackDrag.confirming ? ""
            : playlistSidebarList.hoveredDropTarget.length > 0
            ? (playlistSidebarList.hoveredDropTarget === root.springOpened
               ? qsTr("Opened \"%1\": drop it where it goes").arg(root.springOpened)
               : trackDrag.playlistNames.indexOf(playlistSidebarList.hoveredDropTarget) >= 0
               ? qsTr("Already in \"%1\"").arg(playlistSidebarList.hoveredDropTarget)
               : qsTr("Drop to add to \"%1\"").arg(playlistSidebarList.hoveredDropTarget))
            : dropGap.gapIndex < 0 && dropGap.hovering && dropGap.applies && dropGap.toIndexFor(dropGap.pending) >= 0
            ? (dropGap.addsTrack ? qsTr("Hold to add it here") : qsTr("Hold to move it here"))
            : dropGap.gapIndex < 0 && root.springOpened.length > 0
            ? qsTr("Opened \"%1\": drop it where it goes").arg(root.springOpened) : ""
        dimmed: dropGap.gapIndex >= 0
        // Staged like an "Add to" from the row menu; Save writes it. Let go
        // at a place in the list, the add carries that place.
        onAddChosen: playlist => root.addDraggedTrack(playlist)
        // The halves staged in this order: the add is looked up first.
        // The removal from the source is staged with the add, after it,
        // or not at all when the add is not (see missingLibrariesDialog).
        onMoveChosen: (playlist, fromPlaylist) => root.addDraggedTrack(playlist, fromPlaylist)
        // Already in the playlist: a move to its last place, as a drag
        // within the playlist stages it.
        onMoveToEndChosen: playlist => {
            const count = (scanController.playlistTrackCounts[playlist] ?? 0)
                + playlistSidebarList.trackCountChange(playlist);
            root.playlistEditController.moveInPlaylist(root.rekordboxPath, root.enginePath, playlist,
                                                       trackDrag.filePath, trackDrag.title, Math.max(0, count - 1));
        }
    }

    // While a track is dragged, the sidebar and the track list scroll when
    // the pointer is held at their top or bottom edge, so a playlist or a
    // place out of sight can still be reached. The pointer is the card's
    // hot spot; `x` and `y` are the card's, read so the binding follows it.
    function dragPointIn(view, x, y) {
        return trackDrag.mapToItem(view, trackDrag.Drag.hotSpot.x, trackDrag.Drag.hotSpot.y);
    }
    function dragIsOver(view, at) {
        return at.x >= 0 && at.x < view.width && at.y >= 0 && at.y < view.height;
    }
    DragAutoScroller {
        id: listAutoScroller
        objectName: "trackListAutoScroller"
        property point at: root.dragging ? root.dragPointIn(trackListView, trackDrag.x, trackDrag.y) : Qt.point(-1, -1)
        view: trackListView
        active: root.dragging && root.dragIsOver(trackListView, at)
        pointerY: at.y
    }
    DragAutoScroller {
        id: sidebarAutoScroller
        objectName: "sidebarAutoScroller"
        property point at: root.dragging ? root.dragPointIn(playlistSidebarList, trackDrag.x, trackDrag.y)
                                         : Qt.point(-1, -1)
        view: playlistSidebarList
        active: root.dragging && root.dragIsOver(playlistSidebarList, at)
        pointerY: at.y
    }

    // One add, at the place the track was let go when it was (-1: at the
    // end): one change on the Save button, not an add and a move.
    function addDraggedTrack(playlist, moveFrom) {
        if (trackDrag.targetIndex >= 0) {
            root.settle(trackDrag.targetIndex);
        }
        if (!root.addTrackToPlaylist(playlist, trackDrag.filePath, trackDrag.title, trackDrag.targetIndex,
                                     moveFrom ?? "")) {
            root.settled();
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: scanController.busy
        current: scanController.scanCurrent
        total: scanController.scanTotal
        label: "Scanning library..."
        cancellable: scanController.scanCancellable
        onCancelRequested: scanController.cancelScan()
    }
}
