import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Metadata Backup: read what the DJ added to a stick's tracks into a
// database on this computer, and browse what is in there.
//
// The two halves sit on one page on purpose. "Back up" and "look at what
// I have backed up" are the same question asked before and after, and a
// browse view on its own page would be a place nobody visits until
// something has already gone wrong.
//
// One list answers both, and the source picker chooses which question it
// is answering. Pick a stick and the list is that stick measured against
// the store -- only the tracks a backup would actually change, ticked to
// add. Pick "Everything stored" and it is the store itself, ticked to
// forget. Two lists side by side was the alternative and it made a tall
// page where the thing you were looking at was never the whole width.
//
// Nothing here writes to a stick. Putting metadata back is a different
// page, linked from the line at the top, because it is a different
// decision. But the shape is the standard one either way: nothing
// happens until Save, the button says what it would do, and leaving with
// something staged asks first.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property string libraryId
    // For the source picker. Optional so the QML tests can build the
    // page without the whole media stack behind it; the picker then
    // offers the stick the page was opened on and the store, which is
    // exactly what it offered before there was a picker at all.
    property var mediaController: null

    readonly property bool hasStick: root.rekordboxPath.length > 0 || root.enginePath.length > 0
    // Any catalog directory will do: the controller reads every catalog
    // on the stick that one belongs to.
    readonly property string libraryPath: root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath

    // The row whose detail is showing, or -1/"" . One at a time: this is
    // a list to scan, not a tree to keep open. The two populations keep
    // their own, because a store row id and a stick path are not
    // comparable and a shared one would open a row in the other list.
    property var expandedTrackId: -1
    property string expandedProposalPath: ""

    signal metadataRestoreRequested()

    function formatBytes(bytes) {
        if (bytes <= 0) return "0 MB";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + " KB";
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB";
    }

    // ---- the source picker's model ------------------------------------
    //
    // Index 0 is the store, and every stick that has a library on it
    // follows. Rebuilt whenever the detected sticks change rather than
    // bound per row: a ComboBox wants a plain array, and this one is a
    // handful of entries that changes when someone plugs something in.
    property var sourceModel: []

    function rebuildSourceModel() {
        var list = [{
            name: "Everything stored",
            isStore: true,
            catalogPath: "",
            rekordboxPath: "",
            enginePath: "",
            libraryId: "",
        }];
        var seen = {};
        if (root.mediaController && root.mediaController.sticks) {
            var sticks = root.mediaController.sticks;
            for (var i = 0; i < sticks.count; i++) {
                var stick = sticks.get(i);
                if (!stick.hasRekordbox && !stick.hasEngine) {
                    continue;
                }
                var path = stick.rekordboxPath.length > 0 ? stick.rekordboxPath : stick.enginePath;
                seen[path] = true;
                list.push({
                    name: stick.label,
                    isStore: false,
                    catalogPath: path,
                    rekordboxPath: stick.rekordboxPath,
                    enginePath: stick.enginePath,
                    libraryId: stick.libraryId,
                });
            }
        }
        // The stick this page was opened on, when the media controller
        // is not there to list it (tests) or has not caught up yet.
        // Without this the picker could open on a page whose own stick
        // was not among its options.
        if (root.hasStick && !seen[root.libraryPath]) {
            list.push({
                name: root.stickLabel,
                isStore: false,
                catalogPath: root.libraryPath,
                rekordboxPath: root.rekordboxPath,
                enginePath: root.enginePath,
                libraryId: root.libraryId,
            });
        }
        root.sourceModel = list;
    }

    Component.onCompleted: root.rebuildSourceModel()

    Connections {
        target: root.mediaController && root.mediaController.sticks ? root.mediaController.sticks : null
        function onCountsChanged() { root.rebuildSourceModel(); }
    }

    // Which entry the combo should be showing, derived from the
    // controller rather than held beside it: the controller can refuse a
    // switch (staged changes), and a combo holding its own index would
    // then show a source the list is not actually showing.
    readonly property int currentSourceIndex: {
        if (controller.browsingStore) {
            return 0;
        }
        // Matched on the catalog path, not the label. Two sticks called
        // NO NAME or REKORDBOX is the ordinary case, not a corner one,
        // and matching on the label highlighted whichever of them came
        // first while the scan -- which is keyed on the path -- was
        // reading the other.
        for (var i = 1; i < root.sourceModel.length; i++) {
            if (root.sourceModel[i].catalogPath === controller.sourceLibraryPath) {
                return i;
            }
        }
        return 0;
    }

    // ---- leaving with something staged --------------------------------
    property var pendingLeave: null
    // Set when the user answered the leave dialog with "Back Up". The
    // leaving then waits for saveCompleted rather than happening beside
    // the save: popping this page destroys the controller, whose
    // destructor cancels the write and waits for the thread, so leaving
    // and saving at the same moment wrote a partial backup and showed no
    // summary. And when deletions are staged, save() does not save at
    // all -- it stops to ask -- so leaving straight away threw every
    // staged change away with the dialog still on its way up.
    property bool leaveAfterSave: false

    function requestLeave(leaveFn) {
        if (controller.busy) {
            return;
        }
        if (controller.dirty) {
            root.pendingLeave = leaveFn;
            unsavedDialog.open();
            return;
        }
        leaveFn();
    }

    function runPendingLeave() {
        var fn = root.pendingLeave;
        root.pendingLeave = null;
        root.leaveAfterSave = false;
        if (fn) {
            fn();
        }
    }

    // Whoever was leaving is no longer leaving: the save was cancelled,
    // refused at the delete confirmation, or failed. Staying put with
    // the staging intact is the right answer to all three.
    function abandonPendingLeave() {
        root.pendingLeave = null;
        root.leaveAfterSave = false;
    }

    MetadataBackupController {
        id: controller
    }

    Connections {
        target: controller
        function onActionFeedback(message, isError) {
            messagePopup.show(message, isError);
        }
        // A save that would only add things just runs. One that would
        // forget something asks first.
        function onDeletionConfirmationRequired() {
            confirmDeleteDialog.open();
        }
        // The save is done and everything staged is safe. Only now is it
        // right to leave, if leaving is what started it.
        function onSaveCompleted() {
            if (root.leaveAfterSave) {
                root.runPendingLeave();
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
        // Opaque background override: KDE's Breeze style bleeds the
        // window behind Seabass through an unstyled ToolBar.
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Metadata Backup"
                onHomeRequested: root.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: root.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: controller.busy
                visible: controller.busy
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        // Body text, not a section heading. It is two sentences of
        // explanation at normal reading size, and setting it in the
        // subtitle face made the first thing on the page compete with
        // the page's own title.
        Label {
            objectName: "pageIntro"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pointSize: Theme.fontNormal
            // A link, so the counterpart is reachable from the page that
            // explains what it would put back, instead of only from the
            // stick list two screens away.
            textFormat: Text.StyledText
            linkColor: Theme.accent
            text: "The cues, ratings and comments you put on your tracks, kept on this computer. "
                + "The audio can be re-imported from anywhere. This cannot. "
                + "You can restore the locally backed up metadata to any stick "
                + "<a href=\"restore\">here</a>."
            onLinkActivated: root.requestLeave(() => root.metadataRestoreRequested())
            // A link that does not say it is one is a link nobody
            // clicks.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }

        // ---- what the list is showing -------------------------------
        //
        // Not in a card. A bordered box indents everything inside it by
        // its own padding, which is how this page ended up with four
        // different left edges at once.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            Label {
                text: "Source:"
                color: Theme.textMuted
                font.pointSize: Theme.fontNormal
            }

            ComboBox {
                objectName: "sourcePicker"
                Layout.preferredWidth: Math.max(180, Math.min(300, root.width * 0.3))
                enabled: !controller.busy
                id: sourcePicker
                model: root.sourceModel
                textRole: "name"
                // A ComboBox assigns its own currentIndex when activated,
                // which breaks this binding. Every path that does not end
                // in a source change therefore puts it back by hand --
                // otherwise refusing a switch left the combo displaying a
                // stick the list was never showing, which is the exact
                // failure the derived index exists to prevent.
                currentIndex: root.currentSourceIndex
                onActivated: index => {
                    var entry = root.sourceModel[index];
                    if (!entry) {
                        return;
                    }
                    // The controller refuses while anything is staged,
                    // and says so by returning false rather than by
                    // silently doing nothing. The question belongs to
                    // the page, so it is asked here.
                    var accepted = entry.isStore
                        ? controller.browseStore()
                        : controller.selectStick(entry.rekordboxPath.length > 0 ? entry.rekordboxPath
                                                                                : entry.enginePath,
                                                 entry.libraryId, entry.name);
                    if (!accepted) {
                        switchSourceDialog.pendingEntry = entry;
                        switchSourceDialog.open();
                    }
                    // Whether it was accepted or not, the truth is the
                    // controller's, so re-read it rather than leave the
                    // combo showing what was merely clicked.
                    sourcePicker.currentIndex = Qt.binding(() => root.currentSourceIndex);
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: controller.browsingStore
                    ? "Showing everything backed up on this computer. Pick a stick to see what backing it up would change."
                    : "Showing what backing up " + controller.sourceStickLabel + " would change. "
                      + "Pick \"Everything stored\" to browse the backup itself."
            }

            Label {
                visible: !controller.browsingStore
                text: "Playlist:"
                color: Theme.textMuted
                font.pointSize: Theme.fontNormal
            }

            PlaylistPickerCombo {
                objectName: "playlistPicker"
                visible: !controller.browsingStore
                Layout.preferredWidth: Math.max(160, Math.min(260, root.width * 0.26))
                enabled: !controller.busy && controller.hasScanned
                model: {
                    var list = [{ name: "All tracks", count: controller.proposalCount }];
                    for (var i = 0; i < controller.playlistNames.length; i++) {
                        var name = controller.playlistNames[i];
                        list.push({ name: name, count: controller.playlistTrackCounts[name] });
                    }
                    return list;
                }
                currentIndex: {
                    if (controller.selectedPlaylist.length === 0) {
                        return 0;
                    }
                    for (var i = 0; i < controller.playlistNames.length; i++) {
                        if (controller.playlistNames[i] === controller.selectedPlaylist) {
                            return i + 1;
                        }
                    }
                    return 0;
                }
                onPlaylistPicked: (index, modelData) => controller.setPlaylist(index === 0 ? "" : modelData.name)
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "Back up one playlist instead of the whole stick"
            }

            Item { Layout.fillWidth: true }

            InfoButton {
                objectName: "backupInfoButton"
                explanationTitle: "What a metadata backup stores"
                summaryText: "Everything you added to your tracks yourself, kept on this computer so "
                    + "a reformatted or rebuilt stick does not take it with it."
                explanationText: "Seabass reads every catalog on the stick (DeviceLibrary, "
                    + "Device Library Plus and Engine) and folds them into one entry per file, so a "
                    + "track all three list is stored once with the union of its cues.\n\n"
                    + "Stored: cues and loops, rating, comment, play count, playlist membership, "
                    + "cover art, and enough of the title, artist and length to find the track "
                    + "again later.\n\n"
                    + "Not stored: waveforms, beat grids, analysis files and audio. All of it is "
                    + "derived from the audio file, all of it is large, and none of it is your work.\n\n"
                    + "## Only what would change\n\n"
                    + "Picking a stick lists the tracks a backup would actually add to or update in "
                    + "the store, and nothing else. A track the store already holds everything for is "
                    + "counted, not listed: it is not a decision anyone needs to make.\n\n"
                    + "## How a track is recognised\n\n"
                    + "On its title, artist and length, the same rule Seabass uses to match tracks "
                    + "between rekordbox and Engine everywhere else, falling back to the filename "
                    + "when a catalog has no title and artist to offer.\n\n"
                    + "Deliberately not on where the file sits. A path is the strongest signal while "
                    + "two catalogs are describing one stick, and the weakest thing to key a backup "
                    + "on: this store is meant to outlive the stick it came from, and a re-export "
                    + "renames folders, a rebuilt library moves Contents/ around, and the same track "
                    + "bought again lands somewhere else entirely. Title and artist travel with the "
                    + "recording, so a backup taken from one stick can be put back on a different "
                    + "one.\n\n"
                    + "Length is a guard rather than part of the key: two readings of one file differ "
                    + "by a rounding, so it has to agree within a couple of seconds, and a track "
                    + "whose length could not be read is not held against it. What it catches is a "
                    + "radio edit and an extended mix filed under one name.\n\n"
                    + controller.mergeRuleHelp
            }
        }

        // ---- what this source amounts to ----------------------------
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.tightSpacing

            Label {
                objectName: "sourceSummary"
                Layout.fillWidth: true
                visible: !controller.busy
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                text: {
                    if (controller.browsingStore) {
                        return "Nothing on any stick is changed or at risk. This only ever writes here: "
                             + controller.storeLocation;
                    }
                    if (controller.scanCancelled) {
                        return "Reading " + controller.sourceStickLabel
                             + " was stopped, so there is no list to show. Pick a source again to retry.";
                    }
                    if (!controller.hasScanned) {
                        return "Reading " + controller.sourceStickLabel + "...";
                    }
                    if (controller.proposalCount === 0) {
                        return "Everything on " + controller.sourceStickLabel
                             + " is already backed up. Nothing to do.";
                    }
                    var line = controller.proposalCount
                             + (controller.proposalCount === 1 ? " track on " : " tracks on ")
                             + controller.sourceStickLabel + " would add something to the backup";
                    if (controller.alreadyCurrent > 0) {
                        line += "; " + controller.alreadyCurrent
                             + (controller.alreadyCurrent === 1 ? " is" : " are") + " already stored and current";
                    }
                    line += ".";
                    if (controller.withoutIdentity > 0) {
                        line += " " + controller.withoutIdentity
                             + (controller.withoutIdentity === 1
                                 ? " track has too little to go on to be stored"
                                 : " tracks have too little to go on to be stored")
                             + " and were left out.";
                    }
                    return line;
                }
            }

            ProgressReport {
                Layout.fillWidth: true
                visible: controller.busy
                phase: controller.currentPhase
                unitsDone: controller.progressCurrent
                unitsTotal: controller.progressTotal
                unitName: "tracks"
                cancellable: true
                onCancelRequested: controller.cancel()
            }
        }

        // Said before the save, not discovered in the log afterwards.
        // What the unreadable catalog alone held is missing from this
        // plan, and a DJ who believes their Engine cues were just backed
        // up when Engine was never opened has been misled.
        Label {
            objectName: "unreadableCatalogsLabel"
            Layout.fillWidth: true
            visible: controller.catalogsUnreadable.length > 0 && !controller.busy
            wrapMode: Text.WordWrap
            color: Theme.warnText
            font.pointSize: Theme.fontSmall
            text: {
                var names = controller.catalogsUnreadable.join(", ");
                return (controller.catalogsUnreadable.length === 1
                         ? "One catalog on this stick could not be read: "
                         : "Some catalogs on this stick could not be read: ")
                     + names + ". Tracks only " + (controller.catalogsUnreadable.length === 1 ? "it" : "they")
                     + " listed are missing from the list below, and backing up will not store them.";
            }
        }

        SelectableText {
            objectName: "errorLabel"
            Layout.fillWidth: true
            visible: controller.errorMessage.length > 0
            text: controller.errorMessage
            color: Theme.danger
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.borderSubtle
        }

        // ---- one toolbar over whichever list is showing --------------
        MetadataListToolbar {
            objectName: "browseToolbar"
            Layout.fillWidth: true
            selectionEnabled: controller.browsingStore
                ? (controller.loadedCount > 0 && !controller.busy)
                : (controller.proposalCount > 0 && !controller.busy)
            selectAllTooltip: controller.browsingStore
                ? (controller.canLoadMore
                    ? "Mark every track loaded so far for deletion. Scroll to the end of the list to load the rest."
                    : "Mark every track in the list for deletion from the Metadata Backup")
                : "Stage every track on this stick's list, including any the search or playlist is hiding"
            summary: {
                if (!controller.browsingStore) {
                    if (controller.stagedAddCount > 0) {
                        return controller.stagedAddCount + " of " + controller.proposalCount + " staged";
                    }
                    if (controller.visibleProposalCount !== controller.proposalCount) {
                        return controller.visibleProposalCount + " of " + controller.proposalCount + " shown";
                    }
                    return controller.proposalCount
                         + (controller.proposalCount === 1 ? " track" : " tracks") + " to back up";
                }
                if (controller.storedTrackCount === 0) {
                    return "Nothing stored yet";
                }
                if (controller.stagedForDeletionCount > 0) {
                    return controller.stagedForDeletionCount + " of " + controller.matchCount + " staged";
                }
                if (controller.matchCount === controller.storedTrackCount) {
                    return controller.storedTrackCount + " tracks stored, "
                         + root.formatBytes(controller.artworkBytes) + " of cover art";
                }
                return controller.matchCount + " of " + controller.storedTrackCount + " tracks";
            }
            onSearchChanged: text => controller.search(text)
            onSelectAllRequested: controller.browsingStore ? controller.stageAllForDeletion()
                                                           : controller.stageAllForAdd()
            onSelectNoneRequested: controller.browsingStore ? controller.clearDeletionStaging()
                                                            : controller.unstageAllForAdd()
        }

        // ---- the stick's list ---------------------------------------
        ListView {
            id: proposalList
            objectName: "proposalList"
            visible: !controller.browsingStore
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: controller.proposals
            spacing: 2
            ScrollBar.vertical: BigScrollBar {}

            delegate: MetadataTrackDelegate {
                id: proposalRow
                // Roles the shared delegate does not already declare a
                // property for.
                required property bool isNew
                required property int cuesAdded
                required property bool cuesConflict
                required property bool cuesOffered
                required property int storedCueCount
                required property string cueSummary
                required property string changeSummary
                required property bool staged

                // And the ones it does, marked required here so the
                // model fills them.
                required index
                required title
                required artist
                required filename
                required relativePath
                required durationText
                required rating
                required comment
                required cueCount
                required artworkUrl
                required storedFrom

                // Ticking a row IS staging it. There is no second
                // selection to keep in step with this one, and so no way
                // for the two to disagree.
                selected: proposalRow.staged
                selectTooltip: "Stage this track to be added to the Metadata Backup"
                cueTooltip: proposalRow.cueSummary
                expanded: root.expandedProposalPath === proposalRow.relativePath
                // What the badge counts is not what is on the track but
                // what a backup would change about it, and on a track
                // the store already partly holds those are different
                // numbers.
                cueBadgeLabel: proposalRow.changeSummary.length > 0
                    ? proposalRow.changeSummary
                    : proposalRow.cueCount + (proposalRow.cueCount === 1 ? " cue" : " cues")
                cueBadgeColor: proposalRow.isNew ? Theme.good
                             : (proposalRow.cuesConflict ? Theme.warnIcon : Theme.good)
                detailNote: proposalRow.isNew
                    ? "The backup has never seen this track."
                    : (proposalRow.cuesConflict && proposalRow.cuesOffered
                        ? "The backup has " + proposalRow.storedCueCount
                          + " cues of its own for this track. Backing up replaces them."
                        : (proposalRow.cuesConflict
                            ? "The backup's own cues are staying: this stick's did not beat them."
                            : ""))

                onSelectionToggled: controller.toggleStagedForAdd(proposalRow.index)
                onExpandToggled: root.expandedProposalPath =
                    proposalRow.expanded ? "" : proposalRow.relativePath

                actionItems: [
                    Button {
                        objectName: "stageAddButton"
                        text: proposalRow.staged ? "Staged" : "Back up"
                        enabled: !controller.busy
                        onClicked: controller.toggleStagedForAdd(proposalRow.index)
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: proposalRow.staged
                            ? "Staged. Press again to take it back off the list."
                            : "Stage this track to be added to the Metadata Backup"
                    }
                ]
            }

            Label {
                anchors.centerIn: parent
                width: parent.width * 0.7
                visible: proposalList.count === 0 && !controller.busy
                         && (controller.hasScanned || controller.scanCancelled)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: controller.scanCancelled
                    ? "Nothing was read, so nothing is listed."
                    : (controller.proposalCount === 0
                        ? "Everything on this stick is already in the backup, and up to date."
                        : "No track on this stick matches that search.")
            }
        }

        // ---- what is in the store -----------------------------------
        ListView {
            id: trackList
            objectName: "storedTrackList"
            visible: controller.browsingStore
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: controller.browseModel
            spacing: 2
            ScrollBar.vertical: BigScrollBar {}

            // Paged from SQL, so the list asks for the next page only
            // when the user has actually scrolled to the end of this one.
            onAtYEndChanged: if (atYEnd && controller.canLoadMore) controller.loadMore()

            delegate: MetadataTrackDelegate {
                id: trackRow
                // Roles the shared delegate does not already declare a
                // property for.
                required property var trackId
                required property int playlistCount
                required property string updatedAt
                required property string stickLabel
                required property bool stagedForDeletion

                // And the ones it does, marked required here so the
                // model fills them.
                required index
                required title
                required artist
                required filename
                required relativePath
                required durationText
                required rating
                required comment
                required cueCount
                required artworkUrl

                // Ticking a row marks it, exactly as the button on the
                // end of it does. One state, so the two cannot disagree
                // and there is no second button to turn one into the
                // other.
                selected: trackRow.stagedForDeletion
                selectTooltip: "Mark this track for deletion from the Metadata Backup"
                storedFrom: trackRow.stickLabel
                // The stored timestamp is an ISO instant; the date is
                // the part a person reads, and the rest is noise in a
                // list.
                storedAt: trackRow.updatedAt.substring(0, 10)
                markedForRemoval: trackRow.stagedForDeletion
                expanded: root.expandedTrackId === trackRow.trackId
                // Fetched for the row the pointer is over, or the one
                // that is open, and for no others. The list is paged
                // precisely so that showing twenty rows costs twenty
                // rows, and pulling every cue of every row to fill
                // tooltips nobody opens would undo that.
                cueTooltip: (trackRow.hovered || trackRow.expanded) && trackRow.cueCount > 0
                    ? controller.cueSummaryFor(trackRow.trackId) : ""
                playlistNames: trackRow.expanded && trackRow.playlistCount > 0
                    ? controller.playlistsFor(trackRow.trackId).join(", ") : ""
                detailNote: trackRow.stagedForDeletion
                    ? "Staged for deletion from the metadata backup." : ""

                onSelectionToggled: controller.toggleStagedForDeletion(trackRow.index)
                onExpandToggled: root.expandedTrackId = trackRow.expanded ? -1 : trackRow.trackId

                actionItems: [
                    ToolButton {
                        objectName: "stageDeleteButton"
                        icon.name: trackRow.stagedForDeletion ? "edit-undo" : "edit-delete"
                        // Icon only, and the text is still set because
                        // that is what an assistive reader announces.
                        // Leaving the display at its default drew both:
                        // Breeze's own trash icon with the emoji next to
                        // it, two delete symbols on every row.
                        display: AbstractButton.IconOnly
                        text: trackRow.stagedForDeletion ? "Keep" : "Delete"
                        onClicked: controller.toggleStagedForDeletion(trackRow.index)
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: trackRow.stagedForDeletion
                            ? "Keep this track in the Metadata Backup after all"
                            : "Stage for deletion from Metadata Backup"
                    }
                ]
            }

            Label {
                anchors.centerIn: parent
                width: parent.width * 0.7
                visible: trackList.count === 0 && !controller.busy
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: controller.storedTrackCount === 0
                    ? "Nothing has been backed up yet. Pick a stick above to see what it would add."
                    : "No stored track matches that search."
                color: Theme.textMuted
            }
        }

        // ---- a way out of a staging you did not mean -----------------
        //
        // Under the list rather than beside the Save button: it is the
        // opposite of the primary action, and the two should not be
        // adjacent enough to hit by accident.
        RowLayout {
            Layout.fillWidth: true
            visible: controller.dirty && !controller.busy
            spacing: Theme.rowSpacing

            Label {
                Layout.fillWidth: true
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                wrapMode: Text.WordWrap
                text: {
                    var parts = [];
                    if (controller.stagedAddCount > 0) {
                        parts.push(controller.stagedAddCount + " to back up");
                    }
                    if (controller.stagedForDeletionCount > 0) {
                        parts.push(controller.stagedForDeletionCount + " to forget");
                    }
                    return parts.join(", ") + ". Nothing has happened yet.";
                }
            }
            Button {
                objectName: "clearStagingButton"
                text: "Clear"
                onClicked: controller.clearAllStaging()
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "Unstage everything, in both the stick's list and the backup's"
            }
        }
    }

    // ---- the one button that commits ----------------------------------
    //
    // The standard floating Save, with the word this page's save
    // actually means. Everywhere else in Seabass that button writes to a
    // stick; here it writes to this computer, which is why the dialogs
    // around it say so in their own words rather than borrowing the
    // stick wording.
    SaveOverlayButton {
        objectName: "saveOverlay"
        session: controller
        label: "Back Up"
        destinationPhrase: "stored on this computer"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
        z: 900
    }

    MessagePopup { id: messagePopup }

    UnsavedChangesDialog {
        id: unsavedDialog
        objectName: "unsavedDialog"
        pendingCount: controller.pendingCount
        message: "You have staged changes to your metadata backup. Save them before leaving?"
        // The stock line says "not on the stick yet", which is the one
        // thing that is never true here.
        detailText: controller.pendingCount > 0
            ? controller.pendingCount + " change(s) are staged and not in the backup yet." : ""
        saveText: "Back Up"
        onSaveRequested: {
            // Not followed by runPendingLeave(): onSaveCompleted does
            // that, once there is actually something to leave behind.
            root.leaveAfterSave = true;
            controller.save();
        }
        onDiscardRequested: {
            controller.clearAllStaging();
            root.runPendingLeave();
        }
        onRejected: root.abandonPendingLeave()
    }

    // Changing source throws the staging away, because it was decided
    // against numbers the next scan replaces. Asked rather than done.
    MessageDialog {
        id: switchSourceDialog
        objectName: "switchSourceDialog"
        property var pendingEntry: null
        severity: SeabassDialog.Warning
        title: "Change source"
        headline: "Discard what you have staged?"
        detailText: controller.pendingCount
            + " change(s) are staged against "
            + (controller.browsingStore ? "the backup" : controller.sourceStickLabel)
            + ". Changing source discards them, because what they were decided against is about to be "
            + "read again. Nothing has been written, so nothing is lost but the ticking."
        acceptText: "Discard and change"
        rejectText: "Stay here"
        onAccepted: {
            var entry = switchSourceDialog.pendingEntry;
            switchSourceDialog.pendingEntry = null;
            if (!entry) {
                return;
            }
            if (entry.isStore) {
                controller.discardStagingAndBrowseStore();
            } else {
                controller.discardStagingAndSelectStick(
                    entry.rekordboxPath.length > 0 ? entry.rekordboxPath : entry.enginePath,
                    entry.libraryId, entry.name);
            }
        }
        onRejected: switchSourceDialog.pendingEntry = null
    }

    // A backup is the one copy of these cues that may still exist, so
    // forgetting one is confirmed rather than clicked. The Save button
    // is the same standard one every editing page has; what makes this
    // page's save different is that part of it is destructive, and that
    // part gets said out loud before it happens.
    MessageDialog {
        id: confirmDeleteDialog
        objectName: "confirmDeleteDialog"
        severity: SeabassDialog.Warning
        title: "Delete from the metadata backup"
        headline: controller.stagedForDeletionCount === 1
            ? "Forget the cues, rating and comment stored for this track?"
            : "Forget the cues, ratings and comments stored for these "
              + controller.stagedForDeletionCount + " tracks?"
        detailText: "Nothing on any stick changes. What goes is this computer's copy, which may be the "
            + "only one left if the stick it came from has been rebuilt since."
            + (controller.stagedAddCount > 0
                ? " The " + controller.stagedAddCount + " track(s) staged to be backed up are stored first."
                : "")
        acceptText: "Delete"
        rejectText: "Cancel"
        onAccepted: controller.saveConfirmed()
        // Backing out of the confirmation backs out of the save, and so
        // out of any leaving that save was for.
        onRejected: root.abandonPendingLeave()
    }

    // ---- what the run did ---------------------------------------------
    OperationSummaryDialog {
        id: summaryDialog
    }

    Connections {
        target: controller
        function onResultChanged() {
            if (!controller.hasResult) {
                return;
            }
            var run = controller.lastRun;
            var detail = [];
            if (run.tracksAdded > 0) detail.push(run.tracksAdded + " newly stored");
            if (run.tracksUpdated > 0) detail.push(run.tracksUpdated + " brought up to date");
            if (run.tracksSkipped > 0) detail.push(run.tracksSkipped + " where the stored copy won");
            if (run.tracksUnchanged > 0) detail.push(run.tracksUnchanged + " already current");
            if (run.tracksWithoutIdentity > 0) detail.push(run.tracksWithoutIdentity + " with too little to go on");
            var lines = [detail.join(", ") + "."];
            lines.push(run.cuesStored + (run.cuesStored === 1 ? " cue" : " cues") + " stored"
                       + (run.artworkFilesAdded > 0
                           ? ", " + run.artworkFilesAdded + " new cover "
                             + (run.artworkFilesAdded === 1 ? "image" : "images")
                           : "") + ".");
            summaryDialog.show({
                written: run.tracksAdded + run.tracksUpdated,
                total: run.tracksSeen,
                unit: "tracks",
                verb: "stored",
                cancelled: run.cancelled,
                detail: lines.join("\n")
            });
        }
    }
}
