// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Library Statistics' tempo by key: one column per BPM step, one row per
// Camelot key (1A, 1B, 2A, ... 12B, then the tracks without a key), each
// cell shaded by how many tracks sit there. Where a library clusters in
// tempo AND key is what a DJ mixing harmonically wants to see; a bar per
// tempo said only the first half.
//
// One hue, light to dark with the count (square-root scaled, so a cell of
// three tracks still shows beside one of three hundred); an empty cell is
// the plain ground. Every key row is always there, so the shape compares
// between catalogs; the columns run from the slowest to the fastest step
// any track is in, gaps included, so tempo stays to scale.
ColumnLayout {
    id: heatmap
    // [{rangeStart, camelotNumber, isMinor, count}], non-empty cells only;
    // camelotNumber 0 is a track without a usable key.
    property var cells: []
    property int stepWidth: 5
    // "camelot" or "traditional", as KeyBadge takes it.
    property string notation: "camelot"

    spacing: 6
    visible: cells.length > 0

    readonly property var rows: {
        const list = [];
        for (let n = 1; n <= 12; n++) {
            list.push({number: n, isMinor: true});
            list.push({number: n, isMinor: false});
        }
        list.push({number: 0, isMinor: false});
        return list;
    }
    readonly property var columns: {
        if (cells.length === 0) return [];
        let low = cells[0].rangeStart;
        let high = low;
        for (const c of cells) {
            low = Math.min(low, c.rangeStart);
            high = Math.max(high, c.rangeStart);
        }
        const list = [];
        for (let start = low; start <= high; start += stepWidth) list.push(start);
        return list;
    }
    readonly property var counts: {
        const map = {};
        for (const c of cells) map[heatmap.cellKey(c.rangeStart, c.camelotNumber, c.isMinor)] = c.count;
        return map;
    }
    readonly property int maxCount: {
        let m = 0;
        for (const c of cells) m = Math.max(m, c.count);
        return m;
    }

    // The ramp's dark end. The accent is a light blue: the darkest step
    // on Kelp's dark ground, but on a light one the busiest cell has to
    // be the darkest, so the end is taken down there.
    readonly property color rampHigh: Theme.isLightBackground ? Qt.darker(Theme.accent, 2.2) : Theme.accent
    readonly property color rampLow: Theme.mix(Theme.groupBackground, rampHigh, 0.32)

    function cellKey(start, number, minor) { return start + "|" + number + "|" + (minor ? "A" : "B"); }
    function countAt(start, row) { return counts[cellKey(start, row.number, row.isMinor)] || 0; }
    function shade(count) {
        if (count <= 0 || maxCount <= 0) return Theme.groupBackground;
        return Theme.mix(rampLow, rampHigh, Math.sqrt(count / maxCount));
    }
    function rowLabel(row) {
        if (row.number === 0) return "No key";
        const camelot = row.number + (row.isMinor ? "A" : "B");
        return notation === "traditional" ? Theme.traditionalLabel(camelot) : camelot;
    }
    function rowSpoken(row) {
        if (row.number === 0) return "no key";
        const camelot = row.number + (row.isMinor ? "A" : "B");
        return camelot + " (" + Theme.traditionalSpokenLabel(camelot) + ")";
    }
    function tracks(n) { return n + (n === 1 ? " track" : " tracks"); }

    readonly property real labelWidth: Theme.scaled(56)
    readonly property real cellHeight: Theme.scaled(13)
    readonly property real gap: 2

    Item {
        id: plot
        objectName: "bpmKeyPlot"
        Layout.fillWidth: true
        implicitHeight: heatmap.rows.length * (heatmap.cellHeight + heatmap.gap) + axis.height + heatmap.gap
        readonly property real cellWidth: heatmap.columns.length === 0 ? 0
            : (width - heatmap.labelWidth - heatmap.gap * heatmap.columns.length) / heatmap.columns.length

        Column {
            id: grid
            x: 0
            spacing: heatmap.gap
            Repeater {
                model: heatmap.rows
                delegate: Row {
                    id: rowItem
                    required property var modelData
                    spacing: heatmap.gap
                    Label {
                        width: heatmap.labelWidth
                        height: heatmap.cellHeight
                        verticalAlignment: Text.AlignVCenter
                        font.pointSize: Theme.fontSmall
                        font.family: Theme.dataFamily
                        color: Theme.textMuted
                        text: heatmap.rowLabel(rowItem.modelData)
                    }
                    Repeater {
                        model: heatmap.columns
                        delegate: Rectangle {
                            required property int modelData
                            objectName: "bpmKeyCell"
                            readonly property int count: heatmap.countAt(modelData, rowItem.modelData)
                            width: plot.cellWidth
                            height: heatmap.cellHeight
                            radius: 2
                            color: heatmap.shade(count)
                        }
                    }
                }
            }
        }

        // BPM along the bottom, every 10 BPM so the labels never crowd.
        Item {
            id: axis
            y: grid.height + heatmap.gap
            x: heatmap.labelWidth + heatmap.gap
            width: plot.width - x
            height: axisMetrics.height
            Repeater {
                model: heatmap.columns
                delegate: Label {
                    required property int modelData
                    required property int index
                    visible: modelData % 10 === 0
                    x: index * (plot.cellWidth + heatmap.gap)
                    font.pointSize: Theme.fontSmall
                    font.family: Theme.dataFamily
                    color: Theme.textMuted
                    text: modelData
                }
            }
            Label { id: axisMetrics; visible: false; font.pointSize: Theme.fontSmall; text: "0" }
        }

        // One hover for the whole grid: a MouseArea per cell is hundreds
        // of them on a wide library.
        MouseArea {
            id: hover
            objectName: "bpmKeyHover"
            x: heatmap.labelWidth + heatmap.gap
            width: plot.width - x
            height: grid.height
            hoverEnabled: true
            readonly property int column: plot.cellWidth <= 0 ? -1
                : Math.floor(mouseX / (plot.cellWidth + heatmap.gap))
            readonly property int row: Math.floor(mouseY / (heatmap.cellHeight + heatmap.gap))
            readonly property bool onCell: containsMouse && column >= 0 && column < heatmap.columns.length
                && row >= 0 && row < heatmap.rows.length
            ToolTip.visible: onCell
            ToolTip.delay: 0
            ToolTip.text: {
                if (!onCell) return "";
                const start = heatmap.columns[column];
                const r = heatmap.rows[row];
                return start + "-" + (start + heatmap.stepWidth - 1) + " BPM, " + heatmap.rowSpoken(r) + ": "
                    + heatmap.tracks(heatmap.countAt(start, r));
            }
        }
    }

    // The legend: what the lightest and darkest step mean.
    RowLayout {
        objectName: "bpmKeyLegend"
        Layout.leftMargin: heatmap.labelWidth + heatmap.gap
        spacing: 8
        Label { text: "1"; font.pointSize: Theme.fontSmall; font.family: Theme.dataFamily; color: Theme.textMuted }
        Rectangle {
            implicitWidth: Theme.scaled(120)
            implicitHeight: heatmap.cellHeight
            radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: heatmap.rampLow }
                GradientStop { position: 1; color: heatmap.rampHigh }
            }
        }
        Label {
            text: heatmap.tracks(heatmap.maxCount) + " in one cell"
            font.pointSize: Theme.fontSmall
            font.family: Theme.dataFamily
            color: Theme.textMuted
        }
    }
}
