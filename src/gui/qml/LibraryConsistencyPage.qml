import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Finds catalog rows whose backing audio file is missing (e.g. left
// behind by Clean Up before it knew to also clean up OneLibrary, see
// LibraryConsistencyController's class comment) and repairs or flags
// them. Scans every present catalog progressively, one after another,
// combining all their results into one list rather than making you
// switch between them one at a time.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController

    // The hub has usually just scanned this library, and a scan reads
    // three catalogs and stats a few thousand files -- minutes on a full
    // stick. When it hands its controller over, this page shows those
    // results instead of asking the user to wait through the same work a
    // second time. Opened directly (no hub), it scans for itself.
    property var sharedController: null
    readonly property var consistencyController: root.sharedController !== null
        && root.sharedController !== undefined ? root.sharedController : ownController

    LibraryConsistencyController {
        id: ownController
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "library-health"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }
    // Backs "Resolve..." on a Conflict row: reuses the exact same manual
    // two-track-merge feature Browse Library's own "Merge with..."
    // picker uses. A Conflict issue already names both tracks (the
    // survivor and the specific broken sibling), so there's no picker
    // step needed here, just planManualMerge() and the same review UI.
    CleanupController {
        id: mergeController
    }

    function formatLabel(format) {
        if (format === "engine") return "Engine OS";
        if (format === "onelibrary") return "OneLibrary";
        return "DeviceLibrary";
    }
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
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

    function rescan() {
        consistencyController.scan(root.rekordboxPath, root.enginePath);
    }

    // Only when there is nothing to show yet: a shared controller arrives
    // already holding this library's results, and re-running the scan
    // would throw them away and make the user wait again.
    Component.onCompleted: {
        if (root.sharedController === null || root.sharedController === undefined) {
            rescan();
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
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Library Health"
                backEnabled: !consistencyController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            RowLayout {
                visible: consistencyController.busy
                spacing: 8
                BusyIndicator { running: true; implicitWidth: 20; implicitHeight: 20 }
                Label {
                    text: consistencyController.scanningFormat.length > 0
                        ? "Scanning " + root.formatLabel(consistencyController.scanningFormat) + "..."
                        : "Scanning..."
                    color: Theme.textMuted
                }
            }
        }
    }

    MessageDialog {
        id: confirmRepairAllDialog
        severity: SeabassDialog.Question
        title: "Stage repairing " + consistencyController.repairableCount + " row(s)?"
        headline: "Merges any cues these rows have onto their already-valid survivor (only where the "
            + "survivor doesn't already have them), then removes the broken row."
        detailText: "Nothing is written until you press Save; everything is backed up first."
        acceptText: "Stage Repairs"
        onAccepted: consistencyController.repairAll()
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
        onAccepted: if (pendingIndex >= 0) consistencyController.repairOne(pendingIndex)
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
        onAccepted: if (pendingIndex >= 0) consistencyController.deleteOrphan(pendingIndex)
    }

    MessageDialog {
        id: confirmRemoveJunkCueDialog
        property int pendingIndex: -1
        severity: SeabassDialog.Question
        title: "Stage removing this cue?"
        headline: "Removes this memory cue sitting at 0:00 from the track."
        detailText: "Backed up first."
        acceptText: "Stage Removal"
        onAccepted: if (pendingIndex >= 0) consistencyController.removeJunkCue(pendingIndex)
    }

    MessageDialog {
        id: confirmRemoveAllJunkCuesDialog
        severity: SeabassDialog.Warning
        destructive: true
        // Not junkCueRepeater.count: that Repeater lives inside the
        // ListView footer, whose ids are scoped to the footer
        // component, so from out here it is a ReferenceError and this
        // title never binds. The model is the shared thing both can see.
        title: "Stage removing all " + consistencyController.junkCues.count + " memory cue(s) at 0:00?"
        headline: "This stages removing every memory cue at 0:00 currently listed, across every catalog on "
            + "this stick -- once you press Save that is a real write, not just dismissing them from view."
        detailText: "Everything is backed up first, but make sure this is really what you want."
        acceptText: "Stage Removal"
        onAccepted: consistencyController.removeAllJunkCues()
    }

    MessageDialog {
        id: confirmIgnoreAllJunkCuesDialog
        severity: SeabassDialog.Question
        title: "Ignore all memory cues at 0:00"
        headline: "Dismisses every memory cue at 0:00 currently listed, just for this view."
        detailText: "Nothing is written, they'll show up again the next time you scan."
        acceptText: "Ignore All"
        onAccepted: consistencyController.ignoreAllJunkCues()
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
        x: (root.width - width) / 2
        y: (root.height - height) / 2
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: consistencyController.errorMessage.length > 0
            text: consistencyController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: consistencyController.statusMessage.length > 0
            text: consistencyController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                text: "I found " + issueListView.count + " row(s) with a missing file, across every catalog on this stick"
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: consistencyController.stagedCount > 0
                text: consistencyController.stagedCount + " staged, not saved yet"
                color: Theme.warnText
            }
            Button {
                text: "Undo Last Save"
                visible: consistencyController.canUndo
                enabled: !consistencyController.busy && !consistencyController.writing
                onClicked: consistencyController.undoLastOperation()
            }
            Button {
                text: "Stage All Safe Repairs"
                enabled: !consistencyController.busy && !consistencyController.writing && consistencyController.repairableCount > 0
                ToolTip.visible: hovered
                ToolTip.text: "Repair every entry with an exact healthy match. Conflicts are left for you."
                onClicked: confirmRepairAllDialog.open()
            }
        }

        // One continuous scroll area for both the missing-file issues and
        // the 0:00-memory-cue rows below, rather than two separately
        // height-capped lists. A real ListView (not a bare ScrollView
        // wrapping a plain ColumnLayout, which was tried here first and
        // produced a scrollbar thumb that rendered stuck near the top-
        // left instead of docked to the right edge) -- the junk-cue
        // section lives in footer: instead, still inside the same
        // Flickable content flow, so it scrolls together with the issue
        // rows above it and shares the one BigScrollBar, the same proven
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
            model: consistencyController.issues
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
                            enabled: !consistencyController.busy && !consistencyController.writing
                            onClicked: consistencyController.unstageIssue(issueDelegate.index)
                        }
                        Button {
                            visible: issueDelegate.kind === "repairable" && !issueDelegate.staged
                            text: "Repair"
                            enabled: !consistencyController.busy && !consistencyController.writing
                            onClicked: {
                                confirmRepairOneDialog.pendingIndex = issueDelegate.index;
                                confirmRepairOneDialog.open();
                            }
                        }
                        Button {
                            visible: issueDelegate.kind === "conflict"
                            text: "Resolve..."
                            enabled: !consistencyController.busy && issueDelegate.format !== "onelibrary"
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
                            enabled: !consistencyController.busy && !consistencyController.writing
                            onClicked: {
                                confirmDeleteOrphanDialog.pendingIndex = issueDelegate.index;
                                confirmDeleteOrphanDialog.open();
                            }
                        }
                        Label {
                            text: issueDelegate.expanded ? "▾" : "▸"
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
                                            text: "▶ Play"
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

            // The 0:00-memory-cue section lives in the footer rather than
            // a second ListView/model, still inside this same Flickable's
            // content flow so it scrolls together with the issue rows
            // above and shares the one BigScrollBar. A memory cue at
            // 0:00 doesn't mean the track's file is missing (this check
            // is entirely independent of the model above), it's almost
            // always an accidental leftover from analysis or import, see
            // domain::JunkCueFinder's own doc comment for why hot cues at
            // 0:00 are deliberately left out of this check.
            footer: ColumnLayout {
                id: junkCueFooter
                width: ListView.view.width
                // Matches issueListView's own spacing bump above, same
                // reasoning.
                spacing: 10

                RowLayout {
                    visible: junkCueRepeater.count > 0
                    Layout.fillWidth: true
                    Layout.topMargin: 12
                    spacing: 12
                    Label {
                        text: "I found " + junkCueRepeater.count + " memory cue(s) sitting at 0:00, likely accidental"
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: "Remove All"
                        enabled: !consistencyController.busy
                        ToolTip.visible: hovered
                        // It stages; it does not remove. The row buttons
                        // beside it and the confirmation this opens both
                        // said so already -- this one promised an
                        // immediate permanent delete, which is the wrong
                        // thing to tell someone in both directions: they
                        // either avoid a reversible action thinking it is
                        // final, or click it and believe the cues are
                        // already gone.
                        ToolTip.text: "Stage removing every 0:00 memory cue listed, in all catalogs. Save writes it."
                        onClicked: confirmRemoveAllJunkCuesDialog.open()
                    }
                    Button {
                        text: "Ignore All"
                        enabled: !consistencyController.busy
                        ToolTip.visible: hovered
                        ToolTip.text: "Hide these from this view only. Nothing on the stick changes."
                        onClicked: confirmIgnoreAllJunkCuesDialog.open()
                    }
                }

                Repeater {
                    id: junkCueRepeater
                    model: consistencyController.junkCues
                    // Was a bare format-badge + title/artist line with no
                    // way to see or hear the cue in question at all --
                    // the same shared track delegate the missing-file
                    // detail view and Sync/Duplicates use elsewhere, so
                    // this row shows a real waveform with the 0:00 memory
                    // cue about to be removed, and a Play button that
                    // simply didn't exist here before.
                    delegate: ColumnLayout {
                        id: junkDelegate
                        Layout.fillWidth: true
                        spacing: 4

                        required property int index
                        required property var track
                        required property bool staged

                        TrackWaveformCard {
                            Layout.fillWidth: true
                            track: junkDelegate.track
                            formatLabelText: root.formatLabel(junkDelegate.track.side)
                            // Highlights the memory cue at 0:00 -- the one
                            // this row is actually about -- and dims every
                            // other cue the track happens to have, so it's
                            // unambiguous which one Remove kills. See
                            // WaveformView's own doc comment on this
                            // property.
                            highlightCuePositionMs: 0
                            actionButtonText: junkDelegate.staged ? "Unstage" : "Remove"
                            actionButtonTooltip: junkDelegate.staged
                                ? "Staged for removal, not on the stick yet: press Save. Click to take it back out."
                                : "Stage removing this memory cue at 0:00 from the track; Save writes it. Backed up first."
                            actionButtonEnabled: !consistencyController.busy && !consistencyController.writing
                            onActionTriggered: {
                                if (junkDelegate.staged) {
                                    consistencyController.unstageJunkCue(junkDelegate.index);
                                } else {
                                    confirmRemoveJunkCueDialog.pendingIndex = junkDelegate.index;
                                    confirmRemoveJunkCueDialog.open();
                                }
                            }
                            playbackController: root.playbackController
                            playbackPath: root.pathForFormat(junkDelegate.track.side)
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            Button {
                                text: "Ignore"
                                enabled: !consistencyController.busy
                                ToolTip.visible: hovered
                                ToolTip.text: "Dismiss this one, just for this view -- nothing on the stick changes"
                                onClicked: consistencyController.ignoreJunkCue(junkDelegate.index)
                            }
                        }
                    }
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 24
                    Layout.bottomMargin: 12
                    visible: issueListView.count === 0 && junkCueRepeater.count === 0 && !consistencyController.busy
                    text: "Every row on this stick has its file, and no memory cues are sitting at 0:00."
                    color: Theme.textMuted
                }
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: consistencyController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: consistencyController.busy
        current: consistencyController.scanCurrent
        total: consistencyController.scanTotal
        label: (consistencyController.scanningFormat.length > 0
                ? "Scanning " + root.formatLabel(consistencyController.scanningFormat) + "..." : "Scanning...")
        cancellable: consistencyController.scanCancellable
        onCancelRequested: consistencyController.cancelScan()
    }
}
