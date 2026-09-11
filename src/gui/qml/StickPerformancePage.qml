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

    function verdictText(key) {
        if (key === "fine") return "FINE";
        if (key === "slower") return "SLOWER";
        if (key === "sluggish") return "SLUGGISH";
        return "NOT MEASURED";
    }

    readonly property bool hasResults: Object.keys(controller.score).length > 0
    readonly property bool hasWriteResults: Object.keys(controller.writeEstimate).length > 0

    Component.onCompleted: controller.measure(root.stickLabel, root.rekordboxPath, root.enginePath)

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
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: controller.busy; visible: controller.busy; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    PageScrollView {
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
                spacing: 2
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

            // -- DJ Workload Score ---------------------------------------
            GroupBox {
                label: Subtitle { text: "DJ Workload Score" }
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
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
                            color: Theme.text
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
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
                            text: root.hasResults ? "Measure Again" : "Measure"
                            enabled: !controller.busy
                            onClicked: controller.measure(root.stickLabel, root.rekordboxPath, root.enginePath)
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            text: (root.hasResults ? "Measured " + controller.measuredAt + " · " : "")
                                + "Reads real files already on the stick, never writes."
                        }
                    }
                }
            }

            // -- What was measured ---------------------------------------
            GroupBox {
                label: Subtitle { text: "What was measured" }
                Layout.fillWidth: true
                visible: root.hasResults

                ColumnLayout {
                    anchors.fill: parent
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
                            note: controller.measurement.smallFilesRead > 0
                                ? "Analysis files, like a track load" : "No analysis files found on this stick"
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
                            + " · " + Number(controller.facts.analysisFiles).toLocaleString(Qt.locale(), "f", 0) + " analysis files in "
                            + Number(controller.facts.analysisFolders).toLocaleString(Qt.locale(), "f", 0) + " folders"
                            + " · " + Number(controller.facts.audioFiles).toLocaleString(Qt.locale(), "f", 0) + " audio files"
                    }
                }
            }

            // -- On a player ---------------------------------------------
            GroupBox {
                label: Subtitle { text: "On a player" }
                Layout.fillWidth: true
                visible: root.hasResults

                ColumnLayout {
                    anchors.fill: parent
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
                                implicitHeight: advisoryLayout.implicitHeight + Theme.cardPadding
                                radius: 3
                                color: index % 2 === 0 ? Theme.rowOdd : "transparent"

                                RowLayout {
                                    id: advisoryLayout
                                    anchors.fill: parent
                                    anchors.margins: Theme.cardPadding / 2
                                    spacing: Theme.rowSpacing

                                    ColumnLayout {
                                        Layout.preferredWidth: 250
                                        Layout.maximumWidth: 250
                                        spacing: 2
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
                                        Layout.preferredWidth: 96
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

            // -- Write test (optional) -----------------------------------
            GroupBox {
                label: Subtitle { text: "Write Test" }
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
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
                            text: controller.writeBusy ? "Writing..." : (root.hasWriteResults ? "Run Write Test Again" : "Run Write Test")
                            enabled: !controller.writeBusy && !controller.busy
                            onClicked: controller.measureWrites(root.rekordboxPath, root.enginePath)
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
                                Layout.preferredWidth: 96
                                label: root.verdictText(controller.writeEstimate.cueSaveVerdict)
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
                                Layout.preferredWidth: 96
                                label: root.verdictText(controller.writeEstimate.exportVerdict)
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

    // A cancelled measurement takes the user back to where they came
    // from, like Library Statistics. Bound to the real controller, not
    // the overridable one: a test's fake is a plain object with no
    // signals, and Connections refuses those loudly.
    Connections {
        target: realController
        function onCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: controller.busy
        label: "Measuring the stick..."
        cancellable: controller.busy
        onCancelRequested: controller.cancel()
    }
}
