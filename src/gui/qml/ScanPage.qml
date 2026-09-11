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

    // Browse is read-only until a cue is added: the session is opened
    // here (no lock), the first staged cue takes the lock and enables
    // the floating Save.
    EditSessionHost {
        id: editHost
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
    // lastBrowsePlaylistName) can't drift out of sync with each other.
    function selectPlaylist(index, name) {
        root.selectedPlaylistIndex = index;
        scanController.filterByPlaylist(index === 0 ? "" : name);
        root.appSettingsController.lastBrowsePlaylistName = index === 0 ? "" : name;
    }

    // Called once scanController.playlistNames is populated (see
    // onPlaylistNamesChanged above). Falls back to "All tracks" if the
    // last-selected name doesn't match any playlist on this stick --
    // e.g. it was deleted, or this is a different stick than last time.
    function restoreSelectedPlaylist() {
        var wanted = root.appSettingsController.lastBrowsePlaylistName;
        var idx = wanted.length > 0 ? scanController.playlistNames.indexOf(wanted) : -1;
        root.selectPlaylist(idx >= 0 ? idx + 1 : 0, wanted);
    }

    // ---- Matching (Experimental, see docs/experimental-
    // features.md) -- the playlist selection above is shared between the
    // always-on left Pane (when this is off) and the off-canvas Drawer +
    // the panel's own "This Playlist" chip (when it's on); the anchor
    // properties below track whichever Browse row's edit button was last
    // clicked, read by MatchingPage to find compatible tracks. ----
    readonly property bool matchingEnabled: root.appSettingsController.experimentalFeaturesEnabled
    // Closed by default even when the feature is on -- only the edit
    // button (or the panel's own close button) toggles it, so turning on
    // Experimental features doesn't itself change what Browse looks like
    // until a track is actually being edited.
    property bool matchingPanelOpen: false
    // The track details column: opened by clicking a row, closed by its
    // own button. Sits between the track list and the Matching panel.
    property bool trackPanelOpen: false
    // Starts open (matching the classic left Pane it replaces, which is
    // always visible) -- the header pill collapses/expands it, unlike
    // matchingPanelOpen above which starts collapsed.
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

    property string anchorSourceId: ""
    property string anchorTitle: ""
    property string anchorArtist: ""
    property string anchorKey: ""
    property double anchorBpm: 0
    property string anchorArtworkPath: ""
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
        root.anchorPlaylistNames = delegate.playlistNames;
        root.matchingPanelOpen = true;
    }

    function rescan() {
        selectedPlaylistIndex = 0;
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
                    visible: root.matchingEnabled
                    text: (root.playlistSidebarOpen ? "◀ " : "▶ ") + root.currentPlaylistLabel
                    ToolTip.visible: hovered
                    ToolTip.text: root.playlistSidebarOpen ? "Collapse the playlist sidebar" : "Show the playlist sidebar"
                    onClicked: root.playlistSidebarOpen = !root.playlistSidebarOpen
                }
                TextField {
                    id: searchField
                    Layout.preferredWidth: 260
                    // Also filters the playlist list below (see its own
                    // model binding) -- one search field instead of two,
                    // since "search" and "filter playlists" were doing
                    // the same job on two different, easy-to-miss fields.
                    placeholderText: "Search title, artist, or playlist..."
                    rightPadding: searchClearButton.visible ? searchClearButton.width + 4 : 0
                    onTextChanged: scanController.search(text)

                    ToolButton {
                        id: searchClearButton
                        visible: searchField.text.length > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 2
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: Theme.iconSizeSmall + 8
                        implicitHeight: Theme.iconSizeSmall + 8
                        flat: true
                        text: "✕"
                        ToolTip.visible: hovered
                        ToolTip.text: "Clear search"
                        onClicked: searchField.text = ""
                    }
                }
                Item { Layout.fillWidth: true }
                Label { text: "Sort by" }
                ComboBox {
                    id: sortCombo
                    Layout.preferredWidth: 140
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { text: "Playlist Order", value: "playlist" },
                        { text: "Title", value: "title" },
                        { text: "Artist", value: "artist" },
                        { text: "Key", value: "key" },
                        { text: "BPM", value: "bpm" },
                        { text: "Duration", value: "duration" },
                        { text: "Cues", value: "cues" },
                        { text: "Plays", value: "plays" },
                    ]
                    onActivated: scanController.setSort(currentValue, sortDirectionButton.checked)
                }
                ToolButton {
                    id: sortDirectionButton
                    checkable: true
                    checked: true
                    text: checked ? "▲ Ascending" : "▼ Descending"
                    onCheckedChanged: scanController.setSort(sortCombo.currentValue, checked)
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
        spacing: 0

        // Left pane: playlists -- only when Matching (Experimental)
        // is off. When it's on, this same list lives in a collapsible
        // SplitView pane instead (see the SplitView below), freeing this
        // column for the new panel.
        Pane {
            visible: !root.matchingEnabled
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            padding: 0

            PlaylistListView {
                anchors.fill: parent
                scanController: scanController
                searchQuery: searchField.text
                selectedIndex: root.selectedPlaylistIndex
                onPlaylistPicked: (index, name) => root.selectPlaylist(index, name)
            }
        }

        Rectangle {
            visible: !root.matchingEnabled
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            color: Theme.borderSubtle
        }

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

        // Playlists sidebar (Experimental only) -- a real, resizable
        // SplitView pane instead of the off-canvas Drawer this replaced:
        // collapsible via the header's own pill button rather than an
        // overlay you have to close before doing anything else. Same
        // PlaylistListView the classic left Pane above uses.
        Pane {
            visible: root.matchingEnabled && root.playlistSidebarOpen
            SplitView.preferredWidth: 240
            SplitView.minimumWidth: 160
            padding: 0

            PlaylistListView {
                anchors.fill: parent
                scanController: scanController
                searchQuery: searchField.text
                selectedIndex: root.selectedPlaylistIndex
                onPlaylistPicked: (index, name) => root.selectPlaylist(index, name)
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
                TableHeaderLabel { label: "Title"; Layout.fillWidth: true; visible: root.browseTier >= 1 }
                TableHeaderLabel { label: "Key"; Layout.preferredWidth: 50; visible: root.browseTier >= 2 }
                TableHeaderLabel { label: "BPM"; Layout.preferredWidth: 50; visible: root.browseTier >= 2 }
                TableHeaderLabel { label: "Time"; Layout.preferredWidth: 60; visible: root.browseTier >= 2 }
                TableHeaderLabel { label: "Cues"; Layout.preferredWidth: 50; visible: root.browseTier >= 2 }
                TableHeaderLabel { label: "Plays"; Layout.preferredWidth: 50; visible: root.browseTier >= 2 }
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
                    Layout.preferredWidth: root.matchingEnabled
                        ? Theme.iconSizeSmall * 2 + 8 : Theme.iconSizeSmall
                }
            }

            ListView {
                // Room to scroll the last row clear of the Save overlay (bottom right).
                bottomMargin: 80
                id: trackListView
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

                    readonly property bool isPlaying: playbackController.hasTrack
                        && playbackController.currentFormat === root.format
                        && playbackController.currentSourceId === trackDelegate.sourceId

                    // Alternating row shading, makes it much easier to
                    // track a row across the wide, densely-columned list.
                    // Solid, muted colors rather than a translucent overlay,
                    // so the result doesn't depend on (and can't pick up an
                    // unexpected tint from) whatever's rendered underneath.
                    color: rowMouseArea.pressed ? Theme.rowPressed
                        : rowMouseArea.containsMouse ? Theme.rowHover
                        : (trackDelegate.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)

                    // Now-playing highlight, an accent-colored stripe,
                    // same idiom as most media players use for "this one."
                    // Rounded to match every other accent-bordered highlight
                    // in the app (BackupsPage/LocalCuePage's active-field
                    // outline, etc.), all radius: 4. This one was square.
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
                        onClicked: {
                            trackDetailPanel.showFor(trackDelegate);
                            root.trackPanelOpen = true;
                        }
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
                            Image {
                                anchors.fill: parent
                                visible: artworkPath.length > 0
                                source: artworkPath
                                fillMode: Image.PreserveAspectCrop
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
                                Label {
                                    anchors.centerIn: parent
                                    text: "▶"
                                    color: "white"
                                    font.pointSize: Theme.fontLarge
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
                                    playbackController.load(root.format, root.currentPath(), trackDelegate.sourceId,
                                        trackDelegate.filePath, trackDelegate.title, trackDelegate.artist,
                                        trackDelegate.artworkPath, trackDelegate.cues);
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
                        }
                        Label { visible: root.browseTier >= 2; text: root.formatBpm(bpm); Layout.preferredWidth: 50 }
                        Label {
                            visible: root.browseTier >= 2
                            text: root.formatDuration(durationSeconds)
                            Layout.preferredWidth: 60
                        }
                        Label { visible: root.browseTier >= 2; text: cueCount; Layout.preferredWidth: 50 }
                        Label {
                            visible: root.browseTier >= 2
                            text: playCount >= 0 ? playCount : "--"
                            Layout.preferredWidth: 50
                        }
                        ToolButton {
                            // Monochrome, like every other glyph the app draws:
                            // the family forces the flat outline instead of the
                            // system's color-emoji fallback. U+26D3 rather than
                            // the 🔗 this used to be because Noto Sans Symbols2
                            // has no glyph for that one, so it would fall back to
                            // color anyway -- same chain-link reading either way.
                            text: "⛓"
                            font.family: "Noto Sans Symbols2"
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
                        ToolButton {
                            id: editButton
                            visible: root.matchingEnabled
                            text: "🔍"
                            font.family: "Noto Sans Symbols2"
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
                x: (root.width - width) / 2
                y: (root.height - height) / 2
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
                x: (root.width - width) / 2
                y: (root.height - height) / 2
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
                                CheckBox {
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
            // Jumping to another track by the same artist. The panel names
            // the track; finding it is this page's job, because only the
            // page knows the current sort and filter.
            //
            // The wanted track may not be in view at all -- the list can
            // be narrowed to one playlist or a search while the artist
            // list deliberately searches the whole library -- so a miss
            // says so rather than doing nothing, which would read as a
            // dead click.
            onJumpToTrackRequested: (sourceId) => {
                for (var i = 0; i < trackListView.count; ++i) {
                    var candidate = scanController.tracks.trackAt(i);
                    if (candidate && candidate.sourceId === sourceId) {
                        trackListView.currentIndex = i;
                        trackListView.positionViewAtIndex(i, ListView.Contain);
                        var item = trackListView.itemAtIndex(i);
                        if (item) {
                            trackDetailPanel.showFor(item);
                        }
                        return;
                    }
                }
                trackDetailPanel.reportJumpMiss();
            }
        }

        MatchingPage {
            id: matchingPage
            visible: root.matchingEnabled && root.matchingPanelOpen
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
