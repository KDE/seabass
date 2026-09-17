// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Sync Cue Points: every track whose copies in the stick's catalogs carry
// different cues, and the one decision each needs.
//
// Laid out like Clean Up, top to bottom: what this does, what is in scope,
// the controls that act on the list, then the list. The header holds only
// the breadcrumb -- the playlist, search and staging controls used to sit
// beside it, as far from the list they act on as the window allowed, and
// ran off the edge of any narrower one.
//
// One list, two sections (see SyncPlanListModel): tracks waiting for a
// decision, then tracks ready to sync. Both are the same row, the shared
// MetadataTrackDelegate, and expand into the same panel: the two copies'
// waveforms side by side at equal width, one under the other on a narrow
// window. They used to be two designs -- amber cards with frames sized to
// whatever text sat next to them, and text rows whose expanded group hung
// off a 160px label column -- so picking a side moved a track from one look
// to the other.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController
    required property var playbackController

    // "" scopes analyze() to the whole library (every catalog present),
    // same empty-means-all convention every other picker in this app uses.
    property string selectedPlaylistName: ""

    // From a decision whose side carries a 0:00 memory cue: Clean Up Stray
    // Cues is where that gets fixed. Main.qml pushes the page; this one
    // only asks, through the same leave guard Back uses, because changes
    // staged here hold the library's edit session.
    signal junkCueCleanupRequested(string stickLabel, string rekordboxPath, string enginePath)

    // Wide enough for the row's catalog and cue columns beside the title,
    // and for a decision's two waveforms beside each other. Below it both
    // fold: the columns move into the artist line and the waveforms stack.
    readonly property bool wide: width >= 980

    SyncController {
        id: syncController
        objectName: "syncController"
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
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

    readonly property bool searching: toolbar.searchText.length > 0

    function formatLabel(format) { return FormatLabels.label(format); }
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    function analyzeInScope() {
        syncController.analyze(root.rekordboxPath, root.enginePath, root.selectedPlaylistName);
    }
    function plural(n, word) {
        return n + " " + word + (n === 1 ? "" : "s");
    }
    function describeCues(cues) {
        var hot = 0;
        var memory = 0;
        for (var i = 0; i < cues.length; i++) {
            if (cues[i].kind === "hot") {
                hot++;
            } else {
                memory++;
            }
        }
        var parts = [];
        if (hot > 0) {
            parts.push(hot + " hot");
        }
        if (memory > 0) {
            parts.push(memory + " memory");
        }
        return parts.length > 0 ? parts.join(" · ") : "no cues";
    }
    function formatDuration(ms) {
        if (!(ms > 0)) {
            return "";
        }
        var totalSeconds = Math.round(ms / 1000);
        var m = Math.floor(totalSeconds / 60);
        var s = totalSeconds % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    // What each side's header says next to its catalog. A decision's two
    // sides are just their cues; a ready track's target also says what the
    // write does to it, counted the way the row counts it.
    function sideCueText(row, side) {
        var text = root.describeCues(row.tracks[side].cues);
        if (row.needsDecision || side === 0) {
            return text;
        }
        var change = row.cueChange;
        var gained = change.gainedHot + change.gainedMemory;
        var dropped = change.droppedHot + change.droppedMemory;
        if (gained > 0) {
            text += " · gains " + gained;
        }
        if (dropped > 0) {
            text += " · loses " + dropped;
        }
        return text;
    }

    // The sentence under a row's waveforms: what staging or picking does,
    // in the numbers the row already showed.
    function panelSentence(row) {
        if (row.needsDecision) {
            if (row.samePair) {
                return "Whichever side you pick is staged onto the other copy, replacing its hot cues. "
                    + "Nothing is written until Save.";
            }
            return root.formatLabel(row.targetFormat) + " needs cues, but "
                + root.formatLabel(row.tracks[0].side) + " and " + root.formatLabel(row.tracks[1].side)
                + " disagree. The side you pick is staged onto " + root.formatLabel(row.targetFormat)
                + ". Nothing is written until Save.";
        }
        var change = row.cueChange;
        var source = root.formatLabel(row.sourceFormat);
        var target = root.formatLabel(row.targetFormat);
        var gained = [];
        if (change.gainedHot > 0) {
            gained.push(root.plural(change.gainedHot, "hot cue"));
        }
        if (change.gainedMemory > 0) {
            gained.push(root.plural(change.gainedMemory, "memory cue"));
        }
        var sentence = gained.length > 0
            ? "Adds " + gained.join(" and ") + " from " + source
            : "Copies the " + source + " cues";
        var kept = change.keptHot + change.keptMemory;
        var dropped = change.droppedHot + change.droppedMemory;
        if (kept > 0) {
            sentence += (dropped > 0 ? ", keeps the " : " and keeps the ")
                + root.plural(kept, "cue") + " " + target + " already has";
        }
        if (dropped > 0) {
            sentence += " and replaces " + root.plural(dropped, "cue") + " of its own";
        }
        return sentence + ". Nothing is written until Save.";
    }

    // Opens on the playlist last picked on any page with a picker.
    Component.onCompleted: {
        root.selectedPlaylistName = root.appSettingsController.lastPlaylistName;
        root.analyzeInScope();
    }

    // A remembered playlist this library does not have would scan
    // nothing. The picker lists the whole library's playlists whatever
    // the scope, so once a scan is done a missing one falls back to all
    // tracks -- without forgetting it, since the next stick may have it.
    // Checked a turn later, not from inside the controller's own
    // busyChanged, and never after a cancel: a cancelled scan leaves.
    property bool cancellingScan: false
    function dropMissingPlaylist() {
        if (root.cancellingScan || syncController.busy || root.selectedPlaylistName.length === 0
            || syncController.playlistNames.indexOf(root.selectedPlaylistName) >= 0) {
            return;
        }
        root.selectedPlaylistName = "";
        root.analyzeInScope();
    }

    // Fixed widths for the row's right-hand columns, measured from the
    // widest thing each can hold, so a catalog badge, a cue count and a
    // status badge sit in the same place on every row whatever their text.
    TextMetrics {
        id: catalogNameMetrics
        font.bold: true
        font.pointSize: Theme.fontTiny
        text: FormatLabels.label("rekordbox")
    }
    TextMetrics {
        id: cueSummaryMetrics
        font.family: Theme.dataFamily
        font.pointSize: Theme.fontSmall
        text: "+16 hot, +2 memory, keeps 16"
    }
    TextMetrics {
        id: statusBadgeMetrics
        font.bold: true
        font.pointSize: Theme.fontTiny
        text: "CONFLICT"
    }
    TextMetrics {
        id: unstageMetrics
        text: "Unstage"
    }
    readonly property real catalogBadgeWidth: Math.ceil(catalogNameMetrics.advanceWidth) + 12
    readonly property real directionMarkWidth: Theme.iconSizeSmall * 0.75
    readonly property real cueColumnWidth: Math.ceil(cueSummaryMetrics.advanceWidth) + 1
    readonly property real statusColumnWidth: Math.ceil(statusBadgeMetrics.advanceWidth) + 12
        + Theme.tightSpacing + Math.ceil(unstageMetrics.advanceWidth) + 2 * Theme.cardPadding

    // A catalog as Home shows it: an accent badge with its FormatLabels
    // name, in a slot as wide as the longest name so the arrow between two
    // of them never moves. The first badge sits against the arrow from the
    // left and the second from the right, so a short name leaves its gap
    // on the outside rather than between the two. Dimmed for the side a
    // ready track writes to.
    component CatalogSlot: Item {
        id: slot
        property string format: ""
        property bool dimmed: false
        property bool towardsRight: false
        implicitWidth: root.catalogBadgeWidth
        implicitHeight: slotBadge.implicitHeight
        StatusBadge {
            id: slotBadge
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: slot.towardsRight ? parent.right : undefined
            anchors.left: slot.towardsRight ? undefined : parent.left
            label: root.formatLabel(slot.format)
            badgeColor: slot.dimmed ? Theme.textMuted : Theme.accent
        }
    }

    // One copy of a track inside an expanded row.
    component SideCard: ColumnLayout {
        id: card
        required property var track
        property string eyebrow: ""
        property bool dimmed: false
        property string cueText: ""
        property string actionText: ""
        property string actionTooltip: ""
        property bool showJunkNote: false
        signal actionTriggered()
        signal junkLinkActivated()

        objectName: "sideCard"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.alignment: Qt.AlignTop
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumHeight: actionButton.implicitHeight
            spacing: Theme.tightSpacing
            TableHeaderLabel {
                visible: card.eyebrow.length > 0
                label: card.eyebrow
            }
            StatusBadge {
                label: root.formatLabel(card.track.side)
                badgeColor: card.dimmed ? Theme.textMuted : Theme.accent
            }
            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: card.cueText
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: Theme.textMuted
                elide: Text.ElideRight
            }
            Button {
                id: actionButton
                objectName: "useTheseCuesButton"
                visible: card.actionText.length > 0
                text: card.actionText
                enabled: !syncController.busy && !syncController.writing
                ToolTip.visible: hovered
                ToolTip.text: card.actionTooltip
                onClicked: card.actionTriggered()
            }
        }

        Item {
            objectName: "sideWaveform"
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            WaveformView {
                anchors.fill: parent
                opacity: card.dimmed ? 0.75 : 1.0
                // Fetched on demand, and only for a row someone opened --
                // see SyncPlanListModel::setAnalysis() for why that matters.
                waveformData: root.playbackController
                    ? root.playbackController.waveformFor(card.track.side, root.pathForFormat(card.track.side),
                                                          card.track.sourceId)
                    : []
                format: card.track.side
                cueData: card.track.cues
                trackDurationMs: card.track.durationMs
            }
            // In the waveform's corner rather than beside it, so both
            // sides' waveforms are exactly as wide as their column.
            ToolButton {
                id: playButton
                anchors.right: parent.right
                anchors.rightMargin: Theme.tightSpacing
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Theme.iconSizeSmall * 0.75
                implicitHeight: Theme.iconSizeSmall * 0.75
                padding: 0
                display: AbstractButton.IconOnly
                text: "Play"
                // SeabassIcon, as the Re-Analyze button: icon.source drew
                // an empty square under the KDE style.
                contentItem: Item {
                    SeabassIcon {
                        anchors.centerIn: parent
                        iconName: "media-playback-start"
                        size: Theme.iconSizeSmall * 0.5
                        color: playButton.enabled ? Theme.text : Theme.textMuted
                    }
                }
                enabled: card.track.filePath.length > 0 && root.playbackController !== null
                background: Rectangle {
                    radius: 4
                    color: playButton.hovered ? Theme.rowHover : Theme.surface
                    opacity: 0.9
                }
                ToolTip.visible: hovered
                ToolTip.text: "Play this copy of the track"
                onClicked: root.playbackController.load(card.track.side, root.pathForFormat(card.track.side),
                    card.track.sourceId, card.track.filePath, card.track.title, card.track.artist,
                    card.track.artworkPath, card.track.cues)
            }
        }

        CueFallbackNotice {
            cues: card.track.cues
            durationMs: card.track.durationMs
        }

        RowLayout {
            objectName: "junkCueNote"
            visible: card.showJunkNote
            Layout.fillWidth: true
            spacing: Theme.tightSpacing
            SeabassIcon {
                Layout.alignment: Qt.AlignTop
                iconName: "dialog-warning"
                size: junkLabel.font.pixelSize > 0 ? junkLabel.font.pixelSize * 1.3 : Theme.iconSizeSmall * 0.5
                color: Theme.conflictText
            }
            Label {
                id: junkLabel
                objectName: "junkCueLink"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.conflictText
                font.pointSize: Theme.fontSmall
                textFormat: Text.StyledText
                linkColor: Theme.accent
                text: "A 0:00 memory cue on this side is usually accidental. "
                    + "<a href=\"clean-up-stray-cues\">Clean Up Stray Cues</a>"
                onLinkActivated: card.junkLinkActivated()
                HoverHandler {
                    cursorShape: junkLabel.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
            }
        }
    }

    component StatFigure: RowLayout {
        id: figure
        property string value: ""
        property string label: ""
        property color valueColor: Theme.text
        spacing: Theme.tightSpacing
        StatValue {
            Layout.alignment: Qt.AlignBaseline
            text: figure.value
            color: figure.valueColor
        }
        TableHeaderLabel {
            Layout.alignment: Qt.AlignBaseline
            label: figure.label
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
        }
    }

    MessageDialog {
        id: confirmDialog
        objectName: "confirmStageDialog"
        severity: SeabassDialog.Question

        // Same choice Clean Up offers when a search is hiding some of what
        // is ticked: stage what the search shows (the default, since that
        // is what is on screen) or everything ticked.
        readonly property int shownSelected: syncController.selectedVisibleCount
        readonly property int hiddenSelected: syncController.selectedCount - shownSelected
        readonly property bool searchHidesSome: root.searching && hiddenSelected > 0
        // Taken when the dialog opens: directionCountsFor() is a call, not
        // a property, so nothing would re-evaluate it.
        property var directions: []
        function prepare() {
            confirmDialog.directions = syncController.directionCountsFor(confirmDialog.searchHidesSome);
        }
        function tracks(n) { return n + (n === 1 ? " Track" : " Tracks"); }

        title: "Stage syncing " + root.plural(searchHidesSome ? shownSelected : syncController.selectedCount, "track") + "?"
        acceptText: searchHidesSome ? "Stage " + tracks(shownSelected) + " Matching the Search"
                                    : "Stage " + tracks(syncController.selectedCount)
        acceptEnabled: !searchHidesSome || shownSelected > 0
        alternateText: searchHidesSome ? "Stage All " + tracks(syncController.selectedCount) + " Selected" : ""
        onAlternateRequested: syncController.stageSelected(false)
        onAccepted: syncController.stageSelected(searchHidesSome)

        // A per-direction list rather than one sentence, so it stays in the
        // content slot instead of being flattened into `headline`.
        Repeater {
            model: confirmDialog.directions
            delegate: Label {
                required property var modelData
                Layout.fillWidth: true
                text: "Copy cues to " + root.formatLabel(modelData.targetFormat) + " from "
                    + root.formatLabel(modelData.sourceFormat) + " for " + root.plural(modelData.count, "track") + "."
                wrapMode: Text.WordWrap
            }
        }
        Label {
            objectName: "hiddenSelectionWarning"
            Layout.fillWidth: true
            visible: confirmDialog.searchHidesSome
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: "Your search (\"" + toolbar.searchText + "\") hides " + confirmDialog.hiddenSelected
                + " of the " + syncController.selectedCount + " selected tracks. Only the "
                + confirmDialog.shownSelected + " it shows are staged unless you stage all selected; "
                + "the hidden ones stay selected either way."
        }
        Label {
            Layout.fillWidth: true
            visible: {
                for (var i = 0; i < confirmDialog.directions.length; i++) {
                    if (confirmDialog.directions[i].targetFormat === "rekordbox") return true;
                }
                return false;
            }
            text: "DeviceLibrary writing is the least-proven part of Seabass. Verify the result "
                + "on real hardware before trusting it for a gig."
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
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        RowLayout {
            objectName: "introRow"
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: "A track exported to more than one catalog on this stick can end up with hot cues in "
                    + "one and none, or different ones, in another. Seabass matches the copies across the "
                    + "stick's catalogs and lists every track where they differ. Tick the ones you want and "
                    + "stage them; Save copies the cues across. Every catalog is backed up first, and "
                    + "\"Undo Last Save\" puts back every file it touched."
            }
            InfoButton {
                Layout.alignment: Qt.AlignTop
                explanationTitle: "How Sync Cue Points decides"
                summaryText: "Copies are matched by file first. A track with cues on only one side is ready "
                    + "to sync; one whose two sides have different hot cues waits for you to pick."
                explanationText:
                      "## Matching\n"
                    + "The same audio file on the stick is the same track, whichever catalog lists it. "
                    + "Only when a catalog has no file path for a track does Seabass fall back to title, "
                    + "artist and length.\n\n"
                    + "## Ready to sync\n"
                    + "One side has cues the other lacks, and staging copies them across. A hot cue the "
                    + "other side already has, on the same pad at the same place, is kept rather than "
                    + "copied again: that is what *keeps 1* on a row means.\n\n"
                    + "## Needs a decision\n"
                    + "Both sides have hot cues, and they differ. No timestamp can say which set you "
                    + "meant -- Engine moves a track's edit time for a rating or a BPM change just as for "
                    + "a cue. Pick the side whose hot cues should be on both; the pick is staged straight "
                    + "away.\n\n"
                    + "## DeviceLibrary and OneLibrary\n"
                    + "They are one library written in two formats, so they are never synced against each "
                    + "other. Every write to DeviceLibrary is mirrored into OneLibrary.\n\n"
                    + "## Saving\n"
                    + "Nothing is written until Save. Every catalog involved is backed up first, and "
                    + "*Undo Last Save* restores every file the last save touched.\n"
            }
        }

        RowLayout {
            objectName: "scopeRow"
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            Flow {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.tightSpacing
                StatusBadge {
                    visible: root.rekordboxPath.length > 0
                    label: root.formatLabel("rekordbox") + "  " + syncController.rekordboxTrackCount
                    badgeColor: Theme.accent
                    tooltipText: root.plural(syncController.rekordboxTrackCount, "DeviceLibrary track") + " in scope"
                }
                StatusBadge {
                    visible: root.enginePath.length > 0
                    label: root.formatLabel("engine") + "  " + syncController.engineTrackCount
                    badgeColor: Theme.accent
                    tooltipText: root.plural(syncController.engineTrackCount, "Engine track") + " in scope"
                }
                StatusBadge {
                    visible: syncController.oneLibraryTrackCount > 0
                    label: root.formatLabel("onelibrary") + "  " + syncController.oneLibraryTrackCount
                    badgeColor: Theme.accent
                    tooltipText: root.plural(syncController.oneLibraryTrackCount, "OneLibrary track") + " in scope"
                }
                Label {
                    height: parent.children[0].implicitHeight
                    verticalAlignment: Text.AlignVCenter
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    text: root.selectedPlaylistName.length > 0
                        ? "tracks in “" + root.selectedPlaylistName + "”"
                        : "tracks on the stick"
                }
            }

            StatFigure {
                objectName: "needSyncFigure"
                value: syncController.planCount
                label: "need sync"
            }
            StatFigure {
                Layout.leftMargin: Theme.rowSpacing
                value: syncController.conflictCount
                label: "conflicts"
                valueColor: syncController.conflictCount > 0 ? Theme.conflictText : Theme.text
            }
            StatFigure {
                Layout.leftMargin: Theme.rowSpacing
                value: syncController.stagedCount
                label: "staged"
                valueColor: syncController.stagedCount > 0 ? Theme.warnText : Theme.text
            }
        }

        GridLayout {
            id: toolsRow
            objectName: "filterRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            columns: root.wide ? 3 : 1
            columnSpacing: Theme.sectionSpacing
            rowSpacing: Theme.tightSpacing

            RowLayout {
                Layout.minimumWidth: 0
                spacing: Theme.tightSpacing
                Label {
                    text: "Playlist:"
                    color: Theme.textMuted
                }
                PlaylistPickerCombo {
                    objectName: "playlistPicker"
                    Layout.preferredWidth: Math.max(160, Math.min(240, root.width * 0.2))
                    Layout.minimumWidth: 0
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
                        root.appSettingsController.lastPlaylistName = root.selectedPlaylistName;
                        root.analyzeInScope();
                    }
                }
                ToolButton {
                    id: reanalyzeButton
                    objectName: "reanalyzeButton"
                    implicitWidth: Theme.iconSizeSmall
                    implicitHeight: Theme.iconSizeSmall
                    padding: 0
                    flat: true
                    // Still needed with a contentItem of our own: the KDE
                    // style paints the label itself, over the icon, unless
                    // told the button is icon-only.
                    display: AbstractButton.IconOnly
                    text: "Re-Analyze"
                    // Drawn by SeabassIcon rather than icon.source: under
                    // the KDE style an icon-only ToolButton's own icon
                    // rendered as an empty square.
                    contentItem: Item {
                        SeabassIcon {
                            anchors.centerIn: parent
                            iconName: "view-refresh"
                            size: Theme.iconSizeSmall * 0.5
                            color: reanalyzeButton.enabled ? Theme.text : Theme.textMuted
                        }
                    }
                    enabled: !syncController.busy && !syncController.writing
                    ToolTip.visible: hovered
                    ToolTip.text: "Re-scan the catalogs and recompute what needs syncing"
                    onClicked: root.analyzeInScope()
                }
            }

            MetadataListToolbar {
                id: toolbar
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                placeholder: "Search title or artist"
                selectionEnabled: !syncController.busy && !syncController.writing
                    && syncController.visiblePlanCount > 0
                selectAllTooltip: root.searching ? "Tick every track the search shows" : "Tick every track ready to sync"
                summary: {
                    var shown = syncController.visiblePlanCount + syncController.visibleConflictCount;
                    var total = syncController.planCount + syncController.conflictCount;
                    return root.searching
                        ? shown + " of " + total + " shown · " + syncController.selectedVisibleCount + " selected"
                        : root.plural(total, "track") + " · " + syncController.selectedCount + " selected";
                }
                onSelectAllRequested: syncController.setAllIncluded(true)
                onSelectNoneRequested: syncController.setAllIncluded(false)
                onSearchChanged: (text) => syncController.search(text)
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                spacing: Theme.rowSpacing
                Label {
                    visible: syncController.stagedCount > 0
                    text: syncController.stagedCount + " staged, not saved yet"
                    color: Theme.warnText
                    font.pointSize: Theme.fontSmall
                }
                Button {
                    text: "Undo Last Save"
                    visible: syncController.canUndo
                    enabled: !syncController.busy && !syncController.writing
                    ToolTip.visible: hovered
                    ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                    onClicked: syncController.undoLastOperation()
                }
                Button {
                    objectName: "stageSelectedButton"
                    highlighted: true
                    text: "Stage " + (root.searching ? syncController.selectedVisibleCount : syncController.selectedCount)
                        + " Selected"
                    enabled: !syncController.busy && !syncController.writing && syncController.selectedCount > 0
                    ToolTip.visible: hovered
                    ToolTip.text: "Review what will be copied, then stage every ticked track; Save writes them"
                    onClicked: {
                        confirmDialog.prepare();
                        confirmDialog.open();
                    }
                }
            }
        }

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

        ListView {
            id: plansListView
            objectName: "plansList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            clip: true
            model: syncController.plans
            spacing: 2
            // Scrolls the last row clear of the floating Save button, which
            // it would otherwise sit behind for good.
            bottomMargin: editHost.saveClearance
            ScrollBar.vertical: BigScrollBar {}

            // BigScrollBar overlays the list rather than reserving its own
            // layout space, so rows stop short of it instead of running
            // underneath the thumb.
            readonly property real delegateWidth: width - 14

            section.property: "section"
            section.criteria: ViewSection.FullString
            section.delegate: Item {
                id: sectionHeader
                required property string section
                readonly property bool decisions: section === "decision"
                // Room above the second section, none above the first.
                readonly property real gapAbove: !decisions && syncController.visibleConflictCount > 0
                    ? Theme.sectionSpacing : 0
                width: plansListView.delegateWidth
                height: gapAbove + headerRow.implicitHeight + Theme.tightSpacing

                RowLayout {
                    id: headerRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.tightSpacing
                    spacing: Theme.rowSpacing
                    Subtitle {
                        Layout.alignment: Qt.AlignBaseline
                        text: sectionHeader.decisions ? "Needs a decision" : "Ready to sync"
                    }
                    Label {
                        Layout.alignment: Qt.AlignBaseline
                        text: sectionHeader.decisions ? syncController.visibleConflictCount : syncController.visiblePlanCount
                        font.family: Theme.dataFamily
                        color: Theme.textMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignBaseline
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: sectionHeader.decisions
                            ? "Both sides have their own hot cues. Pick the side that should be on both."
                            : "One side has cues the other lacks. Staging copies them across."
                    }
                }
            }

            delegate: MetadataTrackDelegate {
                id: row
                required property string section
                required property bool needsDecision
                required property int planIndex
                required property int conflictIndex
                required property bool samePair
                required property string sourceFormat
                required property string targetFormat
                required property string filename
                required property var tracks
                required property string cueSummary
                required property var cueChange
                required property var junkCues
                required property bool included
                required property bool staged
                required property string stagedDescription

                // The row is about the track, not one copy of it; the
                // first copy stands for it.
                readonly property var shown: tracks[0]
                readonly property string directionText: root.formatLabel(tracks[0].side)
                    + (needsDecision ? " vs " : " → ") + root.formatLabel(tracks[1].side)

                width: plansListView.delegateWidth
                title: shown.title
                // Narrow, the catalog and cue columns fold into this line.
                artist: root.wide ? shown.artist : shown.artist + " · " + directionText + " · " + cueSummary
                artworkUrl: shown.artworkPath
                selectable: !needsDecision
                reserveSelectionSpace: true
                selected: included
                selectTooltip: staged ? "Already staged; Unstage takes it out" : "Include this track when staging"
                onSelectionToggled: (checked) => syncController.setIncluded(row.planIndex, checked)
                onExpandToggled: row.expanded = !row.expanded

                actionItems: [
                    RowLayout {
                        objectName: "directionColumn"
                        visible: root.wide
                        spacing: 0
                        CatalogSlot {
                            format: row.tracks[0].side
                            towardsRight: true
                        }
                        Item {
                            implicitWidth: root.directionMarkWidth
                            implicitHeight: Theme.iconSizeSmall * 0.5
                            Label {
                                anchors.centerIn: parent
                                visible: row.needsDecision
                                text: "vs"
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                            SeabassIcon {
                                anchors.centerIn: parent
                                visible: !row.needsDecision
                                iconName: "arrow-right"
                                size: Theme.iconSizeSmall * 0.5
                                color: Theme.textMuted
                            }
                        }
                        CatalogSlot {
                            format: row.tracks[1].side
                            dimmed: !row.needsDecision
                        }
                    },
                    Label {
                        objectName: "cueSummaryColumn"
                        visible: root.wide
                        Layout.preferredWidth: root.cueColumnWidth
                        Layout.leftMargin: Theme.rowSpacing
                        text: row.cueSummary
                        font.family: Theme.dataFamily
                        font.pointSize: Theme.fontSmall
                        color: Theme.textMuted
                        elide: Text.ElideRight
                    },
                    RowLayout {
                        objectName: "statusColumn"
                        // Not fillWidth, though a layout holding a filler
                        // defaults to it: it would then share the row's
                        // spare width with the title, in proportion to the
                        // title's length, and move from row to row.
                        Layout.fillWidth: false
                        Layout.preferredWidth: root.statusColumnWidth
                        spacing: Theme.tightSpacing
                        Item { Layout.fillWidth: true }
                        StatusBadge {
                            visible: row.needsDecision
                            label: "CONFLICT"
                            badgeColor: Theme.conflictText
                            tooltipText: row.samePair
                                ? root.formatLabel(row.tracks[0].side) + " (" + root.describeCues(row.tracks[0].cues)
                                  + ") and " + root.formatLabel(row.tracks[1].side) + " ("
                                  + root.describeCues(row.tracks[1].cues) + ") have different hot cues for this "
                                  + "track. Choose the side whose hot cues should be on both."
                                : root.formatLabel(row.targetFormat) + " needs cues, but "
                                  + root.formatLabel(row.tracks[0].side) + " and "
                                  + root.formatLabel(row.tracks[1].side) + " disagree."
                        }
                        StatusBadge {
                            visible: row.staged
                            label: "STAGED"
                            badgeColor: Theme.warnText
                            tooltipText: row.stagedDescription + "\n\nNot on the stick yet: press Save."
                        }
                        ToolButton {
                            visible: row.staged
                            text: "Unstage"
                            enabled: !syncController.writing
                            ToolTip.visible: hovered
                            ToolTip.text: "Take this track back out of the changes to save"
                            onClicked: syncController.unstage(row.planIndex)
                        }
                    },
                    SeabassIcon {
                        iconName: row.expanded ? "arrow-down" : "arrow-right"
                        size: Theme.iconSizeSmall * 0.75
                        color: Theme.textMuted
                    }
                ]

                expandedItems: [
                    Rectangle {
                        objectName: "syncPanel"
                        Layout.fillWidth: true
                        implicitHeight: panelColumn.implicitHeight + 2 * Theme.rowSpacing
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        radius: 4

                        // Takes clicks on the panel's own background, which
                        // would otherwise reach the row underneath and fold
                        // it shut.
                        MouseArea {
                            anchors.fill: parent
                        }

                        ColumnLayout {
                            id: panelColumn
                            anchors.fill: parent
                            anchors.margins: Theme.rowSpacing
                            spacing: Theme.rowSpacing

                            // The file and its facts: here rather than in
                            // the row, where they competed with the title.
                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.sectionSpacing
                                Label {
                                    text: row.filename
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontSmall
                                    color: Theme.textMuted
                                    width: Math.min(implicitWidth, panelColumn.width)
                                    elide: Text.ElideMiddle
                                }
                                Label {
                                    visible: text.length > 0
                                    text: root.formatDuration(row.shown.durationMs)
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontSmall
                                    color: Theme.textMuted
                                }
                                Label {
                                    visible: row.shown.bpm > 0
                                    text: Math.round(row.shown.bpm * 10) / 10 + " BPM"
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontSmall
                                    color: Theme.textMuted
                                }
                                Label {
                                    visible: row.shown.key.length > 0
                                    text: row.shown.key
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontSmall
                                    color: Theme.textMuted
                                }
                            }

                            GridLayout {
                                objectName: "sidesGrid"
                                Layout.fillWidth: true
                                // uniformCellWidths splits a width the columns
                                // do not divide into half pixels, which the
                                // layout rounds one up and one down; giving up
                                // the remainder keeps the two copies the same.
                                Layout.maximumWidth: parent.width
                                    - (parent.width - (columns - 1) * columnSpacing) % columns
                                columns: root.wide ? 2 : 1
                                uniformCellWidths: true
                                columnSpacing: Theme.rowSpacing
                                rowSpacing: Theme.rowSpacing

                                // Built only while the row is open, so a
                                // waveform is read only for a row someone
                                // looks at.
                                Repeater {
                                    model: row.expanded ? 2 : 0
                                    delegate: SideCard {
                                        id: sideCard
                                        required property int index
                                        track: row.tracks[index]
                                        eyebrow: row.needsDecision ? "" : (index === 0 ? "source" : "target")
                                        dimmed: !row.needsDecision && index === 1
                                        cueText: root.sideCueText(row, index)
                                        actionText: row.needsDecision ? "Use These Cues" : ""
                                        actionTooltip: row.samePair
                                            ? "Stage these hot cues onto the other catalog's copy of this track, "
                                              + "replacing its own; Save writes it"
                                            : "Stage copying these cue points onto the "
                                              + root.formatLabel(row.targetFormat) + " copy; Save writes it"
                                        showJunkNote: row.needsDecision && row.junkCues[index] === true
                                        onActionTriggered: syncController.resolveConflict(row.conflictIndex, sideCard.index === 0)
                                        onJunkLinkActivated: editHost.requestLeave(() => root.junkCueCleanupRequested(
                                            root.stickLabel, root.rekordboxPath, root.enginePath))
                                    }
                                }
                            }

                            Label {
                                objectName: "panelSentence"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: root.panelSentence(row)
                            }
                        }
                    }
                ]
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, parent.width)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                visible: plansListView.count === 0 && !syncController.busy
                text: root.searching
                    ? "No track needing sync matches “" + toolbar.searchText + "”."
                    : "Nothing to sync. Matched tracks' cues are already consistent."
                color: Theme.textMuted
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: syncController
        function onScanCancelled() { root.StackView.view.pop(); }
        function onBusyChanged() {
            if (!syncController.busy && !root.cancellingScan) {
                Qt.callLater(root.dropMissingPlaylist);
            }
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: syncController.busy
        current: syncController.scanCurrent
        total: syncController.scanTotal
        label: "Scanning for sync differences..."
        unitName: "tracks"
        cancellable: syncController.scanCancellable
        onCancelRequested: {
            root.cancellingScan = true;
            syncController.cancelScan();
        }
    }
}
