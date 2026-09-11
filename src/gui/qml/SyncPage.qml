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

    // "" scopes analyze() to the whole library (every catalog present),
    // same empty-means-all convention every other picker in this app uses.
    property string selectedPlaylistName: ""
    property string searchQuery: ""

    SyncController {
        id: syncController
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "sync"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // "All tracks" first (no meaningful single count across up to three
    // independent catalogs, so left blank rather than showing a
    // misleading sum), then the union of playlist names across whichever
    // catalogs are present -- built from syncController's own unfiltered
    // scan, so this list doesn't shrink once a playlist is selected.
    readonly property var playlistPickerModel: [{name: "All tracks", count: ""}].concat(
        syncController.playlistNames.map((n) => ({name: n, count: syncController.playlistTrackCounts[n] ?? 0})))

    function formatLabel(format) { return FormatLabels.label(format); }
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
    }

    // A conflict's own title/artist alone can't tell two conflicts on
    // duplicate copies of the same track apart (same title, same
    // artist, different file) -- appending the actual filename makes
    // "this title shown 5 times" legible as 5 distinct files instead
    // of looking like a bug.
    function conflictHeading(conflict) {
        var filename = conflict.targetPath.split("/").pop();
        if (conflict.targetTitle.length === 0) {
            return conflict.targetPath;
        }
        return conflict.targetTitle + " - " + conflict.targetArtist + "  [" + filename + "]";
    }

    Component.onCompleted: syncController.analyze(root.rekordboxPath, root.enginePath)

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

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Sync Cue Points"
                backEnabled: !syncController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "Playlist:"
                color: Theme.textMuted
            }
            PlaylistPickerCombo {
                Layout.minimumWidth: 140
                enabled: !syncController.busy
                model: root.playlistPickerModel
                currentIndex: {
                    if (root.selectedPlaylistName.length === 0) {
                        return 0;
                    }
                    for (var i = 1; i < root.playlistPickerModel.length; i++) {
                        if (root.playlistPickerModel[i].name === root.selectedPlaylistName) {
                            return i;
                        }
                    }
                    return 0;
                }
                ToolTip.visible: hovered
                ToolTip.text: "Scope Sync Cue Points to one playlist instead of the whole library"
                onPlaylistPicked: (index, modelData) => {
                    root.selectedPlaylistName = index === 0 ? "" : modelData.name;
                    syncController.analyze(root.rekordboxPath, root.enginePath, root.selectedPlaylistName, root.searchQuery);
                }
            }
            TextField {
                id: searchField
                Layout.preferredWidth: 160
                enabled: !syncController.busy
                placeholderText: "Search title/artist"
                text: root.searchQuery
                // analyze() does a real background scan+match pass, not a
                // free in-memory filter over already-loaded tracks (unlike
                // ScanController's own search box) -- debounced rather
                // than firing on every keystroke, so typing doesn't spam
                // redundant background tasks.
                onTextEdited: {
                    root.searchQuery = text;
                    searchDebounce.restart();
                }
                ToolTip.visible: hovered
                ToolTip.text: "Filter by title or artist, on top of the playlist scope"
            }
            Timer {
                id: searchDebounce
                interval: 350
                onTriggered: syncController.analyze(root.rekordboxPath, root.enginePath, root.selectedPlaylistName, root.searchQuery)
            }
            Button {
                text: "Re-Analyze"
                enabled: !syncController.busy
                ToolTip.visible: hovered
                ToolTip.text: "Re-scan both libraries and recompute what needs syncing"
                onClicked: syncController.analyze(root.rekordboxPath, root.enginePath, root.selectedPlaylistName, root.searchQuery)
            }
            Label {
                visible: syncController.stagedCount > 0
                text: syncController.stagedCount + " staged, not saved yet"
                color: Theme.warnText
            }
            Button {
                text: "Stage All " + plansListView.count
                enabled: !syncController.busy && !syncController.writing
                    && plansListView.count > syncController.stagedCount
                ToolTip.visible: hovered
                ToolTip.text: "Review what will be copied, then stage every listed track; Save writes them"
                onClicked: confirmDialog.open()
            }
            Button {
                text: "Undo Last Save"
                visible: syncController.canUndo
                enabled: !syncController.busy && !syncController.writing
                ToolTip.visible: hovered
                ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                onClicked: syncController.undoLastOperation()
            }
        }
    }

    MessageDialog {
        id: confirmDialog
        severity: SeabassDialog.Question
        title: "Stage all changes?"
        acceptText: "Stage All"
        onAccepted: syncController.apply()

        // A per-direction list rather than one sentence, so it stays in the
        // content slot instead of being flattened into `headline`.
        Repeater {
            model: syncController.directionCounts
            delegate: Label {
                required property var modelData
                Layout.fillWidth: true
                text: "Copy cues to " + root.formatLabel(modelData.targetFormat) + " from "
                    + root.formatLabel(modelData.sourceFormat) + " for " + modelData.count + " track(s)."
                wrapMode: Text.WordWrap
            }
        }
        Label {
            Layout.fillWidth: true
            visible: {
                for (var i = 0; i < syncController.directionCounts.length; i++) {
                    if (syncController.directionCounts[i].targetFormat === "rekordbox") return true;
                }
                return false;
            }
            text: "DeviceLibrary writing is the least-proven part of Seabass. Verify the result\non real hardware before trusting it for a gig."
            color: Theme.conflictText
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: "Nothing is written yet: this stages the changes, and Save writes them. Every catalog "
                + "involved is backed up first; afterwards \"Undo Last Save\" reverts every file it touched."
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: syncController.errorMessage.length > 0
            text: syncController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: syncController.statusMessage.length > 0
            text: syncController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: !syncController.busy
            // Counts already reflect the selected playlist/search
            // (SyncController scopes matching to both before counting) --
            // the trailing note makes that explicit rather than leaving a
            // small number unexplained.
            text: "DeviceLibrary tracks: " + syncController.rekordboxTrackCount
                + "   Engine tracks: " + syncController.engineTrackCount
                + (syncController.oneLibraryTrackCount > 0 ? "   OneLibrary tracks: " + syncController.oneLibraryTrackCount : "")
                + "   needing sync: " + plansListView.count
                + (root.selectedPlaylistName.length > 0 ? "   (playlist: " + root.selectedPlaylistName + ")" : "")
                + (root.searchQuery.length > 0 ? "   (search: \"" + root.searchQuery + "\")" : "")
            color: Theme.textMuted
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: plansListView
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: syncController.plans
            spacing: 4
            ScrollBar.vertical: BigScrollBar {}

            // BigScrollBar overlays the list rather than reserving its
            // own layout space (a real, deliberate overlay scrollbar,
            // same as the platform default it replaces) -- delegate/
            // header content sized to the ListView's full width used to
            // extend all the way under it, right up to (and visually
            // clipped by) the scrollbar thumb. Every "width: ..." below
            // that used to read plansListView.width/ListView.view.width
            // directly now reads this instead, leaving a small gutter
            // for the scrollbar to sit in without overlapping real
            // content.
            readonly property real delegateWidth: width - 14

            // One list, one scrollbar: unresolved conflicts need
            // resolving before their track can sync at all, so they
            // belong above the actionable plans, not in a second,
            // separately-scrolled list next to them. A ListView header
            // (not a second ListView) keeps this one virtualized list
            // for the -- potentially thousands-long -- plans below,
            // while the conflicts themselves (always few) render plainly
            // via Repeater.
            header: Column {
                id: conflictsHeader
                width: plansListView.delegateWidth
                spacing: 8
                bottomPadding: visible ? 12 : 0
                visible: syncController.unresolvedConflicts.length > 0
                height: visible ? implicitHeight : 0

                // Each conflict card's waveforms load asynchronously
                // (see the inner Repeater's own comment below), so this
                // header's real height only reaches its final value
                // gradually, over several frames, not in one shot.
                // ListView's own default behavior when content *above*
                // the current viewport changes size is to adjust
                // contentY to keep whatever's currently visible looking
                // stationary -- exactly wrong here: it means every
                // incremental growth pushes the header (and the
                // conflicts it's meant to surface) further up and out of
                // view, since nothing has scrolled away from the top on
                // purpose. positionViewAtBeginning() (not a direct
                // contentY assignment -- that raced against ListView's
                // own internal repositioning for this exact change and
                // lost) re-pins to the real top; deferred via
                // Qt.callLater() so it runs after ListView's own
                // response to this same height change has already
                // happened, not before it. Stops mattering once the
                // height stops changing (i.e. once the user might
                // actually be scrolling themselves).
                onHeightChanged: Qt.callLater(plansListView.positionViewAtBeginning)

                Label {
                    text: "Unresolved conflicts (" + syncController.unresolvedConflicts.length + ")"
                    font.bold: true
                    color: Theme.warnText
                }

                Repeater {
                    model: syncController.unresolvedConflicts
                    delegate: Rectangle {
                        id: conflictCard
                        required property var modelData
                        required property int index
                        width: plansListView.delegateWidth
                        color: Theme.warnBg
                        border.color: Theme.warnBorder
                        radius: 4
                        height: conflictColumn.implicitHeight + 16

                        ColumnLayout {
                            id: conflictColumn
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    color: Theme.warnText
                                    font.bold: true
                                    text: root.conflictHeading(conflictCard.modelData)
                                }
                                StatusBadge {
                                    // Same "CONFLICT" wording and
                                    // dedicated conflictText color
                                    // LibraryConsistencyPage's own
                                    // conflict badge uses -- warnText is
                                    // the generic "needs attention" color
                                    // (still used for this section's
                                    // header/card chrome above and
                                    // below), conflictText is Theme.qml's
                                    // own dedicated "these two things
                                    // disagree" token.
                                    label: "CONFLICT"
                                    badgeColor: Theme.conflictText
                                    // Spells out the actual conflicting
                                    // options, not just which two formats
                                    // disagree -- so the choice below
                                    // ("Use this" per side) is legible
                                    // from the badge alone, before even
                                    // opening the per-track cards.
                                    tooltipText: root.formatLabel(conflictCard.modelData.targetFormat) + " needs cues, but "
                                        + root.formatLabel(conflictCard.modelData.sourceAFormat) + " ("
                                        + conflictCard.modelData.sourceASummary + ") and "
                                        + root.formatLabel(conflictCard.modelData.sourceBFormat) + " ("
                                        + conflictCard.modelData.sourceBSummary + ") disagree."
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                // Same investigation tools (waveform with cue
                                // markers, Play, CueFallbackNotice) the
                                // ordinary plan list below already gives each
                                // side of a match -- a conflict deserves the
                                // same look, not just a one-line summary.
                                //
                                // Conflicts render via a plain Repeater
                                // (see the header comment above on why:
                                // "one list, one scrollbar"), not a
                                // virtualized ListView -- so unlike the
                                // real plan list below, every conflict
                                // card here gets created immediately,
                                // all at once, whether on-screen yet or
                                // not. Each TrackWaveformCard's waveform
                                // fetch is a real, synchronous disk read
                                // (PlaybackController::waveformFor(),
                                // uncached on first access) -- with
                                // several conflicts on a real library,
                                // that's a real, measurable UI-thread
                                // stall right when this page opens.
                                // asynchronous: true spreads each card's
                                // creation (and so its waveform read)
                                // across idle frames instead of doing
                                // them all in one block, without needing
                                // to make the read itself async or turn
                                // this into a second, separately-
                                // scrolled virtualized list.
                                Repeater {
                                    model: [
                                        { track: conflictCard.modelData.sourceATrack, format: conflictCard.modelData.sourceAFormat,
                                          summary: conflictCard.modelData.sourceASummary, hasJunkCue: conflictCard.modelData.sourceAHasJunkCue, useSourceA: true },
                                        { track: conflictCard.modelData.sourceBTrack, format: conflictCard.modelData.sourceBFormat,
                                          summary: conflictCard.modelData.sourceBSummary, hasJunkCue: conflictCard.modelData.sourceBHasJunkCue, useSourceA: false }
                                    ]
                                    delegate: Loader {
                                        id: cardLoader
                                        required property var modelData
                                        Layout.fillWidth: true
                                        asynchronous: true
                                        sourceComponent: TrackWaveformCard {
                                            track: cardLoader.modelData.track
                                            formatLabelText: root.formatLabel(cardLoader.modelData.format)
                                            formatLabelTooltip: cardLoader.modelData.summary
                                            actionButtonText: "Use this"
                                            actionButtonTooltip: "Stage copying (and overwriting) these cue points onto the other track; Save writes it"
                                            onActionTriggered: syncController.resolveConflict(conflictCard.index, cardLoader.modelData.useSourceA)
                                            hintText: cardLoader.modelData.hasJunkCue
                                                ? "This side has a 0:00 memory cue that's usually accidental - consider cleaning it up in Clean Up before deciding."
                                                : ""
                                            playbackController: root.playbackController
                                            playbackPath: root.pathForFormat(cardLoader.modelData.track.side)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            delegate: Column {
                id: delegateRoot
                width: plansListView.delegateWidth
                spacing: 4

                required property int index
                required property string sourceFormat
                required property string targetFormat
                required property string filename
                required property string description
                required property bool conflict
                required property var tracks
                required property bool staged
                required property string stagedDescription

                property bool expanded: false

                ItemDelegate {
                    id: planRow
                    width: parent.width
                    hoverEnabled: true
                    onClicked: delegateRoot.expanded = !delegateRoot.expanded

                    ToolTip.visible: hovered
                    ToolTip.text: delegateRoot.filename

                    contentItem: ColumnLayout {
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: root.formatLabel(delegateRoot.sourceFormat) + " -> " + root.formatLabel(delegateRoot.targetFormat)
                                font.bold: true
                                color: Theme.good
                                Layout.preferredWidth: 160
                            }
                            Label {
                                text: delegateRoot.tracks.length > 0
                                    ? (delegateRoot.tracks[0].title + " - " + delegateRoot.tracks[0].artist)
                                    : delegateRoot.filename
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            StatusBadge {
                                visible: delegateRoot.staged
                                label: "Staged"
                                badgeColor: Theme.warnText
                                tooltipText: delegateRoot.stagedDescription + "\n\nNot on the stick yet: press Save."
                            }
                            Button {
                                text: delegateRoot.staged ? "Unstage" : "Stage"
                                enabled: !syncController.busy && !syncController.writing
                                ToolTip.visible: hovered
                                ToolTip.text: delegateRoot.staged
                                    ? "Take this track back out of the changes to save"
                                    : "Stage syncing just this one track; Save writes it"
                                onClicked: delegateRoot.staged
                                    ? syncController.unstage(delegateRoot.index)
                                    : syncController.applyOne(delegateRoot.index)
                            }
                            Label {
                                text: delegateRoot.expanded ? "▾" : "▸"
                                color: Theme.textMuted
                            }
                        }
                        Label {
                            text: delegateRoot.description
                            color: Theme.textMuted
                            leftPadding: 168
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // Groups the two sides' copies of this track visually -
                // matches DuplicatesPage.qml's treatment, and same
                // rationale: several stacked expanded groups otherwise read
                // as one long list rather than distinct matched pairs.
                Rectangle {
                    x: 168
                    width: parent.width - 168
                    visible: delegateRoot.expanded
                    height: delegateRoot.expanded ? syncGroupColumn.implicitHeight + 16 : 0
                    color: Theme.groupBackground
                    border.color: Theme.borderSubtle
                    radius: 4

                    ColumnLayout {
                        id: syncGroupColumn
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                    // The "meta track" both sides below are matched copies
                    // of, ties the pair together as one group rather than
                    // two unrelated-looking Rekordbox/Engine frames.
                    Label {
                        Layout.fillWidth: true
                        text: (delegateRoot.tracks.length > 0
                            ? delegateRoot.tracks[0].title + " - " + delegateRoot.tracks[0].artist
                            : delegateRoot.filename)
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Repeater {
                        model: delegateRoot.tracks
                        delegate: TrackWaveformCard {
                            required property var modelData
                            track: modelData
                            formatLabelText: root.formatLabel(modelData.side)
                            playbackController: root.playbackController
                            playbackPath: root.pathForFormat(modelData.side)
                        }
                    }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: plansListView.count === 0 && !syncController.busy
                text: "Nothing to sync. Matched tracks' cues are already consistent."
                color: Theme.textMuted
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: syncController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: syncController.busy
        current: syncController.scanCurrent
        total: syncController.scanTotal
        label: "Scanning for sync differences..."
        unitName: "tracks"
        cancellable: syncController.scanCancellable
        onCancelRequested: syncController.cancelScan()
    }
}
