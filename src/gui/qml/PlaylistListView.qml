// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The playlist list -- filter-by-name (see the shared search field's own
// comment in ScanPage.qml for why this reuses that field instead of a
// second one), delegated to PlaylistRowDelegate.qml (also used by the
// "This Playlist" ComboBox's own popup, so both pickers render
// identically). Used in two places in ScanPage.qml: always in the left
// Pane when the Matching panel (Experimental) is off, or inside
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
    // asks). Browse answers it only with Experimental on.
    signal playlistContextRequested(string name, Item row)
    // Whether a playlist is staged for deletion; re-read when
    // pendingRevision moves.
    property var isPendingDelete: name => false
    property int pendingRevision: 0
    // Creating a playlist (Browse, Experimental): the playlists staged to
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
    // Dropping a dragged track on a playlist (Browse, Experimental): the
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
    signal createRequested()
    // A playlist staged to be created was clicked.
    signal newPlaylistPicked(string name)
    // Hovering a playlist shows its summary (PlaylistSummaryPopup.qml),
    // "All tracks" the whole library's. Switched off while the pointer
    // is busy with something else, such as dragging tracks onto a row.
    property bool summariesEnabled: true
    property int summaryDelay: 1500
    property string keyNotation: "camelot"

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
    currentIndex: root.selectedIndex

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
        summaryEnabled: root.summariesEnabled && !isNew
        summaryDelay: root.summaryDelay
        summaryFor: () => root.scanController.playlistSummary(index === 0 ? "" : modelData)
        keyNotation: root.keyNotation
        // The index the page gets is the playlist's place among the
        // stick's (plus one for "All tracks"), whatever rows the list has
        // before it or has filtered out.
        onPicked: isNew ? root.newPlaylistPicked(modelData)
            : root.playlistPicked(index === 0 ? 0 : root.scanController.playlistNames.indexOf(modelData) + 1, modelData)
        acceptsTrackDrop: root.trackDropsEnabled && index > 0 && !pendingDelete
            && (modelData !== root.shownPlaylist || root.droppedTrackPlaylists.indexOf(modelData) < 0)
        heldDropTarget: root.heldDropTarget.length > 0 && root.heldDropTarget === modelData
        onTrackDropped: root.trackDropped(modelData)
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
