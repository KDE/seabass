// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The playlist list, narrowed by name with searchQuery (Browse's sidebar
// filter, which filters nothing else: see FilterField in ScanPage.qml),
// delegated to PlaylistRowDelegate.qml (also used by the
// "This Playlist" ComboBox's own popup, so both pickers render
// identically). Used in two places in ScanPage.qml: always in the left
// Pane when the Matching panel is off, or inside
// a Drawer when it's on -- both instances share this one definition and
// both write to the same root.selectedPlaylistIndex, so switching
// playlists from either place stays in sync.
ListView {
    id: root
    // Not draggable when everything already fits.
    interactive: contentHeight > height
    required property var scanController
    property string searchQuery: ""
    property int selectedIndex: 0
    signal playlistPicked(int index, string name)
    // The rows above the playlists that are no playlist, each with a
    // kind: "all" ("All tracks", picked as playlistPicked(0, ...)), then
    // whatever the host adds (Browse: "recordings", Own Recordings, picked
    // as fixedRowPicked). Always at the top in this order, whatever the
    // filter; never renamed or deleted. A fixed row may stand for a stick
    // playlist as well (`playlist`, Browse's Own Recordings for the
    // playlist of that name): that playlist is then this row and not
    // listed a second time below, and the row counts, changes, sums up
    // and takes dropped tracks and files as that playlist. Any other stick
    // playlist of a fixed row's name is a row of its own below them.
    property var fixedRows: [{kind: "all", name: "All tracks"}]
    readonly property int fixedRowCount: root.fixedRows.length
    function fixedKindAt(index) {
        return index >= 0 && index < root.fixedRowCount ? root.fixedRows[index].kind : "";
    }
    function fixedPlaylistAt(index) {
        return index >= 0 && index < root.fixedRowCount ? (root.fixedRows[index].playlist ?? "") : "";
    }
    // The stick playlists a fixed row stands for.
    readonly property var fixedPlaylists: root.fixedRows.map(row => row.playlist ?? "").filter(name => name.length > 0)
    // The fixed row shown, by kind; "" while a playlist is.
    property string shownFixedRow: root.shownPlaylist.length === 0 ? "all" : ""
    signal fixedRowPicked(string kind)
    // A playlist row's context menu was asked for ("All tracks" never
    // asks). Browse answers it with the playlist's menu.
    signal playlistContextRequested(string name, Item row)
    // ...and a fixed row's that stands for a playlist: no playlist menu
    // (it is not renamed or deleted), only what takes back what is staged
    // for it, which the host offers when there is any.
    signal fixedRowContextRequested(string kind, Item row)
    // Whether a playlist is staged for deletion; re-read when
    // pendingRevision moves.
    property var isPendingDelete: name => false
    property int pendingRevision: 0
    // Creating a playlist (Browse): the playlists staged to
    // be created, shown after the stick's, and a last row that asks for a
    // new one. Off, the list ends with the stick's playlists.
    property bool canCreate: false
    property var newPlaylists: []
    // Whether tracks are staged into or out of a playlist, and by how
    // many its count changes at Save (read again on pendingRevision).
    property var isEdited: name => false
    property var isRenamed: name => false
    property var trackCountChange: name => 0
    // The playlist the page shows, so a new one picked is marked current.
    property string shownPlaylist: ""
    // Dropping a dragged track on a playlist (Browse): the
    // rows that take one are every playlist but "All tracks", the one
    // shown and one staged for deletion. heldDropTarget stays marked while
    // the drag card asks what the drop should do.
    property bool trackDropsEnabled: false
    // The playlists the dragged track is in: the shown one takes no drop
    // when it is among them.
    property var droppedTrackPlaylists: []
    // The playlist a dragged track hovers right now, "" when none: the
    // page's drag card says what the drop will do there.
    property string hoveredDropTarget: ""
    function noteDropTarget(name, over) {
        if (over) {
            root.hoveredDropTarget = name;
        } else if (root.hoveredDropTarget === name) {
            root.hoveredDropTarget = "";
        }
    }
    property string heldDropTarget: ""
    signal trackDropped(string name)
    // Audio files dragged in from the computer (Browse's track import):
    // every playlist row takes them but one staged for deletion, "All
    // tracks" takes them for no playlist (filesDropped with ""), and the
    // empty part of the list and its last row take them for a playlist
    // still to be named (filesDroppedForNew).
    property bool fileDropsEnabled: false
    signal filesDropped(string name, var urls)
    signal filesDroppedForNew(var urls)
    DropArea {
        id: newPlaylistFileDropArea
        objectName: "newPlaylistFileDropArea"
        // The list itself, under its rows: theirs are on top of it.
        parent: root
        anchors.fill: parent
        z: -1
        enabled: root.fileDropsEnabled && root.canCreate
        keys: ["text/uri-list"]
        onDropped: drop => {
            if (drop.hasUrls) {
                drop.accept(Qt.CopyAction);
                root.filesDroppedForNew(drop.urls);
            }
        }
    }
    // Files held at the lower or upper edge of a list with more rows than
    // fit scroll it, so that every row and "Create new playlist" below
    // the last can be reached without letting go. An edge is a scroll
    // zone only while there is somewhere to scroll to: at the list's end
    // the rows under it take the drop again.
    readonly property real edgeScrollZone: 36
    readonly property real edgeScrollTop: root.originY
    readonly property real edgeScrollBottom: Math.max(root.originY, root.originY + root.contentHeight - root.height)
    function edgeScrollStep(direction) {
        root.contentY = Math.max(root.edgeScrollTop, Math.min(root.edgeScrollBottom, root.contentY + direction * 12));
    }
    DropArea {
        id: edgeScrollDown
        objectName: "edgeScrollDown"
        parent: root
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.edgeScrollZone
        z: 2
        enabled: root.fileDropsEnabled && root.contentY < root.edgeScrollBottom - 0.5
        keys: ["text/uri-list"]
    }
    DropArea {
        id: edgeScrollUp
        objectName: "edgeScrollUp"
        parent: root
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.edgeScrollZone
        z: 2
        enabled: root.fileDropsEnabled && root.contentY > root.edgeScrollTop + 0.5
        keys: ["text/uri-list"]
    }
    Timer {
        objectName: "edgeScrollTimer"
        interval: 16
        repeat: true
        running: edgeScrollDown.containsDrag || edgeScrollUp.containsDrag
        onTriggered: root.edgeScrollStep(edgeScrollDown.containsDrag ? 1 : -1)
    }
    // Spring-loaded playlists: a dragged track held over a row that takes
    // it for springLoadDelay opens that playlist (springLoaded) while the
    // drag goes on, so it can be dropped at its place there. Another row
    // starts the wait again; leaving the list stops it, and switches
    // nothing back. As long as a summary waits, by default: the row asks
    // for the same patience either way.
    property int springLoadDelay: 1500
    signal springLoaded(string name)
    onHoveredDropTargetChanged: {
        if (root.hoveredDropTarget.length > 0) {
            springTimer.restart();
        } else {
            springTimer.stop();
        }
    }
    Timer {
        id: springTimer
        objectName: "springLoadTimer"
        interval: root.springLoadDelay
        onTriggered: {
            if (root.hoveredDropTarget.length > 0 && root.hoveredDropTarget !== root.shownPlaylist) {
                root.springLoaded(root.hoveredDropTarget);
            }
        }
    }
    // Tick boxes on the rows (Beat Grids & Waveforms): checkStateOf(name)
    // gives a row's state ("" for "All tracks"), read again when
    // checkRevision moves; a click asks with playlistCheckToggled.
    property bool checkable: false
    property var checkStateOf: name => Qt.Unchecked
    property var checkEnabledFor: name => true
    // Every box at once: off while what is ticked cannot change.
    property bool checksEnabled: true
    property int checkRevision: 0
    signal playlistCheckToggled(string name, bool checked)
    signal createRequested()
    // A playlist staged to be created was clicked.
    signal newPlaylistPicked(string name)
    // Hovering a playlist shows its summary (PlaylistSummaryPopup.qml),
    // "All tracks" the whole library's, after summaryDelay. One popup for
    // the whole list, made the first time it is needed and kept: the rows
    // only say when the pointer is on them, and the list puts the popup
    // beside the hovered one. With it open, another row held for
    // summarySwitchDelay moves it there, content and all, without closing
    // (a pointer travelling down the list passes rows by quicker than
    // that); off every row and out of the popup for summaryGrace, it
    // closes. A press or the wheel in a row closes it too, and it stays
    // shut over that row until the pointer has been elsewhere.
    // Switched off while the pointer is busy with something else, such as
    // dragging tracks onto a row.
    property bool summariesEnabled: true
    property int summaryDelay: 600
    property int summarySwitchDelay: 250
    property int summaryGrace: 200
    property string keyNotation: "camelot"
    // The row under the pointer that has a summary, or null.
    property Item summaryHoverRow: null
    // The row the popup stands beside, or is about to.
    property Item summaryRow: null
    // The row a press or the wheel dismissed the summary on.
    property Item summaryDismissedRow: null
    readonly property var summaryPopup: summaryLoader.item
    // Shown, or on its way in: not once it is on its way out.
    property bool summaryShown: false
    readonly property bool summaryPopupHovered: summaryLoader.item !== null && summaryLoader.item.hovered === true
    onSummaryPopupHoveredChanged: root.updateSummary()
    onSummariesEnabledChanged: root.updateSummary()

    function noteSummaryHover(row, over) {
        if (over) {
            if (row !== root.summaryDismissedRow) {
                root.summaryDismissedRow = null;
            }
            root.summaryHoverRow = row;
        } else if (root.summaryHoverRow === row) {
            root.summaryHoverRow = null;
        }
        root.updateSummary();
    }
    function dismissSummary(row) {
        root.summaryDismissedRow = row;
        root.closeSummary();
    }
    function closeSummary() {
        summaryOpenTimer.stop();
        summarySwitchTimer.stop();
        summaryGraceTimer.stop();
        root.summaryShown = false;
        if (root.summaryPopup !== null) {
            root.summaryPopup.close();
        }
    }
    function updateSummary() {
        if (!root.summariesEnabled) {
            root.closeSummary();
            return;
        }
        const row = root.summaryHoverRow;
        const showing = root.summaryShown;
        if (row !== null && row !== root.summaryDismissedRow) {
            summaryGraceTimer.stop();
            if (!showing) {
                if (root.summaryRow !== row || !summaryOpenTimer.running) {
                    root.summaryRow = row;
                    summaryOpenTimer.restart();
                }
            } else if (row !== root.summaryRow) {
                summarySwitchTimer.restart();
            } else {
                summarySwitchTimer.stop();
            }
            return;
        }
        summaryOpenTimer.stop();
        summarySwitchTimer.stop();
        if (showing && !root.summaryPopupHovered) {
            summaryGraceTimer.restart();
        } else {
            summaryGraceTimer.stop();
        }
    }
    // The popup beside `row`, with its playlist's summary.
    function showSummaryFor(row) {
        root.summaryRow = row;
        summaryLoader.active = true;
        const center = row.mapToItem(root, 0, row.height / 2).y;
        root.summaryPopup.showPlaylist(row.name, row.summaryFor(), center);
        if (!root.summaryShown) {
            root.summaryShown = true;
            root.summaryPopup.open();
        }
    }
    Timer {
        id: summaryOpenTimer
        interval: root.summaryDelay
        onTriggered: {
            if (root.summaryHoverRow !== null && root.summaryHoverRow === root.summaryRow) {
                root.showSummaryFor(root.summaryRow);
            }
        }
    }
    Timer {
        id: summarySwitchTimer
        interval: root.summarySwitchDelay
        onTriggered: {
            if (root.summaryHoverRow !== null) {
                root.showSummaryFor(root.summaryHoverRow);
            }
        }
    }
    Timer {
        id: summaryGraceTimer
        interval: root.summaryGrace
        onTriggered: root.closeSummary()
    }
    Loader {
        id: summaryLoader
        objectName: "summaryLoader"
        active: false
        sourceComponent: PlaylistSummaryPopup {
            parent: root
            notation: root.keyNotation
        }
    }

    clip: true
    ScrollBar.vertical: BigScrollBar {}
    // The playlists staged to be created come right under "All tracks":
    // they are what is being put together, and at the end of a long
    // list they sat below the window's edge, where a track dragged in
    // from the right could not reach them.
    // A new playlist the stick has in some library already (made where
    // the others lack it, Own Recordings') is listed once, as the stick's.
    property var allNames: root.fixedRows.map(row => row.name)
        .concat((root.canCreate ? root.newPlaylists.filter(name => scanController.playlistNames.indexOf(name) < 0) : [])
                .concat(scanController.playlistNames)
                .filter(name => root.fixedPlaylists.indexOf(name) < 0))
    model: root.searchQuery.length === 0
        ? allNames
        : allNames.filter((n, i) => i < root.fixedRowCount || n.toLowerCase().includes(root.searchQuery.toLowerCase()))
    // A fixed row shown is that row: the filter never hides one.
    // Unfiltered, the row selectedIndex names, as always (it counts "All
    // tracks" as the one row before the stick's playlists). Filtered, the
    // row of the playlist shown, found by name below the fixed rows in
    // the list as it is narrowed, or none (-1) while the filter hides it;
    // clearing the filter changes searchQuery, so selectedIndex is taken
    // again (#106).
    currentIndex: {
        // Taken again whenever the rows change, as the view moves its
        // current index itself when its model is replaced.
        const rows = root.model;
        const fixed = root.fixedRows.findIndex(row => row.kind === root.shownFixedRow);
        if (fixed >= 0) {
            return fixed;
        }
        // A playlist a fixed row stands for is not among the rows, and
        // the places after it are not selectedIndex's: found by name.
        if (root.searchQuery.length === 0 && root.fixedPlaylists.length === 0) {
            return root.selectedIndex > 0 ? root.selectedIndex + root.fixedRowCount - 1 : root.selectedIndex;
        }
        return root.shownPlaylist.length > 0 ? rows.indexOf(root.shownPlaylist, root.fixedRowCount) : -1;
    }

    delegate: PlaylistRowDelegate {
        // index isn't redeclared here -- PlaylistRowDelegate's own root
        // already declares it as required, and the delegate model injects
        // it there directly; redeclaring it in this derived scope shadows
        // that slot instead of filling it, leaving the *base* class's own
        // required `index` permanently unsatisfied and the whole item
        // silently refusing to render (exactly what broke this list AND
        // the This Playlist combo's popup, which reuses this same
        // delegate the same broken way).
        required property string modelData
        name: modelData
        // A fixed row's kind ("all", "recordings"), "" for a playlist: by
        // its place, as a stick playlist may have a fixed row's name.
        readonly property string fixedKind: root.fixedKindAt(index)
        readonly property bool isFixed: fixedKind.length > 0
        readonly property bool isAll: fixedKind === "all"
        readonly property bool isRecordings: fixedKind === "recordings"
        // The stick playlist the row is: its own name, or the playlist a
        // fixed row stands for ("" for none, "All tracks" among them).
        readonly property string playlist: isFixed ? root.fixedPlaylistAt(index) : modelData
        readonly property bool hasPlaylist: playlist.length > 0
        // Staged to be created: not on the stick yet, so no count, no
        // summary, nothing to delete or rename; its menu takes it back.
        readonly property bool isNew: !isFixed && root.canCreate && root.newPlaylists.indexOf(modelData) >= 0
            && root.scanController.playlistNames.indexOf(modelData) < 0
        pendingNew: isNew
        // "All tracks" counts what it lists: the rows a save wrote too,
        // until the stick has been read again.
        // Own Recordings counts its rows too: the playlist's tracks and
        // every recording.
        count: isAll ? root.scanController.totalTrackCount + (root.scanController.importAwaitingReadCount ?? 0)
            : isRecordings ? (root.scanController.recordingCount ?? 0)
                + (hasPlaylist ? (root.scanController.playlistTrackCounts[playlist] ?? 0) : 0)
            : isNew ? 0 : (root.scanController.playlistTrackCounts[modelData] ?? 0)
        // By the row shown, not ListView.isCurrentItem: a scan rebuilds
        // the model, and the view then puts its current index back at the
        // first row while the page keeps showing the playlist it opened on.
        isCurrent: isFixed ? root.shownFixedRow === fixedKind
            : root.shownFixedRow.length === 0 && modelData === root.shownPlaylist
        onDropTargetChanged: root.noteDropTarget(playlist, dropTarget)
        // A fixed row that is a playlist shows its delete as that
        // playlist's row would, and takes nothing while it waits.
        pendingDelete: (!isFixed || (!isAll && hasPlaylist)) && !isNew && root.pendingRevision >= 0
            && root.isPendingDelete(playlist)
        // "All tracks" changes by the tracks an import brings, a fixed
        // row's playlist as that playlist.
        edited: !isFixed ? !isNew && root.pendingRevision >= 0 && root.isEdited(modelData)
            : isAll ? countChange !== 0 : hasPlaylist && root.pendingRevision >= 0 && root.isEdited(playlist)
        pendingRename: !isFixed && !isNew && root.pendingRevision >= 0 && root.isRenamed(modelData)
        countChange: root.pendingRevision >= 0 && (!isRecordings || hasPlaylist)
            ? root.trackCountChange(isAll ? "" : playlist) : 0
        showsThumbnail: true
        iconName: isRecordings ? "media-record" : ""
        checkable: root.checkable
        checkState: root.checkable && root.checkRevision >= 0 ? root.checkStateOf(isAll ? "" : modelData) : Qt.Unchecked
        checkEnabled: root.checkable && root.checksEnabled && root.checkRevision >= 0 && root.checkEnabledFor(isAll ? "" : modelData)
        // What the box shows, in words, and what a click does next: the
        // half-filled box of a playlist with some of its tracks ticked was
        // not clear by itself.
        checkTooltip: {
            const what = isAll ? "tracks" : "tracks of this playlist";
            if (checkState === Qt.Checked) {
                return "All " + what + " are ticked. Click to untick them all.";
            }
            if (checkState === Qt.PartiallyChecked) {
                return "Some " + what + " are ticked. Click to tick them all.";
            }
            return "No " + what + " are ticked. Click to tick them all.";
        }
        onCheckToggled: checked => root.playlistCheckToggled(isAll ? "" : modelData, checked)
        thumbnailSource: isNew || isRecordings ? "" : ((root.scanController.playlistThumbnails || {})[isAll ? "" : modelData] ?? "")
        // The summary's part: the pointer on a row that has one, a
        // press or the wheel there, and what to show for it. Own
        // Recordings sums up its playlist's tracks, once the stick has
        // that playlist; the recordings no library lists are left out.
        readonly property bool hasSummary: !isNew && (!isRecordings
            || (hasPlaylist && root.scanController.playlistNames.indexOf(playlist) >= 0))
        function summaryFor() {
            return root.scanController.playlistSummary(isAll ? "" : playlist);
        }
        onRowHoveredChanged: root.noteSummaryHover(this, rowHovered && hasSummary)
        onSummaryDismissed: root.dismissSummary(this)
        Component.onDestruction: root.noteSummaryHover(this, false)
        // The index the page gets is the playlist's place among the
        // stick's (plus one for "All tracks"), whatever rows the list has
        // before it or has filtered out.
        onPicked: isNew ? root.newPlaylistPicked(modelData)
            : isFixed && !isAll ? root.fixedRowPicked(fixedKind)
            : root.playlistPicked(isAll ? 0 : root.scanController.playlistNames.indexOf(modelData) + 1, modelData)
        acceptsTrackDrop: root.trackDropsEnabled && hasPlaylist && !pendingDelete
            && (playlist !== root.shownPlaylist || root.droppedTrackPlaylists.indexOf(playlist) < 0)
        heldDropTarget: hasPlaylist && root.heldDropTarget.length > 0 && root.heldDropTarget === playlist
        onTrackDropped: root.trackDropped(playlist)
        // Files go into a playlist, or into none ("All tracks"); a fixed
        // row that is no playlist takes none.
        acceptsFileDrop: root.fileDropsEnabled && !pendingDelete && (isAll || hasPlaylist)
        onFilesDropped: urls => root.filesDropped(isAll ? "" : playlist, urls)
        onContextRequested: {
            if (!isFixed) {
                root.playlistContextRequested(modelData, this);
            } else if (hasPlaylist && !isAll) {
                root.fixedRowContextRequested(fixedKind, this);
            }
        }
    }

    footer: Column {
        width: root.width
        visible: root.canCreate
        height: visible ? implicitHeight : 0
        ItemDelegate {
            objectName: "createPlaylistRow"
            width: root.width
            height: 32
            leftPadding: 8
            onClicked: root.createRequested()
            // Files held over the empty part of the list go into a
            // new playlist: an outline over the row says so. Over it, not
            // in place of its background, which shows the row hovered,
            // pressed and in focus.
            Rectangle {
                objectName: "createPlaylistDropOutline"
                anchors.fill: parent
                visible: newPlaylistFileDropArea.containsDrag
                color: "transparent"
                border.color: Theme.accent
                border.width: 2
                radius: Theme.cornerRadius
            }
            contentItem: Row {
                spacing: Theme.tightSpacing
                SeabassIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "list-add"
                    size: Theme.iconSizeSmall * 0.75
                    color: Theme.textMuted
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Create new playlist")
                    color: Theme.textMuted
                }
            }
        }
    }
}
