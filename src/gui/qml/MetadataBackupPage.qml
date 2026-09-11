import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Metadata Backup: read what the DJ added to this stick's tracks into a
// database on this computer, and browse what is in there.
//
// The two halves sit on one page on purpose. "Back up" and "look at what
// I have backed up" are the same question asked before and after, and a
// browse view on its own page would be a place nobody visits until
// something has already gone wrong.
//
// Nothing here writes to the stick. Putting metadata back is a different
// page, linked from the line at the top, because it is a different
// decision. The one destructive thing this page can do is forget an
// entry, and that is staged and confirmed rather than done on a click.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property string libraryId

    readonly property bool hasStick: root.rekordboxPath.length > 0 || root.enginePath.length > 0
    // Any catalog directory will do: the controller reads every catalog
    // on the stick that one belongs to.
    readonly property string libraryPath: root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath

    // The row whose detail is showing, or -1. One at a time: this is a
    // list to scan, not a tree to keep open.
    property var expandedTrackId: -1

    signal metadataRestoreRequested()

    function formatBytes(bytes) {
        if (bytes <= 0) return "0 MB";
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + " KB";
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GB";
    }

    MetadataBackupController {
        id: controller
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
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
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
            onLinkActivated: root.metadataRestoreRequested()
            // A link that does not say it is one is a link nobody
            // clicks.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }

        // ---- run a backup -------------------------------------------
        //
        // Not in a card. A bordered box indents everything inside it by
        // its own padding, which is how this page ended up with four
        // different left edges at once; there is one block above the
        // list, so a border separating it from nothing costs the
        // alignment and buys nothing.
        ColumnLayout {
            id: runLayout
            Layout.fillWidth: true
            spacing: Theme.tightSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Label {
                    text: root.hasStick ? "Back up " + root.stickLabel : "No stick selected"
                    color: Theme.text
                    font.family: Theme.titleFamily
                    font.weight: Theme.cardTitleWeight
                    font.pointSize: Theme.fontMedium
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "backUpNowButton"
                    text: "Add"
                    enabled: root.hasStick && !controller.busy
                    highlighted: true
                    onClicked: controller.backUp(root.libraryPath, root.libraryId, root.stickLabel)
                    ToolTip.visible: hovered
                    ToolTip.delay: 400
                    ToolTip.text: root.hasStick
                        ? "Read " + root.stickLabel + " and add what it holds to the backup"
                        : "Choose a stick first"
                }
                InfoButton {
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

            Label {
                Layout.fillWidth: true
                visible: !controller.busy
                text: "Nothing on the stick is changed or at risk. This only ever writes here: "
                    + controller.storeLocation
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                wrapMode: Text.WordWrap
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

        // ---- what is in the store -----------------------------------
        MetadataListToolbar {
            objectName: "browseToolbar"
            Layout.fillWidth: true
            selectionEnabled: controller.loadedCount > 0 && !controller.busy
            selectAllTooltip: controller.canLoadMore
                ? "Mark every track loaded so far for deletion. Scroll to the end of the list to load the rest."
                : "Mark every track in the list for deletion from the Metadata Backup"
            summary: controller.storedTrackCount === 0
                ? "Nothing stored yet"
                : (controller.stagedForDeletionCount > 0
                    ? controller.stagedForDeletionCount + " of " + controller.matchCount + " staged"
                    : (controller.matchCount === controller.storedTrackCount
                        ? controller.storedTrackCount + " tracks stored, "
                          + root.formatBytes(controller.artworkBytes) + " of cover art"
                        : controller.matchCount + " of " + controller.storedTrackCount + " tracks"))
            onSearchChanged: text => controller.search(text)
            onSelectAllRequested: controller.stageAllForDeletion()
            onSelectNoneRequested: controller.clearDeletionStaging()
        }

        ListView {
            id: trackList
            objectName: "storedTrackList"
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
                    ? "Nothing has been backed up yet. Add reads this stick and stores what you added to it."
                    : "No stored track matches that search."
                color: Theme.textMuted
            }
        }

        // ---- the one destructive action -----------------------------
        //
        // Under the list rather than floating over it: this page has no
        // edit session and writes nothing to the stick, so it must not
        // borrow the floating Save button's shape, which everywhere else
        // in Seabass means "write my edits to the stick".
        RowLayout {
            Layout.fillWidth: true
            visible: controller.stagedForDeletionCount > 0
            spacing: Theme.rowSpacing

            Label {
                Layout.fillWidth: true
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                wrapMode: Text.WordWrap
                text: controller.stagedForDeletionCount + " marked for deletion. Nothing has gone yet."
            }
            Button {
                objectName: "deleteStagedButton"
                text: "Delete"
                onClicked: confirmDeleteDialog.open()
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: controller.allLoadedStaged
                    ? "Delete every track in the list from the Metadata Backup on this computer"
                    : "Delete the marked tracks from the Metadata Backup on this computer"
            }
        }
    }

    MessagePopup { id: messagePopup }

    // A backup is the one copy of these cues that may still exist, so
    // forgetting one is confirmed rather than clicked.
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
        acceptText: "Delete"
        rejectText: "Cancel"
        onAccepted: {
            var removed = controller.deleteStaged();
            messagePopup.show(removed === 1 ? "One track removed from the metadata backup."
                                            : removed + " tracks removed from the metadata backup.",
                              false);
        }
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
            lines.push("Read from: " + run.catalogsRead.join(", ") + ".");
            if (run.catalogsUnreadable.length > 0) {
                lines.push("Could not read: " + run.catalogsUnreadable.join(", ")
                           + ". Anything only those held was not backed up.");
            }
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
