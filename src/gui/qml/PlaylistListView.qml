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
    // A playlist row's context menu was asked for ("All tracks" never
    // asks). Browse answers it with the playlist's menu.
    signal playlistContextRequested(string name, Item row)
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
    property var allNames: ["All tracks"].concat(root.canCreate ? root.newPlaylists : [])
        .concat(scanController.playlistNames)
    model: root.searchQuery.length === 0
        ? allNames
        : allNames.filter((n, i) => i === 0 || n.toLowerCase().includes(root.searchQuery.toLowerCase()))
    // Unfiltered, the row selectedIndex names, as always. Filtered, the
    // row of the playlist shown, found by name in the list as it is
    // narrowed (selectedIndex counts the unfiltered list), or none (-1)
    // while the filter hides it; clearing the filter changes searchQuery,
    // so selectedIndex is taken again (#106).
    currentIndex: {
        if (root.searchQuery.length === 0) {
            return root.selectedIndex;
        }
        const wanted = root.shownPlaylist.length > 0 ? root.shownPlaylist
            : root.selectedIndex === 0 ? "All tracks" : "";
        return wanted.length > 0 ? root.model.indexOf(wanted) : -1;
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
        // Staged to be created: not on the stick yet, so no count, no
        // summary, nothing to delete or rename; its menu takes it back.
        readonly property bool isNew: root.canCreate && root.newPlaylists.indexOf(modelData) >= 0
        pendingNew: isNew
        count: index === 0 ? root.scanController.totalTrackCount
            : isNew ? 0 : (root.scanController.playlistTrackCounts[modelData] ?? 0)
        // By the playlist shown, not ListView.isCurrentItem: a scan
        // rebuilds the model, and the view then puts its current index
        // back at the first row while the page keeps showing the playlist
        // it opened on.
        isCurrent: index === 0 ? root.shownPlaylist.length === 0 : modelData === root.shownPlaylist
        onDropTargetChanged: root.noteDropTarget(modelData, dropTarget)
        pendingDelete: index > 0 && !isNew && root.pendingRevision >= 0 && root.isPendingDelete(modelData)
        edited: index > 0 && !isNew && root.pendingRevision >= 0 && root.isEdited(modelData)
        pendingRename: index > 0 && !isNew && root.pendingRevision >= 0 && root.isRenamed(modelData)
        countChange: index > 0 && root.pendingRevision >= 0 ? root.trackCountChange(modelData) : 0
        showsThumbnail: true
        checkable: root.checkable
        checkState: root.checkable && root.checkRevision >= 0 ? root.checkStateOf(index === 0 ? "" : modelData) : Qt.Unchecked
        checkEnabled: root.checkable && root.checksEnabled && root.checkRevision >= 0 && root.checkEnabledFor(index === 0 ? "" : modelData)
        // What the box shows, in words, and what a click does next: the
        // half-filled box of a playlist with some of its tracks ticked was
        // not clear by itself.
        checkTooltip: {
            const what = index === 0 ? "tracks" : "tracks of this playlist";
            if (checkState === Qt.Checked) {
                return "All " + what + " are ticked. Click to untick them all.";
            }
            if (checkState === Qt.PartiallyChecked) {
                return "Some " + what + " are ticked. Click to tick them all.";
            }
            return "No " + what + " are ticked. Click to tick them all.";
        }
        onCheckToggled: checked => root.playlistCheckToggled(index === 0 ? "" : modelData, checked)
        thumbnailSource: isNew ? "" : ((root.scanController.playlistThumbnails || {})[index === 0 ? "" : modelData] ?? "")
        // The summary's part: the pointer on a row that has one, a
        // press or the wheel there, and what to show for it.
        function summaryFor() {
            return root.scanController.playlistSummary(index === 0 ? "" : modelData);
        }
        onRowHoveredChanged: root.noteSummaryHover(this, rowHovered && !isNew)
        onSummaryDismissed: root.dismissSummary(this)
        Component.onDestruction: root.noteSummaryHover(this, false)
        // The index the page gets is the playlist's place among the
        // stick's (plus one for "All tracks"), whatever rows the list has
        // before it or has filtered out.
        onPicked: isNew ? root.newPlaylistPicked(modelData)
            : root.playlistPicked(index === 0 ? 0 : root.scanController.playlistNames.indexOf(modelData) + 1, modelData)
        acceptsTrackDrop: root.trackDropsEnabled && index > 0 && !pendingDelete
            && (modelData !== root.shownPlaylist || root.droppedTrackPlaylists.indexOf(modelData) < 0)
        heldDropTarget: root.heldDropTarget.length > 0 && root.heldDropTarget === modelData
        onTrackDropped: root.trackDropped(modelData)
        acceptsFileDrop: root.fileDropsEnabled && !pendingDelete
        onFilesDropped: urls => root.filesDropped(index === 0 ? "" : modelData, urls)
        onContextRequested: {
            if (index > 0) {
                root.playlistContextRequested(modelData, this);
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
            // new playlist: this row says so.
            background: Rectangle {
                color: "transparent"
                border.color: Theme.accent
                border.width: newPlaylistFileDropArea.containsDrag ? 2 : 0
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
