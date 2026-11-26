// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// The beat grid drawn with a waveform (#40, as a ruler since #104): a
// lane above the waveform with a tick where the library's own grid puts
// each mark and a tall, numbered one at each stronger mark, and lines
// behind the bars, only as many as the width has room for. Nothing of the
// grid is drawn over the bars. Seabass draws a grid here and never makes
// one up.
TestCase {
    id: testCase
    name: "WaveformBeatGrid"
    width: 1300
    height: 400
    visible: true
    when: windowShown

    readonly property string fixtureRoot: Qt.resolvedUrl("../fixtures/anonymized_library").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    // A grid as both libraries hand it over: every beat, in order, the
    // first of every four a downbeat.
    function grid(beatMs, fromMs, toMs, firstIndex) {
        const beats = [];
        let index = firstIndex || 0;
        for (let t = fromMs; t < toMs - 1e-6; t += beatMs) {
            beats.push({timeMs: t, downbeat: index % 4 === 0});
            ++index;
        }
        return beats;
    }

    function xs(ticks) {
        return ticks.map((tick) => tick.x);
    }

    // Where a view handed already-selected beats (beatsSelected) puts
    // them: no second pass of the rule, only time * width / span.
    function placed(beats, spanMs, widthPx) {
        return beats.map((beat) => beat.timeMs * widthPx / spanMs);
    }

    // ---- The density rule, as a function ----------------------------

    // 120 BPM is a beat every 500 ms. A minute across 1200 px puts them
    // 10 px apart: room for every beat, the downbeats marked.
    function test_everyBeatWhereThereIsRoom() {
        const beats = grid(500, 0, 60000);
        const layout = BeatGridTicks.layout(beats, 60000, 1200);
        compare(layout.everyBeat, true);
        compare(layout.ticks.length, 120);
        for (let i = 0; i < layout.ticks.length; ++i) {
            fuzzyCompare(layout.ticks[i].x, i * 10, 1e-9);
            compare(layout.ticks[i].downbeat, i % 4 === 0, "beat " + i);
        }
    }

    // Half the width: 5 px a beat is under the beat spacing, so only
    // the downbeats, 20 px apart, all of them.
    function test_onlyDownbeatsBelowTheBeatSpacing() {
        verify(5 < BeatGridTicks.minimumBeatSpacing());
        verify(20 >= BeatGridTicks.minimumDownbeatSpacing());
        const layout = BeatGridTicks.layout(grid(500, 0, 60000), 60000, 600);
        compare(layout.everyBeat, false);
        compare(layout.barsPerTick, 1);
        compare(layout.ticks.length, 30);
        for (let k = 0; k < 30; ++k) {
            fuzzyCompare(layout.ticks[k].x, k * 20, 1e-9);
            compare(layout.ticks[k].downbeat, true);
        }
    }

    // Exactly at a threshold is room enough; a hair under is not.
    function test_theThresholdsAreTheBoundary() {
        const beatPx = BeatGridTicks.minimumBeatSpacing();
        // A beat every 500 ms over 60 s: width w puts beats w/120 apart.
        compare(BeatGridTicks.layout(grid(500, 0, 60000), 60000, beatPx * 120).everyBeat, true);
        compare(BeatGridTicks.layout(grid(500, 0, 60000), 60000, beatPx * 120 - 1).everyBeat, false);
        const downPx = BeatGridTicks.minimumDownbeatSpacing();
        // Downbeats are w/30 apart.
        compare(BeatGridTicks.layout(grid(500, 0, 60000), 60000, downPx * 30).barsPerTick, 1);
        compare(BeatGridTicks.layout(grid(500, 0, 60000), 60000, downPx * 30 - 1).barsPerTick, 2);
    }

    // A whole ten minute track at 128 BPM in about 600 px: 1280 beats.
    // A downbeat every 1.9 px would be a grey smear, so every Nth: the
    // smallest power of two that keeps them the downbeat spacing apart.
    function test_aWholeTrackDrawsEveryNthDownbeat() {
        const beatMs = 60000 / 128;
        const beats = grid(beatMs, 0, 600000);
        compare(beats.length, 1280);
        const layout = BeatGridTicks.layout(beats, 600000, 600);
        compare(layout.everyBeat, false);
        const downbeatPx = 4 * beatMs * 600 / 600000;   // 1.875
        let n = 1;
        while (n * downbeatPx < BeatGridTicks.minimumDownbeatSpacing()) {
            n *= 2;
        }
        compare(layout.barsPerTick, n);
        verify(n >= 4 && n <= 16, "4, 8 or 16 bars at this width, got " + n);
        compare(layout.ticks.length, Math.ceil(320 / n));
        for (let k = 0; k < layout.ticks.length; ++k) {
            compare(layout.ticks[k].downbeat, true);
            fuzzyCompare(layout.ticks[k].x, k * n * 4 * beatMs * 600 / 600000, 1e-9);
            if (k > 0) {
                verify(layout.ticks[k].x - layout.ticks[k - 1].x >= BeatGridTicks.minimumDownbeatSpacing());
            }
        }
    }

    // Wider is finer: the same grid laid out at three widths.
    function test_theRuleFollowsTheWidth() {
        const beats = grid(500, 0, 60000);
        compare(BeatGridTicks.layout(beats, 60000, 150).barsPerTick, 4);
        compare(BeatGridTicks.layout(beats, 60000, 600).everyBeat, false);
        compare(BeatGridTicks.layout(beats, 60000, 600).barsPerTick, 1);
        compare(BeatGridTicks.layout(beats, 60000, 1200).everyBeat, true);
    }

    // A tempo change halfway: 120 BPM for a minute, then 60 BPM. The
    // ticks fall where the data says, both sides of the change, and the
    // density holds for the tightest part.
    function test_aTempoChangeIsDrawnAsTheDataSays() {
        const beats = grid(500, 0, 60000).concat(grid(1000, 60000, 120000, 120));
        compare(beats.length, 180);
        // 960 px for 120 s: 4 px a beat before the change, 8 after.
        const narrow = BeatGridTicks.layout(beats, 120000, 960);
        compare(narrow.everyBeat, false, "4 px a beat in the first half is too close");
        compare(narrow.barsPerTick, 1);
        // 30 downbeats at 16 px, then 15 at 32 px.
        compare(narrow.ticks.length, 45);
        fuzzyCompare(narrow.ticks[29].x, 29 * 16, 1e-9);
        fuzzyCompare(narrow.ticks[30].x, 480, 1e-9);
        fuzzyCompare(narrow.ticks[31].x, 480 + 32, 1e-9);
        fuzzyCompare(narrow.ticks[44].x, 480 + 14 * 32, 1e-9);

        const wide = BeatGridTicks.layout(beats, 120000, 1920);
        compare(wide.everyBeat, true);
        compare(wide.ticks.length, 180);
        fuzzyCompare(wide.ticks[119].x, 119 * 8, 1e-9);
        fuzzyCompare(wide.ticks[120].x, 960, 1e-9);
        fuzzyCompare(wide.ticks[121].x, 976, 1e-9);
        compare(wide.ticks[120].downbeat, true);
        compare(wide.ticks[121].downbeat, false);
    }

    // Nothing to place against: no length, no width, no beats.
    function test_noLengthNoGrid() {
        const beats = grid(500, 0, 60000);
        compare(BeatGridTicks.layout(beats, 0, 600).ticks.length, 0);
        compare(BeatGridTicks.layout(beats, 60000, 0).ticks.length, 0);
        compare(BeatGridTicks.layout([], 60000, 600).ticks.length, 0);
        compare(BeatGridTicks.layout(undefined, 60000, 600).ticks.length, 0);
    }

    // Beats past the end of the track are not drawn off the edge.
    function test_beatsOutsideTheTrackAreLeftOut() {
        const layout = BeatGridTicks.layout(grid(500, -1000, 70000), 60000, 1200);
        compare(layout.ticks[0].x, 0);
        verify(layout.ticks[layout.ticks.length - 1].x <= 1200);
    }

    // A grid that does not say where the bar starts has no downbeats:
    // every beat where there is room, otherwise nothing.
    function test_aGridWithoutBarsHasOnlyBeats() {
        const beats = grid(500, 0, 60000).map((beat) => ({timeMs: beat.timeMs, downbeat: false}));
        compare(BeatGridTicks.layout(beats, 60000, 1200).ticks.length, 120);
        compare(BeatGridTicks.layout(beats, 60000, 600).ticks.length, 0);
    }

    // ---- Stronger marks and bar numbers (#104) ------------------------

    function strongBarsOf(layout) {
        return layout.ticks.filter((tick) => tick.strong).map((tick) => tick.bar);
    }

    // Every beat drawn: every downbeat is a stronger mark, numbered with
    // its bar, and every beat carries the bar it is in.
    function test_everyDownbeatIsStrongWhenEveryBeatIsDrawn() {
        const layout = BeatGridTicks.layout(grid(500, 0, 60000), 60000, 1200);
        compare(layout.everyBeat, true);
        compare(layout.strongBars, 1);
        for (let i = 0; i < layout.ticks.length; ++i) {
            compare(layout.ticks[i].strong, i % 4 === 0, "beat " + i);
            compare(layout.ticks[i].bar, Math.floor(i / 4) + 1, "beat " + i + "'s bar");
            fuzzyCompare(layout.ticks[i].timeMs, i * 500, 1e-9);
        }
    }

    // Every bar drawn: every fourth bar is a stronger mark.
    function test_everyFourthBarIsStrongWhenEveryBarIsDrawn() {
        const layout = BeatGridTicks.layout(grid(500, 0, 60000), 60000, 600);
        compare(layout.barsPerTick, 1);
        compare(layout.strongBars, 4);
        compare(strongBarsOf(layout), [1, 5, 9, 13, 17, 21, 25, 29]);
        compare(layout.ticks.map((tick) => tick.bar).slice(0, 6), [1, 2, 3, 4, 5, 6]);
    }

    // Bars thinned: every 16th bar, or every Nth where more than 16 are
    // thinned to one mark, doubled until the numbers stay apart.
    function test_every16BarsIsStrongWhenBarsAreThinned() {
        const layout = BeatGridTicks.layout(grid(500, 0, 60000), 60000, 150);
        compare(layout.barsPerTick, 4);
        compare(layout.strongBars, 16);
        compare(strongBarsOf(layout), [1, 17]);

        // Ten minutes at 128 BPM: a bar is 1.875 px at 600 px, so every
        // 8th bar has a mark and every 16th is stronger; at 300 px every
        // 16th has a mark and 16 bars (15 px) are too close for numbers.
        const beats = grid(60000 / 128, 0, 600000);
        const at600 = BeatGridTicks.layout(beats, 600000, 600);
        compare(at600.barsPerTick, 8);
        compare(at600.strongBars, 16);
        compare(strongBarsOf(at600).slice(0, 3), [1, 17, 33]);
        const at300 = BeatGridTicks.layout(beats, 600000, 300);
        compare(at300.barsPerTick, 16);
        compare(at300.strongBars, 32);
        compare(strongBarsOf(at300).slice(0, 3), [1, 33, 65]);
        for (const layout of [at600, at300]) {
            const xs = layout.ticks.filter((tick) => tick.strong).map((tick) => tick.x);
            for (let k = 1; k < xs.length; ++k) {
                verify(xs[k] - xs[k - 1] >= BeatGridTicks.minimumBarNumberSpacing(),
                       "numbers " + (xs[k] - xs[k - 1]) + " px apart");
            }
        }
    }

    // Bars count from the grid's first downbeat, before the track's start
    // or not; beats before it are in bar 0 and never a stronger mark.
    function test_barsCountFromTheFirstDownbeat() {
        // Two beats of a bar that started before 0:00 (at -1000 ms).
        const early = BeatGridTicks.layout(grid(500, -1000, 70000), 60000, 1200);
        compare(early.ticks[0].timeMs, 0);
        compare(early.ticks[0].bar, 1);
        compare(early.ticks[0].strong, false);
        compare(early.ticks[2].timeMs, 1000);
        compare(early.ticks[2].bar, 2);
        compare(early.ticks[2].strong, true);
        // The grid's first two beats are the end of a bar it does not have.
        const late = BeatGridTicks.layout(grid(500, 0, 60000, 2), 60000, 1200);
        compare(late.ticks[0].bar, 0);
        compare(late.ticks[0].strong, false);
        compare(late.ticks[2].bar, 1);
        compare(late.ticks[2].strong, true);
        // Thinned, the marks stay on bars 1, 1 + N ... wherever 0:00 is.
        const thinned = BeatGridTicks.layout(grid(500, -1000, 70000), 60000, 150);
        compare(thinned.barsPerTick, 4);
        compare(thinned.ticks.map((tick) => tick.bar).slice(0, 3), [5, 9, 13]);
    }

    // An MP3's grid moved onto the player's timeline (rekordbox's grid,
    // a LAME file: 52 ms earlier) can put the first downbeat before 0:00:
    // the player bar and the popup then draw no mark for bar 1, but every
    // bar keeps its number, the same as the details pane gives it from the
    // unmoved grid. The same track is never numbered two ways.
    function test_aGridMovedBeforeTheStartKeepsItsBarNumbers() {
        const library = grid(500, 20, 60000);
        const moved = library.map((beat) => ({timeMs: beat.timeMs - 52, downbeat: beat.downbeat}));
        for (const width of [600, 1200, 150]) {
            const pane = BeatGridTicks.layout(library, 60000, width);
            const bar = BeatGridTicks.layout(moved, 60000, width);
            verify(bar.ticks.length > 0 && pane.ticks.length > 0);
            compare(pane.ticks[0].bar, 1, "the pane's first mark is bar 1 at " + width);
            for (const tick of bar.ticks) {
                const same = pane.ticks.filter((t) => Math.abs(t.timeMs - 52 - tick.timeMs) < 1e-6);
                if (same.length === 1) {
                    compare(tick.bar, same[0].bar, "the same bar number at " + tick.timeMs + " ms, " + width + " px");
                    compare(tick.strong, same[0].strong, "and the same strength");
                }
            }
            verify(bar.ticks.every((tick) => tick.bar !== 1 || !tick.strong), "bar 1's mark is before 0:00");
        }
    }

    // ---- The view ----------------------------------------------------

    Component {
        id: viewComponent
        Rectangle {
            id: ground
            property alias view: waveform
            // One SignalSpy on each canvas's painted signal (paintSpies).
            property var paints: []
            width: 600
            height: 80
            color: Theme.background
            WaveformView {
                id: waveform
                anchors.fill: parent
                gridStyle: "ruler"
            }
        }
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // A WaveformView draws on three canvases: the bars (the view itself),
    // the grid and the overlay. A Canvas paints once it is available,
    // which may be frames after it is made or changed: under the software
    // scene graph (the offscreen platform on macOS) a grab right after the
    // first rendering found only the ground, and a paint that came late
    // was counted as a repaint by a test that had started counting. So
    // nothing is looked at, or counted, before every canvas has painted.
    // grabImage() renders the window itself; waitForRendering() after the
    // paint only sat out its five seconds for a frame nothing asked for.
    function paintSpies(view) {
        const canvases = [view, findChild(view, "beatGridLayer"), findChild(view, "waveformOverlay")];
        return canvases.map((canvas) => {
            verify(canvas !== null, "a WaveformView has three canvases");
            return createTemporaryObject(spyComponent, testCase, {target: canvas, signalName: "painted"});
        });
    }

    function waitUntilPainted(spies, what) {
        tryVerify(() => spies.every((spy) => spy.count > 0), 5000, what || "every canvas has painted");
        waitForPolish(testCase.Window.window);
    }

    // `change` run, and then every canvas painted again.
    function repaintedAfter(host, change, what) {
        host.paints.forEach((spy) => spy.clear());
        change();
        waitUntilPainted(host.paints, what);
    }

    function makeView(props) {
        const host = createTemporaryObject(viewComponent, testCase);
        verify(host !== null);
        host.paints = paintSpies(host.view);
        for (const key in props) {
            host.view[key] = props[key];
        }
        waitUntilPainted(host.paints);
        return host;
    }

    // How far a pixel is from the ground it is drawn on.
    function inkAt(host, x, y) {
        const image = grabImage(host);
        const c = PixelScale.pixel(image, host, x, y);
        const g = Theme.background;
        return Math.abs(c.r - g.r) + Math.abs(c.g - g.g) + Math.abs(c.b - g.b);
    }

    function distance(a, b) {
        return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);
    }

    // `count` columns of one colour at full height.
    function columns(count, band) {
        const out = [];
        for (let i = 0; i < count; ++i) {
            out.push(band);
        }
        return out;
    }

    // 600 px for a minute at 120 BPM: a mark at every bar, 20 px apart,
    // every fourth (bars 1, 5, 9 ...) stronger. The view is 80 px high:
    // a lane of 16 above 64 of waveform.
    function test_theRulerLaneIsAboveTheWaveform() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        const view = host.view;
        compare(view.beatTicks.length, 30, "600 px: the bars");
        compare(xs(view.beatTicks).slice(0, 3), [0, 20, 40]);
        compare(view.rulerHeight, 16, "a fifth of the height");
        compare(view.waveTop, 16);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(host).save(screenshotDir + "/waveform-beat-grid-600.png");
        }
        // The lane's ground is a shade off the view's, between the marks;
        // under the lane, between the lines, the ground is the view's.
        verify(inkAt(host, 70, 4) > 0.04, "the lane's ground, ink " + inkAt(host, 70, 4));
        verify(inkAt(host, 70, 40) < 0.01, "the waveform's ground, ink " + inkAt(host, 70, 40));
    }

    // A tall tick at each stronger mark with the bar's number beside it,
    // a short one at the foot of the lane at every other mark.
    function test_rulerTicksAndNumbersAreWhereTheBarsAre() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        const view = host.view;
        compare(view.beatTicks[8].bar, 9);
        compare(view.beatTicks[8].strong, true, "bar 9 is a stronger mark");
        compare(view.beatTicks[9].strong, false, "bar 10 is not");
        const image = grabImage(host);
        const lane = PixelScale.pixel(image, host, 70, 4);
        const at = (x, y) => distance(PixelScale.pixel(image, host, x, y), lane);
        // Bar 9 at x 160: its tick from the top of the lane to the foot.
        verify(at(160, 1) > 0.6, "a tall tick at bar 9, " + at(160, 1));
        verify(at(160, 14) > 0.6, "down to the foot");
        // Bar 10 at 180: only at the foot.
        verify(at(180, 2) < 0.02, "no tall tick at bar 10, " + at(180, 2));
        verify(at(180, 14) > 0.4, "a short one at its foot, " + at(180, 14));
        // A number right of bar 9's tick, nothing right of bar 10's.
        let numberInk = 0;
        let besideShort = 0;
        for (let x = 162; x < 170; ++x) {
            for (let y = 2; y < 10; ++y) {
                numberInk = Math.max(numberInk, at(x, y));
                besideShort = Math.max(besideShort, at(x + 20, y));
            }
        }
        verify(numberInk > 0.4, "bar 9's number, ink " + numberInk);
        verify(besideShort < 0.02, "no number at bar 10, ink " + besideShort);
    }

    // Behind the waveform a faint line at every mark, a stronger one at
    // every stronger mark; nothing between them.
    function test_linesStandBehindTheWaveform() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        const faint = inkAt(host, 180, 30);
        const strong = inkAt(host, 160, 30);
        verify(faint > 0.03, "a faint line at bar 10, ink " + faint);
        verify(strong > faint + 0.1, "a stronger one at bar 9: " + strong + " vs " + faint);
        verify(inkAt(host, 190, 30) < 0.01, "ground between them");
    }

    // The bars cover the lines, played (opaque) and not (translucent):
    // a bar's pixel over a line is the bar's colour, the same as beside
    // it, and nothing of the grid shows on any bar.
    function test_nothingOfTheGridIsDrawnOverTheBars() {
        for (const progress of [1, -1]) {
            const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), progress: progress,
                                   waveformData: columns(600, {low: 1, mid: 0, high: 0})});
            const image = grabImage(host);
            const bar = PixelScale.pixel(image, host, 165, 48);
            verify(bar.b > 0.5 && bar.r < bar.b, "a blue bar: " + bar);
            let worst = 0;
            let worstX = -1;
            // Short of the right edge, where a played track's playhead
            // stands with its glow.
            for (let x = 1; x < 585; ++x) {
                const d = distance(PixelScale.pixel(image, host, x, 48), bar);
                if (d > worst) {
                    worst = d;
                    worstX = x;
                }
            }
            verify(worst < 0.02, "progress " + progress + ": every bar the same colour, the worst at "
                   + worstX + " off by " + worst);
            // And the lane is beside the bars, not over them.
            verify(distance(PixelScale.pixel(image, host, 70, 4), bar) > 0.3, "the lane is not a bar");
            if (progress === 1 && screenshotDir && screenshotDir.length > 0) {
                image.save(screenshotDir + "/waveform-beat-grid-behind-bars.png");
            }
        }
    }

    // Wider, every beat has its mark: a short tick and a faint line;
    // every downbeat a tall, numbered one.
    function test_everyBeatHasAFaintLineWhereThereIsRoom() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        repaintedAfter(host, () => host.width = 1200);
        tryCompare(host.view.beatTicks, "length", 120);
        const downbeat = 400;   // beat 40, bar 11
        const beat = 410;       // beat 41
        verify(inkAt(host, beat, 30) > 0.03, "beat 41 has a line, ink " + inkAt(host, beat, 30));
        verify(inkAt(host, beat, 14) > 0.3, "and a short tick");
        verify(inkAt(host, beat, 2) < inkAt(host, 405, 2) + 0.02, "but no tall one");
        verify(inkAt(host, downbeat, 2) > 0.6, "the downbeat's tick is tall");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(host).save(screenshotDir + "/waveform-beat-grid-1200.png");
        }
    }

    // The rule is the width's: narrowing the view thins the grid again,
    // on screen and not just in the numbers.
    function test_resizingTheViewRedrawsTheGrid() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        repaintedAfter(host, () => host.width = 1200);
        tryCompare(host.view.beatTicks, "length", 120);
        verify(inkAt(host, 410, 30) > 0.03);
        repaintedAfter(host, () => host.width = 600);
        tryCompare(host.view.beatTicks, "length", 30);
        verify(inkAt(host, 205, 30) < 0.01, "beat 41's line is gone at 600 px, ink " + inkAt(host, 205, 30));
        verify(inkAt(host, 200, 30) > 0.03, "bar 11's stays");
    }

    // A view too low for a lane to be read (a list row, a 40 px card)
    // draws the lines behind its bars and no lane, no ticks, no numbers.
    function test_aLowViewHasLinesButNoLane() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        repaintedAfter(host, () => host.height = 40);
        compare(host.view.rulerHeight, 0);
        compare(host.view.waveTop, 0);
        verify(inkAt(host, 180, 10) > 0.03, "a line at bar 10, ink " + inkAt(host, 180, 10));
        verify(inkAt(host, 170, 2) < 0.01, "no lane, ink " + inkAt(host, 170, 2));
        verify(inkAt(host, 182, 4) < 0.01, "no number beside bar 9's line");
    }

    // A waveform with no length to place it against (the player bar
    // between a load and the player's duration, or for good when the file
    // cannot be decoded; a card whose row has none) is spread across the
    // width, as it always was: bars, not nothing.
    function test_aWaveformWithoutALengthIsSpreadAcrossTheWidth() {
        const host = makeView({trackDurationMs: 0,
                               waveformData: columns(300, {low: 1, mid: 0, high: 0})});
        compare(host.view.drawsBars, true);
        for (const x of [10, 300, 590]) {
            verify(inkAt(host, x, 40) > 0.3, "a bar at " + x + ", ink " + inkAt(host, x, 40));
        }
    }

    // More than two columns to a pixel: each pixel draws the loudest of
    // the columns under it, so one loud column among quiet ones shows.
    function test_aPixelDrawsTheLoudestOfItsColumns() {
        const data = columns(6000, {low: 0.1, mid: 0, high: 0});
        data[3005] = {low: 1, mid: 0, high: 0};
        const host = makeView({trackDurationMs: 60000, waveformData: data});
        // Ten columns a pixel: column 3005 is pixel 300.
        verify(inkAt(host, 300, 6) > 0.3, "the loud column's pixel is a full bar, ink " + inkAt(host, 300, 6));
        verify(inkAt(host, 310, 6) < 0.01, "its neighbours are quiet, ink " + inkAt(host, 310, 6));
    }

    // Columns handed over as bytes start where columnStartMs says: on the
    // player's timeline, moved as the beats are.
    function test_byteColumnsStartAtTheirStart() {
        const buffer = new ArrayBuffer(100 * 3);
        const bytes = new Uint8Array(buffer);
        bytes[0] = 255;   // the first column loud, the rest silent
        const host = makeView({trackDurationMs: 60000, columnBytes: buffer, columnMs: 100, columnStartMs: 1000});
        // 600 px for 60 s: 1000 ms is x 10.
        verify(inkAt(host, 10, 6) > 0.3, "the first column at 1 s, ink " + inkAt(host, 10, 6));
        verify(inkAt(host, 0, 6) < 0.01 && inkAt(host, 5, 6) < 0.01, "nothing before it");
    }

    // A mark exactly at the track's end is drawn, on the last pixel.
    function test_aMarkAtTheEndIsDrawn() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60001)});
        compare(host.view.beatTicks[host.view.beatTicks.length - 1].x, 600, "bar 31 at 60 s");
        verify(inkAt(host, 599, 30) > 0.03, "its line on the last pixel, ink " + inkAt(host, 599, 30));
        verify(inkAt(host, 599, 14) > 0.3, "and its tick");
    }

    // A stronger mark near the end has its number left of its tick, not
    // cut off at the edge.
    function test_aNumberAtTheEndIsNotCutOff() {
        // 57 s on 600 px: bar 29 at 56 s is a stronger mark at 589.5 px,
        // and its number would run past 600.
        const host = makeView({trackDurationMs: 57000, beatData: grid(500, 0, 57000)});
        const tick = host.view.beatTicks.filter((t) => t.bar === 29)[0];
        verify(tick.strong && tick.x > 585, "bar 29 is a stronger mark at the end, at " + tick.x);
        const image = grabImage(host);
        const lane = PixelScale.pixel(image, host, 575, 13);
        let leftInk = 0;
        for (let x = 572; x < 588; ++x) {
            for (let y = 2; y < 10; ++y) {
                leftInk = Math.max(leftInk, distance(PixelScale.pixel(image, host, x, y), lane));
            }
        }
        verify(leftInk > 0.4, "its number left of the tick, ink " + leftInk);
    }

    // Columns a little narrower than a pixel do not overwrite each other:
    // drawn one bar a pixel, the loudest column of each.
    function test_narrowColumnsDoNotOverwriteEachOther() {
        const data = columns(800, {low: 0.1, mid: 0, high: 0});
        for (let i = 0; i < 800; i += 4) {
            data[i] = {low: 1, mid: 0, high: 0};
        }
        // 800 columns on 600 px: 0.75 px a column, every fourth loud.
        const host = makeView({trackDurationMs: 60000, waveformData: data, progress: 1});
        const image = grabImage(host);
        const full = PixelScale.pixel(image, host, 0, 40);
        verify(distance(full, Theme.background) > 0.3, "a loud bar at x 0: " + full);
        // Every loud column is 3 px apart; every pixel that holds one is a
        // full, unblended bar.
        // Short of the right edge, where the played track's playhead
        // stands with its glow.
        for (let i = 0; i < 770; i += 4) {
            const x = Math.floor(i * 0.75);
            const c = PixelScale.pixel(image, host, x, 40);
            verify(distance(c, full) < 0.05, "column " + i + " at x " + x + " is whole: " + c + " vs " + full);
        }
    }

    // The sliced grid (the player bar, option G): cut out of the bars as
    // gaps in the ground, narrow at a mark and wide at a stronger one,
    // with short marks at the top and bottom edge, accent at the stronger;
    // no lane and no lines.
    function test_theSlicedGridIsCutOutOfTheBars() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), gridStyle: "sliced",
                               progress: 1, waveformData: columns(600, {low: 1, mid: 0, high: 0})});
        compare(host.view.rulerHeight, 0, "no lane");
        const image = grabImage(host);
        const at = (x, y) => PixelScale.pixel(image, host, x, y);
        const bar = at(185, 40);
        verify(bar.b > 0.5, "a bar: " + bar);
        const ground = Theme.background;
        // Bar 10 at 180, an ordinary mark: a gap two pixels wide (179, 180).
        verify(distance(at(179, 40), ground) < 0.02 && distance(at(180, 40), ground) < 0.02,
               "the gap is the ground: " + at(179, 40) + " " + at(180, 40));
        verify(distance(at(178, 40), bar) < 0.02 && distance(at(181, 40), bar) < 0.02, "the bars beside it");
        // Its edge marks, muted, at the top and the bottom.
        verify(distance(at(180, 1), ground) > 0.1 && distance(at(180, 1), bar) > 0.1, "a mark at the top");
        verify(distance(at(180, 78), ground) > 0.1, "and the bottom");
        verify(distance(at(180, 20), ground) < 0.02, "the gap between them is the ground");
        // Bar 9 at 160, a stronger mark: a wider gap and accent marks.
        verify(distance(at(159, 40), ground) < 0.02 && distance(at(161, 40), ground) < 0.02, "a wider gap");
        verify(distance(at(160, 1), Theme.accent) < 0.1, "an accent mark: " + at(160, 1));
        // Nothing of the grid elsewhere: no lines, no lane.
        const empty = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), gridStyle: "sliced"});
        verify(inkAt(empty, 180, 30) < 0.01, "no line behind, ink " + inkAt(empty, 180, 30));
        verify(inkAt(empty, 170, 4) < 0.01, "no lane");
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-sliced.png");
        }
    }

    // Each look draws as the software whose analysis it is (#108's
    // sibling plan, "waveform colour follows the data"). rekordbox's 3Band
    // overview stands on the bottom edge, its bands stacked outward: high
    // at the base, mid above it, low on top, in Theme's colours. Engine's
    // is centred, each band as tall as its own value. Blue is one bar a
    // column, blue going white with the whiteness. Columns of low 0.6,
    // mid 0.3, high 0.12: stacked, the high layer reaches 0.1 of the
    // height, mid + high 0.35, all three 0.85.
    // `change` run, and then the bars painted again: a look changes only
    // them, not the grid's canvas or the overlay.
    function barsRepaintedAfter(host, change) {
        host.paints[0].clear();
        change();
        tryVerify(() => host.paints[0].count > 0, 5000, "the bars painted again");
        waitForPolish(testCase.Window.window);
    }
    function test_eachLookDrawsAsItsSoftwareDoes() {
        const band = {low: 0.6, mid: 0.3, high: 0.12};
        const near = (c, want) => distance(c, want) < 0.12;
        const host = makeView({trackDurationMs: 60000, gridStyle: "lines", progress: 1,
                               look: "rekordbox-3band", waveformData: columns(600, band)});
        verify(host.view.bottomAnchored, "a rekordbox overview stands on the bottom edge");
        const top = host.view.waveTop, h = host.view.waveHeight;
        let image = grabImage(host);
        let at = (fraction) => PixelScale.pixel(image, host, 300, top + h * (1 - fraction));
        verify(near(at(0.05), Theme.waveformRekordboxHigh), "white at the base: " + at(0.05));
        verify(near(at(0.22), Theme.waveformRekordboxMid), "amber above it: " + at(0.22));
        verify(near(at(0.6), Theme.waveformRekordboxLow), "blue on top: " + at(0.6));
        verify(near(at(0.95), Theme.background), "the ground above: " + at(0.95));
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-look-rekordbox-3band.png");
        }

        // Zoomed in (not an overview): around the centre line.
        barsRepaintedAfter(host, () => host.view.overview = false);
        verify(!host.view.bottomAnchored);
        image = grabImage(host);
        const centre = top + h / 2;
        at = (dy) => PixelScale.pixel(image, host, 300, centre + dy * h);
        verify(near(at(0.02), Theme.waveformRekordboxHigh), "white at the centre line: " + at(0.02));
        verify(near(at(-0.11), Theme.waveformRekordboxMid), "amber around it: " + at(-0.11));
        verify(near(at(0.3), Theme.waveformRekordboxLow), "blue outside: " + at(0.3));
        verify(near(at(-0.47), Theme.background), "the ground past it: " + at(-0.47));

        // Engine: centred, each band as tall as itself, high on top.
        barsRepaintedAfter(host, () => host.view.look = "engine");
        verify(!host.view.bottomAnchored, "Engine's overview is centred");
        image = grabImage(host);
        verify(near(at(0.03), Theme.waveformEngineHigh), "Engine's white core: " + at(0.03));
        verify(near(at(-0.1), Theme.waveformEngineMid), "Engine's green: " + at(-0.1));
        verify(near(at(0.22), Theme.waveformEngineLow), "Engine's blue: " + at(0.22));
        verify(near(at(0.4), Theme.background), "the ground past the low band: " + at(0.4));

        // Blue: a whiteness of none is blue, a full one nearly white.
        barsRepaintedAfter(host, () => {
            host.view.look = "rekordbox-blue";
            host.view.overview = true;
            host.view.waveformData = columns(300, {low: 0.8, mid: 0.44, high: 0})
                .concat(columns(300, {low: 0.8, mid: 0.8, high: 0.8}));
        });
        verify(host.view.bottomAnchored);
        image = grabImage(host);
        const blue = PixelScale.pixel(image, host, 150, top + h * 0.7);
        const white = PixelScale.pixel(image, host, 450, top + h * 0.7);
        verify(near(blue, Theme.waveformRekordboxBlue), "no whiteness: blue " + blue);
        verify(white.r > 0.85 && white.g > 0.85 && white.b > 0.9, "full whiteness: nearly white " + white);
        verify(near(PixelScale.pixel(image, host, 150, top + h * 0.1), Theme.background), "0.8 of the height, from the bottom");
    }

    // A look not known draws as Blue draws one height, never nothing.
    function test_anUnknownLookStillDraws() {
        const host = makeView({trackDurationMs: 60000, gridStyle: "lines", progress: 1,
                               waveformData: columns(600, {low: 1, mid: 0, high: 0})});
        compare(host.view.look, "");
        verify(!host.view.bottomAnchored, "centred");
        verify(inkAt(host, 300, host.view.waveTop + host.view.waveHeight / 2) > 0.3, "bars drawn");
    }

    // A view states its grid style; one that does not gets the lines only,
    // never a lane by accident.
    Component {
        id: plainView
        WaveformView { width: 600; height: 80 }
    }
    function test_theDefaultGridStyleIsLines() {
        const view = createTemporaryObject(plainView, testCase, {trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        compare(view.gridStyle, "lines");
        compare(view.rulerHeight, 0);
    }

    // Sliced, every beat would cut too much of the bars away: the gaps of
    // ordinary marks stay at least 12 px apart.
    function test_slicedGapsAreThinned() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), gridStyle: "sliced",
                               progress: 1, waveformData: columns(1200, {low: 1, mid: 0, high: 0})});
        repaintedAfter(host, () => host.width = 1200);
        tryCompare(host.view.beatTicks, "length", 120);
        const image = grabImage(host);
        const at = (x) => PixelScale.pixel(image, host, x, 40);
        const ground = Theme.background;
        verify(distance(at(410), ground) < 0.02, "beat 41's gap");
        verify(distance(at(420), ground) > 0.3, "beat 42, 10 px on, has none: " + at(420));
        verify(distance(at(430), ground) < 0.02, "beat 43's gap");
    }

    // A view handed beats already selected for its width (every view in
    // the app is) places them and selects nothing again: the rule runs
    // once per picture. A whole grid handed that way is drawn whole.
    function test_selectedBeatsAreDrawnAsHanded() {
        const host = makeView({trackDurationMs: 60000, beatsSelected: true, beatData: grid(500, 0, 60000)});
        compare(host.view.beatTicks.length, 120, "all 120, though the rule would keep 30 at 600 px");
        compare(xs(host.view.beatTicks), placed(grid(500, 0, 60000), 60000, 600));
        host.view.beatsSelected = false;
        compare(host.view.beatTicks.length, 30, "selected by the view itself otherwise");
    }

    // A track without a grid, or one whose grid the app does not show
    // (an Engine row that records no sample rate hands an empty list,
    // see beat_grid_display_test), draws no lane and no lines at all and
    // says nothing about it.
    function test_noBeatsDrawNoGrid() {
        const host = makeView({trackDurationMs: 60000, beatData: []});
        compare(host.view.beatTicks.length, 0);
        compare(host.view.rulerHeight, 0, "no lane");
        for (let x = 0; x < 600; x += 5) {
            verify(inkAt(host, x, 4) < 0.01, "no ink at " + x);
            verify(inkAt(host, x, 30) < 0.01, "no line at " + x);
        }
    }

    // A cue sitting on a mark is drawn over its line, in the waveform's
    // band under the lane.
    function test_cueMarkersDrawOnTopOfTheGrid() {
        const host = makeView({
            trackDurationMs: 60000,
            beatData: grid(500, 0, 60000),
            cueData: [{kind: "hot", hotCueNumber: 1, positionMs: 20000, color: "#ff0000", isLoop: false}],
        });
        const image = grabImage(host);
        // x 200: bar 11 and the cue. The flag stands at the top of the
        // waveform's band (its number starts two pixels in), the line
        // runs down it.
        const top = host.view.waveTop;
        const flag = PixelScale.pixel(image, host, 200, top + 1);
        verify(flag.r > 0.9 && flag.g < 0.2 && flag.b < 0.2, "the cue's flag is on top: " + flag);
        const line = PixelScale.pixel(image, host, 200, host.height - 3);
        verify(line.r > 0.9 && line.g < 0.2 && line.b < 0.2, "and so is its line: " + line);
    }

    // The lane is drawn once per change of data or size, never per frame
    // of playback: moving the playhead repaints the playhead.
    function test_playbackDoesNotRepaintTheGrid() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), progress: 0});
        const gridLayer = findChild(host.view, "beatGridLayer");
        verify(gridLayer !== null, "the lane has a layer of its own");
        const overlay = findChild(host.view, "waveformOverlay");
        verify(overlay !== null);
        const gridPaints = createTemporaryObject(spyComponent, testCase, {target: gridLayer, signalName: "paint"});
        const overlayPaints = createTemporaryObject(spyComponent, testCase, {target: overlay, signalName: "paint"});
        for (let i = 1; i <= 10; ++i) {
            host.view.progress = i / 20;
            wait(20);
        }
        verify(overlayPaints.count > 0, "the playhead moved and was repainted");
        compare(gridPaints.count, 0, "the lane was not");
        host.width = 700;
        tryVerify(() => gridPaints.count > 0, 2000, "a new width repaints it");
    }

    // The overlay is repainted for everything it draws: a highlighted
    // cue, new cues, and the loop being dragged out.
    function test_theOverlayRepaintsForWhatItDraws() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), cueEditable: true,
                               cueData: [{kind: "hot", hotCueNumber: 1, positionMs: 20000, color: "#ff0000", isLoop: false}]});
        const overlay = findChild(host.view, "waveformOverlay");
        const gridLayer = findChild(host.view, "beatGridLayer");
        const paints = createTemporaryObject(spyComponent, testCase, {target: overlay, signalName: "paint"});
        const gridPaints = createTemporaryObject(spyComponent, testCase, {target: gridLayer, signalName: "paint"});

        host.view.highlightCuePositionMs = 20000;
        tryVerify(() => paints.count > 0, 2000, "a highlighted cue repaints the overlay");
        paints.clear();
        host.view.cueData = [{kind: "memory", positionMs: 30000, isLoop: false}];
        tryVerify(() => paints.count > 0, 2000, "new cues repaint it");
        paints.clear();
        mousePress(host.view, 100, 40);
        mouseMove(host.view, 160, 40);
        tryVerify(() => paints.count > 0, 2000, "dragging out a loop repaints it");
        mouseRelease(host.view, 160, 40);
        compare(gridPaints.count, 0, "none of it repaints the lane");
    }

    // A view showing a stretch of the track (a tile of the zoomed popup)
    // places the bars, the marks and the cues by time within it.
    function test_aViewShowsAStretchOfTheTrack() {
        const host = makeView({trackDurationMs: 60000, beatsSelected: true,
                               beatData: BeatGridTicks.layout(grid(500, 0, 60000), 60000, 6000).ticks,
                               viewStartMs: 10000, viewSpanMs: 6000});
        // 6 s across 600 px: a beat every 50 px, from 10 s on.
        compare(host.view.beatTicks[20].timeMs, 10000);
        fuzzyCompare(host.view.beatTicks[20].x, 0, 1e-9);
        fuzzyCompare(host.view.beatTicks[21].x, 50, 1e-9);
        compare(host.view.beatTicks[20].bar, 6);
        compare(host.view.beatTicks[20].strong, true);
        verify(inkAt(host, 0, 1) > 0.6, "bar 6's tall tick at the left edge");
        verify(inkAt(host, 50, 30) > 0.03, "a line at the next beat");
        verify(inkAt(host, 25, 30) < 0.01, "ground between beats");
        compare(host.view.timeAtX(300), 13000);
    }

    Component {
        id: fadedComponent
        Rectangle {
            property alias view: fadedView
            width: 600
            height: 80
            color: Theme.background
            WaveformView {
                id: fadedView
                anchors.fill: parent
            }
        }
    }

    // A faded waveform (a row backdrop, a dimmed card) is faded as one
    // picture, as the single canvas it was: a cue line over a bar shows
    // the cue's colour faded over the ground, not over the bar.
    function test_aFadedViewFadesAsOnePicture() {
        const columns = [];
        for (let i = 0; i < 400; ++i) {
            columns.push({low: 1, mid: 1, high: 1});
        }
        const host = createTemporaryObject(fadedComponent, testCase);
        const paints = paintSpies(host.view);
        host.view.waveformData = columns;
        host.view.trackDurationMs = 60000;
        host.view.cueData = [{kind: "hot", hotCueNumber: 1, positionMs: 30000, color: "#ff0000", isLoop: false}];
        host.view.opacity = 0.5;
        waitUntilPainted(paints, "the bars and the cue are painted");
        const image = grabImage(host);
        // x 300 is the cue line, two pixels wide over a full-height bar.
        const got = PixelScale.pixel(image, host, 300, host.height / 2);
        const ground = Theme.background;
        const expected = {r: 0.5 * 1 + 0.5 * ground.r, g: 0.5 * ground.g, b: 0.5 * ground.b};
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-faded.png");
        }
        verify(Math.abs(got.r - expected.r) < 0.02 && Math.abs(got.g - expected.g) < 0.02
               && Math.abs(got.b - expected.b) < 0.02,
               "the cue line at half opacity is " + JSON.stringify(expected) + ", got " + got);
    }

    // ---- Where the grid comes from -----------------------------------

    Component {
        id: barComponent
        PlayerBar { width: 1000 }
    }

    // The player bar asks the controller for the loaded track's grid at
    // its own width and length, and asks again when either changes.
    function test_thePlayerBarDrawsTheLoadedTracksGrid() {
        const asked = [];
        const controller = {title: "Major Tom", artist: "DJ Amador", artworkPath: "", position: 0, duration: 60000,
                            playing: false, errorMessage: "", waveform: [], cues: [], trackSerial: 1,
                            beatTicksAt: function(durationMs, widthPx) {
                                asked.push(widthPx);
                                return durationMs === 60000 && widthPx > 0 ? testCase.grid(500, 0, 60000) : [];
                            },
                            togglePlay: function() {}, seek: function(ms) {}, stop: function() {}};
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        verify(bar !== null);
        const view = findChild(bar, "playerWaveform");
        verify(view !== null);
        tryVerify(() => view.beatTicks.length > 0, 2000, "the bar's waveform has a grid");
        compare(asked[asked.length - 1], view.width, "asked for at the waveform's width");
        compare(view.gridStyle, "sliced", "cut out of the bars, not a ruler lane (#104)");
        bar.width = 700;
        tryVerify(() => asked[asked.length - 1] === view.width && view.width < 700, 2000, "and again at a new width");
    }

    Component {
        id: liveTrack
        PlaybackController {}
    }

    // The real controller with a track loaded: a fixture track's analysis,
    // played from the click file. The bar's list is only the beats drawn
    // at its width, laid out as the same ticks as the whole grid.
    function test_theLoadedTracksTicksAreThinnedForTheWidth() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0);
        const pioneer = stick + "/PIONEER";
        const ids = stickFixture.rekordboxTrackIds(pioneer, 10);
        const player = createTemporaryObject(liveTrack, testCase);
        let id = "";
        for (let i = 0; i < ids.length && id === ""; ++i) {
            if (player.beatGridFor("rekordbox", pioneer, ids[i]).length > 100) {
                id = ids[i];
            }
        }
        verify(id !== "", "a fixture track with a grid");
        const clickFile = Qt.resolvedUrl("../fixtures/mp3-click/click-D-lame-44100.mp3").toString()
            .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1");
        const serial = player.trackSerial;
        player.load("rekordbox", pioneer, id, clickFile, "Click", "Artist", "", []);
        verify(player.trackSerial !== serial, "a new track is a new serial");
        const whole = player.beatGridFor("rekordbox", pioneer, id);
        const durationMs = Math.ceil(whole[whole.length - 1].timeMs / 1000) * 1000 + 1000;
        for (const width of [300, 600, 1200]) {
            const kept = player.beatTicksAt(durationMs, width);
            verify(kept.length > 0 && kept.length < whole.length, width + " px: " + kept.length + " of " + whole.length);
            // On the playback timeline, moved as the beats the ring keeps
            // time by are (a LAME MP3 plays earlier than rekordbox counts).
            const shiftMs = player.beatTimesMs[0] - whole[0].timeMs;
            const shifted = whole.map((beat) => ({timeMs: beat.timeMs + shiftMs, downbeat: beat.downbeat}));
            const got = placed(kept, durationMs, width);
            const want = BeatGridTicks.layout(shifted, durationMs, width).ticks;
            compare(got.length, want.length, "as many ticks at " + width);
            for (let i = 0; i < want.length; ++i) {
                fuzzyCompare(got[i], want[i].x, 1e-6, "tick " + i + " at " + width);
                compare(kept[i].downbeat, want[i].downbeat);
            }
        }
        compare(player.beatTicksAt(0, 600).length, 0, "no length, nothing");
        player.stop();
        compare(player.beatTicksAt(durationMs, 600).length, 0, "nothing once stopped");
    }

    Component {
        id: player
        PlaybackController {}
    }

    Component {
        id: detailComponent
        TrackDetailPage { width: 1200; height: 500 }
    }

    // The real controller, on a copy of the committed fixture: a
    // rekordbox track's grid comes with its waveform, every beat with its
    // time and whether it starts a bar, and the detail page draws it.
    function test_theDetailPageDrawsTheLibrarysGrid() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "a copy of the fixture");
        const pioneer = stick + "/PIONEER";
        const ids = stickFixture.rekordboxTrackIds(pioneer, 10);
        verify(ids.length > 0, "the fixture has rekordbox tracks");
        const controller = createTemporaryObject(player, testCase);
        let id = "";
        let beats = [];
        for (let i = 0; i < ids.length && beats.length === 0; ++i) {
            beats = controller.beatGridFor("rekordbox", pioneer, ids[i]);
            id = ids[i];
        }
        verify(beats.length > 100, "a fixture track with a grid, got " + beats.length);
        for (let i = 1; i < beats.length; ++i) {
            verify(beats[i].timeMs > beats[i - 1].timeMs, "in order at " + i);
        }
        const downbeats = beats.filter((beat) => beat.downbeat === true).length;
        verify(downbeats > 0 && Math.abs(downbeats - beats.length / 4) <= 2,
               downbeats + " downbeats of " + beats.length + ": one a bar");
        const waveform = controller.waveformFor("rekordbox", pioneer, id);
        verify(waveform.length > 0, "and the waveform of the same read");

        // A list row is handed only the beats it will draw, and draws the
        // same ticks as the whole grid would at its width.
        const durationMs = Math.ceil(beats[beats.length - 1].timeMs / 1000) * 1000 + 1000;
        for (const width of [300, 600, 1200]) {
            const kept = controller.beatTicksFor("rekordbox", pioneer, id, durationMs, width);
            const whole = BeatGridTicks.layout(beats, durationMs, width).ticks;
            verify(kept.length > 0 && kept.length < beats.length || whole.length === beats.length,
                   "a row is handed fewer beats at " + width + " px: " + kept.length + " of " + beats.length);
            compare(placed(kept, durationMs, width), xs(whole), "same ticks at " + width);
            verify(whole.some((tick) => tick.strong === true), "stronger marks at " + width);
            compare(kept.map((beat) => beat.strong), whole.map((tick) => tick.strong), "same stronger marks at " + width);
            compare(kept.map((beat) => beat.bar), whole.map((tick) => tick.bar), "same bar numbers at " + width);
        }
        compare(controller.beatTicksFor("rekordbox", pioneer, id, 0, 600).length, 0, "no length, nothing");
        compare(controller.beatTicksFor("rekordbox", pioneer, id, durationMs, 0).length, 0, "no width, nothing");

        const lastMs = beats[beats.length - 1].timeMs;
        const track = {title: "Fixture track", artist: "Anonymized", key: "8A", bpm: 120,
                       durationSeconds: Math.ceil(lastMs / 1000) + 1, artworkPath: "",
                       cues: [{kind: "hot", hotCueNumber: 1, positionMs: beats[64].timeMs, color: "#30d050",
                               isLoop: false}],
                       filePath: "", sourceId: id};
        const page = createTemporaryObject(detailComponent, testCase, {
            scanController: {trackAt: (index) => track, trackCount: () => 1,
                             keyRelation: () => ({label: "", relation: "unknown"})},
            trackIndex: 0, format: "rekordbox", libraryPath: pioneer,
            playbackController: controller, appSettingsController: {keyNotation: "camelot"},
        });
        verify(page !== null);
        const view = findChild(page, "trackDetailWaveform");
        verify(view !== null);
        waitForPolish(testCase.Window.window);
        tryVerify(() => view.beatTicks.length > 0, 2000, "the page draws the grid");
        verify(view.beatData.length < beats.length, "handed only the beats drawn at its width, "
               + view.beatData.length + " of " + beats.length);
        compare(xs(view.beatTicks), xs(BeatGridTicks.layout(beats, track.durationSeconds * 1000, view.width).ticks),
                "the same ticks as the whole grid");
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/track-detail-beat-grid.png");
        }
    }
}
