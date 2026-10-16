// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// The drawn checkmark in a ring that says "done": Sync Cue Points' "N cues
// synced" mark, and the confirmation toast's. Square; size it from
// outside. With `remaining` set (1 down to 0) the ring is faint and the
// remaining part of it is drawn solid from the top, a countdown; left at
// its default the ring is whole.
Canvas {
    id: mark
    property color ink: Theme.good
    property real remaining: -1
    onInkChanged: requestPaint()
    onRemainingChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const w = width;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";
        const counting = mark.remaining >= 0;
        ctx.lineWidth = w * (counting ? 0.06 : 0.07);
        ctx.strokeStyle = String(counting ? Qt.rgba(mark.ink.r, mark.ink.g, mark.ink.b, 0.25) : mark.ink);
        ctx.beginPath();
        ctx.arc(w / 2, w / 2, w * 0.44, 0, 2 * Math.PI);
        ctx.stroke();
        if (counting) {
            ctx.strokeStyle = String(mark.ink);
            ctx.beginPath();
            ctx.arc(w / 2, w / 2, w * 0.44, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * Math.min(1, mark.remaining));
            ctx.stroke();
        }
        ctx.strokeStyle = String(mark.ink);
        ctx.lineWidth = w * (counting ? 0.08 : 0.07);
        ctx.beginPath();
        ctx.moveTo(w * 0.29, w * 0.52);
        ctx.lineTo(w * 0.44, w * 0.67);
        ctx.lineTo(w * 0.72, w * 0.36);
        ctx.stroke();
    }
}
