import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Restore Metadata: put the cues from this computer's metadata store
// back on a stick that has lost them.
//
// The counterpart to Metadata Backup, and deliberately its own page.
// Reading a stick into the store risks nothing; writing to the stick is
// a save like every other one in Seabass, so nothing here reaches the
// stick until Restore. Each accepted track is staged into the library's
// edit session, and the save backs up before it touches a file.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property string libraryId

    readonly property bool hasStick: root.rekordboxPath.length > 0 || root.enginePath.length > 0
    readonly property string libraryPath: root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath

    // The row whose detail is showing, or "". One at a time.
    //
    // Keyed on the store's row id rather than the filename. A filename
    // is not unique -- two folders on a rebuilt stick routinely hold the
    // same basename, and those rows opened and closed together -- and
    // the empty starting value matched any proposal whose stick track
    // had no filename at all, rendering it expanded before anyone
    // touched it.
    property string expandedStoredId: ""

    MetadataRestoreController {
        id: controller
    }

    EditSessionHost {
        id: editHost
        feature: "metadata-restore"
        anchors.fill: parent
        libraryId: root.libraryId
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
        // The one button that writes to the stick says what it does.
        // "Save" is right on a page where you have been editing; here
        // the whole page is one verb, and it is this one.
        saveLabel: "Restore"
    }

    MessagePopup { id: messagePopup }
    Connections {
        target: controller
        function onActionFeedback(message, isError) {
            messagePopup.show(message, isError);
        }
    }

    Component.onCompleted: if (root.hasStick) controller.scan(root.libraryPath)

    header: ToolBar {
        // Opaque background override: KDE's Breeze style bleeds the
        // window behind Seabass through an unstyled ToolBar.
        background: Rectangle { color: Theme.surface }
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Restore Metadata"
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
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

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                objectName: "pageIntro"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pointSize: Theme.fontNormal
                text: "Restore metadata from your local backup to the USB stick " + root.stickLabel + ". "
                    + "Nothing is written until you press Restore."
            }
            InfoButton {
                explanationTitle: "Putting stored metadata back"
                summaryText: "What this computer has backed up, offered to the tracks on "
                    + root.stickLabel + " that would gain something from it."
                explanationText: "## How a track is recognised\n\n"
                    + "On its title, artist and length, the same rule Seabass uses to match tracks "
                    + "between rekordbox and Engine everywhere else, falling back to the filename when "
                    + "a catalog has no title and artist to offer.\n\n"
                    + "Deliberately not on where the file sits. That is what makes a restore flexible: "
                    + "a backup taken from one stick can go back onto a rebuilt one, onto a stick whose "
                    + "folders have been reorganised, or onto a fresh copy of a track bought again, "
                    + "because title and artist travel with the recording and a path does not.\n\n"
                    + "Length is a guard rather than part of the key: it has to agree within a couple of "
                    + "seconds, and a track whose length could not be read is not held against it. What "
                    + "it catches is a radio edit and an extended mix filed under one name.\n\n"
                    + controller.mergeRuleHelp
                    + "\n\n## Nothing is written until you press Restore\n\n"
                    + "Staged tracks are held until then, and the save backs up every file it is about "
                    + "to change before it changes it."
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.tightSpacing

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                text: {
                    if (controller.busy) {
                        return "Reading this stick and matching it against the store...";
                    }
                    if (!controller.hasScanned) {
                        return root.hasStick ? "Not checked yet." : "No stick selected.";
                    }
                    if (controller.storedTrackCount === 0) {
                        return "The metadata store is empty. Run a Metadata Backup on a stick that still "
                             + "has your cues, and they can be put back here afterwards.";
                    }
                    var line = "Matched " + controller.stickTrackCount + " tracks on the stick against "
                             + controller.storedTrackCount + " in the store.";
                    if (controller.conflictsLeftAlone > 0) {
                        line += " " + controller.conflictsLeftAlone
                             + (controller.conflictsLeftAlone === 1
                                 ? " track has cues of its own that the stored copy did not beat; it is"
                                 : " tracks have cues of their own that the stored copy did not beat; they are")
                             + " left alone.";
                    }
                    return line;
                }
            }

            ProgressReport {
                Layout.fillWidth: true
                Layout.topMargin: Theme.tightSpacing
                visible: controller.busy
                phase: controller.currentPhase
                unitsDone: controller.progressCurrent
                unitsTotal: controller.progressTotal
                unitName: "tracks"
                cancellable: true
                onCancelRequested: controller.cancelScan()
            }
        }

        SelectableText {
            objectName: "errorLabel"
            Layout.fillWidth: true
            visible: controller.errorMessage.length > 0
            text: controller.errorMessage
            color: Theme.danger
        }

        // Said before the save, not discovered in the log afterwards.
        // A page that silently declines to write a field teaches the DJ
        // that the field is unreliable; one that says which format
        // cannot hold it teaches them something true about their own
        // library.
        Label {
            Layout.fillWidth: true
            visible: controller.commentsRekordboxCannotTake > 0
            wrapMode: Text.WordWrap
            color: Theme.warnText
            font.pointSize: Theme.fontSmall
            text: {
                var n = controller.commentsRekordboxCannotTake;
                return (n === 1 ? "One track's comment cannot be put back: it is"
                                : n + " tracks' comments cannot be put back: they are")
                     + " catalogued only in DeviceLibrary, which stores a comment in a fixed space "
                     + "decided when the stick was exported and cannot make room for a new one. "
                     + "Their cues and ratings still go back. Engine and Device Library Plus take "
                     + "comments of any length.";
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.borderSubtle
        }

        // Below the explanation and above the list, the same shape the
        // backup page's list has. A proposal list runs to hundreds of
        // rows on a stick that has lost its cues, which is exactly the
        // case this page exists for, so it needs filtering as much as
        // the browse list does.
        MetadataListToolbar {
            objectName: "proposalToolbar"
            Layout.fillWidth: true
            placeholder: "Search title, artist or filename"
            selectionEnabled: proposalList.count > 0 && !controller.busy && !editHost.writing
            selectAllTooltip: "Stage every track on the list for restoring"
            summary: controller.stagedCount > 0
                ? controller.stagedCount + " of " + controller.proposalCount + " staged"
                : controller.proposalCount + (controller.proposalCount === 1 ? " track" : " tracks")
                  + " to restore"
            onSearchChanged: text => controller.search(text)
            onSelectAllRequested: controller.stageAll()
            onSelectNoneRequested: controller.unstageAll()
        }

        ListView {
            id: proposalList
            objectName: "proposalList"
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
                required property int cuesAdded
                required property bool fillsAGap
                required property bool conflict
                required property bool cuesOffered
                required property string cueSummary
                required property bool staged
                required property string storedId

                // And the ones it does, marked required here so the
                // model fills them.
                required index
                required storedFrom
                required title
                required artist
                required filename
                required relativePath
                required durationText
                required rating
                required comment
                required cueCount

                // Ticking a row IS staging it. There is no second
                // selection to keep in step with this one, and so no way
                // for the two to disagree.
                selected: proposalRow.staged
                cueTooltip: proposalRow.cueSummary
                expanded: root.expandedStoredId === proposalRow.storedId
                // What this row's badge is counting is not what is on
                // the track but what a restore would leave on it, and
                // the two are different numbers whenever it replaces
                // rather than fills.
                cueBadgeLabel: proposalRow.conflict
                    ? "replaces " + (proposalRow.cueCount - proposalRow.cuesAdded)
                    : proposalRow.cuesAdded + (proposalRow.cuesAdded === 1 ? " cue" : " cues")
                cueBadgeColor: proposalRow.conflict ? Theme.warnIcon : Theme.good
                // Only when the cues are actually on offer. A conflict
                // the stored copy lost still reaches this list whenever
                // the rating or comment is offered, and the row used to
                // promise a cue replacement that was never going to
                // happen -- with no badge beside it to contradict the
                // claim, because an unoffered cue set has no count.
                detailNote: proposalRow.conflict && proposalRow.cuesOffered
                    ? "This track has cues of its own. Restoring replaces them with the stored ones."
                    : (proposalRow.conflict
                        ? "This track's own cues are staying: the stored ones did not beat them."
                        : (proposalRow.fillsAGap ? "This track has no cues on the stick at all." : ""))

                onSelectionToggled: stage => stage ? controller.stage(proposalRow.index)
                                                   : controller.unstage(proposalRow.index)
                onExpandToggled: root.expandedStoredId =
                    proposalRow.expanded ? "" : proposalRow.storedId

                actionItems: [
                    Button {
                        objectName: "stageButton"
                        text: proposalRow.staged ? "Staged" : "Restore"
                        enabled: !editHost.writing
                        onClicked: proposalRow.staged ? controller.unstage(proposalRow.index)
                                                      : controller.stage(proposalRow.index)
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: proposalRow.staged
                            ? "Staged. Press again to take it back off the list."
                            : "Stage this track's metadata for restoring"
                    }
                ]
            }

            Label {
                anchors.centerIn: parent
                width: parent.width * 0.7
                visible: proposalList.count === 0 && !controller.busy && controller.hasScanned
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: controller.storedTrackCount === 0
                    ? "Nothing is stored yet, so there is nothing to put back."
                    : "Every track on this stick already has everything the store holds for it."
            }
        }

    }
}
