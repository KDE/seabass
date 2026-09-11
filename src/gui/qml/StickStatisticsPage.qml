import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// USB Stick Statistics: filesystem/hardware facts, per-catalog library
// stats, a Filelight-style disk usage breakdown, and a local read-speed
// benchmark with history. Read-only, like Browse Library -- this page
// never writes anything to the stick.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController
    required property var appSettingsController

    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)

    StickStatisticsController {
        id: controller
    }

    property string currentSource: root.rekordboxPath.length > 0 ? "rekordbox"
        : (root.enginePath.length > 0 ? "engine" : "onelibrary")
    // Drill-down stack for the treemap: root category view first, pushed
    // into when a box with children is clicked.
    property var treemapStack: []
    readonly property var currentTreemapNode: treemapStack.length > 0 ? treemapStack[treemapStack.length - 1] : null

    function humanBytes(bytes) {
        if (!bytes || bytes <= 0) return "0 B";
        var units = ["B", "KiB", "MiB", "GiB", "TiB"];
        var value = bytes;
        var unitIndex = 0;
        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024;
            unitIndex++;
        }
        return value.toFixed(unitIndex === 0 ? 0 : 1) + " " + units[unitIndex];
    }

    // A strong asymmetry in cue-point counts between rekordbox and Engine
    // is the signal Sebas actually spotted by eye comparing tabs on this
    // page -- surfacing it directly means noticing it doesn't depend on
    // remembering to compare two numbers across a tab switch. More than
    // 2x apart is well past normal per-format variation (e.g. Engine's
    // single memory-cue slot vs. rekordbox's richer per-track memory
    // cues) and points at cues that simply never propagated -- see
    // domain::matchTracks()'s own fix history for a real, confirmed
    // cause of exactly this (a duration-read failure on one side
    // silently blocking the cross-format match Sync Cue Points needs).
    readonly property bool cueCountsLookOutOfSync: {
        var rbCues = controller.rekordboxStats.totalCuePoints || 0;
        var enCues = controller.engineStats.totalCuePoints || 0;
        if (Object.keys(controller.rekordboxStats).length === 0 || Object.keys(controller.engineStats).length === 0) {
            return false;
        }
        var maxCues = Math.max(rbCues, enCues);
        var minCues = Math.min(rbCues, enCues);
        return maxCues > 0 && (minCues / maxCues) < 0.5;
    }

    // isoUtc: an ISO 8601 UTC timestamp as stored by StickBenchmarkHistory
    // (e.g. "2026-08-30T13:34:00Z"). JS Date parses that format natively
    // and Qt.formatDateTime renders it in the user's own locale/timezone,
    // no manual parsing needed.
    function formatTimestamp(isoUtc) {
        if (!isoUtc) return "";
        var date = new Date(isoUtc);
        if (isNaN(date.getTime())) return isoUtc;
        return Qt.formatDateTime(date, "MMM d, yyyy h:mm AP");
    }

    function statsForSource(source) {
        if (source === "engine") return controller.engineStats;
        if (source === "onelibrary") return controller.oneLibraryStats;
        return controller.rekordboxStats;
    }

    function rescan() {
        root.treemapStack = [];
        controller.scan(root.stickLabel, root.rekordboxPath, root.enginePath);
    }

    Connections {
        target: controller
        function onResultsChanged() {
            if (controller.diskUsage && controller.diskUsage.root) {
                root.treemapStack = [controller.diskUsage.root];
            }
        }
    }

    Component.onCompleted: rescan()

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Library Statistics"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator { running: controller.busy; visible: controller.busy; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    // A plain Flickable, not ScrollView: KDE's org.kde.desktop style
    // positions a ScrollView's attached scrollbar itself, and doesn't
    // know how to place a foreign BigScrollBar -- it was rendering
    // unanchored at the content's top-left instead of docked to the
    // right edge. Every other scrollable page in this app pairs
    // BigScrollBar with a real Flickable/ListView instead (see e.g.
    // SyncPage.qml's own comment on the same pairing) precisely because
    // that positions the attached scrollbar itself, independent of style.
    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: width
        contentHeight: statsColumn.height
        clip: true
        // See AboutPage.qml's own comment on this same fix -- a Flickable
        // is draggable by default even with nothing to scroll.
        interactive: contentHeight > height
        ScrollBar.vertical: BigScrollBar {}

        ColumnLayout {
            id: statsColumn
            width: parent.width
            spacing: 16

            Label {
                visible: controller.errorMessage.length > 0
                text: controller.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // -- Filesystem & capacity --------------------------------
            GroupBox {
                label: Subtitle { text: "Filesystem" }
                Layout.fillWidth: true
                visible: Object.keys(controller.filesystemInfo).length > 0

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            text: controller.filesystemInfo.displayName || "Unknown"
                            font.bold: true
                            font.pointSize: Theme.fontLarge
                        }
                        Rectangle {
                            radius: 3
                            color: controller.filesystemInfo.recommendedForDjHardware ? Theme.warnBg : Theme.dangerBg
                            border.color: controller.filesystemInfo.recommendedForDjHardware ? Theme.warnBorder : Theme.dangerBorder
                            implicitWidth: recLabel.implicitWidth + 12
                            implicitHeight: recLabel.implicitHeight + 6
                            Label {
                                id: recLabel
                                anchors.centerIn: parent
                                text: controller.filesystemInfo.recommendedForDjHardware
                                    ? "Good for DJ hardware" : "Not typical for DJ hardware"
                                font.pointSize: Theme.fontTiny
                                font.bold: true
                                color: controller.filesystemInfo.recommendedForDjHardware ? Theme.warnText : Theme.dangerText
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: controller.filesystemInfo.usbSpeedLabel || "USB speed unknown"
                            color: Theme.textMuted
                        }
                    }

                    Label {
                        text: "Max file size: " + (controller.filesystemInfo.maxFileSize || "Unknown")
                        color: Theme.textMuted
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: controller.filesystemInfo.hardwareNotes || ""
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 18
                            radius: 4
                            color: Theme.groupBackground
                            border.color: Theme.borderSubtle
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                radius: 4
                                width: parent.width * (controller.filesystemInfo.totalBytes > 0
                                    ? (1 - controller.filesystemInfo.freeBytes / controller.filesystemInfo.totalBytes) : 0)
                                color: Theme.accent
                            }
                        }
                        Label {
                            text: root.humanBytes(controller.filesystemInfo.totalBytes - controller.filesystemInfo.freeBytes)
                                + " used of " + root.humanBytes(controller.filesystemInfo.totalBytes)
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                        }
                    }
                }
            }

            // -- Library statistics ------------------------------------
            GroupBox {
                label: Subtitle { text: "Library Statistics" }
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    LibrarySourceToggle {
                        current: root.currentSource
                        hasRekordbox: Object.keys(controller.rekordboxStats).length > 0
                        hasEngine: Object.keys(controller.engineStats).length > 0
                        hasOneLibrary: Object.keys(controller.oneLibraryStats).length > 0
                        onSourceRequested: (value) => root.currentSource = value
                    }

                    Rectangle {
                        visible: root.cueCountsLookOutOfSync
                        Layout.fillWidth: true
                        implicitHeight: syncWarningRow.implicitHeight + 16
                        color: Theme.warnBg
                        border.color: Theme.warnBorder
                        radius: 4

                        RowLayout {
                            id: syncWarningRow
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8
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
                                text: "DeviceLibrary has " + (controller.rekordboxStats.totalCuePoints || 0)
                                    + " cue point(s), Engine has " + (controller.engineStats.totalCuePoints || 0)
                                    + "; these catalogs look out of sync."
                            }
                            Button {
                                text: "Go to Sync Cue Points"
                                onClicked: root.syncRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
                            }
                        }
                    }

                    ColumnLayout {
                        id: statsSection
                        Layout.fillWidth: true
                        spacing: 10
                        visible: Object.keys(root.statsForSource(root.currentSource)).length > 0

                        readonly property var stats: root.statsForSource(root.currentSource)

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 4
                            columnSpacing: 16
                            rowSpacing: 8

                            component StatTile: ColumnLayout {
                                id: statTile
                                property string label
                                property string value
                                spacing: 2
                                StatValue { text: statTile.value }
                                // Qualified with statTile.: an unqualified `label: label`
                                // here would bind TableHeaderLabel's own `label` property
                                // to itself (same scoping gotcha PageTitle.qml hit with
                                // `text`), not to StatTile's outer one.
                                TableHeaderLabel { label: statTile.label }
                            }

                            StatTile { label: "Tracks"; value: statsSection.stats.trackCount || 0 }
                            StatTile { label: "Playlists"; value: statsSection.stats.playlistCount || 0 }
                            StatTile { label: "Cue points"; value: statsSection.stats.totalCuePoints || 0 }
                            StatTile {
                                label: "Hot / memory cues"
                                value: (statsSection.stats.hotCueCount || 0) + " / " + (statsSection.stats.memoryCueCount || 0)
                            }
                            StatTile { label: "Rated tracks"; value: statsSection.stats.ratedTrackCount || 0 }
                            StatTile { label: "Commented tracks"; value: statsSection.stats.commentedTrackCount || 0 }
                            StatTile { label: "Streaming tracks"; value: statsSection.stats.streamingTrackCount || 0 }
                        }

                        RowLayout {
                            visible: Object.keys(root.statsForSource(root.currentSource).streamingTracksByService || {}).length > 0
                            spacing: 8
                            Label { text: "Streaming services:"; color: Theme.textMuted }
                            Repeater {
                                model: Object.keys(root.statsForSource(root.currentSource).streamingTracksByService || {})
                                delegate: Label {
                                    required property string modelData
                                    text: modelData + " (" + root.statsForSource(root.currentSource)
                                        .streamingTracksByService[modelData] + ")"
                                    font.bold: true
                                }
                            }
                        }

                        component DistributionSection: ColumnLayout {
                            id: distSection
                            property string title
                            property var entries  // [{label, count}] -- any order, doesn't need to be sorted by count
                            // "Tracks per key" renders each row's label as
                            // a KeyBadge (Camelot notation, colored pill)
                            // instead of plain text -- the same component
                            // Browse Library uses, per BRAINSTORM.md's own
                            // "re-use the design from the library view"
                            // ask. Every other DistributionSection (file
                            // formats, BPM) keeps plain text.
                            property bool isKeySection: false
                            // Computed from entries rather than assumed
                            // to be entries[0] -- BPM deliberately sorts
                            // by rangeStart (ascending), not by count, so
                            // the tallest bar isn't necessarily first.
                            readonly property real maxCount: {
                                var m = 0;
                                for (var i = 0; i < entries.length; i++) {
                                    if (entries[i].count > m) m = entries[i].count;
                                }
                                return m;
                            }
                            readonly property var _barColors: [
                                Theme.accent, Theme.good, Theme.conflictText, Theme.warnBorder,
                                Theme.danger, Qt.lighter(Theme.accent, 1.4), Qt.lighter(Theme.good, 1.4),
                            ]
                            Layout.fillWidth: true
                            spacing: 4
                            visible: entries.length > 0
                            Subtitle { text: distSection.title; Layout.topMargin: 8 }
                            Repeater {
                                model: entries
                                delegate: RowLayout {
                                    id: barRow
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Label {
                                        visible: !distSection.isKeySection
                                        text: barRow.modelData.label
                                        Layout.preferredWidth: 90
                                        elide: Text.ElideRight
                                    }
                                    KeyBadge {
                                        visible: distSection.isKeySection
                                        keyName: barRow.modelData.label
                                        notation: root.appSettingsController.keyNotation
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        implicitHeight: 14
                                        radius: 3
                                        color: Theme.groupBackground
                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            radius: 3
                                            width: parent.width * (distSection.maxCount > 0
                                                ? barRow.modelData.count / distSection.maxCount : 0)
                                            color: distSection._barColors[barRow.index % distSection._barColors.length]
                                        }
                                    }
                                    Label { text: barRow.modelData.count; Layout.preferredWidth: 36; horizontalAlignment: Text.AlignRight }
                                }
                            }
                        }

                        DistributionSection {
                            Layout.fillWidth: true
                            title: "Tracks per key"
                            isKeySection: true
                            entries: {
                                var stats = root.statsForSource(root.currentSource);
                                var keys = stats.tracksPerKey || {};
                                var list = [];
                                for (var k in keys) list.push({label: k, count: keys[k]});
                                list.sort((a, b) => b.count - a.count);
                                return list.slice(0, 12);
                            }
                        }

                        DistributionSection {
                            Layout.fillWidth: true
                            title: "File formats"
                            entries: {
                                var stats = root.statsForSource(root.currentSource);
                                var formats = stats.tracksPerFileFormat || {};
                                var list = [];
                                for (var f in formats) list.push({label: f, count: formats[f]});
                                list.sort((a, b) => b.count - a.count);
                                return list;
                            }
                        }

                        DistributionSection {
                            Layout.fillWidth: true
                            title: "BPM distribution"
                            entries: {
                                // Sorted by BPM (ascending), not by count
                                // like the sections above -- a BPM
                                // distribution reads as an actual shape
                                // (where the DJ's tracks cluster) only
                                // when the buckets stay in tempo order.
                                var stats = root.statsForSource(root.currentSource);
                                var buckets = stats.bpmDistribution || [];
                                var sortedBuckets = buckets.slice().sort((a, b) => a.rangeStart - b.rangeStart);
                                var list = [];
                                for (var i = 0; i < sortedBuckets.length; i++) {
                                    list.push({label: sortedBuckets[i].rangeStart + "-" + (sortedBuckets[i].rangeStart + 9), count: sortedBuckets[i].count});
                                }
                                return list;
                            }
                        }
                    }
                }
            }

            // -- Disk usage ---------------------------------------------
            GroupBox {
                label: Subtitle { text: "Disk Usage" }
                Layout.fillWidth: true
                Layout.preferredHeight: 420

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            text: "◂ Back"
                            enabled: root.treemapStack.length > 1
                            onClicked: root.treemapStack = root.treemapStack.slice(0, root.treemapStack.length - 1)
                        }
                        Label {
                            text: root.currentTreemapNode ? root.currentTreemapNode.label : ""
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: controller.diskUsage.usedBytes !== undefined
                                ? (root.humanBytes(controller.diskUsage.usedBytes) + " used, "
                                    + root.humanBytes(controller.diskUsage.freeBytes) + " free")
                                : ""
                            color: Theme.textMuted
                        }
                    }

                    TreemapView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        node: root.currentTreemapNode
                        onBoxClicked: (childNode) => root.treemapStack = root.treemapStack.concat([childNode])
                    }
                }
            }

            // -- Speed benchmark -----------------------------------------
            GroupBox {
                label: Subtitle { text: "Read-Speed Benchmark" }
                Layout.fillWidth: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Button {
                            text: controller.benchmarkRunning ? "Running..." : "Run Benchmark"
                            enabled: !controller.benchmarkRunning && !controller.busy
                            onClicked: controller.runBenchmark()
                        }
                        BusyIndicator {
                            running: controller.benchmarkRunning
                            visible: controller.benchmarkRunning
                            implicitWidth: 20
                            implicitHeight: 20
                        }
                        Label {
                            visible: controller.benchmarkErrorMessage.length > 0
                            text: controller.benchmarkErrorMessage
                            color: Theme.danger
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "Reads real sample files already on this stick to measure MiB/s, then computes a "
                            + "comparative-only score (not an absolute/manufacturer number) so sticks tested on "
                            + "this computer can be ranked against each other over time."
                    }

                    Label {
                        text: "History for this stick (" + controller.benchmarkHistory.length + " run(s))"
                        font.bold: true
                        Layout.topMargin: 8
                    }
                    Repeater {
                        model: controller.benchmarkHistory
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            contentItem: RowLayout {
                                spacing: 12
                                Label {
                                    text: root.formatTimestamp(modelData.ranAt)
                                    color: Theme.textMuted
                                    font.pointSize: Theme.fontSmall
                                }
                                Label { text: "Score: " + modelData.score; font.bold: true }
                                Label { text: modelData.usbSpeedLabel; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                                Label {
                                    text: "DB " + modelData.databaseReadMbps.toFixed(1) + " MiB/s, Audio "
                                        + modelData.audioReadMbps.toFixed(1) + " MiB/s"
                                    color: Theme.textMuted
                                    font.pointSize: Theme.fontSmall
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                    }
                    Label {
                        visible: controller.benchmarkHistory.length === 0
                        text: "No benchmark runs yet for this stick."
                        color: Theme.textMuted
                    }
                }
            }
        }
    }

    // Without this, the content area was just blank while the initial
    // scan ran -- only a small BusyIndicator tucked into the header (see
    // BusyOverlay.qml's own comment on exactly this problem elsewhere).
    // No current/total to report here yet, so this renders as the
    // indeterminate sweep animation rather than a real progress bar.
    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: controller
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: controller.busy
        label: "Scanning stick statistics..."
        cancellable: controller.scanCancellable
        onCancelRequested: controller.cancelScan()
    }
}
