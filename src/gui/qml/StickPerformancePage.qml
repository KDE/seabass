import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// USB Stick Performance: reads real files on the stick the three ways a
// DJ player reads it, turns that into a DJ Workload Score and a verdict
// per player generation, and offers an optional write test for the two
// write workloads a stick sees from the computer. The measurement itself
// never writes; the write test writes throwaway files into a hidden
// folder of its own and removes them again. Design and sources:
// docs/design/stick-performance-mockups.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    // The stick root. Empty is tolerated when a catalog path is given
    // (the root is then its parent); a stick without a library needs it.
    property string mountPoint: ""

    // Overridable so a test can hand in a fake with known numbers; the
    // real app never sets it.
    property var controller: realController

    StickPerformanceController {
        id: realController
    }

    function megabytesPerSecond(bytesPerSecond) {
        if (!bytesPerSecond || bytesPerSecond <= 0) return "n/a";
        var mb = bytesPerSecond / 1e6;
        return (mb >= 10 ? mb.toFixed(0) : mb.toFixed(1));
    }

    function milliseconds(ms) {
        if (ms === undefined || ms === null || ms <= 0) return "n/a";
        return ms >= 10 ? ms.toFixed(0) : ms.toFixed(1);
    }

    // "0.05 s", "7 s", "1.5 min", "2 h 10 min"
    function seconds(value) {
        if (value === undefined || value === null || value <= 0) return "n/a";
        if (value < 1) return value.toFixed(2) + " s";
        if (value < 10) return value.toFixed(1) + " s";
        if (value < 120) return Math.round(value) + " s";
        if (value < 3600) return (value / 60).toFixed(value < 600 ? 1 : 0) + " min";
        var hours = Math.floor(value / 3600);
        return hours + " h " + Math.round((value - hours * 3600) / 60) + " min";
    }

    function verdictColor(key) {
        if (key === "fine") return Theme.good;
        if (key === "slower") return Theme.warnIcon;
        if (key === "sluggish") return Theme.danger;
        return Theme.textMuted;
    }

    // The score is relative to a current stick, so its colour follows the
    // speed class rather than the verdicts: a slow-but-fine stick reads
    // amber here and green in the rows below, which is the whole point.
    function speedClassColor(key) {
        if (key === "veryfast" || key === "fast") return Theme.good;
        if (key === "average") return Theme.info;
        if (key === "slow") return Theme.warnIcon;
        if (key === "veryslow") return Theme.danger;
        return Theme.textMuted;
    }

    function wearColor(state) {
        if (state === "healthy") return Theme.good;
        if (state === "watch") return Theme.warnIcon;
        if (state === "failing") return Theme.danger;
        return Theme.textMuted;
    }


    // One column width for the player-group names and one for the badges,
    // shared by every row, so the three columns line up down the page.
    // The badge width is a minimum, not a fixed width: the verdict badges
    // all fit in it, and a longer one ("NO SIGN OF WEAR") grows rather
    // than spilling its text past its own border.
    readonly property real advisoryGroupWidth: 250
    readonly property real badgeWidth: 96

    readonly property bool hasResults: Object.keys(controller.score).length > 0
    readonly property bool hasWriteResults: Object.keys(controller.writeEstimate).length > 0
    readonly property bool hasWearResults: Object.keys(controller.wearAssessment).length > 0

    Component.onCompleted: controller.measureOnOpen(root.stickLabel, root.rekordboxPath, root.enginePath, root.mountPoint)

    header: ToolBar {
        // See StickStatisticsPage.qml's header comment: every side zeroed
        // so the header's inset is Theme.pageMargin and nothing else.
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
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "USB Stick Performance"
                // Leaving mid-operation waits for the current file; the
                // probes now stop within a megabyte, but the page says so
                // rather than letting the window freeze on a click.
                backEnabled: !controller.anyBusy
                backDisabledTooltip: "Cancel or wait for the measurement to finish before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: controller.busy; visible: controller.busy; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    PageScrollView {
        objectName: "scroller"
        anchors.fill: parent
        padding: Theme.pageMargin

        ColumnLayout {
            width: parent.width
            spacing: Theme.sectionSpacing

            component StatTile: ColumnLayout {
                id: statTile
                property string label
                property string value
                property string unit: ""
                property string note: ""
                property color valueColor: Theme.text
                // Equal columns in whichever grid holds these, so a long
                // note under one tile does not push its neighbours.
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                // Top-aligned, so a tile whose note wraps to two lines does
                // not float its number above its neighbours'.
                Layout.alignment: Qt.AlignTop
                spacing: Theme.tightSpacing / 3
                RowLayout {
                    spacing: Theme.tightSpacing
                    StatValue { text: statTile.value; color: statTile.valueColor }
                    Label {
                        text: statTile.unit
                        visible: statTile.unit.length > 0
                        color: Theme.textMuted
                        font.family: Theme.dataFamily
                        Layout.alignment: Qt.AlignBaseline
                    }
                }
                TableHeaderLabel { label: statTile.label }
                Label {
                    // Wrapping is what lets the columns be equal: an
                    // unwrapped note sets the column's minimum width to
                    // its own length.
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: statTile.note
                    visible: statTile.note.length > 0
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                }
            }

            Label {
                objectName: "errorMessage"
                visible: controller.errorMessage.length > 0
                text: controller.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // -- Nothing to read: offer the throwaway-file measurement ------
            Rectangle {
                objectName: "scratchNotice"
                visible: controller.needsScratchFiles && !controller.busy
                Layout.fillWidth: true
                implicitHeight: scratchRow.implicitHeight + Theme.cardPadding
                color: Theme.warnBg
                border.color: Theme.warnBorder
                radius: 4

                RowLayout {
                    id: scratchRow
                    anchors.fill: parent
                    anchors.margins: Theme.rowSpacing
                    spacing: Theme.rowSpacing
                    Label {
                        text: "⚠"
                        font.family: "Noto Sans Symbols2"
                        font.pointSize: Theme.fontMedium
                        color: Theme.warnIcon
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.warnText
                        text: "There is nothing on this stick to read. Measuring it means writing about 22 MiB of "
                            + "throwaway files into a hidden folder, reading them back, and removing them; that also "
                            + "gives the write results below."
                    }
                    Button {
                        objectName: "scratchMeasureButton"
                        text: "Measure With Throwaway Files"
                        enabled: !controller.anyBusy
                        onClicked: controller.measureWithScratchFiles(root.stickLabel, root.mountPoint)
                    }
                }
            }

            // -- DJ Workload Score ---------------------------------------
            // No bordered card around a single block: it would indent the
            // section off the page's one left line (see Theme.qml's spacing
            // scale and the alignment note in the design docs).
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Subtitle { text: "DJ Workload Score" }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.cardPadding
                        Label {
                            objectName: "scoreValue"
                            text: root.hasResults ? controller.score.score : "n/a"
                            font.family: Theme.dataFamily
                            font.weight: Font.Medium
                            font.pointSize: Theme.titleLarge
                            color: root.hasResults ? root.speedClassColor(controller.score.speedClassKey) : Theme.textMuted
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing / 3
                            Label {
                                objectName: "speedClass"
                                text: root.hasResults ? controller.score.speedClass + " by today's standards" : "Not measured yet"
                                font.family: Theme.titleFamily
                                font.weight: Theme.cardTitleWeight
                                font.pointSize: Theme.fontLarge
                            }
                            Label {
                                objectName: "setWaitText"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textMuted
                                text: root.hasResults ? controller.score.setWaitText : ""
                            }
                            Label {
                                objectName: "trendLine"
                                Layout.fillWidth: true
                                visible: root.hasResults && (controller.trend.summary || "").length > 0
                                wrapMode: Text.WordWrap
                                color: controller.trend.state === "slowing" || controller.trend.state === "worsened"
                                    || controller.trend.state === "unavailable"
                                    ? Theme.warnText : Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: controller.trend.summary || ""
                            }
                        }
                    }

                    GridLayout {
                        visible: root.hasResults
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: Theme.cardPadding
                        rowSpacing: Theme.tightSpacing
                        StatTile {
                            label: "Browsing"
                            value: root.hasResults ? controller.score.browseScore : ""
                            note: "per search or scroll: " + root.seconds(controller.score.browseActionSeconds)
                            valueColor: root.verdictColor(controller.score.browseVerdict)
                        }
                        StatTile {
                            label: "Track loads"
                            value: root.hasResults ? controller.score.trackLoadScore : ""
                            note: "per load: " + root.seconds(controller.score.trackLoadSeconds)
                            valueColor: root.verdictColor(controller.score.trackLoadVerdict)
                        }
                        StatTile {
                            label: "Plugging in"
                            value: root.hasResults ? controller.score.mountScore : ""
                            note: "until the library shows: " + root.seconds(controller.score.mountSeconds)
                            valueColor: root.verdictColor(controller.score.mountVerdict)
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "100 is a current USB 3 stick on the USB 2.0 port every player has: it models a two-hour set "
                            + "(200 searches and scrolls, 40 track loads, one plug-in) and compares the waiting. "
                            + "The colours say whether anyone would notice: green is under what a person perceives as a pause."
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "measureButton"
                            text: controller.busy ? "Cancel" : (root.hasResults ? "Measure Again" : "Measure")
                            enabled: controller.busy || !controller.anyBusy
                            onClicked: controller.busy ? controller.cancel()
                                : controller.facts.sampleKind === "scratch"
                                ? controller.measureWithScratchFiles(root.stickLabel, root.mountPoint)
                                : controller.measure(root.stickLabel, root.rekordboxPath, root.enginePath, root.mountPoint)
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: (root.hasResults ? "Measured " + controller.measuredAt + " · " : "")
                                + (controller.facts.sampleKind === "scratch"
                                   ? "Measured on throwaway files written for the purpose and removed again."
                                   : "Reads real files already on the stick, never writes.")
                        }
                    }
                }
            }

            // -- What was measured ---------------------------------------
            // No bordered card around a single block: it would indent the
            // section off the page's one left line (see Theme.qml's spacing
            // scale and the alignment note in the design docs).
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasResults
                spacing: Theme.rowSpacing
                Subtitle { text: "What was measured" }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: Theme.cardPadding
                        rowSpacing: Theme.tightSpacing
                        StatTile {
                            label: "Streaming read"
                            value: root.megabytesPerSecond(controller.measurement.streamingBytesPerSecond)
                            unit: "MB/s"
                            note: "Long reads of audio files"
                        }
                        StatTile {
                            label: "Small random read"
                            value: root.milliseconds(controller.measurement.randomReadMedianMs)
                            unit: "ms"
                            note: "4 KiB from a random spot, median; p95 " + root.milliseconds(controller.measurement.randomReadP95Ms) + " ms"
                        }
                        StatTile {
                            label: "Small file open + read"
                            value: controller.measurement.smallFilesRead > 0
                                ? Math.round(controller.measurement.smallFileOpensPerSecond) : "n/a"
                            unit: "files/s"
                            note: controller.measurement.smallFilesRead === 0 ? "No small files found on this stick"
                                : controller.facts.sampleKind === "library" ? "Analysis files, like a track load"
                                : "Small files, like a track load's analysis file"
                        }
                        StatTile {
                            label: "Link to this computer"
                            value: controller.filesystemInfo.usbSpeedMbps >= 5000 ? "USB 3"
                                : controller.filesystemInfo.usbSpeedMbps >= 480 ? "USB 2.0"
                                : controller.filesystemInfo.usbSpeedMbps > 0 ? "USB 1.1" : "n/a"
                            note: controller.filesystemInfo.usbSpeedLabel || "USB speed unknown"
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: (controller.filesystemInfo.displayName || "Unknown filesystem")
                            + (controller.facts.clusterBytes > 0 ? ", " + Theme.humanBytes(controller.facts.clusterBytes) + " clusters" : "")
                            + (controller.facts.sampleKind === "library"
                               ? " · " + Number(controller.facts.analysisFiles).toLocaleString(Qt.locale(), "f", 0) + " analysis files in "
                                 + Number(controller.facts.analysisFolders).toLocaleString(Qt.locale(), "f", 0) + " folders"
                                 + " · " + Number(controller.facts.audioFiles).toLocaleString(Qt.locale(), "f", 0) + " audio files"
                               : controller.facts.sampleKind === "files"
                               ? " · no DJ library; sampled " + Number(controller.facts.audioFiles).toLocaleString(Qt.locale(), "f", 0)
                                 + " larger and " + Number(controller.facts.analysisFiles).toLocaleString(Qt.locale(), "f", 0) + " small files on the stick"
                               : " · no files on the stick; measured on throwaway files")
                    }
                    Label {
                        objectName: "tailLine"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: (controller.measurement.randomReadOutliers + controller.measurement.smallFileOutliers) > 0
                            ? Theme.warnText : Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: (controller.measurement.randomReadOutliers + controller.measurement.smallFileOutliers) === 0
                            ? "Small-read tail is flat: none of the " + (controller.measurement.randomReads + controller.measurement.smallFilesRead)
                              + " small reads took over five times the median, which is what healthy flash looks like."
                            : (controller.measurement.randomReadOutliers + controller.measurement.smallFileOutliers) + " of "
                              + (controller.measurement.randomReads + controller.measurement.smallFilesRead)
                              + " small reads took over five times the median. Weak cells being retried look like this; run the wear check below."
                    }
                }
            }

            // -- On a player ---------------------------------------------
            // No bordered card around a single block: it would indent the
            // section off the page's one left line (see Theme.qml's spacing
            // scale and the alignment note in the design docs).
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasResults
                spacing: Theme.rowSpacing
                Subtitle { text: "On a player" }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "Players are grouped by the database they read, because that decides which measurement matters. "
                            + "Each row is judged on the measurement that matters for it."
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Repeater {
                            model: controller.advisories
                            delegate: Rectangle {
                                id: advisoryRow
                                required property var modelData
                                required property int index
                                objectName: "advisoryRow"
                                Layout.fillWidth: true
                                implicitHeight: advisoryLayout.implicitHeight + 2 * Theme.rowSpacing
                                color: "transparent"

                                // A rule under each row rather than a filled
                                // band: a fill needs an inset for its text,
                                // and that inset is a second left edge.
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 1
                                    color: Theme.borderSubtle
                                    visible: advisoryRow.index < controller.advisories.length - 1
                                }

                                RowLayout {
                                    id: advisoryLayout
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.rowSpacing

                                    ColumnLayout {
                                        // Fixed, not preferred: a preferred width
                                        // shrinks to the text and the badge column
                                        // then wanders from row to row.
                                        Layout.minimumWidth: root.advisoryGroupWidth
                                        Layout.preferredWidth: root.advisoryGroupWidth
                                        Layout.maximumWidth: root.advisoryGroupWidth
                                        spacing: Theme.tightSpacing / 3
                                        Label { text: advisoryRow.modelData.group; font.bold: true }
                                        Label {
                                            Layout.fillWidth: true
                                            text: advisoryRow.modelData.players
                                            color: Theme.textMuted
                                            font.pointSize: Theme.fontTiny
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    StatusBadge {
                                        Layout.minimumWidth: root.badgeWidth
                                        Layout.alignment: Qt.AlignVCenter
                                        label: advisoryRow.modelData.verdictLabel
                                        badgeColor: root.verdictColor(advisoryRow.modelData.verdict)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: advisoryRow.modelData.summary
                                        wrapMode: Text.WordWrap
                                        font.pointSize: Theme.fontSmall
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // -- Wear check ------------------------------------------------
            // No bordered card around a single block: it would indent the
            // section off the page's one left line (see Theme.qml's spacing
            // scale and the alignment note in the design docs).
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Subtitle { text: "Wear" }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "A stick cannot report how worn it is, but worn flash gives itself away: the controller "
                            + "retries weak cells, so some files read far slower than the rest, and later some do not read "
                            + "at all. This reads every file on the stick once, the way a backup would, and looks for both. "
                            + "Read-only; a few minutes on a big stick."
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "wearButton"
                            text: controller.wearBusy ? "Cancel" : (root.hasWearResults ? "Check for Wear Again" : "Check for Wear")
                            enabled: controller.wearBusy || !controller.anyBusy
                            onClicked: controller.wearBusy
                                ? controller.cancelWearCheck()
                                : controller.checkWear(root.stickLabel, root.rekordboxPath, root.enginePath, root.mountPoint)
                        }
                        // ProgressTrack, not a bare ProgressBar: Breeze's
                        // own delegate reads its background's edges before
                        // the background exists and logs a TypeError per
                        // frame while indeterminate.
                        ProgressTrack {
                            visible: controller.wearBusy
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, controller.wearBytesTotal)
                            value: controller.wearBytesDone
                            indeterminate: controller.wearBytesTotal === 0
                        }
                        Label {
                            visible: controller.wearBusy
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: controller.wearBytesTotal > 0
                                ? Theme.humanBytes(controller.wearBytesDone) + " of " + Theme.humanBytes(controller.wearBytesTotal)
                                  + ", " + controller.wearFilesDone + " of " + controller.wearFilesTotal + " files"
                                : "Counting files..."
                        }
                        Label {
                            objectName: "wearErrorMessage"
                            Layout.fillWidth: true
                            visible: controller.wearErrorMessage.length > 0
                            text: controller.wearErrorMessage
                            color: Theme.danger
                            wrapMode: Text.WordWrap
                        }
                    }

                    RowLayout {
                        visible: root.hasWearResults
                        Layout.fillWidth: true
                        spacing: Theme.rowSpacing
                        StatusBadge {
                            objectName: "wearBadge"
                            Layout.minimumWidth: root.badgeWidth
                            Layout.alignment: Qt.AlignTop
                            label: (controller.wearAssessment.label || "").toUpperCase()
                            badgeColor: root.wearColor(controller.wearAssessment.state)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.tightSpacing
                            Label {
                                objectName: "wearSummary"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: controller.wearAssessment.summary || ""
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: "Read " + Theme.humanBytes(controller.wearCheck.bytesRead) + " in "
                                    + root.seconds(controller.wearCheck.seconds) + " at a median of "
                                    + root.megabytesPerSecond(controller.wearCheck.medianBytesPerSecond) + " MB/s per file."
                            }
                            Repeater {
                                model: (controller.wearCheck.unreadable || []).slice(0, 10)
                                delegate: Label {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    color: Theme.danger
                                    font.pointSize: Theme.fontSmall
                                    text: "Could not read: " + modelData
                                }
                            }
                            Repeater {
                                model: (controller.wearCheck.slow || []).slice(0, 10)
                                delegate: Label {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    color: Theme.warnText
                                    font.pointSize: Theme.fontSmall
                                    text: root.megabytesPerSecond(modelData.bytesPerSecond) + " MB/s: " + modelData.path
                                }
                            }
                        }
                    }
                }
            }

            // -- Write test (optional) -----------------------------------
            // No bordered card around a single block: it would indent the
            // section off the page's one left line (see Theme.qml's spacing
            // scale and the alignment note in the design docs).
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.rowSpacing
                Subtitle { text: "Write Test" }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.rowSpacing

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "Optional. Writes about 22 MiB of throwaway files into a hidden folder on the stick and removes "
                            + "them again; nothing in the library is touched. It answers two questions: how long saving cue "
                            + "points takes, and how long a library export from rekordbox or Engine DJ takes. Skipped by "
                            + "default because every write wears a stick a little."
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.rowSpacing
                        Button {
                            objectName: "writeTestButton"
                            text: controller.writeBusy ? "Cancel" : (root.hasWriteResults ? "Run Write Test Again" : "Run Write Test")
                            enabled: controller.writeBusy || !controller.anyBusy
                            onClicked: controller.writeBusy ? controller.cancelWrites()
                                : controller.measureWrites(root.rekordboxPath, root.enginePath, root.mountPoint)
                        }
                        BusyIndicator {
                            running: controller.writeBusy
                            visible: controller.writeBusy
                            implicitWidth: 20
                            implicitHeight: 20
                        }
                        Label {
                            objectName: "writeErrorMessage"
                            Layout.fillWidth: true
                            visible: controller.writeErrorMessage.length > 0
                            text: controller.writeErrorMessage
                            color: Theme.danger
                            wrapMode: Text.WordWrap
                        }
                    }

                    GridLayout {
                        visible: root.hasWriteResults
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: Theme.cardPadding
                        rowSpacing: Theme.tightSpacing
                        StatTile {
                            label: "Streaming write"
                            value: root.megabytesPerSecond(controller.writeMeasurement.streamingWriteBytesPerSecond)
                            unit: "MB/s"
                            note: "Copying audio onto the stick"
                        }
                        StatTile {
                            label: "Small file write"
                            value: root.milliseconds(controller.writeMeasurement.smallFileWriteMedianMs)
                            unit: "ms"
                            note: "16 KiB file written and flushed, like an analysis file"
                        }
                        StatTile {
                            label: "In-place update"
                            value: root.milliseconds(controller.writeMeasurement.inPlaceUpdateMedianMs)
                            unit: "ms"
                            note: "4 KiB overwritten and flushed, like a database page"
                        }
                    }

                    ColumnLayout {
                        visible: root.hasWriteResults
                        Layout.fillWidth: true
                        spacing: Theme.tightSpacing
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.rowSpacing
                            StatusBadge {
                                Layout.minimumWidth: root.badgeWidth
                                label: controller.writeEstimate.cueSaveVerdictLabel || "NOT MEASURED"
                                badgeColor: root.verdictColor(controller.writeEstimate.cueSaveVerdict)
                            }
                            Label {
                                objectName: "cueSaveEstimate"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: "Saving cue points on one track: about " + root.seconds(controller.writeEstimate.cueSaveSeconds)
                                    + " (two analysis files rewritten, a few database pages updated)."
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.rowSpacing
                            StatusBadge {
                                Layout.minimumWidth: root.badgeWidth
                                label: controller.writeEstimate.exportVerdictLabel || "NOT MEASURED"
                                badgeColor: root.verdictColor(controller.writeEstimate.exportVerdict)
                            }
                            Label {
                                objectName: "exportEstimate"
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: "Exporting 100 tracks from rekordbox or Engine DJ: about "
                                    + root.seconds(controller.writeEstimate.exportHundredTracksSeconds)
                                    + " (" + root.seconds(controller.writeEstimate.exportTrackSeconds) + " per 8 MB track)."
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: "Wrote " + Theme.humanBytes(controller.writeMeasurement.bytesWritten)
                                + " of throwaway files and removed them again."
                        }
                    }
                }
            }
        }
    }

    // A cancelled measurement stays on the page, unlike Library
    // Statistics' scan: the page is still useful with nothing measured,
    // and the button reads "Measure" again.
    BusyOverlay {
        anchors.fill: parent
        busy: controller.busy
        label: controller.facts.sampleKind === "scratch" || controller.needsScratchFiles
            ? "Writing throwaway files and reading them back..." : "Measuring the stick..."
        cancellable: controller.busy
        onCancelRequested: controller.cancel()
    }
}
