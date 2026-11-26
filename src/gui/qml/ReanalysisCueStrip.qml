// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// One cue of Analyse again's cue review, where it was and where it would
// go: the new analysis's waveform a few beats either side of it, the old
// grid's beat lines and the new grid's, and the cue's old and new place
// (a loop's two ends as well). Everything comes from the controller on
// one timeline, in ms from the strip's start (TrackImportController::
// cueReview); this only draws it.
//
// Old and new are told apart by more than colour: the old grid's lines
// are dashed and grey, the new grid's solid in the accent; the old cue is
// an open marker labelled "old", the new one a filled marker labelled
// "new".
Item {
    id: root

    // {spanMs, columns, oldBeats, newBeats, oldCueMs, newCueMs,
    //  oldEndMs, newEndMs}.
    property var strip: ({})
    readonly property real spanMs: strip.spanMs ?? 0
    readonly property var oldBeats: strip.oldBeats ?? []
    readonly property var newBeats: strip.newBeats ?? []
    readonly property real oldCueMs: strip.oldCueMs ?? -1
    readonly property real newCueMs: strip.newCueMs ?? -1
    readonly property bool isLoop: strip.oldEndMs !== undefined
    readonly property real oldEndMs: strip.oldEndMs ?? -1
    readonly property real newEndMs: strip.newEndMs ?? -1
    readonly property color oldColor: Theme.textMuted
    readonly property color newColor: Theme.accent

    implicitHeight: Theme.scaled(72)

    function xOf(ms) {
        return root.spanMs > 0 ? ms / root.spanMs * root.width : 0;
    }

    WaveformView {
        id: bars
        objectName: "cueStripWaveform"
        anchors.fill: parent
        anchors.topMargin: Theme.scaled(14)
        waveformData: root.strip.columns ?? []
        trackDurationMs: root.spanMs
        opacity: 0.8
    }

    Canvas {
        id: marks
        objectName: "cueStripMarks"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const top = Theme.scaled(14);
            const h = height;
            ctx.lineWidth = 1;
            // The old grid: dashed.
            ctx.strokeStyle = String(root.oldColor);
            ctx.setLineDash([3, 3]);
            for (const ms of root.oldBeats) {
                const x = Math.round(root.xOf(ms)) + 0.5;
                ctx.beginPath();
                ctx.moveTo(x, top);
                ctx.lineTo(x, h);
                ctx.stroke();
            }
            // The new grid: solid.
            ctx.strokeStyle = String(root.newColor);
            ctx.setLineDash([]);
            for (const ms of root.newBeats) {
                const x = Math.round(root.xOf(ms)) + 0.5;
                ctx.beginPath();
                ctx.moveTo(x, top);
                ctx.lineTo(x, h);
                ctx.stroke();
            }
            // The cue's places: an open marker where it was, a filled one
            // where it would go, each with a line through the waveform.
            const marker = (ms, filled, color) => {
                const x = root.xOf(ms);
                const size = Theme.scaled(6);
                ctx.lineWidth = 2;
                ctx.strokeStyle = String(color);
                ctx.fillStyle = String(color);
                ctx.setLineDash(filled ? [] : [2, 2]);
                ctx.beginPath();
                ctx.moveTo(x, top);
                ctx.lineTo(x, h);
                ctx.stroke();
                ctx.setLineDash([]);
                ctx.beginPath();
                ctx.moveTo(x - size, top - size);
                ctx.lineTo(x + size, top - size);
                ctx.lineTo(x, top);
                ctx.closePath();
                if (filled) {
                    ctx.fill();
                } else {
                    ctx.stroke();
                }
                ctx.lineWidth = 1;
            };
            marker(root.oldCueMs, false, root.oldColor);
            marker(root.newCueMs, true, root.newColor);
            if (root.isLoop) {
                marker(root.oldEndMs, false, root.oldColor);
                marker(root.newEndMs, true, root.newColor);
            }
        }
        Connections {
            target: root
            function onStripChanged() { marks.requestPaint(); }
            function onWidthChanged() { marks.requestPaint(); }
        }
    }

    // The words beside the markers, so neither depends on colour.
    Label {
        objectName: "cueStripOldLabel"
        text: qsTr("old")
        color: Theme.textMuted
        font.pointSize: Theme.fontSmall
        y: 0
        // On the side away from the new place.
        x: Math.max(0, Math.min(root.width - width, root.newCueMs >= root.oldCueMs
            ? root.xOf(root.oldCueMs) - width - Theme.scaled(8) : root.xOf(root.oldCueMs) + Theme.scaled(8)))
    }
    Label {
        objectName: "cueStripNewLabel"
        text: qsTr("new")
        color: Theme.text
        font.bold: true
        font.pointSize: Theme.fontSmall
        y: 0
        x: Math.max(0, Math.min(root.width - width, root.newCueMs >= root.oldCueMs
            ? root.xOf(root.newCueMs) + Theme.scaled(8) : root.xOf(root.newCueMs) - width - Theme.scaled(8)))
    }
}
