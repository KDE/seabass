// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Which beats of a track's grid get a tick on a waveform this wide (#40).
// Decided by pixels, not by which page the waveform is on: a whole ten
// minute track in 600 px is 1300 beats, and drawn every one they are a
// grey smear.
//
//   - Every beat, downbeats marked, when no two neighbouring beats are
//     closer than minimumBeatSpacing() pixels.
//   - Otherwise the downbeats alone, when no two are closer than
//     minimumDownbeatSpacing().
//   - Otherwise every Nth downbeat, N the smallest power of two (2, 4, 8,
//     16 bars ...) that keeps them that far apart.
//
// "Closer than" is measured at the tightest place in the track, so a
// tempo change can only ever thin the grid, never crowd part of it. The
// ticks themselves stay where the data puts them.
.pragma library

// The two thresholds, in logical pixels: the one place they are set.
function minimumBeatSpacing() {
    return 6;
}
function minimumDownbeatSpacing() {
    return 12;
}

// `beats`: [{timeMs, downbeat}] in time order, as WaveformView.beatData
// takes them. `spanMs`: the track's length, which the width stands for.
// Returns {ticks: [{x, downbeat}], everyBeat, barsPerTick}: x in pixels
// from the left edge; everyBeat whether the ordinary beats are drawn;
// barsPerTick the N of "every Nth downbeat" (1 when every downbeat, or
// every beat, is drawn; 0 when nothing is). Beats before the start or
// after the end of the track get no tick.
function layout(beats, spanMs, widthPx) {
    const none = {ticks: [], everyBeat: false, barsPerTick: 0};
    if (!beats || !(beats.length > 0) || !(spanMs > 0) || !(widthPx > 0)) {
        return none;
    }
    // Positions and gaps as ms * width / span, one rounding each, so a
    // spacing exactly at a threshold is not lost to the arithmetic.
    const toPx = (ms) => ms * widthPx / spanMs;
    const inside = [];
    for (let i = 0; i < beats.length; ++i) {
        const beat = beats[i];
        const t = beat ? Number(beat.timeMs) : NaN;
        if (isFinite(t) && t >= 0 && t <= spanMs) {
            inside.push({timeMs: t, downbeat: beat.downbeat === true});
        }
    }
    if (inside.length === 0) {
        return none;
    }

    const asTicks = (list) => list.map((beat) => ({x: toPx(beat.timeMs), downbeat: beat.downbeat}));

    if (toPx(smallestGapMs(inside)) >= minimumBeatSpacing()) {
        return {ticks: asTicks(inside), everyBeat: true, barsPerTick: 1};
    }

    const downbeats = inside.filter((tick) => tick.downbeat);
    if (downbeats.length === 0) {
        return none;
    }
    // N bars span at least N times the smallest bar, so N times it is
    // a spacing every Nth downbeat is sure to keep.
    const bar = toPx(smallestGapMs(downbeats));
    let barsPerTick = 1;
    while (barsPerTick * bar < minimumDownbeatSpacing() && barsPerTick < downbeats.length) {
        barsPerTick *= 2;
    }
    const kept = [];
    for (let k = 0; k < downbeats.length; k += barsPerTick) {
        kept.push(downbeats[k]);
    }
    return {ticks: asTicks(kept), everyBeat: false, barsPerTick: barsPerTick};
}

// The smallest time between neighbours; Infinity for a single beat.
function smallestGapMs(beats) {
    let gap = Infinity;
    for (let i = 1; i < beats.length; ++i) {
        gap = Math.min(gap, beats[i].timeMs - beats[i - 1].timeMs);
    }
    return gap;
}
