// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Templates as T
import SeabassGui

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
        // playlistNames only becomes available once the (async) scan
        // finishes, so restoring the last-selected playlist has to wait
        // for this rather than happening at Component.onCompleted
        // alongside rescan() below.
        onPlaylistNamesChanged: root.restoreSelectedPlaylist()
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

    // Deleting a playlist and taking a track out of one (Experimental):
    // staged into the same session as an added cue, written by the same
    // Save into every library on the stick that has the playlist.
    readonly property bool playlistEditing: root.appSettingsController.experimentalFeaturesEnabled === true
    // The playlist the list is showing, by name: set where the selection
    // is made, since the row number is the FILTERED list's when the
    // playlist search is in use.
    property string shownPlaylist: ""
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

    Menu {
        id: playlistMenu
        objectName: "playlistMenu"
        property string playlist: ""
        readonly property bool staged: root.playlistEditController.pendingRevision >= 0
            && playlist.length > 0 && root.playlistEditController.isPlaylistStaged(playlist)
        // One staged to be created: the menu takes the create back.
        readonly property bool isNew: root.playlistEditController.pendingRevision >= 0
            && playlist.length > 0 && root.playlistEditController.isNewPlaylistStaged(playlist)
        // Staged to be renamed: the name the stick has, which Keep gives back.
        readonly property string oldName: root.playlistEditController.pendingRevision >= 0 && playlist.length > 0
            && ("pendingRenames" in root.playlistEditController)
            ? root.oldPlaylistName(playlist) : ""
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
        MenuItem {
            objectName: "deletePlaylistItem"
            text: playlistMenu.isNew ? qsTr("Don't create this playlist")
                : playlistMenu.staged ? qsTr("Keep this playlist") : qsTr("Delete playlist...")
            onTriggered: {
                if (playlistMenu.isNew) {
                    root.playlistEditController.dropNewPlaylist(playlistMenu.playlist);
                } else if (playlistMenu.staged) {
                    root.playlistEditController.keepPlaylist(playlistMenu.playlist);
                } else {
                    deletePlaylistDialog.playlist = playlistMenu.playlist;
                    deletePlaylistDialog.open();
                }
            }
        }
    }

    // The three playlist dialogs wear SeabassDialog's frame, which is
    // what insets their buttons from the edge: a plain Dialog left them
    // flush against the frame's right and bottom border.
    SeabassDialog {
        id: deletePlaylistDialog
        objectName: "deletePlaylistDialog"
        property string playlist: ""
        severity: SeabassDialog.Question
        title: qsTr("Delete playlist")
        headline: qsTr("Delete \"%1\" from every library on this stick?").arg(deletePlaylistDialog.playlist)
        detailText: qsTr("Its tracks stay on the stick. Nothing changes until you Save.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.playlistEditController.deletePlaylist(root.rekordboxPath, root.enginePath, deletePlaylistDialog.playlist)
    }

    // A new name for a playlist (its last segment), given in every library
    // on the stick on Save.
    SeabassDialog {
        id: renamePlaylistDialog
        objectName: "renamePlaylistDialog"
        // Assigned, as SeabassDialog's selectFooterButton does; set here
        // because OK is still disabled when the name is empty, and that
        // helper passes over a disabled button.
        function markOkAsDefault() {
            standardButton(Dialog.Ok).highlighted = true;
            standardButton(Dialog.Cancel).highlighted = false;
        }
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
            renamePlaylistDialog.markOkAsDefault();
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
    Dialog {
        id: missingLibrariesDialog
        objectName: "missingLibrariesDialog"
        property string playlist: ""
        property string filePath: ""
        property string trackTitle: ""
        property var missing: []
        property var added: []
        readonly property bool engineMissing: missing.indexOf("Engine") >= 0
        readonly property bool pioneerMissing: missing.indexOf("rekordbox") >= 0 || missing.indexOf("OneLibrary") >= 0
        anchors.centerIn: parent
        modal: true
        title: qsTr("Not in every library")
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
        Label {
            objectName: "missingLibrariesText"
            width: Math.min(implicitWidth, root.width * 0.6)
            wrapMode: Text.Wrap
            text: missingLibrariesDialog.text
        }
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
    }

    // Playlist edits a crash, a kill or a power cut left staged (Seabass
    // keeps them on this machine while they are, see StagedEditsStore):
    // offered back once the page has its stick and its session. Not while
    // anything is staged into the session or a save is writing: the
    // question waits until the session is clean (the Connections below),
    // so another page's batch saved from here does not leave this one to
    // be overwritten by the first new edit. Not without Experimental
    // (playlist editing is): the batch stays for next time. Nothing is
    // staged until it is answered.
    property bool leftOverEditsChecked: false
    function offerLeftOverEdits() {
        const controller = root.playlistEditController;
        const session = editHost.session;
        if (session === null || typeof controller.leftOverEdits !== "function") {
            return;
        }
        if (!root.playlistEditing) {
            root.leftOverEditsChecked = true;
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
    Dialog {
        id: leftOverEditsDialog
        objectName: "leftOverEditsDialog"
        property int count: 0
        property string stick: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("Unsaved playlist edits")
        // Answered, never dismissed: Discard drops a batch that may have
        // taken half an hour, and Keep stages it.
        closePolicy: Popup.NoAutoClose
        readonly property string text: (count === 1
            ? qsTr("Seabass last ran with 1 playlist edit staged for %1 and had not saved it. Keep editing it?").arg(stick)
            : qsTr("Seabass last ran with %1 playlist edits staged for %2 and had not saved them. Keep editing them?")
                .arg(count).arg(stick))
        Label {
            objectName: "leftOverEditsText"
            width: Math.min(implicitWidth, root.width * 0.6)
            wrapMode: Text.Wrap
            text: leftOverEditsDialog.text
        }
        footer: DialogButtonBox {
            Button {
                id: keepLeftOverEditsButton
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
        // Keep is the default: the key that answers takes it.
        onOpened: keepLeftOverEditsButton.forceActiveFocus()
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
        function markOkAsDefault() {
            standardButton(Dialog.Ok).highlighted = true;
            standardButton(Dialog.Cancel).highlighted = false;
        }
        severity: SeabassDialog.Question
        title: qsTr("Create new playlist")
        headline: qsTr("It is made in every library on this stick.")
        detailText: qsTr("Nothing changes until you Save.")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: newPlaylistNameField.text = ""
        // Later than SeabassDialog's own onOpened, as in the rename dialog.
        onOpened: Qt.callLater(() => {
            createPlaylistDialog.markOkAsDefault();
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
        if (root.playlistEditing) {
            createPlaylistDialog.open();
        }
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
        // Only for a track in the playlist shown: a Matching result opens
        // this menu too, and need not be in it.
        readonly property bool canRemove: root.playlistEditing && root.shownPlaylist.length > 0 && filePath.length > 0
            && (!row || !row.playlistNames || row.playlistNames.indexOf(root.shownPlaylist) >= 0)
        readonly property bool staged: canRemove && root.playlistEditController.pendingRevision >= 0
            && root.playlistEditController.isRemovalStaged(root.shownPlaylist, filePath)
        // In the playlist shown only because it is staged to be added.
        readonly property bool pendingAddition: canRemove && row !== null && row.pendingAddition === true
        // At a place in the playlist shown that a staged move gives it.
        readonly property bool pendingMove: canRemove && row !== null && row.pendingMove === true
        // Adding to a playlist (Experimental): the playlists this stick
        // has and the ones staged to be created, but not the one shown,
        // which already holds the track.
        readonly property bool canAdd: root.playlistEditing && filePath.length > 0 && !streaming
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
                root.playlistEditController.addToPlaylist(root.rekordboxPath, root.enginePath, playlist,
                                                          filePath, row.title);
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
            enabled: root.format !== "onelibrary" && !trackMenu.streaming
            text: trackMenu.streaming ? qsTr("Merge with another track... (streaming track, no local file)")
                : root.format === "onelibrary" ? qsTr("Merge with another track... (not on OneLibrary yet)")
                : qsTr("Merge with another track...")
            onTriggered: mergePickerPopup.showFor(trackMenu.row)
        }
        MenuItem {
            objectName: "findMatchingItem"
            text: qsTr("Find matching tracks")
            onTriggered: root.toggleAnchor(trackMenu.row)
        }
        MenuSeparator {
            visible: trackMenu.canRemove
            height: visible ? implicitHeight : 0
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
    Menu {
        id: addToMenu
        objectName: "addToMenu"
        title: qsTr("Add to...")
        Instantiator {
            model: trackMenu.addTargets
            delegate: MenuItem {
                required property string modelData
                objectName: "addTo_" + modelData
                text: modelData
                checkable: true
                checked: trackMenu.filePath.length > 0 && trackMenu.addedTo(modelData)
                onTriggered: {
                    trackMenu.toggleAdd(modelData);
                    // A click flips `checked` itself; the staging says.
                    checked = Qt.binding(() => trackMenu.filePath.length > 0 && trackMenu.addedTo(modelData));
                }
            }
            onObjectAdded: (index, object) => addToMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => addToMenu.removeItem(object)
        }
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

    readonly property var newPlaylistNames: ("newPlaylists" in root.playlistEditController)
        ? root.playlistEditController.newPlaylists : []
    // The tracks staged into a playlist show in it, greyed, until Save.
    readonly property var pendingAdditions: ("pendingAdditions" in root.playlistEditController)
        ? root.playlistEditController.pendingAdditions : ({})
    onPendingAdditionsChanged: scanController.setPendingEdits(root.pendingAdditions, root.pendingMoves)
    // The tracks staged to move within a playlist show at their new place,
    // greyed, until Save.
    readonly property var pendingMoves: ("pendingMoves" in root.playlistEditController)
        ? root.playlistEditController.pendingMoves : ({})
    onPendingMovesChanged: scanController.setPendingEdits(root.pendingAdditions, root.pendingMoves)
    // A playlist staged to be renamed is listed under its new name, its
    // tracks the scanned playlist's.
    readonly property var pendingRenames: ("pendingRenames" in root.playlistEditController)
        ? root.playlistEditController.pendingRenames : ({})
    onPendingRenamesChanged: scanController.setPlaylistAliases(root.pendingRenames)

    function openPlaylistMenu(name, row) {
        if (!root.playlistEditing) {
            return;
        }
        playlistMenu.playlist = name;
        playlistMenu.popup(row);
    }

    // The row's menu, at `at` (the row's menu button, or the row itself
    // for a right click).
    function openTrackMenu(row, at) {
        trackMenu.row = row;
        trackMenu.popup(at);
    }

    // ---- Dragging a track to a playlist (Experimental) ----
    // The drag card Browse's rows and the Matching panel's results share;
    // null with playlist editing off, and then nothing drags.
    readonly property var trackDragCard: root.playlistEditing ? trackDrag : null
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
        const edits = ("lastSavedSummary" in root.playlistEditController)
            ? root.playlistEditController.lastSavedSummary : "";
        savedToast.show(edits.length > 0 ? edits : summary.written + " " + summary.unit + " " + summary.verb);
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
        root.selectedPlaylistIndex = index;
        // -1: a playlist staged to be created, listed below the others.
        root.shownPlaylist = index === 0 ? "" : name;
        scanController.filterByPlaylist(index === 0 ? "" : name);
        if (remember !== false) {
            root.appSettingsController.lastPlaylistName = index === 0 ? "" : name;
        }
    }

    // Called once scanController.playlistNames is populated (see
    // onPlaylistNamesChanged above). Falls back to "All tracks" if the
    // last-selected name doesn't match any playlist on this stick --
    // e.g. it was deleted, or this is a different stick than last time --
    // and keeps remembering it, since the next stick may well have it.
    // A staged rename lists it under its new name, and a take-back under
    // its old one again: currentPlaylistName follows it there.
    function restoreSelectedPlaylist() {
        const wanted = scanController.currentPlaylistName(root.appSettingsController.lastPlaylistName);
        const idx = wanted.length > 0 ? scanController.playlistNames.indexOf(wanted) : -1;
        root.selectPlaylist(idx >= 0 ? idx + 1 : 0, wanted, false);
    }

    // ---- Matching (Experimental, see docs/experimental-
    // features.md) -- the playlist selection above is shared between the
    // always-on left Pane (when this is off) and the off-canvas Drawer +
    // the panel's own "This Playlist" chip (when it's on); the anchor
    // properties below track whichever Browse row's edit button was last
    // clicked, read by MatchingPage to find compatible tracks. ----
    // Matching graduated from experimental on 2026-09-17, so this page has
    // one layout: the playlists sidebar and the panel, no classic playlist
    // column behind a setting. The panel still wears its own PREVIEW badge,
    // because the search side is real and the write side is not (no format
    // has a playlist writer yet). See docs/experimental-features.md.
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
                    && root.playbackController.currentLibraryPath === root.currentPath()) {
                trackDetailPanel.showFor(scanController.tracks.trackAt(row));
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
        root.offerQueue();
        root.playbackController.load(root.format, root.currentPath(), row.sourceId, row.filePath, row.title,
            row.artist, row.artworkPath, row.cues);
        trackDetailPanel.showFor(row);
        root.trackPanelOpen = true;
    }
    // Starts open, like the playlist column it replaced -- the header pill
    // collapses and expands it, unlike matchingPanelOpen above, which starts
    // collapsed.
    property bool playlistSidebarOpen: true
    // The playlist shown, by the name it is listed under: a new one staged
    // to be created (picked at index -1, not among the scan's names) and a
    // renamed one's new name included.
    readonly property string currentPlaylistLabel: root.shownPlaylist.length > 0 ? root.shownPlaylist : "All tracks"

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
    readonly property string unsortedOrderName: root.shownPlaylist.length > 0
        ? "the playlist's own order" : "the library's own order"
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
                TextField {
                    id: searchField
                    objectName: "searchField"
                    Layout.preferredWidth: 260
                    // Also filters the playlist list below (see its own
                    // model binding) -- one search field instead of two,
                    // since "search" and "filter playlists" were doing
                    // the same job on two different, easy-to-miss fields.
                    placeholderText: "Search title, artist, or playlist..."
                    rightPadding: searchClearButton.visible ? searchClearButton.width + 4 : 0
                    // As tall as the combo beside it, and on a ground
                    // Theme chose: left to the style, this sat 25 px
                    // taller than the combo and square beside its
                    // rounded neighbours.
                    implicitHeight: Theme.compactControlHeight
                    color: Theme.text
                    placeholderTextColor: Theme.textMuted
                    leftPadding: 8
                    background: Rectangle {
                        radius: Theme.cornerRadius
                        color: Theme.surface
                        border.color: searchField.activeFocus ? Theme.accent : Theme.border
                        border.width: 1
                    }
                    onTextChanged: scanController.search(text)

                    IconToolButton {
                        id: searchClearButton
                        visible: searchField.text.length > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: Theme.iconSizeSmall + 8
                        implicitHeight: Theme.iconSizeSmall + 8
                        flat: true
                        text: "Clear search"
                        iconName: "edit-clear"
                        ToolTip.visible: hovered
                        ToolTip.text: "Clear search"
                        onClicked: searchField.text = ""
                    }
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

    Label {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 12
        visible: scanController.errorMessage.length > 0
        text: scanController.errorMessage
        color: Theme.danger
        wrapMode: Text.WordWrap
    }

    RowLayout {
        anchors.fill: parent
        anchors.topMargin: scanController.errorMessage.length > 0 ? 40 : 0
        // The header's inset, so the list starts on the same left line
        // as the breadcrumb and the search field above it.
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

            PlaylistListView {
                id: playlistSidebarList
                objectName: "playlistList"
                anchors.fill: parent
                scanController: scanController
                searchQuery: searchField.text
                selectedIndex: root.selectedPlaylistIndex
                onPlaylistPicked: (index, name) => root.selectPlaylist(index, name)
                isPendingDelete: name => root.playlistEditController.isPlaylistStaged(name)
                isRenamed: name => ("isRenameStaged" in root.playlistEditController)
                    && root.playlistEditController.isRenameStaged(name)
                isEdited: name => ("isPlaylistEdited" in root.playlistEditController)
                    && root.playlistEditController.isPlaylistEdited(name)
                trackCountChange: name => ("trackCountChange" in root.playlistEditController)
                    ? root.playlistEditController.trackCountChange(name) : 0
                pendingRevision: root.playlistEditController.pendingRevision
                onPlaylistContextRequested: (name, row) => root.openPlaylistMenu(name, row)
                canCreate: root.playlistEditing
                newPlaylists: root.newPlaylistNames
                shownPlaylist: root.shownPlaylist
                keyNotation: root.appSettingsController.keyNotation
                onNewPlaylistPicked: name => root.selectPlaylist(-1, name)
                onCreateRequested: root.openCreatePlaylistDialog()
                // A dragged track is dropped on a playlist row; the card
                // then asks what to do (TrackDragItem).
                trackDropsEnabled: root.playlistEditing
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
                            waveformData: playbackController.waveform
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
                    // Press and move drags the track to a playlist
                    // (Experimental, see TrackDragArea).
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
                            trackDetailPanel.showFor(trackDelegate);
                            root.trackPanelOpen = true;
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

                        Rectangle {
                            id: artworkRect
                            objectName: "rowCell_artwork"
                            Layout.preferredWidth: root.artworkColumnWidth
                            Layout.preferredHeight: Theme.iconSizeNormal
                            color: Theme.surface
                            radius: Theme.coverRadius(Math.min(width, height))
                            // An Engine row's own art first, the same song's
                            // rekordbox art when that does not load.
                            ArtworkImage {
                                objectName: "rowArtwork"
                                anchors.fill: parent
                                source: trackDelegate.artworkPath
                                fallbackSource: trackDelegate.fallbackArtworkPath
                            }
                            // Play, direct from the row -- the rest of the
                            // row (below) now opens the track detail page
                            // instead, so this is the one place left that
                            // plays without an extra click. Only shown on
                            // hover so the artwork itself isn't permanently
                            // obscured, and only for a track that can
                            // actually play (streaming tracks have no
                            // local file -- see the tooltip below).
                            Rectangle {
                                anchors.fill: parent
                                visible: artworkHoverHandler.hovered && trackDelegate.streamingSource.length === 0
                                color: "#80000000"
                                // The cover's own corners.
                                radius: parent.radius
                                SeabassIcon {
                                    anchors.centerIn: parent
                                    iconName: "media-playback-start"
                                    size: Theme.iconSizeSmall * 0.75
                                    color: "white"
                                }
                            }
                            // At the narrowest tier (see root.browseTier's own
                            // comment) every other column is squeezed out --
                            // this hover tooltip is the only way left to see
                            // title/artist/key/BPM/duration for this row.
                            HoverHandler { id: artworkHoverHandler }
                            ToolTip.visible: artworkHoverHandler.hovered
                                && (trackDelegate.streamingSource.length > 0 || root.browseTier === 0)
                            ToolTip.text: trackDelegate.streamingSource.length > 0
                                ? "Streaming track (" + trackDelegate.streamingSource + ") - no local file, can't be played."
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
                                onClicked: {
                                    if (trackDelegate.streamingSource.length > 0) {
                                        return;
                                    }
                                    root.playRow(trackDelegate);
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
                                    text: title
                                    font.bold: true
                                    // Staged to be added to the playlist shown,
                                    // or moved within it.
                                    font.italic: trackDelegate.pendingAddition || trackDelegate.pendingMove
                                    color: trackDelegate.pendingAddition || trackDelegate.pendingMove
                                        ? Theme.textMuted : Theme.text
                                    // Staged to come out of the playlist shown.
                                    font.strikeout: root.playlistEditing && root.shownPlaylist.length > 0
                                        && root.playlistEditController.pendingRevision >= 0
                                        && root.playlistEditController.isRemovalStaged(root.shownPlaylist, filePath)
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                // Plain text, no logo. This project never
                                // reproduces a real brand's mark (see
                                // AboutPage.qml's trademark note). Just
                                // tells you where a file-less row comes
                                // from instead of it looking broken.
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
                            Label {
                                text: artist
                                color: Theme.textMuted
                                elide: Text.ElideRight
                                Layout.fillWidth: true
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
                            text: scanController.cuesPending ? "" : cueCount
                            Layout.preferredWidth: root.cuesColumnWidth
                        }
                        Label {
                            objectName: "rowCell_plays"
                            visible: root.browseTier >= 2 && root.showPlayCount
                            text: playCount >= 0 ? playCount : "--"
                            Layout.preferredWidth: root.playsColumnWidth
                        }
                        // The header row's spacer, for the same tier.
                        Item {
                            visible: root.browseTier === 0
                            Layout.fillWidth: true
                        }
                        // The row's actions in one menu: merging, finding
                        // matches, and (Experimental, a playlist shown)
                        // taking the track out of that playlist. A right
                        // click on the row opens the same menu.
                        IconToolButton {
                            id: rowMenuButton
                            objectName: "trackRowMenuButton"
                            text: "Track actions"
                            iconName: "application-menu"
                            Layout.preferredWidth: root.actionsColumnWidth
                            ToolTip.visible: hovered
                            ToolTip.text: "Merge, find matching tracks" + (root.playlistEditing && root.shownPlaylist.length > 0
                                ? ", remove from this playlist" : "")
                            onClicked: root.openTrackMenu(trackDelegate, rowMenuButton)
                        }
                    }
                }

                EmptyState {
                    visible: trackListView.count === 0 && !scanController.busy
                    iconName: "edit-find"
                    text: "No tracks found."
                }

                // Dropping a track of the playlist shown between two rows
                // moves it there (Experimental): the drag item carries the
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
                PlaylistDropGap {
                    id: dropGap
                    objectName: "playlistDropGap"
                    parent: trackListView
                    anchors.fill: parent
                    draggedTrack: trackDropArea.containsDrag ? trackDropArea.drag.source : null
                    draggedIndex: draggedTrack && draggedTrack.sourceId !== undefined
                        ? scanController.tracks.indexOfSourceId(draggedTrack.sourceId) : -1
                    playlist: root.playlistEditing ? root.shownPlaylist : ""
                    // The playlist whole, in its own order: sorted, searched
                    // or with rows hidden, a row's place is not its place in
                    // the playlist.
                    orderShown: root.sortField === "playlist" && root.sortAscending && searchField.text.length === 0
                        && !root.appSettingsController.hideStreamingTracks
                    hovering: trackDropArea.containsDrag
                    pointerY: trackDropArea.drag.y
                    contentY: trackListView.contentY
                    originY: trackListView.originY
                    rowCount: trackListView.count
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
            format: root.format
            libraryPath: root.currentPath()
            scanController: scanController
            keyNotation: root.appSettingsController.keyNotation
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
        root.playlistEditController.addToPlaylist(root.rekordboxPath, root.enginePath, playlist,
                                                  trackDrag.filePath, trackDrag.title, trackDrag.targetIndex,
                                                  moveFrom ?? "");
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
