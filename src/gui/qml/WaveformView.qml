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
    // What hovering the flat placeholder line says when there is no
    // waveform to draw, for a caller that knows why. Empty (the default)
    // keeps today's behaviour. The metadata pages set it: a metadata
    // backup stores no waveforms, and a row that shows cues without one
    // says so rather than looking broken.
    property string missingText: ""
    readonly property bool hasWaveform: root.waveformData && root.waveformData.length > 0
    // Place the cues even when the track's length is unknown, against a
    // span taken from the cues themselves (the last cue or loop end plus
    // a tenth, and never under minimumCueSpanMs). Off by default: pages
    // that show CueFallbackNotice rely on no markers being drawn without
    // a length. The metadata pages turn it on, because a row there exists
    // to show where the cues are, and a metadata row often has no length.
    // Positions are then relative to each other only, and the hover says
    // each cue's time and that the length is unknown rather than
    // implying the line is the whole track.
    property bool placeCuesWithoutLength: false
    readonly property real minimumCueSpanMs: 30000
    readonly property real cueSpanMs: {
        if (root.trackDurationMs > 0) {
            return root.trackDurationMs;
        }
        if (!root.placeCuesWithoutLength || !root.cueData || root.cueData.length === 0) {
            return 0;
        }
        let last = 0;
        for (let i = 0; i < root.cueData.length; i++) {
            const cue = root.cueData[i];
            const end = cue.isLoop === true && cue.loopEndMs > cue.positionMs ? cue.loopEndMs : cue.positionMs;
            last = Math.max(last, end);
        }
        return Math.max(root.minimumCueSpanMs, last * 1.1);
    }
    readonly property bool lengthUnknown: root.trackDurationMs <= 0 && root.cueSpanMs > 0
    // The bars are the whole track, but the cues on a line with no length
    // are placed against their own span, which is not the track: drawn
    // together the markers would land at the wrong place in the music. A
    // waveform carries no length of its own (only its columns), so there
    // is nothing to scale the cues by either. Those rows draw the flat
    // line, the same as a row without a waveform, and the cues on it.
    readonly property bool drawsBars: root.hasWaveform && !root.lengthUnknown

    function timeText(ms) {
        const seconds = Math.floor(Math.max(0, ms) / 1000);
        const rest = seconds % 60;
        return Math.floor(seconds / 60) + ":" + (rest < 10 ? "0" : "") + rest;
    }

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
    onCueSpanMsChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onHighlightCuePositionMsChanged: requestPaint()

    // Hit-tests cueData against a canvas-space x coordinate for the hover
    // tooltip -- a loop's whole span is a hit target (matching the wash's
    // visual footprint), a plain cue's narrow line gets a wider forgiving
    // band since it has no area of its own to hover.
    function cueTextAt(mouseX) {
        const span = root.cueSpanMs;
        if (!cueData || span <= 0) {
            return "";
        }
        for (var i = 0; i < cueData.length; i++) {
            var cue = cueData[i];
            var x = (cue.positionMs / span) * width;
            var title = "";
            var hit = false;
            if (cue.isLoop && cue.loopEndMs > cue.positionMs) {
                var xEnd = (cue.loopEndMs / span) * width;
                hit = mouseX >= x - 3 && mouseX <= xEnd + 3;
                title = cue.kind === "hot" ? "Hot loop " + cue.hotCueNumber : "Loop";
            } else {
                hit = Math.abs(mouseX - x) <= 8;
                title = cue.kind === "hot" ? "Hot cue " + cue.hotCueNumber : "Memory cue";
            }
            if (hit) {
                // Without a length the marker's place says only where it
                // is among the other cues, so the time is said outright.
                if (root.lengthUnknown) {
                    title += " at " + root.timeText(cue.positionMs);
                }
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

        if (!root.drawsBars) {
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

        const span = root.cueSpanMs;
        if (cueData && span > 0) {
            for (var j = 0; j < cueData.length; j++) {
                var cue = cueData[j];
                var x = (cue.positionMs / span) * w;
                var color = (cue.color && cue.color.length > 0 && cue.color.charAt(0) === "#")
                    ? cue.color : "#ffcc00";
                var isLoop = cue.isLoop === true && cue.loopEndMs > cue.positionMs;
                var xEnd = isLoop ? (cue.loopEndMs / span) * w : x;

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
        objectName: "waveformMouseArea"
        anchors.fill: parent
        // Also on without a length when there is a placeholder to explain:
        // the explanation is a hover, and a disabled area never hovers.
        enabled: root.trackDurationMs > 0 || root.lengthUnknown
            || (!root.hasWaveform && root.missingText.length > 0)
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
        // A cue under the pointer still names itself: the placeholder is
        // there to show where the cues are, so hovering one must say which.
        readonly property bool explainMissing: !root.hasWaveform && root.missingText.length > 0
            && root.hoveredCueText.length === 0
        // Away from a cue on a line with no length behind it, the hover
        // says so: the line is the cues' own span, not the track.
        readonly property bool explainLength: root.lengthUnknown && root.hoveredCueText.length === 0
        readonly property string lengthText: "Track length unknown: cues are spaced by their times, up to the last one."
        ToolTip.visible: containsMouse && pressX < 0
            && (noWaveformYet || explainMissing || explainLength || root.hoveredCueText.length > 0)
        readonly property string toolTipText: noWaveformYet
            ? "No waveform yet: Engine OS generates this the first time the track is loaded on the hardware."
            : (explainMissing ? (explainLength ? root.missingText + ". " + lengthText : root.missingText)
                              : (explainLength ? lengthText : root.hoveredCueText))
        ToolTip.text: toolTipText

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
            if (root.trackDurationMs <= 0) {
                // Nothing to seek to or place a cue at: an x on a line
                // without a length is not a time.
            } else if (dragging) {
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
