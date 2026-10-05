// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// A two-slice pie: the tracks that carry at least one cue point against
// the ones that carry none. Drawn on a Canvas rather than a shader so
// the offscreen and xvfb suites can sample what it painted.
//
// The "with cues" slice starts at twelve o'clock and runs clockwise; the
// rest of the disc is "without". No text inside: the counts live in the
// legend beside it, so nothing rests on colour alone.
//
// With no tracks at all it draws an empty disc in the group ground,
// which says "nothing to count" rather than a muted full disc, which
// would claim every track is bare.
Canvas {
    id: root

    property int withCues: 0
    property int withoutCues: 0

    readonly property int total: withCues + withoutCues
    readonly property real withFraction: total > 0 ? withCues / total : 0
    // Where the "with" slice ends, in radians clockwise from twelve
    // o'clock. Exposed for the test, which samples either side of it.
    readonly property real withSweep: 2 * Math.PI * withFraction
    readonly property real radius: Math.max(0, Math.min(width, height) / 2 - 1)

    readonly property color withColor: Theme.accent
    readonly property color withoutColor: Theme.textMuted
    readonly property color emptyColor: Theme.groupBackground

    implicitWidth: 100
    implicitHeight: 100

    onWithCuesChanged: requestPaint()
    onWithoutCuesChanged: requestPaint()
    onWithColorChanged: requestPaint()
    onWithoutColorChanged: requestPaint()
    onEmptyColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    // How many times the pie has finished painting. A Canvas paints after
    // the frame that asked for it, on some platforms well after, so a
    // screenshot taken as soon as the page renders can still be empty
    // where the pie goes; tests wait on this.
    property int paintedCount: 0
    onPainted: root.paintedCount += 1

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const cx = width / 2;
        const cy = height / 2;
        const r = root.radius;
        if (r <= 0) {
            return;
        }
        const top = -Math.PI / 2;

        const slice = function(from, to, colour) {
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.arc(cx, cy, r, from, to, false);
            ctx.closePath();
            ctx.fillStyle = colour;
            ctx.fill();
        };

        if (root.total === 0) {
            slice(0, 2 * Math.PI, root.emptyColor);
            return;
        }
        if (root.withCues === 0) {
            slice(0, 2 * Math.PI, root.withoutColor);
            return;
        }
        if (root.withoutCues === 0) {
            slice(0, 2 * Math.PI, root.withColor);
            return;
        }

        slice(top, top + root.withSweep, root.withColor);
        slice(top + root.withSweep, top + 2 * Math.PI, root.withoutColor);

        // The gap: each edge the two slices share is cut out of the
        // disc rather than painted over, so whatever lies under the pie
        // shows through. A spoke painted in Theme.surface was the first
        // idea, but the page this sits on is Theme.background (the
        // window palette), and a surface-coloured line on it is a
        // visible third colour rather than a gap.
        ctx.globalCompositeOperation = "destination-out";
        ctx.strokeStyle = "black";
        ctx.lineWidth = Theme.chartSliceGap;
        ctx.lineCap = "butt";
        const spoke = function(angle) {
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.lineTo(cx + (r + 1) * Math.cos(angle), cy + (r + 1) * Math.sin(angle));
            ctx.stroke();
        };
        spoke(top);
        spoke(top + root.withSweep);
        ctx.globalCompositeOperation = "source-over";
    }
}
