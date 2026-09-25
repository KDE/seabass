// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "Tracks and their files" check: catalog rows whose
// backing audio file is missing (e.g. left behind by Clean Up before it
// knew to also clean up OneLibrary, see LibraryConsistencyController's
// class comment), and what repairs or flags them. Every present catalog's
// rows are in the one list rather than one catalog at a time.
//
// Only this check. The stray cues, the import prompt, sample rates and
// OneLibrary's leftovers each have a page of their own now (see
// HealthCheckPage for why they were split apart); this is the one of
// them that is still called LibraryConsistencyPage, because it is what
// the page was first built for.
HealthCheckPage {
    id: root
    required property var playbackController

    checkTitle: "Tracks and Their Files"

    // Backs "Resolve..." on a Conflict row: reuses the exact same manual
    // two-track-merge feature Browse Library's own "Merge with..."
    // picker uses. A Conflict issue already names both tracks (the
    // survivor and the specific broken sibling), so there's no picker
    // step needed here, just planManualMerge() and the same review UI.
    CleanupController {
        id: mergeController
    }

    // "3 hot cue(s), 1 memory cue(s)" -- same shape as
    // DuplicatesPage.qml's own cueSummary(), duplicated rather than
    // shared since it's ten lines and the two pages' track models come
    // from different controllers.
    function cueSummary(track) {
        var hot = 0, memory = 0;
        var cues = track.cues || [];
        for (var i = 0; i < cues.length; i++) {
            (cues[i].kind === "hot" ? hot++ : memory++);
        }
        var parts = [];
        if (hot > 0) parts.push(hot + " hot cue(s)");
        if (memory > 0) parts.push(memory + " memory cue(s)");
        return parts.length > 0 ? parts.join(", ") : "no cues";
    }

    // Which playlists actually end up short a track, which is not the
    // same question as which playlists the broken row was in.
    //
    // For a missing row, every playlist it was in loses it. For a
    // repairable or conflicting one the kept copy absorbs the row, so a
    // playlist only loses anything where the kept copy is not itself a
    // member. Returns the names, de-duplicated, in the order first seen.
    //
    // Track.playlists is best effort: a reader that does not report
    // memberships gives an empty list, which means "not known" and never
    // "in no playlist". The caller hides the line rather than claiming
    // the second.
    function playlistsLeftShort(kind, brokenTracks, survivor) {
        const survivorNames = {};
        if (kind !== "missing" && survivor && survivor.playlists) {
            for (const membership of survivor.playlists) {
                survivorNames[membership.name] = true;
            }
        }
        const names = [];
        const seen = {};
        for (const track of (brokenTracks || [])) {
            for (const membership of (track.playlists || [])) {
                if (seen[membership.name] || survivorNames[membership.name]) {
                    continue;
                }
                seen[membership.name] = true;
                names.push(membership.name);
            }
        }
        return names;
    }

    // Whether anything at all is known about this issue's memberships,
    // so "no playlist loses a track" can be told apart from "this reader
    // does not report playlists".
    function playlistsKnown(brokenTracks, survivor) {
        if (survivor && survivor.playlists && survivor.playlists.length > 0) {
            return true;
        }
        for (const track of (brokenTracks || [])) {
            if ((track.playlists || []).length > 0) {
                return true;
            }
        }
        return false;
    }

    function playlistSentence(kind, brokenTracks, survivor) {
        const names = root.playlistsLeftShort(kind, brokenTracks, survivor);
        if (names.length === 0) {
            return kind === "missing"
                ? ""
                : "Every playlist this row was in also holds the copy being kept, so no set loses a track.";
        }
        const listed = names.slice(0, 4).join(", ")
            + (names.length > 4 ? " and " + (names.length - 4) + " more" : "");
        const plural = names.length === 1 ? "playlist" : "playlists";
        return kind === "missing"
            ? "Leaves a gap in " + names.length + " " + plural + ": " + listed
            : "The copy being kept is not in " + names.length + " " + plural + " this row was in: " + listed;
    }

    MessageDialog {
        id: confirmRepairAllDialog
        severity: SeabassDialog.Question
        title: "Stage repairing " + consistencyController?.repairableCount + " row(s)?"
        headline: "Merges any cues these rows have onto their already-valid survivor (only where the "
            + "survivor doesn't already have them), then removes the broken row."
        detailText: "Nothing is written until you press Save; everything is backed up first."
        acceptText: "Stage Repairs"
        onAccepted: consistencyController?.repairAll()
    }

    MessageDialog {
        id: confirmRepairOneDialog
        property int pendingIndex: -1
        severity: SeabassDialog.Question
        title: "Stage repairing this row?"
        headline: "Merges any cues this row has onto its already-valid survivor (only where the survivor "
            + "doesn't already have them), then removes the broken row."
        detailText: "Backed up first."
        acceptText: "Stage Repair"
        onAccepted: if (pendingIndex >= 0) consistencyController?.repairOne(pendingIndex)
    }

    MessageDialog {
        id: confirmDeleteOrphanDialog
        property int pendingIndex: -1
        severity: SeabassDialog.Warning
        destructive: true
        title: "Stage deleting this orphaned entry?"
        headline: "No copy of this track was found anywhere else in this catalog. It's really gone."
        detailText: "Backed up first, but there's nothing to restore it from besides re-adding the track "
            + "via Rekordbox or Engine's own software and re-exporting."
        acceptText: "Stage Deletion"
        onAccepted: if (pendingIndex >= 0) consistencyController?.deleteOrphan(pendingIndex)
    }

    // Step 2 of resolving a Conflict row manually: review the plan
    // (survivor, cues to merge, playlist note) before applying, exactly
    // the same DuplicateCleanupPlanner output and apply() path Browse
    // Library's own manual-merge feature uses, just seeded directly with
    // the conflict's own two tracks via planManualMerge() (no picker
    // step needed, both tracks are already known).
    Popup {
        id: conflictResolvePopup
        modal: true
        focus: true
        // Centred in the page body, which sits centred in the page.
        x: Theme.snap((parent.width - width) / 2)
        y: Theme.snap((parent.height - height) / 2)
        width: 520

        property string trackALabel: ""
        property string trackBLabel: ""

        function showFor(format, path, trackA, trackB) {
            conflictResolvePopup.trackALabel = trackA.title + " - " + trackA.artist;
            conflictResolvePopup.trackBLabel = trackB.title + " - " + trackB.artist;
            mergeController.planManualMerge(format, path, trackA.sourceId, trackB.sourceId);
            conflictResolvePopup.open();
        }

        onClosed: root.rescan()

        ColumnLayout {
            width: parent.width
            spacing: 10

            PageTitle {
                text: "Resolve Conflict"
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: "“" + conflictResolvePopup.trackALabel + "” + “" + conflictResolvePopup.trackBLabel + "”"
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
                model: mergeController.statusMessage.length === 0 ? mergeController.plans : null

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
                    onClicked: conflictResolvePopup.close()
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Label {
            objectName: "missingFilesSummary"
            text: issueListView.count === 0
                ? "Every row on this stick has its file."
                : "I found " + issueListView.count + " row(s) with a missing file, across every catalog on this stick"
        }
        Item { Layout.fillWidth: true }
        Label {
            objectName: "stagedIssuesNote"
            // This check's own staged work, not the page's total: the
            // note sat beside the repair buttons and counted staged
            // cue removals too, so removing 29 stray cues put "29
            // staged" next to a button about missing files.
            visible: consistencyController?.stagedIssueCount > 0
            text: consistencyController?.stagedIssueCount + " staged, not saved yet"
            color: Theme.warnText
        }
        Button {
            text: "Stage All Safe Repairs"
            // What it has left to stage, not what the check found:
            // once every safe repair is staged there is nothing
            // behind this button, and it should not look pressable.
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && consistencyController?.unstagedRepairableCount > 0 && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.unstagedRepairableCount === 0 && consistencyController?.repairableCount > 0
                ? "Every safe repair is staged already. Press Save to write them."
                : "Repair every entry with an exact healthy match. Conflicts are left for you."
            onClicked: confirmRepairAllDialog.open()
        }
    }

    // One scroll area for the issue rows. A real ListView (not a bare
    // ScrollView wrapping a plain ColumnLayout, which was tried here first
    // and produced a scrollbar thumb that rendered stuck near the top-left
    // instead of docked to the right edge) -- the same proven
    // ListView+BigScrollBar pairing every other page in this app uses.
    ListView {
        // Room to scroll the last row clear of the Save overlay (bottom right).
        bottomMargin: 80
        id: issueListView
        // Not draggable when everything already fits.
        interactive: contentHeight > height
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: consistencyController?.issues
        // A little more breathing room between findings than the
        // tight 4px this used to be -- each row can expand into a
        // whole track-detail view (waveform, cues), so they read as
        // more distinct "cards" than a plain dense list's rows do.
        spacing: 10

        ScrollBar.vertical: BigScrollBar {}

        delegate: Column {
            id: issueDelegate
            width: ListView.view.width
            spacing: 4

            required property int index
            required property string kind
            required property string format
            required property var survivor
            required property var brokenTracks
            required property bool cueMergeNeeded
            required property bool staged
            required property string stagedDescription

            property bool expanded: false

            readonly property color kindColor: kind === "repairable" ? Theme.good
                : (kind === "conflict" ? Theme.conflictText : Theme.danger)
            readonly property string kindLabel: kind === "repairable" ? "REPAIRABLE"
                : (kind === "conflict" ? "CONFLICT" : "MISSING")
            // Survivor first (when there is one), then every broken
            // copy -- what the expanded detail view below iterates,
            // and how "isSurvivor" (the only one enabled for Play,
            // since the others' files are known missing) is decided.
            readonly property var detailTracks: {
                var list = [];
                if (issueDelegate.survivor && issueDelegate.survivor.sourceId) {
                    list.push(issueDelegate.survivor);
                }
                for (var i = 0; i < issueDelegate.brokenTracks.length; i++) {
                    list.push(issueDelegate.brokenTracks[i]);
                }
                return list;
            }

            ItemDelegate {
            width: parent.width
            hoverEnabled: true
            onClicked: issueDelegate.expanded = !issueDelegate.expanded

            contentItem: ColumnLayout {
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    // Fixed-width slot around the badge itself (which
                    // stays its own natural, compact pill size) rather
                    // than resizing StatusBadge -- "REPAIRABLE" is
                    // noticeably wider than "MISSING", so without this
                    // every row's format-badge/title after it started
                    // at a different X depending on which kind the row
                    // was, reading as misaligned down the list.
                    Item {
                        Layout.preferredWidth: 96
                        Layout.preferredHeight: kindBadge.implicitHeight
                        StatusBadge {
                            id: kindBadge
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            label: issueDelegate.kindLabel
                            badgeColor: issueDelegate.kindColor
                            tooltipText: {
                            if (issueDelegate.kind === "repairable") {
                                var t = "Matches existing copy \"" + issueDelegate.survivor.title + " - "
                                    + issueDelegate.survivor.artist + "\".";
                                if (issueDelegate.cueMergeNeeded) {
                                    t += " Its cues will be merged onto that copy first.";
                                }
                                return t;
                            }
                            if (issueDelegate.kind === "conflict") {
                                var brokenSummary = issueDelegate.brokenTracks.length > 0
                                    ? root.cueSummary(issueDelegate.brokenTracks[0]) : "no cues";
                                return "Matches existing copy \"" + issueDelegate.survivor.title + " - "
                                    + issueDelegate.survivor.artist + "\", but they have genuinely different cues: "
                                    + "kept copy has " + root.cueSummary(issueDelegate.survivor) + "; broken row had "
                                    + brokenSummary + ". Not auto-repaired: use Resolve above to review and merge "
                                    + "them manually.";
                            }
                            return issueDelegate.format === "onelibrary"
                                ? "No copy found anywhere in OneLibrary. Re-add via Rekordbox or Engine's own "
                                  + "software, or delete this orphaned entry."
                                : "No copy found anywhere in this catalog. Re-add the track via "
                                  + (issueDelegate.format === "engine" ? "Engine" : "Rekordbox") + "'s own software.";
                            }
                        }
                    }
                    // Plain text, no logo -- same "original mark, not
                    // a reproduction" convention this app already
                    // applies to every other catalog badge/glyph.
                    Rectangle {
                        radius: 3
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        implicitWidth: formatLabelText.implicitWidth + 8
                        implicitHeight: formatLabelText.implicitHeight + 4
                        Label {
                            id: formatLabelText
                            anchors.centerIn: parent
                            text: root.formatLabel(issueDelegate.format)
                            font.pointSize: Theme.fontTiny
                            font.bold: true
                            color: Theme.textMuted
                        }
                    }
                    Label {
                        // One issue can fold in several broken copies
                        // of the very same song (see
                        // domain::LibraryConsistencyChecker::check()'s
                        // own brokenGroup, one per DuplicateGroup, not
                        // one per row) -- joining every brokenTracks
                        // name here would repeat the identical title
                        // once per copy. Show it once: the survivor's
                        // identity when there is one (that's the copy
                        // that's staying), otherwise the first broken
                        // copy's, with a count appended whenever more
                        // than one broken row shares this issue.
                        text: {
                            var rep = (issueDelegate.survivor && issueDelegate.survivor.sourceId)
                                ? issueDelegate.survivor : issueDelegate.brokenTracks[0];
                            var label = rep.title + " - " + rep.artist;
                            if (issueDelegate.brokenTracks.length > 1) {
                                label += " (" + issueDelegate.brokenTracks.length + " broken copies)";
                            }
                            return label;
                        }
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    StatusBadge {
                        visible: issueDelegate.staged
                        label: "Staged"
                        badgeColor: Theme.warnText
                        tooltipText: issueDelegate.stagedDescription + "\n\nNot on the stick yet: press Save."
                    }
                    Button {
                        visible: issueDelegate.staged
                        text: "Unstage"
                        enabled: !consistencyController?.busy && !consistencyController?.writing
                        onClicked: consistencyController?.unstageIssue(issueDelegate.index)
                    }
                    Button {
                        visible: issueDelegate.kind === "repairable" && !issueDelegate.staged
                        text: "Repair"
                        enabled: !consistencyController?.busy && !consistencyController?.writing
                        onClicked: {
                            confirmRepairOneDialog.pendingIndex = issueDelegate.index;
                            confirmRepairOneDialog.open();
                        }
                    }
                    Button {
                        visible: issueDelegate.kind === "conflict"
                        text: "Resolve..."
                        enabled: !consistencyController?.busy && issueDelegate.format !== "onelibrary"
                        ToolTip.visible: hovered
                        ToolTip.text: issueDelegate.format === "onelibrary"
                            ? "Manual merging isn't supported on OneLibrary yet"
                            : "Review and merge these two tracks manually"
                        onClicked: conflictResolvePopup.showFor(issueDelegate.format,
                            root.pathForFormat(issueDelegate.format), issueDelegate.survivor,
                            issueDelegate.brokenTracks[0])
                    }
                    Button {
                        visible: issueDelegate.kind === "missing" && issueDelegate.format === "onelibrary" && !issueDelegate.staged
                        text: "Delete Orphaned Entry"
                        enabled: !consistencyController?.busy && !consistencyController?.writing
                        onClicked: {
                            confirmDeleteOrphanDialog.pendingIndex = issueDelegate.index;
                            confirmDeleteOrphanDialog.open();
                        }
                    }
                    SeabassIcon {
                        iconName: issueDelegate.expanded ? "arrow-down" : "arrow-right"
                        size: Theme.iconSizeSmall * 0.75
                        color: Theme.textMuted
                    }
                }
            }
            }

            // Same detail idiom Sync Cue Points uses for its own
            // matched pairs: one frame per copy, a waveform with cue
            // markers (falling back to a plain text summary via
            // CueFallbackNotice when duration is unknown, e.g. the
            // "In My Head" survivor whose duration failed to read --
            // see domain::LibraryConsistencyChecker's own duration-0
            // handling), and a Play button. Only the survivor is ever
            // playable here -- every other copy's file is, by
            // definition, the reason this row exists.
            Rectangle {
                width: parent.width
                visible: issueDelegate.expanded
                height: issueDelegate.expanded ? detailColumn.implicitHeight + 16 : 0
                color: Theme.groupBackground
                border.color: Theme.borderSubtle
                radius: 4

                ColumnLayout {
                    id: detailColumn
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    // Where the hole is. "This track's file is gone"
                    // is not the question a DJ has in front of a
                    // deck; "which set am I about to play with a gap
                    // in it" is, and the memberships were already
                    // being read off this very track list to build
                    // the playlist picker.
                    //
                    // Hidden entirely when nothing is known, which is
                    // an ordinary answer: Track::playlists is
                    // populated where the reader supports it, and an
                    // empty list must never be shown as "in no
                    // playlist".
                    Label {
                        objectName: "playlistImpactLabel"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.playlistsKnown(issueDelegate.brokenTracks, issueDelegate.survivor)
                            ? root.playlistSentence(issueDelegate.kind, issueDelegate.brokenTracks,
                                                    issueDelegate.survivor)
                            : ""
                        wrapMode: Text.WordWrap
                        font.pointSize: Theme.fontSmall
                        color: issueDelegate.kind === "missing" ? Theme.warnText : Theme.textMuted
                    }

                    Repeater {
                        model: issueDelegate.detailTracks
                        delegate: Frame {
                            id: trackFrame
                            Layout.fillWidth: true
                            required property var modelData
                            required property int index
                            readonly property bool isSurvivor: index === 0
                                && issueDelegate.survivor && issueDelegate.survivor.sourceId === modelData.sourceId

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 4
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    // Same 40x40 thumbnail convention Browse Library
                                    // uses for its own track rows (see ScanPage.qml).
                                    Rectangle {
                                        Layout.preferredWidth: Theme.iconSizeNormal
                                        Layout.preferredHeight: Theme.iconSizeNormal
                                        color: Theme.surface
                                        Image {
                                            anchors.fill: parent
                                            visible: trackFrame.modelData.artworkPath.length > 0
                                            source: trackFrame.modelData.artworkPath
                                            fillMode: Image.PreserveAspectCrop
                                        }
                                    }
                                    Label {
                                        text: (trackFrame.isSurvivor ? "Kept copy: " : "Broken copy: ")
                                            + trackFrame.modelData.title + " - " + trackFrame.modelData.artist
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Button {
                                        text: "Play"
                                        icon.source: Theme.iconUrl("media-playback-start")
                                        icon.color: enabled ? Theme.text : Theme.textMuted
                                        enabled: trackFrame.isSurvivor && trackFrame.modelData.filePath.length > 0
                                        ToolTip.visible: hovered
                                        ToolTip.text: trackFrame.isSurvivor
                                            ? "Play this copy of the track"
                                            : "No local file: this copy's file is missing"
                                        onClicked: root.playbackController.load(issueDelegate.format,
                                            root.pathForFormat(issueDelegate.format), trackFrame.modelData.sourceId,
                                            trackFrame.modelData.filePath, trackFrame.modelData.title,
                                            trackFrame.modelData.artist, trackFrame.modelData.artworkPath,
                                            trackFrame.modelData.cues)
                                    }
                                }
                                WaveformView {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 40
                                    // Best-effort, on demand -- same PlaybackController
                                    // read Play already uses, only actually returns
                                    // data when this format's own waveform-overview
                                    // blob exists for this specific track (empty for
                                    // a track Engine/rekordbox never analyzed, not a
                                    // bug, see the class's own read-only contract).
                                    waveformData: root.playbackController.waveformFor(issueDelegate.format,
                                        root.pathForFormat(issueDelegate.format), trackFrame.modelData.sourceId)
                                    format: issueDelegate.format
                                    cueData: modelData.cues
                                    trackDurationMs: modelData.durationMs
                                }
                                CueFallbackNotice {
                                    cues: modelData.cues
                                    durationMs: modelData.durationMs
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
