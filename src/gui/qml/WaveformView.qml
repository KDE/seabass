// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Draws a waveform (as normalized 0..1 amplitude bars) with cue markers
// overlaid at their real position. Used both by the live PlayerBar (with
// playhead progress and click-to-seek) and by static per-track previews
// like the Duplicates page, where progress/seeking aren't relevant.
Canvas {
    id: root
    property var waveformData: []
    property var cueData: []
    property real trackDurationMs: 0
    // -1 disables the played/unplayed color split (a static preview).
    property real progress: -1
    // "rekordbox"/"engine"/"onelibrary" -- which catalog this track came
    // from, only used to decide whether the empty-waveform tooltip below
    // applies. Optional: callers that don't care about that tooltip (e.g.
    // PlayerBar, where a track is always actually loaded) can leave it
    // unset.
    property string format: ""
    // Enables press-and-drag loop creation (emits loopRangeSelected)
    // instead of every release being treated as a plain seek/cue click.
    // Off by default -- PlayerBar and every read-only preview (Duplicates,
    // the row backdrop) keep today's click-only behavior; only the "Add
    // Cue" picker opts in. Hover tooltips for existing cues/loops are
    // always on regardless of this, since they're pure display.
    property bool cueEditable: false
    property string hoveredCueText: ""
    // -1 (default) renders every cue identically, today's behavior. A
    // real ms value instead dims every OTHER cue -- e.g.
    // LibraryConsistencyPage highlighting the one memory cue at 0:00
    // that's about to be removed, so it's unambiguous which one gets
    // killed rather than just one more full-strength marker among
    // several.
    property real highlightCuePositionMs: -1

    signal seekRequested(real ratio)
    // Fires on every plain click alongside seekRequested -- callers that
    // only want click-to-seek (PlayerBar) simply don't connect to this
    // one. Added for the "Add Cue" picker, which needs an absolute ms
    // position rather than a 0..1 ratio.
    signal positionClicked(real positionMs)
    // Fires instead of positionClicked when cueEditable is true and the
    // press moved far enough to read as a drag rather than a tap -- see
    // cueMouseArea's own comment for the threshold.
    signal loopRangeSelected(real startMs, real endMs)

    onWaveformDataChanged: requestPaint()
    onCueDataChanged: requestPaint()
    onProgressChanged: requestPaint()
    onTrackDurationMsChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onHighlightCuePositionMsChanged: requestPaint()

    // Hit-tests cueData against a canvas-space x coordinate for the hover
    // tooltip -- a loop's whole span is a hit target (matching the wash's
    // visual footprint), a plain cue's narrow line gets a wider forgiving
    // band since it has no area of its own to hover.
    function cueTextAt(mouseX) {
        if (!cueData || trackDurationMs <= 0) {
            return "";
        }
        for (var i = 0; i < cueData.length; i++) {
            var cue = cueData[i];
            var x = (cue.positionMs / trackDurationMs) * width;
            var title = "";
            var hit = false;
            if (cue.isLoop && cue.loopEndMs > cue.positionMs) {
                var xEnd = (cue.loopEndMs / trackDurationMs) * width;
                hit = mouseX >= x - 3 && mouseX <= xEnd + 3;
                title = cue.kind === "hot" ? "Hot loop " + cue.hotCueNumber : "Loop";
            } else {
                hit = Math.abs(mouseX - x) <= 8;
                title = cue.kind === "hot" ? "Hot cue " + cue.hotCueNumber : "Memory cue";
            }
            if (hit) {
                return (cue.comment && cue.comment.length > 0) ? title + ": “" + cue.comment + "”" : title;
            }
        }
        return "";
    }

    onPaint: {
        var ctx = getContext("2d");
        ctx.reset();
        var w = width, h = height;
        var wf = waveformData;

        if (!wf || wf.length === 0) {
            ctx.fillStyle = String(Theme.textMuted);
            ctx.fillRect(0, h / 2 - 1, w, 2);
        } else {
            var barW = w / wf.length;
            for (var i = 0; i < wf.length; i++) {
                var col = wf[i];
                // Backwards-compatible with a plain 0..1 number (no band
                // split available) as well as a {low, mid, high} column.
                var low = typeof col === "number" ? col : col.low;
                var mid = typeof col === "number" ? col : col.mid;
                var high = typeof col === "number" ? col : col.high;
                var amplitude = Math.max(low, mid, high);
                var barH = Math.max(1, amplitude * h);
                var played = root.progress >= 0 && (i / wf.length) < root.progress;

                // Classic DJ-hardware coloring: bass in blue, mids in
                // green/yellow, highs in red/white, blended by each band's
                // relative strength rather than a single flat hue.
                var r = Math.min(255, Math.round(60 + high * 195));
                var g = Math.min(255, Math.round(60 + mid * 150 + high * 60));
                var b = Math.min(255, Math.round(90 + low * 165));
                var alpha = played ? 1.0 : 0.62;
                ctx.fillStyle = "rgba(" + r + "," + g + "," + b + "," + alpha + ")";
                ctx.fillRect(i * barW, (h - barH) / 2, Math.max(1, barW - 1), barH);
            }
        }

        if (cueData && trackDurationMs > 0) {
            for (var j = 0; j < cueData.length; j++) {
                var cue = cueData[j];
                var x = (cue.positionMs / trackDurationMs) * w;
                var color = (cue.color && cue.color.length > 0 && cue.color.charAt(0) === "#")
                    ? cue.color : "#ffcc00";
                var isLoop = cue.isLoop === true && cue.loopEndMs > cue.positionMs;
                var xEnd = isLoop ? (cue.loopEndMs / trackDurationMs) * w : x;

                // See highlightCuePositionMs's own doc comment. A small
                // tolerance (not exact equality) since the caller passes
                // whatever position it already resolved as "the" cue,
                // and float ms comparisons shouldn't hinge on exact
                // equality.
                var dimmed = root.highlightCuePositionMs >= 0
                    && Math.abs(cue.positionMs - root.highlightCuePositionMs) > 10;
                ctx.save();
                if (dimmed) {
                    ctx.globalAlpha = 0.25;
                }

                if (isLoop) {
                    // Translucent wash across the loop span, plus a
                    // thicker solid bar underneath -- the wash alone
                    // reads too much like "a selection"; the bar is what
                    // actually reads as "this repeats" at a glance,
                    // without covering any more of the waveform than the
                    // wash already does.
                    ctx.save();
                    ctx.fillStyle = color;
                    // Multiplied, not overwritten, by the outer dimming
                    // alpha above -- ctx.globalAlpha here would otherwise
                    // flatten a dimmed loop's wash back to the same 0.22
                    // every loop already uses, undoing the dim.
                    ctx.globalAlpha = dimmed ? 0.22 * 0.25 : 0.22;
                    ctx.fillRect(x, h * 0.55, xEnd - x, h * 0.45);
                    ctx.restore();
                    ctx.fillStyle = color;
                    ctx.fillRect(x, h - 6, xEnd - x, 6);
                }

                ctx.strokeStyle = color;
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(x, 0);
                ctx.lineTo(x, h);
                if (isLoop) {
                    ctx.moveTo(xEnd, 0);
                    ctx.lineTo(xEnd, h);
                }
                ctx.stroke();
                if (cue.kind === "hot") {
                    ctx.fillStyle = color;
                    ctx.fillRect(x, 0, 10, 10);
                    ctx.fillStyle = "#000";
                    ctx.font = "8px sans-serif";
                    ctx.fillText(String(cue.hotCueNumber), x + 2, 8);
                }
                ctx.restore();
            }
        }

        // Live preview of a loop being dragged out -- see
        // cueMouseArea's own comment for the tap/drag threshold.
        if (root.cueEditable && cueMouseArea.dragging) {
            var dragA = Math.min(cueMouseArea.pressX, cueMouseArea.dragX);
            var dragB = Math.max(cueMouseArea.pressX, cueMouseArea.dragX);
            ctx.save();
            ctx.fillStyle = String(Theme.accent);
            ctx.globalAlpha = 0.18;
            ctx.fillRect(dragA, 0, dragB - dragA, h);
            ctx.restore();
            ctx.strokeStyle = String(Theme.accent);
            ctx.lineWidth = 2;
            ctx.setLineDash([4, 3]);
            ctx.beginPath();
            ctx.moveTo(dragA, 0); ctx.lineTo(dragA, h);
            ctx.moveTo(dragB, 0); ctx.lineTo(dragB, h);
            ctx.stroke();
            ctx.setLineDash([]);
        }

        // Playhead -- previously only implied by the played/unplayed
        // color split above, which reads as "which bars are done" rather
        // than "exactly where is 'now'", especially at the 0.35 opacity
        // this view runs at as the library row backdrop. A white halo
        // under an accent-colored core line keeps it legible against
        // whatever mixed bar colors happen to sit behind it; the
        // downward-pointing triangle at the top gives it a distinct
        // silhouette even where the halo/line alone would blend in.
        if (root.progress >= 0 && w > 0) {
            var px = root.progress * w;
            ctx.save();
            ctx.shadowColor = "rgba(255,255,255,0.85)";
            ctx.shadowBlur = 6;
            ctx.strokeStyle = "#ffffff";
            ctx.lineWidth = 3;
            ctx.beginPath();
            ctx.moveTo(px, 0);
            ctx.lineTo(px, h);
            ctx.stroke();
            ctx.restore();

            ctx.strokeStyle = String(Theme.accent);
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.moveTo(px, 0);
            ctx.lineTo(px, h);
            ctx.stroke();

            ctx.fillStyle = String(Theme.accent);
            ctx.beginPath();
            ctx.moveTo(px - 5, 0);
            ctx.lineTo(px + 5, 0);
            ctx.lineTo(px, 7);
            ctx.closePath();
            ctx.fill();
        }
    }

    MouseArea {
        id: cueMouseArea
        anchors.fill: parent
        enabled: root.trackDurationMs > 0
        hoverEnabled: true
        preventStealing: root.cueEditable

        property real pressX: -1
        property real dragX: -1
        // A short, deliberate movement threshold -- same 8px the "Add
        // Cue" mockup this was designed against used -- so a hand that
        // isn't perfectly still during an ordinary tap never misfires as
        // a loop.
        readonly property bool dragging: root.cueEditable && pressX >= 0 && Math.abs(dragX - pressX) > 8

        // Engine only generates a track's waveform preview the first time
        // Engine OS itself loads that track -- a track never opened on
        // real hardware yet has no preview to show here (not a bug), so
        // say so instead of leaving the flat placeholder line unexplained.
        // Takes priority over a cue tooltip since it explains why there's
        // nothing to hover in the first place.
        readonly property bool noWaveformYet: root.format === "engine" && (!root.waveformData || root.waveformData.length === 0)
        ToolTip.visible: containsMouse && pressX < 0 && (noWaveformYet || root.hoveredCueText.length > 0)
        ToolTip.text: noWaveformYet
            ? "No waveform yet: Engine OS generates this the first time the track is loaded on the hardware."
            : root.hoveredCueText

        onPositionChanged: (mouse) => {
            if (pressX >= 0) {
                dragX = mouse.x;
                root.requestPaint();
            } else {
                root.hoveredCueText = root.cueTextAt(mouse.x);
            }
        }
        onPressed: (mouse) => {
            pressX = mouse.x;
            dragX = mouse.x;
        }
        onReleased: (mouse) => {
            if (dragging) {
                var a = Math.min(pressX, dragX) / width;
                var b = Math.max(pressX, dragX) / width;
                root.loopRangeSelected(a * root.trackDurationMs, b * root.trackDurationMs);
            } else {
                root.seekRequested(mouse.x / width);
                root.positionClicked((mouse.x / width) * root.trackDurationMs);
            }
            pressX = -1;
            dragX = -1;
            root.requestPaint();
        }
        onExited: root.hoveredCueText = ""
    }
}
