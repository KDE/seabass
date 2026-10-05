// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// USB Stick Statistics: filesystem/hardware facts, per-catalog library
// stats, and a Filelight-style disk usage breakdown. Read-only, like
// Browse Library -- this page never writes anything to the stick. How
// fast the stick is lives on its own page, StickPerformancePage.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController
    required property var appSettingsController

    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)

    // Overridable so a test can hand in a fake with known numbers; the
    // real app never sets it.
    property var controller: realController

    StickStatisticsController {
        id: realController
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
        bottomPadding: Theme.headerBottomPadding
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

    // PageScrollView, like the other scrolling pages: a Flickable with
    // BigScrollBar (not a ScrollView, whose bar KDE's style misplaces),
    // and it keeps a gutter for the bar. The plain Flickable this page
    // had kept none, and once its sections lost their boxes the
    // right-hand figures ran under the bar.
    //
    // Hidden while the scan runs: the overlay is a see-through scrim, and
    // what showed through it was an empty page -- a disabled Back button
    // and the treemap's "Nothing to show yet." right under Cancel.
    PageScrollView {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        visible: !controller.busy

        ColumnLayout {
            id: statsColumn
            width: parent.width
            spacing: Theme.sectionSpacing

            Label {
                visible: controller.errorMessage.length > 0
                text: controller.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            // -- Filesystem & capacity --------------------------------
            PageSection {
                title: "Filesystem"
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
                            radius: Theme.cornerRadius
                            color: Theme.surface
                            border.color: Theme.borderSubtle
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                radius: Theme.cornerRadius
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
            PageSection {
                title: "Library Statistics"
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

                    WarningBanner {
                        visible: root.cueCountsLookOutOfSync
                        text: "DeviceLibrary has " + (controller.rekordboxStats.totalCuePoints || 0)
                            + " cue point(s), Engine has " + (controller.engineStats.totalCuePoints || 0)
                            + "; these catalogs look out of sync."
                        Button {
                            text: "Go to Sync Cue Points"
                            onClicked: root.syncRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
                        }
                    }

                    ColumnLayout {
                        id: statsSection
                        Layout.fillWidth: true
                        spacing: 10
                        visible: Object.keys(root.statsForSource(root.currentSource)).length > 0

                        readonly property var stats: root.statsForSource(root.currentSource)

                        // #60: the analysis files whose legacy and modern
                        // cue lists disagree, counted while the cues were
                        // read. Said only once every file was examined;
                        // otherwise how many could not be read.
                        Label {
                            objectName: "cueListsNote"
                            Layout.fillWidth: true
                            visible: text.length > 0
                            wrapMode: Text.WordWrap
                            text: statsSection.stats.cueListsText || ""
                            color: (statsSection.stats.cueListsDisagree || 0) > 0 ? Theme.warnText : Theme.textMuted
                        }

                        // The page's first figures, with the cue points pie
                        // beside them at the same height.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sectionSpacing

                            GridLayout {
                                id: statsGrid
                                objectName: "statsGrid"
                                Layout.alignment: Qt.AlignTop
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

                            // Cue points: how many local tracks carry at least
                            // one cue (hot, memory or loop) against how many
                            // carry none. The pie is as tall as the figures
                            // beside it, and the counts sit in a legend so
                            // the drawing is never the only place they are.
                            RowLayout {
                                id: cueCoverageBlock
                                objectName: "cueCoverageBlock"
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: Theme.rowSpacing

                                readonly property var coverage: statsSection.stats.cueCoverage || {}
                                readonly property int withCues: coverage.withCues || 0
                                readonly property int withoutCues: coverage.withoutCues || 0
                                readonly property int total: withCues + withoutCues

                                function percent(count) {
                                    return cueCoverageBlock.total > 0
                                        ? Math.round(100 * count / cueCoverageBlock.total) + "%" : "0%";
                                }

                                CueCoveragePie {
                                    objectName: "cueCoveragePie"
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredWidth: statsGrid.height
                                    Layout.preferredHeight: statsGrid.height
                                    withCues: cueCoverageBlock.withCues
                                    withoutCues: cueCoverageBlock.withoutCues
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    spacing: Theme.tightSpacing

                                    Subtitle { text: "Cue points" }
                                    Label {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        text: "Any hot cue, memory cue or loop counts. Local tracks only."
                                        color: Theme.textMuted
                                        font.pointSize: Theme.fontSmall
                                    }

                                    // Swatch, count, share, name: one row per slice.
                                    GridLayout {
                                        columns: 4
                                        columnSpacing: Theme.rowSpacing
                                        rowSpacing: Theme.tightSpacing

                                        Rectangle {
                                            implicitWidth: Theme.chartSwatchSize
                                            implicitHeight: Theme.chartSwatchSize
                                            radius: 2
                                            color: Theme.accent
                                        }
                                        Label {
                                            objectName: "withCuesCount"
                                            Layout.alignment: Qt.AlignRight
                                            text: cueCoverageBlock.withCues
                                            font.family: Theme.dataFamily
                                        }
                                        Label {
                                            objectName: "withCuesPercent"
                                            Layout.alignment: Qt.AlignRight
                                            text: cueCoverageBlock.percent(cueCoverageBlock.withCues)
                                            font.family: Theme.dataFamily
                                            color: Theme.textMuted
                                        }
                                        Label { text: "with cues" }

                                        Rectangle {
                                            implicitWidth: Theme.chartSwatchSize
                                            implicitHeight: Theme.chartSwatchSize
                                            radius: 2
                                            color: Theme.textMuted
                                        }
                                        Label {
                                            objectName: "withoutCuesCount"
                                            Layout.alignment: Qt.AlignRight
                                            text: cueCoverageBlock.withoutCues
                                            font.family: Theme.dataFamily
                                        }
                                        Label {
                                            objectName: "withoutCuesPercent"
                                            Layout.alignment: Qt.AlignRight
                                            text: cueCoverageBlock.percent(cueCoverageBlock.withoutCues)
                                            font.family: Theme.dataFamily
                                            color: Theme.textMuted
                                        }
                                        Label { text: "without cues" }
                                    }
                                }
                            }
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
                            // A section whose colours MEAN something sets its
                            // own; the rest cycle through the palette above.
                            property var barColor: function(index) {
                                return distSection._barColors[index % distSection._barColors.length];
                            }
                            Layout.fillWidth: true
                            spacing: 4
                            visible: entries.length > 0
                            Subtitle { text: distSection.title; Layout.topMargin: 8 }
                            Repeater {
                                model: entries
                                delegate: RowLayout {
                                    id: barRow
                                    objectName: "distributionRow"
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
                                            color: distSection.barColor(barRow.index)
                                        }
                                    }
                                    Label { text: barRow.modelData.count; Layout.preferredWidth: 36; horizontalAlignment: Text.AlignRight }
                                }
                            }
                        }

                        // The cue points pie's second half: how the cued
                        // tracks spread. Always the same seven buckets, in
                        // order, empty ones included, so its shape compares
                        // between catalogs.
                        DistributionSection {
                            objectName: "cuesPerTrackSection"
                            Layout.fillWidth: true
                            title: "Cues per track"
                            // The pie's colours: the bare tracks muted, every
                            // cued bucket in the accent.
                            barColor: function(index) { return index === 0 ? Theme.textMuted : Theme.accent; }
                            entries: (statsSection.stats.cueCoverage || {}).cuesPerTrack || []
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
            PageSection {
                title: "Disk Usage"
                Layout.fillWidth: true
                Layout.preferredHeight: 420

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Button {
                            text: "Back"
                            icon.source: Theme.iconUrl("go-previous")
                            icon.color: enabled ? Theme.text : Theme.textMuted
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
        }
    }

    // Without this, the content area was just blank while the initial
    // scan ran -- only a small BusyIndicator tucked into the header (see
    // BusyOverlay.qml's own comment on exactly this problem elsewhere).
    // One counted bar for the whole scan (#58): the controller counts the
    // catalog reads and folder walks first, and names the step under way
    // below the bar. A cancelled scan takes the user back to where they
    // came from.
    Connections {
        target: controller
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        objectName: "statisticsBusyOverlay"
        anchors.fill: parent
        busy: controller.busy
        current: controller.scanCurrent
        total: controller.scanTotal
        label: "Scanning stick statistics..."
        currentItem: controller.scanLabel
        cancellable: controller.scanCancellable
        onCancelRequested: controller.cancelScan()
    }
}
