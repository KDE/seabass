// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

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
    // "" when opened from Home; "Metadata Backup" when opened from that
    // page's own restore link, which puts it one level further down.
    property string hubLabel: ""
    required property string rekordboxPath
    required property string enginePath
    required property string libraryId

    // Both optional, so the QML tests can build the page without the app
    // behind it. Without a player there are no waveforms to read, and
    // every opened row shows the placeholder; without settings the
    // playlist picker simply does not remember.
    property var playbackController: null
    property var appSettingsController: null

    readonly property bool hasStick: root.rekordboxPath.length > 0 || root.enginePath.length > 0
    readonly property string libraryPath: root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath

    // The row whose detail is showing, or "". One at a time.
    //
    // Keyed on the store's row id AND the stick file, not either alone. A
    // filename is not unique -- two folders on a rebuilt stick routinely
    // hold the same basename, and those rows opened and closed together --
    // and neither is the stored id: one stored track can match several
    // tracks on the stick, and keyed on it alone every copy opened at
    // once. The key is never empty, so nothing starts out expanded.
    property string expandedKey: ""

    // Readable from outside the page, for the live screenshot case: it
    // waits for the scan to finish rather than for proposals to appear,
    // because how many a real stick offers is data and not a property of
    // the page.
    readonly property alias controller: controller

    MetadataRestoreController {
        id: controller
    }

    // ---- the stick picker's model --------------------------------------
    //
    // Index 0 is every stick, and each stick the proposals were backed up
    // from follows. A label two sticks share says how many tracks each
    // offers, which is the difference a person can actually see.
    readonly property var sourceModel: {
        const list = [{ name: "Every stick in the backup", key: "" }];
        const sticks = controller.sourceSticks;
        const seen = {};
        for (let i = 0; i < sticks.length; i++) {
            seen[sticks[i].label] = (seen[sticks[i].label] || 0) + 1;
        }
        for (let i = 0; i < sticks.length; i++) {
            const stick = sticks[i];
            let name = stick.label.length > 0 ? "USB Stick " + stick.label : "A stick with no name";
            if (seen[stick.label] > 1) {
                name += " (" + stick.count + (stick.count === 1 ? " track)" : " tracks)");
            }
            list.push({ name: name, key: stick.key });
        }
        return list;
    }

    readonly property int currentSourceIndex: {
        for (let i = 1; i < root.sourceModel.length; i++) {
            if (root.sourceModel[i].key === controller.selectedSourceKey) {
                return i;
            }
        }
        return 0;
    }

    // The page opens on the playlist last picked on any page with a
    // picker, when these proposals have it: the same rule the backup page
    // follows, so working through a library playlist by playlist carries
    // from backing up to putting back.
    property bool rememberedPlaylistApplied: false
    function applyRememberedPlaylist() {
        if (root.rememberedPlaylistApplied || !controller.hasScanned || !root.appSettingsController) {
            return;
        }
        root.rememberedPlaylistApplied = true;
        const wanted = root.appSettingsController.lastPlaylistName;
        if (wanted.length > 0 && controller.playlistNames.indexOf(wanted) >= 0) {
            controller.setPlaylist(wanted);
        }
    }
    Connections {
        target: controller
        function onAnalysisChanged() { root.applyRememberedPlaylist(); }
    }

    EditSessionHost {
        id: editHost

        // Cancel on the low-space question leaves, as Back does -- see

        // EditSessionHost's backupLocationDeclined for why it must.

        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
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
                stickLabel: root.stickLabel
                middleLabel: root.hubLabel
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

        Label {
            objectName: "pageIntro"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pointSize: Theme.fontNormal
            text: "Restore metadata from your local backup to the USB stick " + root.stickLabel + ". "
                + "Nothing is written until you press Restore."
        }

        // ---- what the restore is for ----------------------------------
        //
        // The backup page's two pickers, in the same order and the same
        // place: which stick's backup, and which playlist. Here they bound
        // the restore itself, not only the list below: Select All stages
        // what they include, and narrowing them unstages what falls
        // outside. So "restore this one playlist" is what the save does.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            Label {
                text: "Restore metadata from:"
                color: Theme.textMuted
                font.pointSize: Theme.fontNormal
            }

            ComboBox {
                id: sourcePicker
                objectName: "sourcePicker"
                Layout.preferredWidth: Theme.snap(Math.max(180, Math.min(300, root.width * 0.3)))
                enabled: !controller.busy && !editHost.writing && controller.hasScanned
                model: root.sourceModel
                textRole: "name"
                // Derived from the controller, and put back after every
                // pick, for the reason the backup page's picker gives: a
                // combo holding its own index can show a scope the list is
                // not actually in.
                currentIndex: root.currentSourceIndex
                onActivated: index => {
                    const entry = root.sourceModel[index];
                    if (entry) {
                        controller.setSourceStick(entry.key);
                    }
                    sourcePicker.currentIndex = Qt.binding(() => root.currentSourceIndex);
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "Restore only what was backed up from one stick. The backup remembers the "
                    + "stick each track was last backed up from."
            }

            Label {
                text: "Playlist:"
                color: Theme.textMuted
                font.pointSize: Theme.fontNormal
            }

            PlaylistPickerCombo {
                objectName: "playlistPicker"
                Layout.preferredWidth: Theme.snap(Math.max(160, Math.min(260, root.width * 0.26)))
                enabled: !controller.busy && !editHost.writing && controller.hasScanned
                model: {
                    const list = [{ name: "All tracks", count: controller.sourceProposalCount }];
                    for (let i = 0; i < controller.playlistNames.length; i++) {
                        const name = controller.playlistNames[i];
                        list.push({ name: name, count: controller.playlistTrackCounts[name] });
                    }
                    return list;
                }
                currentIndex: {
                    if (controller.selectedPlaylist.length === 0) {
                        return 0;
                    }
                    const found = controller.playlistNames.indexOf(controller.selectedPlaylist);
                    return found >= 0 ? found + 1 : 0;
                }
                onPlaylistPicked: (index, modelData) => {
                    const name = index === 0 ? "" : modelData.name;
                    controller.setPlaylist(name);
                    if (root.appSettingsController) {
                        root.appSettingsController.lastPlaylistName = name;
                    }
                }
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: "Restore one playlist instead of the whole stick. A track counts as in it when "
                    + "the backup or this stick lists it there."
            }

            Item { Layout.fillWidth: true }

            InfoButton {
                objectName: "restoreInfoButton"
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
                objectName: "matchSummary"
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
                    let line = "Matched " + controller.stickTrackCount + " tracks on the stick against "
                             + controller.storedTrackCount + " in the store.";
                    // The match is of the whole stick and the whole store;
                    // the tracks left alone are counted over the pickers'
                    // selection, like everything the page says about the
                    // restore. So once a picker narrows, the sentence says
                    // which of the two it is counting.
                    if (controller.conflictsLeftAlone > 0) {
                        const narrowed = controller.selectedSourceKey.length > 0
                            || controller.selectedPlaylist.length > 0;
                        line += (narrowed ? " In this selection, " : " ") + controller.conflictsLeftAlone
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
                const n = controller.commentsRekordboxCannotTake;
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
            // Everything the stick and playlist pickers include, and
            // nothing they leave out: the pickers say what this restore
            // is for. The search does not narrow it (see stageAll()).
            selectAllTooltip: "Stage every track the stick and playlist pickers include, "
                + "including any the search is hiding"
            // Counted over the pickers' selection, which is what a
            // restore covers: "3 of 1,200 staged" on a page narrowed to a
            // playlist of forty would be a number about something else.
            summary: {
                const scoped = controller.scopedProposalCount;
                if (controller.stagedCount > 0) {
                    return controller.stagedCount + " of " + scoped + " staged";
                }
                if (controller.visibleProposalCount !== scoped) {
                    return controller.visibleProposalCount + " of " + scoped + " shown";
                }
                return scoped + (scoped === 1 ? " track" : " tracks") + " to restore";
            }
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
                required property string restoreSummary
                required property var cues
                required property real durationMs

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
                required artworkUrl
                required playlistNames

                // Ticking a row IS staging it. There is no second
                // selection to keep in step with this one, and so no way
                // for the two to disagree.
                selected: proposalRow.staged
                cueTooltip: proposalRow.cueSummary
                // The badge is the "replaces 4" a person hovers to ask what
                // exactly would happen: what goes to this track, where, and
                // which other tracks on the stick get it too.
                cueBadgeTooltip: proposalRow.restoreSummary
                    + (proposalRow.cueSummary.length > 0 ? "\n\n" + proposalRow.cueSummary : "")
                // And on the title, for the rows with no cue badge: a restore
                // of a rating or a comment alone.
                titleTooltip: proposalRow.restoreSummary
                expanded: root.expandedKey === proposalRow.storedId + "\n" + proposalRow.relativePath
                // The cues this restore would leave on the track, on the
                // stick's own waveform where it has one. Read only for the
                // open row: a collapsed one never asks.
                showWaveform: true
                waveformCues: proposalRow.cues
                waveformDurationMs: proposalRow.durationMs
                waveformData: proposalRow.expanded
                    ? proposalRow.stickWaveform(root.playbackController,
                                                controller.waveformSourceAt(proposalRow.index))
                    : []
                // The waveform is the stick's, so a missing one is too.
                waveformMissingText: "No waveform on the stick for this track"
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
                onExpandToggled: root.expandedKey =
                    proposalRow.expanded ? "" : proposalRow.storedId + "\n" + proposalRow.relativePath

            }

            Label {
                anchors.centerIn: parent
                width: Theme.snap(parent.width * 0.7)
                visible: proposalList.count === 0 && !controller.busy && controller.hasScanned
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: {
                    if (controller.storedTrackCount === 0) {
                        return "Nothing is stored yet, so there is nothing to put back.";
                    }
                    if (controller.proposalCount === 0) {
                        return "Every track on this stick already has everything the store holds for it.";
                    }
                    return controller.scopedProposalCount === 0
                        ? "Nothing from this stick or playlist is left to put back."
                        : "No track to restore matches that search.";
                }
            }
        }

    }
}
