// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Sync after Rekordbox Export (docs/sync-after-rekordbox-export-plan.md,
// "The page"): what rekordbox changed since this stick was last saved, as
// a proposal for the Engine library, then one save.
//
// Top to bottom: what the proposal was compared against, the counts and
// the controls that act on the list, then the list, one section per kind
// of change in the plan's order. A section heads its rows with a
// section-wide checkbox; a row has its own, its direction, what it is and
// why. A conflict has no checkbox: it is answered with one of its two
// buttons, and the answer's edits appear as ticked rows of their own
// sections. Engine's own and the refused adds are listed and never
// written.
//
// Ticking is not staging. "Stage Ticked" hands the ticked rows to the
// controller (stageSelected), which stages them as one batch with the
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
        playlists: "Playlists to create, rename or delete",
        tracksToAdd: "Tracks to add to Engine",
        tracksToRemove: "Tracks to remove from Engine",
        membership: "Playlist membership",
        metadataToEngine: "Ratings and comments to Engine",
        cuesToEngine: "Cues to Engine",
        restoresToRekordbox: "Cues and ratings back onto rekordbox",
        conflicts: "Conflicts",
        engineOwnKept: "Engine's own, kept",
        notAdded: "Not added",
    })
    readonly property var sectionNotes: ({
        restoresToRekordbox: "What the export dropped that Seabass had written",
        conflicts: "Seabass cannot tell which side is right. Pick one, or leave it.",
        engineOwnKept: "Shown, never written",
        notAdded: "Rekordbox lists these, but they cannot be added",
    })
    // The sections a tick writes (RekordboxExportSyncListModel::writable).
    readonly property var writableSections: ["playlists", "tracksToAdd", "tracksToRemove", "membership",
        "metadataToEngine", "cuesToEngine", "restoresToRekordbox"]
    function isWritable(section) {
        return root.writableSections.indexOf(section) >= 0;
    }

    readonly property bool idle: !root.controller.busy && !root.controller.writing
    // A run with no earlier record says once what happens to what is left
    // undecided: it is recorded as it stands, so it counts as Engine's own.
    readonly property string introLine: {
        const intro = root.controller.introText;
        if (intro.length === 0) {
            return "";
        }
        return intro + "." + (root.controller.hasBaseline ? ""
            : " Items you leave undecided here count as Engine's own from now on.");
    }

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
    // every title starts in the same column whatever the row carries.
    readonly property real checkSlotWidth: Theme.scaled(18) + Theme.tightSpacing
    TextMetrics {
        id: chipMetrics
        font.bold: true
        font.pointSize: Theme.fontTiny
        text: "back to rekordbox"
    }
    readonly property real chipSlotWidth: Math.ceil(chipMetrics.advanceWidth) + 12

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin and
        // nothing else (see SyncPage.qml's header for why).
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
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
            id: introLabel
            objectName: "introLabel"
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: root.introLine
        }

        RowLayout {
            objectName: "summaryRow"
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            visible: root.controller.analyzed

            Figure {
                objectName: "checkedFigure"
                value: root.controller.checkedCount
                label: "ticked"
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
                text: root.controller.stagedCount > 0 ? "Stage Again" : "Stage Ticked"
                enabled: root.idle && root.controller.analyzed
                ToolTip.visible: hovered
                ToolTip.text: "Stage every ticked row, and the record of what was left, as one batch; "
                    + "Sync Engine writes it. Staging again replaces the batch."
                onClicked: root.controller.stageSelected()
            }
        }

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
            reuseItems: true
            model: root.controller.rows
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
                // Room above every section but the list's first.
                readonly property real gapAbove: section === "playlists" ? 0 : Theme.sectionSpacing
                width: rowsList.delegateWidth
                height: gapAbove + headerRow.implicitHeight + Theme.tightSpacing

                RowLayout {
                    id: headerRow
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.tightSpacing
                    spacing: Theme.tightSpacing
                    SeabassCheckBox {
                        id: sectionCheck
                        objectName: "sectionCheck_" + sectionHeader.section
                        Layout.alignment: Qt.AlignVCenter
                        visible: sectionHeader.writable
                        padding: 0
                        enabled: root.idle
                        // Ticked when every row of the section is.
                        checked: sectionHeader.total > 0 && sectionHeader.ticked === sectionHeader.total
                        ToolTip.visible: hovered
                        ToolTip.text: checked ? "Untick every row of this section" : "Tick every row of this section"
                        onToggled: {
                            root.controller.setSectionIncluded(sectionHeader.section, checked);
                            // The click broke the binding; the counts decide.
                            sectionCheck.checked = Qt.binding(() => sectionHeader.total > 0
                                && sectionHeader.ticked === sectionHeader.total);
                        }
                    }
                    Subtitle {
                        objectName: "sectionTitle"
                        Layout.alignment: Qt.AlignBaseline
                        text: root.sectionTitles[sectionHeader.section] || sectionHeader.section
                    }
                    Label {
                        objectName: "sectionCount"
                        Layout.alignment: Qt.AlignBaseline
                        text: sectionHeader.total
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
                        text: root.sectionNotes[sectionHeader.section] || ""
                    }
                }
            }

            delegate: Rectangle {
                id: row
                required property int index
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

                readonly property bool writable: root.isWritable(section)

                width: rowsList.delegateWidth
                implicitHeight: rowColumn.implicitHeight + 2 * Theme.tightSpacing
                height: implicitHeight
                color: hover.hovered ? Theme.rowHover : (index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
                HoverHandler { id: hover }

                ColumnLayout {
                    id: rowColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
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
                                    root.controller.setIncluded(row.index, checked);
                                    rowCheck.checked = Qt.binding(() => row.included);
                                }
                            }
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
                            Layout.rightMargin: Theme.tightSpacing
                            elide: Text.ElideMiddle
                            textFormat: Text.PlainText
                            font.family: Theme.dataFamily
                            font.pointSize: Theme.fontSmall
                            color: Theme.textMuted
                            text: row.detail
                        }
                    }

                    Label {
                        objectName: "rowReason"
                        Layout.fillWidth: true
                        Layout.leftMargin: root.checkSlotWidth + root.chipSlotWidth + Theme.tightSpacing
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
                        Layout.leftMargin: root.checkSlotWidth + root.chipSlotWidth + Theme.tightSpacing
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
                            ToolTip.text: "Take rekordbox's side; its edits are ticked in their sections"
                            onClicked: root.controller.resolveConflict(row.index, true)
                        }
                        Button {
                            objectName: "engineChoiceButton"
                            visible: row.engineChoiceLabel.length > 0
                            text: row.engineChoiceLabel
                            highlighted: row.resolvedSide === "engine"
                            enabled: root.idle
                            ToolTip.visible: hovered
                            ToolTip.text: "Keep Engine's side"
                            onClicked: root.controller.resolveConflict(row.index, false)
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
                            onClicked: root.controller.clearConflictResolution(row.index)
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        objectName: "stagedLabel"
                        Layout.fillWidth: true
                        Layout.leftMargin: root.checkSlotWidth + root.chipSlotWidth + Theme.tightSpacing
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
