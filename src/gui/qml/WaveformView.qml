// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// Draws a waveform (as normalized 0..1 amplitude bars) with the track's
// beat grid and cue markers overlaid at their real position. Used both by
// the live PlayerBar (with playhead progress and click-to-seek) and by
// static per-track previews like the Duplicates page, where
// progress/seeking aren't relevant.
//
// The beat grid is a ruler lane above the waveform (#104): a short tick
// at every mark, a tall one with the bar's number at every stronger mark,
// on a ground a little lighter than the waveform's. Behind the bars, so
// that loud music covers them, a faint line at every mark and a stronger
// one at every stronger mark. Nothing of the grid is drawn over the bars.
//
// Three layers, bottom to top: the grid's lines and the bars over them
// (this Canvas itself), the ruler lane (beatGridLayer, beside the bars,
// never over them), and the cues, the loop being dragged out and the
// playhead (overlayLayer). The bars and the overlay repaint as the
// playhead moves; the lane only when its marks, the length or the size
// change, never per frame of playback.
//
// A view can show a stretch of the track rather than all of it
// (viewStartMs, viewSpanMs): the zoomed waveform popup lays such views
// side by side as tiles and moves them, never repainting one per frame.
Canvas {
    id: root
    // Faded by its caller (a row backdrop at 0.35, a dimmed card at 0.75),
    // the three layers are drawn as one picture and faded together, as the
    // single canvas this once was: faded one by one, the bars would show
    // through the cue lines and the playhead.
    layer.enabled: root.opacity < 1
    property var waveformData: []
    property var cueData: []
    // The track's beat grid as the library holds it (#40): one
    // {timeMs, downbeat} per beat, in order. Every view in the app binds
    // it to PlaybackController's beatTicksAt() (the loaded track) or
    // beatTicksFor() (any other), which hand over only the beats drawn at
    // the view's width; a whole grid, as beatGridFor() gives it, is laid
    // out the same. Empty
    // (the default) draws no grid: a track without one, and a track whose
    // grid would be placed by a sample rate the library does not record,
    // which the controller hands over as no grid. Seabass draws a grid
    // here and never makes one up.
    property var beatData: []
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
    // The pointer over the view: a caller whose click opens something
    // (the details pane, the zoomed popup) says so with a hand.
    property int pointerShape: Qt.ArrowCursor
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
    readonly property bool hasWaveform: root.byteColumns > 0 || (root.waveformData && root.waveformData.length > 0)
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
    // Which beats get a tick at this width, and where: see
    // layoutBeatTicks() in beat_grid_display.hpp for the density rule.
    // Against the track's real length only, never the cues' own span: a
    // grid on a line with no length would be placed against nothing. Re-evaluated on every
    // change of the beats, the length or the width.
    readonly property var beatGridLayout: root.beatsSelected || root.viewSpanMs > 0
        ? root.placedBeats(root.beatData, root.viewSpanMs > 0 ? root.viewSpanMs : root.trackDurationMs, root.width)
        : BeatGridTicks.layout(root.beatData, root.trackDurationMs, root.width)
    readonly property var beatTicks: root.beatGridLayout.ticks
    // True when beatData is already the beats to draw at this width and
    // length (PlaybackController's beatTicksAt() and beatTicksFor(), which
    // every view in the app binds to): the view only places them, and the
    // density rule runs once per picture, in C++. False lays out a whole
    // grid here.
    property bool beatsSelected: false

    // The stretch of the track this view shows: from viewStartMs, for
    // viewSpanMs. 0 (the default) shows the whole of it, against
    // cueSpanMs. Only the zoomed popup's tiles set it.
    property real viewStartMs: 0
    property real viewSpanMs: 0
    readonly property real shownStartMs: root.viewSpanMs > 0 ? root.viewStartMs : 0
    readonly property real shownSpanMs: root.viewSpanMs > 0 ? root.viewSpanMs : root.cueSpanMs
    // Where a time falls on this view, in pixels from its left edge.
    function xAt(ms) {
        return (ms - root.shownStartMs) * root.width / root.shownSpanMs;
    }

    // The waveform at a finer resolution than waveformData (the popup's,
    // see PlaybackController.detailWaveform()): an ArrayBuffer of three
    // bytes a column, low, mid and high, 0 to 255. Drawn instead of
    // waveformData when it has a column.
    property var columnBytes: null
    // Where the columns fall: column i at columnStartMs + i * columnMs.
    // columnMs 0 spreads them over trackDurationMs, as waveformData always
    // is.
    property real columnMs: 0
    property real columnStartMs: 0
    readonly property int byteColumns: root.columnBytes && root.columnBytes.byteLength >= 3
        ? Math.floor(root.columnBytes.byteLength / 3) : 0

    // The ruler lane: a fifth of the height, never under its minimum, and
    // none at all on a view too low for it to be read (a list row's
    // waveform) or without marks to draw. Such a view draws the lines
    // behind its bars and nothing else of the grid. showRuler can be set
    // by a caller whose stretch of the track may hold no mark while the
    // track has a grid (the popup's tiles), so that every tile keeps the
    // same lane.
    property bool showRuler: root.beatTicks.length > 0
    // How the grid is drawn, stated by every view that has one:
    //   "ruler"  the lane above the bars and the lines behind them (the
    //            details pane, the detail page, the zoomed popup);
    //   "sliced" cut out of the bars as gaps, a narrow one at every mark
    //            and a wider one at every stronger mark, with a short
    //            mark at the top and bottom edge in each gap so a quiet
    //            passage still shows it (the player bar, option G);
    //   "lines"  the lines behind the bars only (cards, row backdrops);
    //   "none"   nothing.
    // "lines" by default: a view without a style of its own never gets a
    // lane by accident.
    property string gridStyle: "lines"
    readonly property bool drawsLines: root.gridStyle === "ruler" || root.gridStyle === "lines"
    readonly property real rulerHeight: root.gridStyle === "ruler" && root.showRuler
        && root.height >= Theme.beatRulerMinimumViewHeight
        ? Math.min(Theme.beatRulerMaximumHeight,
                   Math.max(Theme.beatRulerMinimumHeight, Math.round(root.height * Theme.beatRulerHeightRatio)))
        : 0
    // The band colours: "classic" as every view has drawn them, or
    // "vivid", the brighter, more contrasted CDJ-style colours of the
    // zoomed popup. See Theme.waveformRgb().
    property string colorScheme: "classic"
    // How opaque a bar not yet played is; a played one is opaque.
    property real unplayedOpacity: 0.62
    // The waveform's own band, under the lane.
    readonly property real waveTop: root.rulerHeight
    readonly property real waveHeight: Math.max(0, root.height - root.rulerHeight)

    // Selected beats at x = time * width / length, the same arithmetic
    // layoutBeatTicks() places them by.
    function placedBeats(beats, spanMs, widthPx) {
        if (!beats || !(spanMs > 0) || !(widthPx > 0)) {
            return {ticks: [], everyBeat: false, barsPerTick: 0};
        }
        const ticks = [];
        for (let i = 0; i < beats.length; ++i) {
            const beat = beats[i];
            ticks.push({x: (beat.timeMs - root.shownStartMs) * widthPx / spanMs, timeMs: beat.timeMs,
                        downbeat: beat.downbeat === true, strong: beat.strong === true, bar: beat.bar || 0});
        }
        return {ticks: ticks, everyBeat: false, barsPerTick: 0};
    }

    // The time at a pixel from the left edge: xAt() turned round.
    function timeAtX(x) {
        return root.shownStartMs + x * root.shownSpanMs / root.width;
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

    // The bars: the played/unplayed split follows the playhead, and
    // whether bars are drawn at all follows the length.
    onWaveformDataChanged: requestPaint()
    onColumnBytesChanged: requestPaint()
    onColumnMsChanged: requestPaint()
    onColumnStartMsChanged: requestPaint()
    onBeatTicksChanged: requestPaint()
    onGridStyleChanged: requestPaint()
    onColorSchemeChanged: requestPaint()
    onUnplayedOpacityChanged: requestPaint()
    onRulerHeightChanged: requestPaint()
    onShownStartMsChanged: {
        requestPaint();
        overlayLayer.requestPaint();
    }
    onShownSpanMsChanged: {
        requestPaint();
        overlayLayer.requestPaint();
    }
    onProgressChanged: {
        requestPaint();
        overlayLayer.requestPaint();
    }
    onTrackDurationMsChanged: {
        requestPaint();
        overlayLayer.requestPaint();
    }
    onCueSpanMsChanged: {
        requestPaint();
        overlayLayer.requestPaint();
    }
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    // A change of colour scheme repaints the lines behind the bars.
    readonly property color lineColor: Theme.beatGridLine
    onLineColorChanged: requestPaint()
    onCueDataChanged: overlayLayer.requestPaint()
    onHighlightCuePositionMsChanged: overlayLayer.requestPaint()

    // Hit-tests cueData against a canvas-space x coordinate for the hover
    // tooltip -- a loop's whole span is a hit target (matching the wash's
    // visual footprint), a plain cue's narrow line gets a wider forgiving
    // band since it has no area of its own to hover.
    function cueTextAt(mouseX) {
        const span = root.cueSpanMs;
        if (!cueData || span <= 0) {
            return "";
        }
        for (let i = 0; i < cueData.length; i++) {
            const cue = cueData[i];
            const x = root.xAt(cue.positionMs);
            let title = "";
            let hit = false;
            if (cue.isLoop && cue.loopEndMs > cue.positionMs) {
                const xEnd = root.xAt(cue.loopEndMs);
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
                    title += " at " + Theme.trackTime(cue.positionMs);
                }
                return (cue.comment && cue.comment.length > 0) ? title + ": “" + cue.comment + "”" : title;
            }
        }
        return "";
    }

    // A mark's pixel column: whole pixels, and a mark at the very end of
    // the track (x = width) on the last pixel rather than off the edge.
    function markX(tick, w) {
        const x = Math.round(tick.x);
        return x >= w && tick.x < w + 0.5 && tick.timeMs >= root.trackDurationMs - 1e-6 ? w - 1 : x;
    }

    // One column's bands, 0..1, from whichever data the view draws.
    function columnAt(i) {
        if (root.byteColumns > 0) {
            return null;
        }
        const col = root.waveformData[i];
        // Backwards-compatible with a plain 0..1 number (no band split
        // available) as well as a {low, mid, high} column.
        return typeof col === "number" ? {low: col, mid: col, high: col} : col;
    }

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const w = width;
        const top = root.waveTop;
        const h = root.waveHeight;

        // The grid's lines first, so that the bars cover them: a faint
        // one at every mark, a stronger one at every stronger mark. Whole
        // pixels, so a line is one sharp pixel and not two half-lit ones.
        const ticks = root.beatTicks;
        if (root.drawsLines && ticks && ticks.length > 0 && w > 0 && h > 0) {
            ctx.fillStyle = String(root.lineColor);
            for (let pass = 0; pass < 2; ++pass) {
                const strong = pass === 1;
                ctx.globalAlpha = strong ? Theme.beatGridStrongLineOpacity : Theme.beatGridLineOpacity;
                for (let k = 0; k < ticks.length; ++k) {
                    if ((ticks[k].strong === true) !== strong) {
                        continue;
                    }
                    const x = root.markX(ticks[k], w);
                    if (x >= 0 && x < w) {
                        ctx.fillRect(x, top, 1, h);
                    }
                }
            }
            ctx.globalAlpha = 1;
        }

        if (!root.drawsBars) {
            ctx.fillStyle = String(Theme.textMuted);
            ctx.fillRect(0, top + h / 2 - 1, w, 2);
            return;
        }
        const bytes = root.byteColumns > 0 ? new Uint8Array(root.columnBytes) : null;
        const n = bytes ? root.byteColumns : root.waveformData.length;
        // With no span to place them against (no length yet: the player
        // between a load and its duration), the columns are spread across
        // the width, one unit of "time" each, as they always were.
        const spread = !(root.shownSpanMs > 0) || !(root.trackDurationMs > 0 || root.columnMs > 0);
        const colMs = spread ? 1 : (root.columnMs > 0 ? root.columnMs : root.trackDurationMs / n);
        const firstMs = spread ? 0 : root.columnStartMs;
        const startMs = spread ? 0 : root.shownStartMs;
        const spanMs = spread ? n : root.shownSpanMs;
        const lengthMs = spread ? n : (root.trackDurationMs > 0 ? root.trackDurationMs : spanMs);
        const pxPerMs = w / spanMs;
        const barW = colMs * pxPerMs;
        const vivid = root.colorScheme === "vivid";
        const unplayed = root.unplayedOpacity;
        // A bar is drawn over the ground, never over a line: its own
        // rectangle is cleared first, so a translucent (unplayed) bar
        // shows the ground through it and not the grid.
        const bar = (x, width, low, mid, high, played) => {
            const amplitude = Math.max(low, mid, high);
            const barH = Math.max(1, amplitude * h);
            const c = Theme.waveformRgb(low, mid, high, vivid);
            const alpha = played ? 1.0 : unplayed;
            const y = top + (h - barH) / 2;
            ctx.clearRect(x, y, width, barH);
            ctx.fillStyle = "rgba(" + c.r + "," + c.g + "," + c.b + "," + alpha + ")";
            ctx.fillRect(x, y, width, barH);
        };
        const played = (ms) => root.progress >= 0 && ms / lengthMs < root.progress;
        // Only the columns this view shows.
        const first = Math.max(0, Math.floor((startMs - firstMs) / colMs) - 1);
        const last = Math.min(n - 1, Math.ceil((startMs + spanMs - firstMs) / colMs) + 1);
        if (barW >= 1) {
            for (let i = first; i <= last; i++) {
                const ms = firstMs + i * colMs;
                const x = (ms - startMs) * pxPerMs;
                let low, mid, high;
                if (bytes) {
                    low = bytes[3 * i] / 255; mid = bytes[3 * i + 1] / 255; high = bytes[3 * i + 2] / 255;
                } else {
                    const col = root.columnAt(i);
                    low = col.low; mid = col.mid; high = col.high;
                }
                bar(x, Math.max(1, barW - 1), low, mid, high, played(ms));
            }
        } else {
            // More than one column to a pixel: each pixel draws the
            // loudest of the columns under it, one bar a pixel, so that
            // neighbouring columns never overwrite each other.
            const pxFrom = Math.max(0, Math.floor((firstMs + first * colMs - startMs) * pxPerMs));
            const pxTo = Math.min(Math.ceil(w), Math.ceil((firstMs + (last + 1) * colMs - startMs) * pxPerMs));
            for (let px = pxFrom; px < pxTo; ++px) {
                const msFrom = startMs + px / pxPerMs;
                const i0 = Math.max(first, Math.floor((msFrom - firstMs) / colMs));
                const i1 = Math.min(last, Math.ceil((msFrom + 1 / pxPerMs - firstMs) / colMs) - 1);
                let low = 0, mid = 0, high = 0;
                for (let i = i0; i <= i1; ++i) {
                    if (bytes) {
                        low = Math.max(low, bytes[3 * i]); mid = Math.max(mid, bytes[3 * i + 1]);
                        high = Math.max(high, bytes[3 * i + 2]);
                    } else {
                        const col = root.columnAt(i);
                        low = Math.max(low, col.low); mid = Math.max(mid, col.mid); high = Math.max(high, col.high);
                    }
                }
                if (bytes) {
                    low /= 255; mid /= 255; high /= 255;
                }
                bar(px, 1, low, mid, high, played(msFrom));
            }
        }

        // The sliced grid: cut out of the bars, then a short mark at the
        // top and bottom edge of each cut. Sizes scale with the height
        // the design was drawn at (Theme.beatSliced*).
        if (root.gridStyle === "sliced" && ticks && ticks.length > 0 && w > 0) {
            const full = height;
            const scale = full / Theme.beatSlicedDesignHeight;
            for (let pass = 0; pass < 2; ++pass) {
                const strong = pass === 1;
                const gap = Math.max(strong ? 2 : 1, Math.round((strong ? Theme.beatSlicedStrongGap : Theme.beatSlicedGap) * scale));
                const mark = Math.max(strong ? 3 : 2,
                                      Math.round((strong ? Theme.beatSlicedStrongMark : Theme.beatSlicedMark) * scale));
                const markW = strong ? Math.max(1, gap / 2) : 1;
                // Where every beat has a mark, a gap at each would cut too
                // much of the bars away: ordinary gaps at least
                // Theme.beatSlicedMinimumSpacing apart.
                let lastX = -Infinity;
                for (let k = 0; k < ticks.length; ++k) {
                    if ((ticks[k].strong === true) !== strong) {
                        continue;
                    }
                    const x = root.markX(ticks[k], w);
                    if (x < -gap || x > w + gap) {
                        continue;
                    }
                    if (!strong && x - lastX < Theme.beatSlicedMinimumSpacing) {
                        continue;
                    }
                    lastX = x;
                    const left = x - Math.floor(gap / 2);
                    ctx.clearRect(left, 0, gap, full);
                    ctx.globalAlpha = strong ? 1 : Theme.beatSlicedMarkOpacity;
                    ctx.fillStyle = String(strong ? Theme.accent : Theme.textMuted);
                    const markLeft = x - Math.floor(markW / 2);
                    ctx.fillRect(markLeft, 0, markW, mark);
                    ctx.fillRect(markLeft, full - mark, markW, mark);
                    ctx.globalAlpha = 1;
                }
            }
        }
    }

    // The ruler lane (#104), a layer of its own so that it is drawn only
    // when its marks, the length or the size change: a short tick at
    // every mark, a tall one with its bar's number at every stronger
    // mark. Above the bars' band, never over it; below the cues.
    Canvas {
        id: beatGridLayer
        objectName: "beatGridLayer"
        anchors.fill: parent
        readonly property var ticks: root.beatTicks
        readonly property real lane: root.rulerHeight
        // A change of colour scheme is a change of data for this layer.
        readonly property color groundColor: Theme.beatRulerGround
        readonly property color tickColor: Theme.beatRulerTick
        readonly property color strongColor: Theme.beatRulerStrongTick
        // The track's own stretch of the lane: none before 0:00 or after
        // its end, where a tile of the zoomed popup can lie.
        readonly property real trackFrom: root.xAt(0)
        readonly property real trackTo: root.xAt(root.trackDurationMs > 0 ? root.trackDurationMs : root.shownSpanMs)
        onTicksChanged: requestPaint()
        onLaneChanged: requestPaint()
        onGroundColorChanged: requestPaint()
        onTickColorChanged: requestPaint()
        onStrongColorChanged: requestPaint()
        onTrackFromChanged: requestPaint()
        onTrackToChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const lane = beatGridLayer.lane;
            const w = width;
            if (lane <= 0 || w <= 0) {
                return;
            }
            const from = Math.max(0, Math.round(beatGridLayer.trackFrom));
            const to = Math.min(w, Math.round(beatGridLayer.trackTo));
            if (to > from) {
                ctx.fillStyle = String(beatGridLayer.groundColor);
                ctx.fillRect(from, 0, to - from, lane);
            }
            const ticks = beatGridLayer.ticks;
            if (!ticks || ticks.length === 0) {
                return;
            }
            const shortTick = Math.max(3, Math.round(lane * Theme.beatRulerTickLength));
            ctx.fillStyle = String(beatGridLayer.tickColor);
            ctx.globalAlpha = Theme.beatRulerTickOpacity;
            for (let i = 0; i < ticks.length; ++i) {
                if (ticks[i].strong !== true) {
                    ctx.fillRect(root.markX(ticks[i], w), lane - shortTick, 1, shortTick);
                }
            }
            // The numbers sit above the short ticks, left-aligned a little
            // right of their own tick.
            const baseline = lane - shortTick - 1;
            const size = Math.max(Theme.beatRulerNumberMinimumSize,
                                  Math.min(Theme.beatRulerNumberMaximumSize, Math.round(lane * Theme.beatRulerNumberRatio)));
            // Where the track ends: a number that would run past it is set
            // left of its tick instead. Not the view's own edge: a tile of
            // the popup leaves a number running over its edge to the next
            // tile, which draws it too.
            const end = beatGridLayer.trackTo;
            ctx.fillStyle = String(beatGridLayer.strongColor);
            ctx.globalAlpha = Theme.beatRulerStrongTickOpacity;
            ctx.font = "600 " + size + "px sans-serif";
            ctx.textBaseline = "alphabetic";
            for (let i = 0; i < ticks.length; ++i) {
                if (ticks[i].strong === true) {
                    const x = root.markX(ticks[i], w);
                    ctx.fillRect(x, 0, 1, lane);
                    if (ticks[i].bar > 0) {
                        const text = String(ticks[i].bar);
                        const width = ctx.measureText(text).width;
                        const right = x + Theme.beatRulerNumberInset + width;
                        const at = right > end && x <= end ? x - Theme.beatRulerNumberInset - width
                                                           : x + Theme.beatRulerNumberInset;
                        ctx.fillText(text, at, baseline);
                    }
                }
            }
        }
    }

    // Cues, the loop being dragged out and the playhead: on top of the
    // bars and the grid.
    Canvas {
        id: overlayLayer
        objectName: "waveformOverlay"
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        // The cues stand in the waveform's band, under the lane; the
        // playhead runs through both.
        readonly property real waveTop: root.waveTop
        onWaveTopChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const w = width;
            const fullHeight = height;
            const top = overlayLayer.waveTop;
            const h = fullHeight - top;
            ctx.translate(0, top);
            const cues = root.cueData;

            const span = root.cueSpanMs;
            if (cues && span > 0) {
                for (let j = 0; j < cues.length; j++) {
                    const cue = cues[j];
                    const x = root.xAt(cue.positionMs);
                    // A memory cue is drawn in the one memory-cue colour, never
                    // its own: see Theme.memoryCue.
                    const isMemory = cue.kind !== "hot";
                    const color = isMemory ? String(Theme.memoryCue)
                        : (cue.color && cue.color.length > 0 && cue.color.charAt(0) === "#") ? cue.color : "#ffcc00";
                    const isLoop = cue.isLoop === true && cue.loopEndMs > cue.positionMs;
                    const xEnd = isLoop ? root.xAt(cue.loopEndMs) : x;

                    // See highlightCuePositionMs's own doc comment. A small
                    // tolerance (not exact equality) since the caller passes
                    // whatever position it already resolved as "the" cue,
                    // and float ms comparisons shouldn't hinge on exact
                    // equality.
                    const dimmed = root.highlightCuePositionMs >= 0
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
                    } else {
                        // The same flag, "M", at the foot of the line: a memory
                        // cue often sits exactly under a hot cue (the pad was
                        // made from the marker), and two flags at the top would
                        // hide one another.
                        ctx.fillStyle = color;
                        ctx.fillRect(x, h - 10, 10, 10);
                        ctx.fillStyle = String(Theme.memoryCueInk);
                        ctx.font = "8px sans-serif";
                        ctx.fillText("M", x + 2, h - 2);
                    }
                    ctx.restore();
                }
            }

            // Live preview of a loop being dragged out -- see
            // cueMouseArea's own comment for the tap/drag threshold.
            if (root.cueEditable && cueMouseArea.dragging) {
                const dragA = Math.min(cueMouseArea.pressX, cueMouseArea.dragX);
                const dragB = Math.max(cueMouseArea.pressX, cueMouseArea.dragX);
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
            ctx.translate(0, -top);
            const h2 = fullHeight;
            if (root.progress >= 0 && w > 0) {
                const px = root.shownSpanMs > 0 ? root.xAt(root.progress * root.cueSpanMs) : root.progress * w;
                ctx.save();
                ctx.shadowColor = "rgba(255,255,255,0.85)";
                ctx.shadowBlur = 6;
                ctx.strokeStyle = "#ffffff";
                ctx.lineWidth = 3;
                ctx.beginPath();
                ctx.moveTo(px, 0);
                ctx.lineTo(px, h2);
                ctx.stroke();
                ctx.restore();

                ctx.strokeStyle = String(Theme.accent);
                ctx.lineWidth = 1.5;
                ctx.beginPath();
                ctx.moveTo(px, 0);
                ctx.lineTo(px, h2);
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
        cursorShape: root.pointerShape
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
                overlayLayer.requestPaint();
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
                const a = root.timeAtX(Math.min(pressX, dragX));
                const b = root.timeAtX(Math.max(pressX, dragX));
                root.loopRangeSelected(a, b);
            } else {
                const ms = root.timeAtX(mouse.x);
                root.seekRequested(ms / root.trackDurationMs);
                root.positionClicked(ms);
            }
            pressX = -1;
            dragX = -1;
            overlayLayer.requestPaint();
        }
        onExited: root.hoveredCueText = ""
    }
}
