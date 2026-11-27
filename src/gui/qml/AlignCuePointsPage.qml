// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Maintain's "Align Cue Points": the tracks with a cue point that is a
// little off the beat grid of its own library. How little is the slider's
// word: a cue point further from its beat than that was put there on
// purpose, and is neither listed nor moved. Ticked tracks are staged with
// "Stage Selected", and the page's Save moves their listed cue points onto
// their beats (CueAlignmentController, AlignCuesChange).
//
// A track whose cue points are all off by one amount is listed and cannot
// be ticked: its grid is more likely shifted than its cues misplaced, and
// moving the cues onto it would move them all off the music.
Page {
    id: root
    required property string stickLabel
    property string hubLabel: ""
    required property string rekordboxPath
    required property string enginePath
    property var appSettingsController: null
    property var playbackController: null

    CueAlignmentController {
        id: ownController
    }
    // Lets a test hand the page a controller it has prepared.
    property QtObject controllerForTesting: null
    readonly property QtObject controller: root.controllerForTesting !== null ? root.controllerForTesting : ownController

    EditSessionHost {
        id: editHost
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "align-cues"
        saveLabel: "Align"
        quietCleanSaves: true
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    function formatLabel(format) {
        if (format === "engine") return "Engine OS";
        if (format === "onelibrary") return "OneLibrary";
        return "DeviceLibrary";
    }
    function plural(n, one, many) {
        return n === 1 ? "1 " + one : n + " " + many;
    }

    Component.onCompleted: root.controller.scan(root.rekordboxPath, root.enginePath)

    header: ToolBar {
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                stickLabel: root.stickLabel
                middleLabel: root.hubLabel
                title: "Align Cue Points"
                backEnabled: !root.controller.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.pageMargin
        anchors.rightMargin: Theme.pageMargin
        spacing: Theme.tightSpacing

        Label {
            objectName: "alignIntro"
            Layout.fillWidth: true
            Layout.topMargin: Theme.tightSpacing
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "Cue points that sit a little off the beat grid of their own library. Aligning moves them onto "
                + "the nearest beat."
        }

        // The tolerance: how far off its beat a cue point may be and
        // still be listed. Filters what was read; nothing is read again.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            Label {
                text: "Off the beat by up to"
                color: Theme.textMuted
            }
            Slider {
                id: toleranceSlider
                objectName: "toleranceSlider"
                Layout.preferredWidth: Theme.scaled(260)
                from: 2
                to: root.controller.maximumToleranceMs
                stepSize: 1
                snapMode: Slider.SnapAlways
                // What is staged was staged at this tolerance.
                enabled: root.controller.stagedCount === 0 && !root.controller.busy
                value: root.controller.toleranceMs
                onMoved: root.controller.toleranceMs = Math.round(value)
                Accessible.name: "How far off the beat, in milliseconds"
            }
            Label {
                objectName: "toleranceLabel"
                text: root.controller.toleranceMs + " ms"
                Layout.preferredWidth: Theme.scaled(56)
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            visible: root.controller.checked
            Label {
                objectName: "alignSummary"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.bold: true
                text: alignList.count === 0
                    ? "No cue point is off its beat by " + root.controller.toleranceMs + " ms or less."
                    : root.plural(root.controller.cuePointCount, "cue point", "cue points") + " off the beat in "
                      + root.plural(alignList.count, "track", "tracks")
                      + (root.controller.tickedCount > 0 ? ", " + root.controller.tickedCount + " ticked" : "")
            }
            Button {
                objectName: "selectAllButton"
                flat: true
                text: "Select All"
                enabled: root.controller.stagedCount === 0 && root.controller.tickedCount < root.controller.tickableCount
                onClicked: root.controller.tickAll(true)
            }
            Button {
                objectName: "selectNoneButton"
                flat: true
                text: "Select None"
                enabled: root.controller.stagedCount === 0 && root.controller.tickedCount > 0
                onClicked: root.controller.tickAll(false)
            }
            Button {
                objectName: "stageButton"
                text: root.controller.stagedCount > 0 ? "Unstage" : "Stage Selected"
                highlighted: root.controller.stagedCount === 0
                enabled: !root.controller.busy && !root.controller.writing
                    && (root.controller.stagedCount > 0 || root.controller.tickedCount > 0)
                ToolTip.visible: hovered
                ToolTip.text: root.controller.stagedCount > 0
                    ? "Take back what is staged. Nothing was written."
                    : "Stage moving the listed cue points of the ticked tracks onto their beats. Align writes it, "
                      + "backed up first."
                onClicked: root.controller.stagedCount > 0 ? root.controller.unstage() : root.controller.stage()
            }
        }

        Label {
            objectName: "alignError"
            Layout.fillWidth: true
            visible: root.controller.errorMessage.length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: root.controller.errorMessage
        }

        ListView {
            id: alignList
            objectName: "alignList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            // Room to scroll the last row clear of the Save overlay.
            bottomMargin: 80
            model: root.controller.tracks
            spacing: 2
            ScrollBar.vertical: ScrollBar {}

            delegate: Rectangle {
                id: alignRow
                required property int index
                required property string key
                required property string format
                required property string title
                required property string artist
                required property int offCount
                required property int largestOffMs
                required property string detail
                required property bool sameOffset
                required property bool ticked
                required property bool staged
                objectName: "alignRow_" + key
                width: ListView.view ? ListView.view.width : 0
                height: rowLayout.implicitHeight + 2 * Theme.tightSpacing
                radius: Theme.cornerRadius
                color: rowHover.hovered ? Theme.rowHover : Theme.surface
                HoverHandler { id: rowHover }

                RowLayout {
                    id: rowLayout
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.tightSpacing
                    anchors.rightMargin: Theme.rowSpacing
                    spacing: Theme.rowSpacing

                    SeabassCheckBox {
                        objectName: "rowTick"
                        enabled: !alignRow.sameOffset && root.controller.stagedCount === 0 && !root.controller.busy
                            && !root.controller.writing
                        checked: alignRow.ticked
                        ToolTip.visible: hovered
                        ToolTip.text: alignRow.sameOffset
                            ? "Every cue point of this track is off by the same amount: its beat grid is more likely "
                              + "shifted than its cue points misplaced, so they are not moved."
                            : alignRow.staged ? "Staged: press Align to write it."
                            : "Tick to move the cue points listed here onto their beats."
                        onToggled: {
                            root.controller.setTicked(alignRow.index, checked);
                            checked = Qt.binding(() => alignRow.ticked);
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing
                            Label {
                                objectName: "rowTitle"
                                text: alignRow.title
                                elide: Text.ElideRight
                                Layout.maximumWidth: implicitWidth
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                            }
                            StatusBadge {
                                objectName: "rowState"
                                label: alignRow.sameOffset ? "All off by " + alignRow.largestOffMs + " ms"
                                    : alignRow.staged ? "Staged"
                                    : root.plural(alignRow.offCount, "cue point", "cue points") + ", up to "
                                      + alignRow.largestOffMs + " ms"
                                badgeColor: alignRow.sameOffset ? Theme.warnText : alignRow.staged ? Theme.good : Theme.textMuted
                                tooltipText: alignRow.sameOffset
                                    ? "The beat grid looks shifted. Analysing the track again, or copying rekordbox's "
                                      + "beat grid, is the way to put that right."
                                    : alignRow.detail
                            }
                            Item { Layout.fillWidth: true }
                        }
                        Label {
                            objectName: "rowDetail"
                            Layout.fillWidth: true
                            text: alignRow.artist.length > 0 ? alignRow.artist + ": " + alignRow.detail : alignRow.detail
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                    Label {
                        objectName: "rowFormat"
                        text: root.formatLabel(alignRow.format)
                        color: Theme.textMuted
                        font.pointSize: Theme.fontTiny
                    }
                }
            }

            EmptyState {
                objectName: "alignEmptyState"
                anchors.centerIn: parent
                visible: root.controller.checked && !root.controller.busy && alignList.count === 0
                iconName: "checkmark"
                tone: "good"
                text: root.controller.tracksMeasured === 0 ? "No track here has both cue points and a beat grid."
                                                           : "Every cue point is on its beat, or far from one."
                detail: root.controller.tracksMeasured === 0 ? ""
                    : root.plural(root.controller.tracksMeasured, "track", "tracks") + " with cue points checked. "
                      + "Move the slider to look further from the beat."
            }
        }
    }

    BusyOverlay {
        objectName: "alignBusyOverlay"
        anchors.fill: parent
        busy: root.controller.busy
        label: "Measuring cue points against the beat grids..."
        cancellable: true
        onCancelRequested: root.controller.cancelScan()
    }
}
