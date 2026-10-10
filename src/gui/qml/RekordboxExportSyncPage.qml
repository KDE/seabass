// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import SeabassGui

// Sync after Rekordbox Export (docs/sync-after-rekordbox-export-plan.md,
// "The page"): what rekordbox changed since this stick was last saved, as
// a proposal for the Engine library, then one save.
//
// Top to bottom: what the proposal was compared against and what the
// three directions mean (folded away while the list scrolls, as Clean Up
// Duplicates' header is), the counts and the controls that act on the
// list, then the list, one section per kind of change, the conflicts
// first: they are the only rows that need an answer before staging. A
// section heads its rows with a section-wide checkbox; a row has its own,
// its direction, what it is and why, and opens on a click to say exactly
// what the save does with it (the model's details). A conflict has no
// checkbox: it is answered with one of its two buttons, or all of them at
// once from the section's header, and the answer's edits appear as
// selected rows of their own sections. Engine's own and the refused adds
// are listed and never written.
//
// Selecting is not staging. "Stage Selected" hands the selected rows to
// the controller (stageSelected), which stages them as one batch with the
// record of this save last; the floating "Sync Engine" writes that batch.
// The host's Save only shows once something is staged, so the page cannot
// stage on its press; staging again replaces the batch. There is no
// per-row Unstage: the record is computed from the whole selection
// (see RekordboxExportSyncController), so only all of it can go back.
//
// The list can hold a few thousand rows, so a row is one light delegate
// for every kind, built only while it is in view.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    property var appSettingsController: null
    // For a cue row's waveforms (PlaybackController::waveformFor), read
    // only for a row someone opens. Without one the strips draw the cues
    // on a flat line.
    property var playbackController: null

    // Overridable so a test can hand in a fake; the app never sets it.
    property var controller: realController

    // Only cues differ: Sync Cue Points shows them with waveforms. Main.qml
    // pushes it; this page only asks, through the leave guard.
    signal syncCuePointsRequested(string stickLabel, string rekordboxPath, string enginePath)

    RekordboxExportSyncController {
        id: realController
        objectName: "rekordboxExportSyncController"
    }

    Component.onCompleted: root.controller.analyze(root.stickLabel, root.rekordboxPath, root.enginePath)

    EditSessionHost {
        id: editHost
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "rekordbox-export-sync"
        saveLabel: "Sync Engine"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // What each section is called, and what its header says beside it.
    readonly property var sectionTitles: ({
        conflicts: "Conflicts",
        playlists: "Playlists to create, rename or delete",
        tracksToAdd: "Tracks to add to Engine",
        tracksToRemove: "Tracks to remove from Engine",
        membership: "Playlist membership",
        metadataToEngine: "Ratings and comments to Engine",
        cuesToEngine: "Cues to Engine",
        restoresToRekordbox: "Cues and ratings back onto rekordbox",
        engineOwnKept: "Engine's own, kept",
        notAdded: "Not added",
    })
    readonly property var sectionNotes: ({
        restoresToRekordbox: "What the export dropped that Seabass had written",
        engineOwnKept: "Kept, nothing is written",
        notAdded: "Rekordbox lists these, but they cannot be added; nothing is written",
    })
    // What picking a side means, in two sentences: the conflicts' help
    // and the buttons that answer all of them. Each row's buttons say
    // what that answer writes (EngineUpdateConflict's labels).
    readonly property string conflictExplanation: "Rekordbox's side makes Engine match what rekordbox has now, "
        + "for example by removing a track rekordbox no longer lists. Engine's side keeps what Engine has, and "
        + "for some cues and ratings puts Engine's version back onto rekordbox, or for a playlist rekordbox may "
        + "have renamed adds rekordbox's beside Engine's."

    // The sections folded shut, by name, so a fold holds while the list is
    // analysed again after a save; for this page only, never stored. Its
    // rows stay in the model (counts and keys do not move) and fold in
    // their delegate. Engine's own and the refused adds start folded:
    // nothing in them needs doing.
    property var collapsedSections: ({engineOwnKept: true, notAdded: true})
    function isCollapsed(section) {
        return root.collapsedSections[section] === true;
    }
    function toggleSection(section) {
        const next = Object.assign({}, root.collapsedSections);
        next[section] = !root.isCollapsed(section);
        root.collapsedSections = next;
        root.applyFolds();
    }
    // A folded section keeps only its first row in the view, as an empty
    // shell of height 0 that carries the section's header; the rest leave
    // the view's "shown" group. Not rows of height 0 each: a thousand of
    // those throw the ListView's estimate of where a row is, and it
    // scrolls to the wrong section. The runs come from the rows' own
    // sections (rowSections), never from counts and an assumed order, so
    // a fold cannot drift onto another section's rows.
    function applyFolds() {
        const sections = root.controller.rows.rowSections();
        if (sections.length !== rowsModel.items.count) {
            return;  // the view has not caught up; the model's signal calls again
        }
        let start = 0;
        while (start < sections.length) {
            const name = sections[start];
            let end = start + 1;
            while (end < sections.length && sections[end] === name) {
                ++end;
            }
            if (end - start > 1) {
                if (root.isCollapsed(name)) {
                    rowsModel.items.removeGroups(start + 1, end - start - 1, "shown");
                } else {
                    rowsModel.items.addGroups(start + 1, end - start - 1, "shown");
                }
            }
            start = end;
        }
    }
    // Model row -> the view's index, -1 for a row a fold leaves out.
    function viewIndexOf(modelRow) {
        if (modelRow < 0 || modelRow >= rowsModel.items.count) {
            return -1;
        }
        const item = rowsModel.items.get(modelRow);
        return item.inShown ? item.shownIndex : -1;
    }
    // The view's rows, for tests: which are shown.
    readonly property alias rowsModel: rowsModel
    DelegateModel {
        id: rowsModel
        model: root.controller.rows
        delegate: rowDelegate
        groups: [
            DelegateModelGroup {
                name: "shown"
                includeByDefault: true
            }
        ]
        filterOnGroup: "shown"
    }
    // Rows come and go (an analysis, an answer): the folds hold, once the
    // view has them.
    Connections {
        target: root.controller.rows
        function onModelReset() { Qt.callLater(root.applyFolds); }
        function onRowsInserted() { Qt.callLater(root.applyFolds); }
        function onRowsRemoved() { Qt.callLater(root.applyFolds); }
    }
    // RekordboxExportSyncListModel::Section's order.
    readonly property var sectionOrder: ["conflicts", "playlists", "tracksToAdd", "tracksToRemove", "membership",
        "metadataToEngine", "cuesToEngine", "restoresToRekordbox", "engineOwnKept", "notAdded"]
    // The first section with rows: no room above its header.
    readonly property string firstSection: {
        const counts = root.controller.sectionCounts;
        for (const name of root.sectionOrder) {
            if ((counts[name] || 0) > 0) {
                return name;
            }
        }
        return "";
    }
    // What the three chips mean, under the intro, each chip as rows show it.
    readonly property var directionLegend: [
        {direction: "to Engine", text: "Will write into the Engine library so it matches rekordbox"},
        {direction: "back to rekordbox",
         text: "Cues or ratings Seabass had put on the rekordbox side that the export dropped will go back onto it"},
        {direction: "kept", text: "Engine's own, nothing will be written"},
    ]

    // What the overview bar draws, left to right: new, changed, removed,
    // other (RekordboxExportSyncController::categoryCounts). Conflicts are
    // not in it: they are questions, not changes yet.
    readonly property var overviewSegments: {
        const counts = root.controller.categoryCounts;
        return [
            // Both kinds of new in good: the playlists washed out with an
            // outline, as SpaceReclaimBar tells two greens apart.
            {key: "newTracks", label: (counts["newTracks"] || 0) === 1 ? "new track" : "new tracks", fill: Theme.good, outline: false, count: counts["newTracks"] || 0},
            {key: "newPlaylists", label: (counts["newPlaylists"] || 0) === 1 ? "new playlist" : "new playlists", fill: Qt.rgba(Theme.good.r, Theme.good.g, Theme.good.b, 0.3),
             outline: true, count: counts["newPlaylists"] || 0},
            {key: "changed", label: "changed", fill: Theme.accent, outline: false, count: counts["changed"] || 0},
            {key: "removed", label: "removed", fill: Theme.danger, outline: false, count: counts["removed"] || 0},
            {key: "other", label: "other", fill: Theme.textMuted, outline: false, count: counts["other"] || 0},
        ];
    }
    readonly property int overviewTotal: root.overviewSegments.reduce((sum, segment) => sum + segment.count, 0)

    // The one row open to show its details, by model row; -1 for none.
    // Kept on its row while rows come and go around it.
    property int expandedIndex: -1
    function toggleExpanded(index) {
        root.expandedIndex = root.expandedIndex === index ? -1 : index;
    }
    Connections {
        target: root.controller.rows
        function onModelReset() { root.expandedIndex = -1; }
        function onRowsInserted(parent, first, last) {
            if (root.expandedIndex >= first) {
                root.expandedIndex += last - first + 1;
            }
        }
        function onRowsRemoved(parent, first, last) {
            if (root.expandedIndex > last) {
                root.expandedIndex -= last - first + 1;
            } else if (root.expandedIndex >= first) {
                root.expandedIndex = -1;
            }
        }
    }

    // The sections a tick writes (RekordboxExportSyncListModel::writable).
    readonly property var writableSections: ["playlists", "tracksToAdd", "tracksToRemove", "membership",
        "metadataToEngine", "cuesToEngine", "restoresToRekordbox"]
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    function isWritable(section) {
        return root.writableSections.indexOf(section) >= 0;
    }

    readonly property bool idle: !root.controller.busy && !root.controller.writing

    function directionColor(direction) {
        if (direction === "to Engine") {
            return Theme.accent;
        }
        if (direction === "back to rekordbox") {
            return Theme.warnText;
        }
        return Theme.textMuted;
    }

    // One width for the row's checkbox slot and its direction chip, so
    // every title starts in the same column whatever the row carries. The
    // box is a row gap from what follows it, in a row and in a section's
    // header alike, so a section's title stands over its rows' chips.
    readonly property real checkSlotWidth: Theme.scaled(18) + Theme.rowSpacing
    // The cover's square, two lines of a row tall (its title and its
    // reason), and its slot after the checkbox: every row keeps the slot,
    // so the chips and titles stay in their columns, and a section's
    // header keeps it too, so its title still stands over the chips.
    readonly property real coverSide: Theme.scaled(40)
    readonly property real coverSlotWidth: root.coverSide + Theme.rowSpacing
    TextMetrics {
        id: chipMetrics
        font.bold: true
        font.pointSize: Theme.fontTiny
        text: "back to rekordbox"
    }
    readonly property real chipSlotWidth: Math.ceil(chipMetrics.advanceWidth) + 12

    // Scrolling the list folds the header's description away (the intro
    // and the legend) and leaves the crumb and the row of counts and
    // buttons: what is read once gives its room to the list, what is used
    // stays. Back at the top of the list it unfolds again. The mechanism
    // is Clean Up Duplicates' (CleanupPage.qml); see ScrollCollapse for
    // when.
    ScrollCollapse {
        id: headerCollapse
        flickable: rowsList
        // At most what the header gives back: all of it but the crumb row,
        // which never folds. An over-estimate only makes it fold a little
        // later on a list that barely overflows.
        collapsibleHeight: root.header ? root.header.height - crumbRow.height : 0
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin and
        // nothing else (see SyncPage.qml's header for why).
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }

        // A ColumnLayout sized by anchors.fill does not feed its implicit
        // size back up (see ScanPage.qml's header). The row overruns the
        // bar by its top margin into bottomPadding, as a one-row header's
        // does (Theme.headerBottomPadding), so this is the one-row
        // header's height for a taller column.
        implicitHeight: headerLayout.implicitHeight + Theme.headerBottomPadding

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.sectionSpacing

            RowLayout {
                id: crumbRow
                objectName: "crumbRow"
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                // The gap a one-row header shows under its crumb
                // (Theme.crumbGap). While the description folds, its own
                // spacing is taken off here as it goes, so the gap closes to
                // exactly this and does not jump when the folded part hides.
                Layout.bottomMargin: Theme.crumbGap - headerLayout.spacing
                    - (headerDetails.visible ? headerLayout.spacing * headerCollapse.progress : 0)
                BackBreadcrumb {
                    objectName: "breadcrumb"
                    stack: root.StackView.view
                    stickLabel: root.stickLabel
                    title: "Sync after Rekordbox Export"
                    backEnabled: !root.controller.writing
                    onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                    onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
                }
                ExperimentalBadge {
                    Layout.alignment: Qt.AlignVCenter
                }
                Item { Layout.fillWidth: true }
            }

            // Everything the header says rather than offers, in one block
            // so it folds as one. Clipped while it folds; hidden once it
            // has, so it gives back its spacing too.
            Item {
                id: headerDetails
                objectName: "headerDetails"
                Layout.fillWidth: true
                Layout.preferredHeight: detailsColumn.implicitHeight * (1 - headerCollapse.progress)
                implicitHeight: detailsColumn.implicitHeight
                visible: headerCollapse.progress < 1 && detailsColumn.implicitHeight > 0
                opacity: 1 - headerCollapse.progress
                clip: true

                ColumnLayout {
                    id: detailsColumn
                    width: parent.width
                    spacing: Theme.tightSpacing

                    // What this page is for, in his words: the first line.
                    Label {
                        objectName: "explanationLabel"
                        Layout.fillWidth: true
                        visible: root.controller.analyzed
                        wrapMode: Text.WordWrap
                        color: Theme.text
                        text: root.controller.proposalEmpty
                            ? "Engine already matches this stick's rekordbox library."
                            : "Rekordbox has changed the library on this stick. This page brings everything back "
                              + "in step with Engine."
                    }

                    // What the proposal does, in one plain sentence, so a
                    // reader can check it makes sense before reading on.
                    Label {
                        objectName: "summaryLabel"
                        Layout.fillWidth: true
                        visible: root.controller.analyzed
                        wrapMode: Text.WordWrap
                        color: Theme.text
                        text: root.controller.summaryText
                    }

                    Label {
                        id: introLabel
                        objectName: "introLabel"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: root.controller.introText
                    }

                    // What the chips say, one line each, the chip as rows
                    // show it.
                    Repeater {
                        model: root.controller.analyzed ? root.directionLegend : []
                        delegate: RowLayout {
                            id: legendLine
                            required property var modelData
                            objectName: "legendLine"
                            Layout.fillWidth: true
                            spacing: 0
                            Item {
                                Layout.preferredWidth: root.chipSlotWidth
                                Layout.alignment: Qt.AlignVCenter
                                implicitHeight: legendChip.implicitHeight
                                StatusBadge {
                                    id: legendChip
                                    objectName: "legendChip"
                                    label: legendLine.modelData.direction
                                    badgeColor: root.directionColor(legendLine.modelData.direction)
                                }
                            }
                            Label {
                                objectName: "legendText"
                                Layout.fillWidth: true
                                Layout.leftMargin: Theme.tightSpacing
                                Layout.alignment: Qt.AlignVCenter
                                elide: Text.ElideRight
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: legendLine.modelData.text
                            }
                        }
                    }

                }
            }

            RowLayout {
                objectName: "summaryRow"
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                visible: root.controller.analyzed

                Figure {
                    objectName: "checkedFigure"
                    value: root.controller.checkedCount
                    label: "selected"
                }
                Figure {
                    objectName: "conflictFigure"
                    Layout.leftMargin: Theme.rowSpacing
                    value: root.controller.conflictCount
                    label: "conflicts"
                    valueColor: root.controller.conflictCount > 0 ? Theme.conflictText : Theme.text
                }
                Figure {
                    objectName: "stagedFigure"
                    Layout.leftMargin: Theme.rowSpacing
                    value: root.controller.stagedCount
                    label: "staged"
                    valueColor: root.controller.stagedCount > 0 ? Theme.warnText : Theme.text
                }
                Label {
                    id: cuesLink
                    objectName: "syncCuePointsLink"
                    Layout.leftMargin: Theme.rowSpacing
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: root.controller.onlyCues
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    textFormat: Text.StyledText
                    linkColor: Theme.accent
                    text: "Only cues differ. <a href=\"sync-cue-points\">Sync Cue Points</a> shows them with waveforms."
                    onLinkActivated: editHost.requestLeave(() => root.syncCuePointsRequested(
                        root.stickLabel, root.rekordboxPath, root.enginePath))
                    HoverHandler {
                        cursorShape: cuesLink.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }
                Item {
                    visible: !cuesLink.visible
                    Layout.fillWidth: true
                }
                Button {
                    objectName: "undoButton"
                    text: "Undo Last Save"
                    visible: root.controller.canUndo
                    enabled: root.idle
                    ToolTip.visible: hovered
                    ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                    onClicked: root.controller.undoLastOperation()
                }
                Button {
                    objectName: "unstageAllButton"
                    text: "Unstage All"
                    visible: root.controller.stagedCount > 0
                    enabled: root.idle
                    ToolTip.visible: hovered
                    ToolTip.text: "Take everything staged here back out of the changes to save"
                    onClicked: root.controller.unstageAll()
                }
                Button {
                    objectName: "stageButton"
                    highlighted: true
                    text: root.controller.stagedCount > 0 ? "Stage Selected Again" : "Stage Selected"
                    enabled: root.idle && root.controller.analyzed
                    ToolTip.visible: hovered
                    ToolTip.text: "Stage every selected row, and the record of what was left, as one batch; "
                        + "Sync Engine writes it. Staging again replaces the batch."
                    onClicked: root.controller.stageSelected()
                }
            }

            // What the proposal holds, drawn to scale, in the
            // idiom of SpaceReclaimBar: the same ground, the same
            // height, a legend so nothing rests on colour alone.
            // Under the counts, and it stays when the header folds:
            // slim then, without its legend, as Clean Up
            // Duplicates' space bar does (SpaceReclaimBar's
            // compactness: 26 down to Theme.tightSpacing).
            ColumnLayout {
                objectName: "overview"
                Layout.fillWidth: true
                spacing: 0
                visible: root.controller.analyzed && root.overviewTotal > 0

                Item {
                    objectName: "overviewBar"
                    Layout.fillWidth: true
                    implicitHeight: 26 + (Theme.tightSpacing - 26) * headerCollapse.progress

                    Rectangle {
                        anchors.fill: parent
                        radius: 3
                        color: Theme.groupBackground
                        border.color: Theme.borderSubtle
                        border.width: 1
                    }
                    Row {
                        id: overviewSegmentsRow
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 2
                        function span(count) {
                            return count > 0 && root.overviewTotal > 0
                                ? Math.max(2, width * (count / root.overviewTotal) - 2) : 0;
                        }
                        Repeater {
                            model: root.overviewSegments
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "overviewSegment_" + modelData.key
                                width: overviewSegmentsRow.span(modelData.count)
                                height: parent.height
                                radius: 2
                                color: modelData.fill
                                border.color: Theme.good
                                border.width: modelData.outline ? 1 : 0
                                visible: width > 0
                            }
                        }
                    }
                }

                // The legend folds with the header and hides once it
                // has, so nothing jumps.
                Item {
                    objectName: "overviewLegend"
                    Layout.fillWidth: true
                    Layout.preferredHeight: (overviewLegendFlow.implicitHeight + 10) * (1 - headerCollapse.progress)
                    visible: headerCollapse.progress < 1
                    opacity: 1 - headerCollapse.progress
                    clip: true
                    Flow {
                        id: overviewLegendFlow
                        y: 10
                        width: parent.width
                        spacing: 18
                        Repeater {
                            model: root.overviewSegments
                            delegate: Row {
                                id: swatchRow
                                required property var modelData
                                objectName: "overviewLegend_" + modelData.key
                                // A category with nothing in it is left out.
                                visible: modelData.count > 0
                                spacing: 7
                                readonly property int count: modelData.count
                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: swatchRow.modelData.fill
                                    border.color: swatchRow.modelData.outline ? Theme.good : Theme.borderSubtle
                                    border.width: 1
                                }
                                Label {
                                    objectName: "overviewLegendText"
                                    text: swatchRow.modelData.count + " " + swatchRow.modelData.label
                                    font.pointSize: Theme.fontSmall
                                    color: Theme.textMuted
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    component Figure: RowLayout {
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.sectionSpacing

        Label {
            objectName: "errorLabel"
            visible: text.length > 0
            text: root.controller.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "statusLabel"
            visible: text.length > 0
            text: root.controller.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            id: rowsList
            objectName: "rowsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            interactive: contentHeight > height
            clip: true
            // No reuse: a pooled row handed back while the "shown" group
            // changes kept another row's data (a Tracks to add row drawn
            // as a playlist, so a section did not open). Rows are built
            // only while in view, and a folded section's are not in it.
            reuseItems: false
            model: rowsModel
            bottomMargin: editHost.saveClearance
            ScrollBar.vertical: BigScrollBar {}
            // BigScrollBar overlays the list, so rows stop short of it.
            readonly property real delegateWidth: width - 14

            section.property: "section"
            section.criteria: ViewSection.FullString
            section.delegate: Item {
                id: sectionHeader
                required property string section
                objectName: "sectionHeader_" + section
                readonly property int total: root.controller.sectionCounts[section] || 0
                readonly property int ticked: root.controller.sectionCheckedCounts[section] || 0
                readonly property bool writable: root.isWritable(section)
                readonly property bool conflicts: section === "conflicts"
                // Room above every section but the list's first.
                readonly property real gapAbove: section === root.firstSection ? 0 : Theme.sectionSpacing
                readonly property bool collapsed: root.isCollapsed(section)
                width: rowsList.delegateWidth
                height: gapAbove + headerColumn.implicitHeight + Theme.tightSpacing

                ColumnLayout {
                    id: headerColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.tightSpacing
                    spacing: Theme.tightSpacing

                    RowLayout {
                        id: headerRow
                        Layout.fillWidth: true
                        spacing: 0
                        // The rows' checkbox slot: the title stands over their
                        // chips. None where a tick writes nothing, so that
                        // title is on the page's left line.
                        Item {
                            visible: sectionHeader.writable
                            Layout.preferredWidth: root.checkSlotWidth
                            Layout.alignment: Qt.AlignVCenter
                            implicitHeight: sectionCheck.implicitHeight
                            SeabassCheckBox {
                                id: sectionCheck
                                objectName: "sectionCheck_" + sectionHeader.section
                                anchors.verticalCenter: parent.verticalCenter
                                visible: sectionHeader.writable
                                padding: 0
                                enabled: root.idle
                                // Selected when every row of the section is.
                                checked: sectionHeader.total > 0 && sectionHeader.ticked === sectionHeader.total
                                ToolTip.visible: hovered
                                ToolTip.text: checked ? "Deselect every row of this section" : "Select every row of this section"
                                onToggled: {
                                    root.controller.setSectionIncluded(sectionHeader.section, checked);
                                    // The click broke the binding; the counts decide.
                                    sectionCheck.checked = Qt.binding(() => sectionHeader.total > 0
                                        && sectionHeader.ticked === sectionHeader.total);
                                }
                            }
                        }
                        Item {
                            visible: sectionHeader.writable
                            Layout.preferredWidth: root.coverSlotWidth
                        }
                        // The title, and its count, fold the section open or
                        // shut, as the chevron does.
                        Subtitle {
                            id: sectionTitle
                            objectName: "sectionTitle"
                            Layout.alignment: Qt.AlignBaseline
                            text: root.sectionTitles[sectionHeader.section] || sectionHeader.section
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                objectName: "sectionTitleTap"
                                onTapped: root.toggleSection(sectionHeader.section)
                            }
                        }
                        Label {
                            objectName: "sectionCount"
                            Layout.alignment: Qt.AlignBaseline
                            Layout.leftMargin: Theme.tightSpacing
                            text: sectionHeader.total
                            font.family: Theme.dataFamily
                            color: Theme.textMuted
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.toggleSection(sectionHeader.section) }
                        }
                        InfoButton {
                            objectName: "conflictInfoButton"
                            visible: sectionHeader.conflicts
                            Layout.leftMargin: Theme.tightSpacing
                            Layout.alignment: Qt.AlignVCenter
                            explanationTitle: "Picking a side"
                            summaryText: root.conflictExplanation
                            explanationText: "Seabass cannot tell which library is right about these, so it asks.\n\n"
                                + "Open a conflict to see, under each button's words, every line that answer writes. "
                                + "An answer's edits are selected in their own sections and staged with the rest."
                        }
                        Label {
                            objectName: "sectionNote"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignBaseline
                            Layout.leftMargin: Theme.rowSpacing
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: root.sectionNotes[sectionHeader.section] || ""
                        }
                        // Every conflict answered at once, answered ones too,
                        // or every answer taken back.
                        Button {
                            objectName: "rekordboxSideForAllButton"
                            visible: sectionHeader.conflicts
                            Layout.leftMargin: Theme.rowSpacing
                            Layout.alignment: Qt.AlignVCenter
                            text: "Rekordbox's side for all"
                            enabled: root.idle
                            ToolTip.visible: hovered
                            // Named, so a test can read what the tip says.
                            readonly property string toolTipText: "Answer every conflict with rekordbox's side; the "
                                + "edits are selected in their sections. " + root.conflictExplanation
                            ToolTip.text: toolTipText
                            onClicked: root.controller.resolveAllConflicts(true)
                        }
                        Button {
                            objectName: "engineSideForAllButton"
                            visible: sectionHeader.conflicts
                            Layout.leftMargin: Theme.tightSpacing
                            Layout.alignment: Qt.AlignVCenter
                            text: "Engine's side for all"
                            enabled: root.idle
                            ToolTip.visible: hovered
                            readonly property string toolTipText: "Answer every conflict with Engine's side. "
                                + root.conflictExplanation
                            ToolTip.text: toolTipText
                            onClicked: root.controller.resolveAllConflicts(false)
                        }
                        ToolButton {
                            objectName: "clearAllChoicesButton"
                            visible: sectionHeader.conflicts
                            Layout.leftMargin: Theme.tightSpacing
                            Layout.alignment: Qt.AlignVCenter
                            text: "Clear all choices"
                            font.pointSize: Theme.fontSmall
                            enabled: root.idle && sectionHeader.ticked > 0
                            ToolTip.visible: hovered
                            ToolTip.text: "Take back every answer: every conflict is open again"
                            onClicked: root.controller.clearAllConflictResolutions()
                        }
                        // Folds the section open or shut, in the rows'
                        // chevron column.
                        IconToolButton {
                            objectName: "sectionChevron"
                            Layout.leftMargin: Theme.tightSpacing
                            Layout.rightMargin: Theme.tightSpacing
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: Theme.iconSizeSmall
                            implicitHeight: Theme.iconSizeSmall
                            padding: 0
                            flat: true
                            iconName: sectionHeader.collapsed ? "arrow-right" : "arrow-down"
                            iconSize: Theme.iconSizeSmall * 0.5
                            iconColor: Theme.textMuted
                            text: sectionHeader.collapsed ? "Show this section" : "Hide this section"
                            ToolTip.visible: hovered
                            ToolTip.text: sectionHeader.collapsed ? "Show this section's rows" : "Hide this section's rows"
                            onClicked: root.toggleSection(sectionHeader.section)
                        }
                    }

                    // One line over the conflicts; the help says what the
                    // two sides mean.
                    Label {
                        objectName: "conflictExplanation"
                        visible: sectionHeader.conflicts
                        Layout.fillWidth: true
                        Layout.rightMargin: Theme.tightSpacing
                        elide: Text.ElideRight
                        color: Theme.textMuted
                        text: "Each button says what it writes. Nothing is written for a conflict you leave open."
                    }
                }
            }

            // A row of the list (rowsModel's delegate).
            Component {
                id: rowDelegate
                Rectangle {
                    id: row
                    required property int index
                    // The row in the controller's model: rows of a folded
                    // section are left out of the view (rowsModel), so the
                    // view's index is not it.
                    readonly property int modelRow: DelegateModel.itemsIndex
                    required property string section
                    required property string kind
                    required property string title
                    required property string artist
                    required property string detail
                    required property string reason
                    required property bool isConflict
                    required property string rekordboxChoiceLabel
                    required property string engineChoiceLabel
                    required property string resolvedSide
                    required property bool included
                    required property bool staged
                    required property string stagedDescription
                    required property string direction
                    required property bool fromConflict
                    required property var details
                    required property bool hasTrack
                    required property string artworkPath
                    required property string fallbackArtworkPath
                    required property var cueSides

                    readonly property bool writable: root.isWritable(section)
                    readonly property bool expanded: root.expandedIndex === modelRow
                    // Where everything under the title row starts: the title's column.
                    readonly property real textIndent: root.checkSlotWidth + root.coverSlotWidth + root.chipSlotWidth
                        + Theme.tightSpacing

                    // Its section folded (root.collapsedSections): no height,
                    // not shown, nothing built inside.
                    readonly property bool folded: root.isCollapsed(section)

                    width: rowsList.delegateWidth
                    implicitHeight: row.folded ? 0 : rowContent.implicitHeight
                    height: implicitHeight
                    visible: !row.folded
                    color: hover.hovered ? Theme.rowHover : (row.modelRow % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
                    HoverHandler {
                        id: hover
                        cursorShape: Qt.PointingHandCursor
                    }
                    // A click on the row's text opens it; the checkbox and the
                    // buttons take their own clicks first.
                    TapHandler {
                        objectName: "rowTap"
                        onTapped: root.toggleExpanded(row.modelRow)
                    }

                    // The row itself, built only while its section is open: a
                    // folded section's rows are this shell alone, height 0 and
                    // hidden, so even Engine's own (a thousand rows and more)
                    // stay light when they all fit in the view at once.
                    Loader {
                        id: rowContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        active: !row.folded
                        sourceComponent: Item {
                            implicitHeight: Math.max(rowColumn.implicitHeight, row.hasTrack ? root.coverSide : 0)
                                + 2 * Theme.tightSpacing
                            // The track's cover, read off the GUI thread at the size it
                            // is drawn (ArtworkImage), on the square a row without one
                            // shows. A playlist's row leaves the slot empty.
                            Rectangle {
                                objectName: "rowCoverSlot"
                                visible: row.hasTrack
                                x: root.checkSlotWidth
                                y: Theme.tightSpacing
                                width: root.coverSide
                                height: root.coverSide
                                radius: 2
                                // Outlined: the surface is the odd rows' own tone.
                                color: Theme.surface
                                border.color: Theme.borderSubtle
                                border.width: 1
                                ArtworkImage {
                                    objectName: "rowArtwork"
                                    anchors.fill: parent
                                    source: row.artworkPath
                                    fallbackSource: row.fallbackArtworkPath
                                }
                            }

                            ColumnLayout {
                                id: rowColumn
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.topMargin: Theme.tightSpacing
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Item {
                                        Layout.preferredWidth: root.checkSlotWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        implicitHeight: rowCheck.implicitHeight
                                        SeabassCheckBox {
                                            id: rowCheck
                                            objectName: "rowCheck"
                                            visible: row.writable
                                            padding: 0
                                            enabled: root.idle
                                            checked: row.included
                                            ToolTip.visible: hovered
                                            ToolTip.text: row.included ? "Leave this out" : "Include this when staging"
                                            onToggled: {
                                                root.controller.setIncluded(row.modelRow, checked);
                                                rowCheck.checked = Qt.binding(() => row.included);
                                            }
                                        }
                                    }
                                    // The cover's slot; the cover itself is drawn over
                                    // it below, two lines tall.
                                    Item {
                                        Layout.preferredWidth: root.coverSlotWidth
                                    }
                                    Item {
                                        Layout.preferredWidth: root.chipSlotWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        implicitHeight: chip.implicitHeight
                                        StatusBadge {
                                            id: chip
                                            objectName: "directionChip"
                                            visible: row.direction.length > 0
                                            label: row.direction
                                            badgeColor: root.directionColor(row.direction)
                                        }
                                    }
                                    Label {
                                        objectName: "rowTitle"
                                        Layout.leftMargin: Theme.tightSpacing
                                        Layout.fillWidth: true
                                        // A short title is never cut for the detail
                                        // beside it; a long one gives way past a third.
                                        Layout.minimumWidth: Math.min(implicitWidth, rowsList.width / 3)
                                        elide: Text.ElideRight
                                        textFormat: Text.PlainText
                                        font.bold: true
                                        text: row.title
                                    }
                                    Label {
                                        objectName: "rowArtist"
                                        Layout.maximumWidth: rowsList.width * 0.25
                                        visible: row.artist.length > 0
                                        Layout.leftMargin: Theme.rowSpacing
                                        elide: Text.ElideRight
                                        textFormat: Text.PlainText
                                        color: Theme.textMuted
                                        text: row.artist
                                    }
                                    Label {
                                        objectName: "rowDetail"
                                        Layout.maximumWidth: rowsList.width * 0.3
                                        visible: row.detail.length > 0
                                        Layout.leftMargin: Theme.rowSpacing
                                        elide: Text.ElideMiddle
                                        textFormat: Text.PlainText
                                        font.family: Theme.dataFamily
                                        font.pointSize: Theme.fontSmall
                                        color: Theme.textMuted
                                        text: row.detail
                                    }
                                    IconToolButton {
                                        objectName: "expandButton"
                                        Layout.leftMargin: Theme.tightSpacing
                                        Layout.rightMargin: Theme.tightSpacing
                                        Layout.alignment: Qt.AlignVCenter
                                        implicitWidth: Theme.iconSizeSmall
                                        implicitHeight: Theme.iconSizeSmall
                                        padding: 0
                                        flat: true
                                        iconName: row.expanded ? "arrow-down" : "arrow-right"
                                        iconSize: Theme.iconSizeSmall * 0.5
                                        iconColor: Theme.textMuted
                                        text: row.expanded ? "Hide details" : "Show details"
                                        ToolTip.visible: hovered
                                        ToolTip.text: row.expanded ? "Hide what happens to this" : "Show exactly what happens to this"
                                        onClicked: root.toggleExpanded(row.modelRow)
                                    }
                                }

                                Label {
                                    objectName: "rowReason"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: row.textIndent
                                    Layout.rightMargin: Theme.tightSpacing
                                    visible: row.reason.length > 0
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    color: Theme.textMuted
                                    font.pointSize: Theme.fontSmall
                                    text: row.reason
                                }

                                // A conflict: one button per side, the chosen one
                                // highlighted, and the answer can be taken back.
                                RowLayout {
                                    objectName: "conflictChoices"
                                    visible: row.isConflict
                                    Layout.leftMargin: row.textIndent
                                    Layout.rightMargin: Theme.tightSpacing
                                    Layout.fillWidth: true
                                    spacing: Theme.tightSpacing
                                    Button {
                                        objectName: "rekordboxChoiceButton"
                                        visible: row.rekordboxChoiceLabel.length > 0
                                        text: row.rekordboxChoiceLabel
                                        highlighted: row.resolvedSide === "rekordbox"
                                        enabled: root.idle
                                        ToolTip.visible: hovered
                                        ToolTip.text: "Take rekordbox's side; its edits are selected in their sections"
                                        onClicked: root.controller.resolveConflict(row.modelRow, true)
                                    }
                                    Button {
                                        objectName: "engineChoiceButton"
                                        visible: row.engineChoiceLabel.length > 0
                                        text: row.engineChoiceLabel
                                        highlighted: row.resolvedSide === "engine"
                                        enabled: root.idle
                                        ToolTip.visible: hovered
                                        ToolTip.text: "Keep Engine's side"
                                        onClicked: root.controller.resolveConflict(row.modelRow, false)
                                    }
                                    Label {
                                        objectName: "chosenSideLabel"
                                        visible: row.resolvedSide.length > 0
                                        Layout.leftMargin: Theme.rowSpacing
                                        color: Theme.good
                                        font.pointSize: Theme.fontSmall
                                        text: row.resolvedSide === "rekordbox" ? "Rekordbox's side chosen" : "Engine's side chosen"
                                    }
                                    ToolButton {
                                        objectName: "undoChoiceButton"
                                        visible: row.resolvedSide.length > 0
                                        text: "Undo choice"
                                        font.pointSize: Theme.fontSmall
                                        enabled: root.idle
                                        onClicked: root.controller.clearConflictResolution(row.modelRow)
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                // Opened: exactly what the save does with this row,
                                // one line each (the model's details).
                                Label {
                                    objectName: "rowDetails"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: row.textIndent
                                    Layout.rightMargin: Theme.tightSpacing
                                    Layout.topMargin: Theme.tightSpacing
                                    Layout.bottomMargin: Theme.tightSpacing
                                    visible: row.expanded
                                    wrapMode: Text.WordWrap
                                    textFormat: Text.PlainText
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontSmall
                                    text: row.expanded ? row.details.join("\n") : ""
                                }

                                // A row with cues, opened: each copy's cues on
                                // its waveform, as Sync Cue Points' open rows
                                // draw them, side by side, rekordbox's first.
                                // The copy the save writes is the target,
                                // dimmed, and says what it gains and loses.
                                // Built only while the row is open, so a
                                // waveform is read only for a row someone
                                // looks at; nothing on a closed row, as on
                                // Sync Cue Points.
                                GridLayout {
                                    id: cueStrips
                                    objectName: "cueStrips"
                                    readonly property bool hasTarget: row.cueSides.some((side) => side.written)
                                    Layout.fillWidth: true
                                    Layout.leftMargin: row.textIndent
                                    Layout.rightMargin: Theme.tightSpacing
                                    Layout.bottomMargin: Theme.tightSpacing
                                    visible: row.expanded && row.cueSides.length > 0
                                    columns: row.cueSides.length > 1 ? 2 : 1
                                    uniformCellWidths: true
                                    columnSpacing: Theme.rowSpacing
                                    rowSpacing: Theme.tightSpacing
                                    Repeater {
                                        model: row.expanded ? row.cueSides : []
                                        delegate: ColumnLayout {
                                            id: strip
                                            required property var modelData
                                            objectName: "cueStrip"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            spacing: Theme.tightSpacing
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.tightSpacing
                                                TableHeaderLabel {
                                                    visible: cueStrips.hasTarget
                                                    label: strip.modelData.written ? "target" : "source"
                                                }
                                                StatusBadge {
                                                    label: FormatLabels.label(strip.modelData.side)
                                                    badgeColor: strip.modelData.written ? Theme.textMuted : Theme.accent
                                                }
                                                Label {
                                                    objectName: "cueStripText"
                                                    Layout.fillWidth: true
                                                    Layout.minimumWidth: 0
                                                    text: strip.modelData.cueText
                                                    font.family: Theme.dataFamily
                                                    font.pointSize: Theme.fontSmall
                                                    color: Theme.textMuted
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            WaveformView {
                                                objectName: "cueStripWaveform"
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 40
                                                opacity: strip.modelData.written ? 0.75 : 1.0
                                                waveformData: root.playbackController
                                                    ? root.playbackController.waveformFor(strip.modelData.side,
                                                        root.pathForFormat(strip.modelData.side), strip.modelData.sourceId)
                                                    : []
                                                format: strip.modelData.side
                                                cueData: strip.modelData.cues
                                                trackDurationMs: strip.modelData.durationMs
                                            }
                                        }
                                    }
                                }

                                Label {
                                    objectName: "stagedLabel"
                                    Layout.fillWidth: true
                                    Layout.leftMargin: row.textIndent
                                    Layout.rightMargin: Theme.tightSpacing
                                    visible: row.staged
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    color: Theme.warnText
                                    font.pointSize: Theme.fontSmall
                                    text: "Staged: " + row.stagedDescription
                                }
                            }
                        }
                    }
                }
            }

            EmptyState {
                objectName: "nothingToSyncLabel"
                visible: rowsList.count === 0 && root.controller.analyzed && !root.controller.busy
                tone: "good"
                iconName: "checkmark"
                text: "Engine already matches this export."
            }
        }
    }

    // A cancelled analysis takes the user back to where they came from.
    Connections {
        target: root.controller
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: root.controller.busy
        current: root.controller.scanCurrent
        total: root.controller.scanTotal
        label: root.controller.scanLabel.length > 0 ? root.controller.scanLabel
                                                    : "Comparing the rekordbox export with Engine..."
        unitName: "tracks"
        cancellable: root.controller.scanCancellable
        onCancelRequested: root.controller.cancelScan()
    }
}
