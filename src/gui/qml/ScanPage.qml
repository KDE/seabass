// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
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
            }
        }
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
        MenuItem {
            objectName: "deletePlaylistItem"
            text: playlistMenu.staged ? qsTr("Keep this playlist") : qsTr("Delete playlist...")
            onTriggered: {
                if (playlistMenu.staged) {
                    root.playlistEditController.keepPlaylist(playlistMenu.playlist);
                } else {
                    deletePlaylistDialog.playlist = playlistMenu.playlist;
                    deletePlaylistDialog.open();
                }
            }
        }
    }

    Dialog {
        id: deletePlaylistDialog
        objectName: "deletePlaylistDialog"
        property string playlist: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete playlist")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label {
            width: Math.min(implicitWidth, root.width * 0.6)
            wrapMode: Text.Wrap
            text: qsTr("Delete \"%1\" from every library on this stick? Its tracks stay on the stick. Nothing changes until you Save.")
                .arg(deletePlaylistDialog.playlist)
        }
        onAccepted: root.playlistEditController.deletePlaylist(root.rekordboxPath, root.enginePath, deletePlaylistDialog.playlist)
    }

    Menu {
        id: trackMenu
        objectName: "trackMenu"
        property string filePath: ""
        property string trackTitle: ""
        readonly property bool staged: root.playlistEditController.pendingRevision >= 0 && filePath.length > 0
            && root.playlistEditController.isRemovalStaged(root.shownPlaylist, filePath)
        MenuItem {
            objectName: "removeFromPlaylistItem"
            text: trackMenu.staged ? qsTr("Keep in \"%1\"").arg(root.shownPlaylist)
                                   : qsTr("Remove from \"%1\"").arg(root.shownPlaylist)
            onTriggered: {
                if (trackMenu.staged) {
                    root.playlistEditController.keepInPlaylist(root.shownPlaylist, trackMenu.filePath);
                } else {
                    root.playlistEditController.removeFromPlaylist(root.rekordboxPath, root.enginePath, root.shownPlaylist,
                                                                   trackMenu.filePath, trackMenu.trackTitle);
                }
            }
        }
    }

    function openPlaylistMenu(name, row) {
        if (!root.playlistEditing) {
            return;
        }
        playlistMenu.playlist = name;
        playlistMenu.popup(row);
    }

    function openTrackMenu(filePath, title, row) {
        if (!root.playlistEditing || root.shownPlaylist.length === 0 || filePath.length === 0) {
            return;
        }
        trackMenu.filePath = filePath;
        trackMenu.trackTitle = title;
        trackMenu.popup(row);
    }

    // Browse is read-only until a cue is added: the session is opened
    // here (no lock), the first staged cue takes the lock and enables
    // the floating Save.
    EditSessionHost {
        id: editHost
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "addcue"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
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
    function restoreSelectedPlaylist() {
        var wanted = root.appSettingsController.lastPlaylistName;
        var idx = wanted.length > 0 ? scanController.playlistNames.indexOf(wanted) : -1;
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
    readonly property string currentPlaylistLabel: root.selectedPlaylistIndex === 0
        ? "All tracks" : (scanController.playlistNames[root.selectedPlaylistIndex - 1] ?? "All tracks")

    // How much of Browse's own row content fits once the panel above has
    // taken its share of the window: 2 (comfortable) shows every column,
    // 1 (tight) drops Key/BPM/Time/Cues/Plays down to just artwork+title/
    // artist, 0 (very tight) drops the title/artist column too, leaving
    // only artwork -- with the full details one hover away, see that
    // Rectangle's own tooltip below -- rather than letting every column
    // get squeezed illegibly thin at once.
    readonly property int browseTier: trackListView.width >= 620 ? 2 : (trackListView.width >= 340 ? 1 : 0)

    // The list's sort, held here so the column headers and the Sort by
    // combo show one state. A header click sorts by its column, ascending
    // for a new column and flipped for the sorted one; the combo picks
    // the field and keeps the direction. Playlist Order and Artist have
    // no column, so no header shows their direction: they always sort
    // ascending, and the direction the headers last showed is kept for
    // the next column sort rather than applied where nobody can see it.
    readonly property var sortOptions: [
        { text: "Playlist Order", value: "playlist" },
        { text: "Title", value: "title" },
        { text: "Artist", value: "artist" },
        { text: "Key", value: "key" },
        { text: "BPM", value: "bpm" },
        { text: "Duration", value: "duration" },
        { text: "Cues", value: "cues" },
        { text: "Plays", value: "plays" },
    ]
    readonly property var columnSortKeys: ["title", "key", "bpm", "duration", "cues", "plays"]
    property string sortField: "playlist"
    property bool sortAscending: true
    readonly property bool sortedByColumn: root.columnSortKeys.indexOf(root.sortField) >= 0

    function applySort() {
        scanController.setSort(root.sortField, root.sortedByColumn ? root.sortAscending : true);
    }
    function sortByColumn(key) {
        if (root.sortField === key) {
            root.sortAscending = !root.sortAscending;
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
                // The gap Theme.headerBottomPadding leaves under a one-row header,
                // kept here too, before this header's second row.
                Layout.bottomMargin: Theme.headerBottomPadding - headerLayout.spacing
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
                // The only way to Playlist Order and Artist, which have no
                // column; the direction is the column headers' (see
                // root.sortField).
                ComboBox {
                    id: sortCombo
                    objectName: "sortCombo"
                    Layout.preferredWidth: 140
                    // As tall as the library picker above it.
                    implicitHeight: Theme.compactControlHeight
                    textRole: "text"
                    valueRole: "value"
                    model: root.sortOptions
                    currentIndex: root.sortOptions.findIndex(option => option.value === root.sortField)
                    onActivated: (index) => root.sortByField(root.sortOptions[index].value)
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
                anchors.fill: parent
                scanController: scanController
                searchQuery: searchField.text
                selectedIndex: root.selectedPlaylistIndex
                onPlaylistPicked: (index, name) => root.selectPlaylist(index, name)
                isPendingDelete: name => root.playlistEditController.isPlaylistStaged(name)
                pendingRevision: root.playlistEditController.pendingRevision
                onPlaylistContextRequested: (name, row) => root.openPlaylistMenu(name, row)
            }
        }

        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0

            RowLayout {
                // Mirrors the track delegate's own RowLayout exactly (same
                // left/right inset, spacing and column widths), otherwise
                // these headers silently drift out of alignment with the
                // columns they're supposed to label.
                Layout.fillWidth: true
                Layout.leftMargin: 8
                Layout.rightMargin: 8
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: 8
                Label { text: ""; Layout.preferredWidth: Theme.iconSizeNormal }
                // Each header with a sort key sorts by it (see
                // root.sortByColumn); the artwork column and the trailing
                // action columns have none.
                SortableTableHeader {
                    objectName: "sortHeader_title"
                    label: "Title"
                    sortKey: "title"
                    sortField: root.sortField
                    sortAscending: root.sortAscending
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
                    ]
                    delegate: SortableTableHeader {
                        required property var modelData
                        objectName: "sortHeader_" + modelData.key
                        label: modelData.label
                        sortKey: modelData.key
                        sortField: root.sortField
                        sortAscending: root.sortAscending
                        onSortRequested: (key) => root.sortByColumn(key)
                        Layout.preferredWidth: modelData.width
                        visible: root.browseTier >= 2
                    }
                }
                // Theme.iconSizeSmall (merge button), the one trailing
                // ToolButton in the delegate below -- two when Matching
                // (Experimental) is on, since the find-matching button
                // joins it. Getting this narrower than the delegate's
                // real trailing content silently pushes every column
                // before it out of alignment (the fill spacer above ends
                // up absorbing a different amount of leftover space in
                // the header than in each row), exactly what happened
                // here before this comment existed.
                Label {
                    text: ""
                    Layout.preferredWidth: Theme.iconSizeSmall * 2 + 8
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
                model: scanController.tracks

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
                        hovered: rowMouseArea.containsMouse
                        pressed: rowMouseArea.pressed
                    }

                    // Now-playing highlight, an accent-colored stripe,
                    // same idiom as most media players use for "this one."
                    // Rounded to match every other accent-bordered highlight
                    // in the app (BackupsPage's active-field outline,
                    // etc.), all radius: 4. This one was square.
                    border.color: trackDelegate.isPlaying ? Theme.accent : "transparent"
                    border.width: trackDelegate.isPlaying ? 2 : 0
                    radius: trackDelegate.isPlaying ? 4 : 0

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
                    WaveformView {
                        visible: trackDelegate.isPlaying
                        anchors.fill: parent
                        anchors.margins: 2
                        opacity: 0.35
                        waveformData: playbackController.waveform
                        format: playbackController.currentFormat
                        cueData: playbackController.cues
                        trackDurationMs: playbackController.duration
                        progress: playbackController.duration > 0
                            ? playbackController.position / playbackController.duration : 0
                    }

                    // Opens the track details column -- playing now
                    // happens only via the artwork's own hover-play
                    // overlay above (which, being declared later/topmost
                    // for just that region, claims its own clicks
                    // first). Streaming tracks are fine here: the column
                    // still shows their cues and playlists, only Play
                    // itself (on the artwork) is unavailable for those.
                    MouseArea {
                        id: rowMouseArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton) {
                                root.openTrackMenu(filePath, title, trackDelegate);
                                return;
                            }
                            trackDetailPanel.showFor(trackDelegate);
                            root.trackPanelOpen = true;
                        }
                        onPressAndHold: root.openTrackMenu(filePath, title, trackDelegate)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        spacing: 8

                        Rectangle {
                            id: artworkRect
                            Layout.preferredWidth: Theme.iconSizeNormal
                            Layout.preferredHeight: Theme.iconSizeNormal
                            color: Theme.surface
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
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: trackDelegate.streamingSource.length === 0
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
                            visible: root.browseTier >= 1
                            Layout.fillWidth: true
                            spacing: 1
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Label {
                                    text: title
                                    font.bold: true
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
                            visible: root.browseTier >= 2
                            keyName: key
                            notation: root.appSettingsController.keyNotation
                            Layout.preferredWidth: root.keyColumnWidth
                        }
                        Label {
                            visible: root.browseTier >= 2
                            text: root.formatBpm(bpm)
                            Layout.preferredWidth: root.bpmColumnWidth
                        }
                        Label {
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
                            visible: root.browseTier >= 2
                            text: playCount >= 0 ? playCount : "--"
                            Layout.preferredWidth: root.playsColumnWidth
                        }
                        IconToolButton {
                            text: "Merge"
                            iconName: "link"
                            Layout.preferredWidth: Theme.iconSizeSmall
                            enabled: root.format !== "onelibrary" && trackDelegate.streamingSource.length === 0
                            ToolTip.visible: hovered
                            ToolTip.text: trackDelegate.streamingSource.length > 0
                                ? "Streaming track (" + trackDelegate.streamingSource + ") - no local file, can't be merged."
                                : (root.format === "onelibrary"
                                    ? "Merging isn't supported on OneLibrary yet - switch to DeviceLibrary or Engine OS"
                                    : "Merge with another track...")
                            onClicked: mergePickerPopup.showFor(trackDelegate)
                        }
                        IconToolButton {
                            id: editButton
                            text: "Find matching tracks"
                            iconName: "edit-find"
                            Layout.preferredWidth: Theme.iconSizeSmall
                            ToolTip.visible: hovered
                            ToolTip.text: "Find matching tracks"
                            onClicked: root.toggleAnchor(trackDelegate)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: trackListView.count === 0 && !scanController.busy
                    text: "No tracks found."
                    color: Theme.textMuted
                }
            }

            // Step 1 of "Merge with...": search for the second track.
            // Deliberately searches ScanController's full unfiltered
            // m_allTracks (via findMergeCandidates()), never the page's
            // own filtered/sorted `tracks` model or scanController.search()
            // Typing here must not disturb whatever's shown on the page
            // underneath once this closes.
            Popup {
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
            Popup {
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
                        BusyIndicator { running: mergeController.busy; implicitWidth: 24; implicitHeight: 24 }
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

            Label {
                Layout.margins: 8
                text: trackListView.count + " tracks"
                color: Theme.textMuted
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
