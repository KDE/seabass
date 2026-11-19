// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// The beat grid drawn over a waveform (#40): ticks where the library's
// own grid puts each beat, downbeats taller and brighter, and only as
// many as the width has room for. Seabass draws a grid here and never
// makes one up.
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

    // ---- The view ----------------------------------------------------

    Component {
        id: viewComponent
        Rectangle {
            id: ground
            property alias view: waveform
            width: 600
            height: 80
            color: Theme.background
            WaveformView {
                id: waveform
                anchors.fill: parent
            }
        }
    }

    function makeView(props) {
        const host = createTemporaryObject(viewComponent, testCase);
        verify(host !== null);
        for (const key in props) {
            host.view[key] = props[key];
        }
        waitForPolish(testCase.Window.window);
        waitForRendering(host);
        return host;
    }

    // How far a pixel is from the ground it is drawn on.
    function inkAt(host, x, y) {
        const image = grabImage(host);
        const c = PixelScale.pixel(image, host, x, y);
        const g = Theme.background;
        return Math.abs(c.r - g.r) + Math.abs(c.g - g.g) + Math.abs(c.b - g.b);
    }

    function test_theViewDrawsTicksWhereTheBeatsAre() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        const view = host.view;
        compare(view.beatTicks.length, 30, "600 px: the downbeats");
        compare(xs(view.beatTicks).slice(0, 3), [0, 20, 40]);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(host).save(screenshotDir + "/waveform-beat-grid-600.png");
        }
        // A tick at the top edge over each downbeat, ground between them.
        verify(inkAt(host, 200, 2) > 0.3, "a downbeat tick at 200, ink " + inkAt(host, 200, 2));
        verify(inkAt(host, 210, 2) < 0.05, "ground between ticks, ink " + inkAt(host, 210, 2));
        verify(inkAt(host, 200, host.height - 3) > 0.3, "and at the foot");
    }

    // Wider, the ordinary beats join in: shorter and fainter than the
    // downbeats, so the bar still reads.
    function test_downbeatsAreTallerAndBrighter() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        host.width = 1200;
        waitForPolish(testCase.Window.window);
        tryCompare(host.view.beatTicks, "length", 120);
        waitForRendering(host);
        const downbeat = 400;   // beat 40
        const beat = 410;       // beat 41
        verify(inkAt(host, beat, 2) > 0.05, "an ordinary beat has a tick, ink " + inkAt(host, beat, 2));
        verify(inkAt(host, downbeat, 2) > inkAt(host, beat, 2) + 0.2,
               "a downbeat's is brighter: " + inkAt(host, downbeat, 2) + " vs " + inkAt(host, beat, 2));
        const below = host.height * 0.25;
        verify(inkAt(host, downbeat, below) > 0.3, "a downbeat's reaches further in");
        verify(inkAt(host, beat, below) < 0.05, "an ordinary beat's does not, ink " + inkAt(host, beat, below));
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(host).save(screenshotDir + "/waveform-beat-grid-1200.png");
        }
    }

    // The rule is the width's: narrowing the view thins the grid again,
    // on screen and not just in the numbers.
    function test_resizingTheViewRedrawsTheGrid() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000)});
        host.width = 1200;
        tryCompare(host.view.beatTicks, "length", 120);
        waitForRendering(host);
        verify(inkAt(host, 410, 2) > 0.05);
        host.width = 600;
        tryCompare(host.view.beatTicks, "length", 30);
        waitForRendering(host);
        verify(inkAt(host, 205, 2) < 0.05, "beat 41 is gone at 600 px, ink " + inkAt(host, 205, 2));
        verify(inkAt(host, 200, 2) > 0.3, "downbeat 40 stays");
    }

    // A track without a grid, or one whose grid the app does not show
    // (an Engine row that records no sample rate hands an empty list,
    // see beat_grid_display_test), draws no ticks at all and says
    // nothing about it.
    function test_noBeatsDrawNoTicks() {
        const host = makeView({trackDurationMs: 60000, beatData: []});
        compare(host.view.beatTicks.length, 0);
        for (let x = 0; x < 600; x += 5) {
            verify(inkAt(host, x, 2) < 0.05, "no ink at " + x);
        }
    }

    // A cue sitting on a downbeat is drawn over the tick, never under it.
    function test_cueMarkersDrawOnTopOfTheGrid() {
        const host = makeView({
            trackDurationMs: 60000,
            beatData: grid(500, 0, 60000),
            cueData: [{kind: "hot", hotCueNumber: 1, positionMs: 20000, color: "#ff0000", isLoop: false}],
        });
        const image = grabImage(host);
        // x 200: downbeat 10 and the cue. The flag covers the tick's top
        // (its number starts two pixels in), the line its foot.
        const flag = PixelScale.pixel(image, host, 200, 1);
        verify(flag.r > 0.9 && flag.g < 0.2 && flag.b < 0.2, "the cue's flag is on top: " + flag);
        const line = PixelScale.pixel(image, host, 200, host.height - 3);
        verify(line.r > 0.9 && line.g < 0.2 && line.b < 0.2, "and so is its line: " + line);
    }

    // The grid is drawn once per change of data or size, never per
    // frame of playback: moving the playhead repaints the playhead.
    function test_playbackDoesNotRepaintTheGrid() {
        const host = makeView({trackDurationMs: 60000, beatData: grid(500, 0, 60000), progress: 0});
        const gridLayer = findChild(host.view, "beatGridLayer");
        verify(gridLayer !== null, "the grid has a layer of its own");
        const overlay = findChild(host.view, "waveformOverlay");
        verify(overlay !== null);
        const gridPaints = createTemporaryObject(spyComponent, testCase, {target: gridLayer, signalName: "paint"});
        const overlayPaints = createTemporaryObject(spyComponent, testCase, {target: overlay, signalName: "paint"});
        for (let i = 1; i <= 10; ++i) {
            host.view.progress = i / 20;
            wait(20);
        }
        verify(overlayPaints.count > 0, "the playhead moved and was repainted");
        compare(gridPaints.count, 0, "the grid was not");
        host.width = 700;
        tryVerify(() => gridPaints.count > 0, 2000, "a new width repaints it");
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // ---- Where the grid comes from -----------------------------------

    Component {
        id: barComponent
        PlayerBar { width: 1000 }
    }

    // The player bar draws the loaded track's grid, which the controller
    // already holds (the same read as the waveform).
    function test_thePlayerBarDrawsTheLoadedTracksGrid() {
        const controller = {title: "Major Tom", artist: "DJ Amador", artworkPath: "", position: 0, duration: 60000,
                            playing: false, errorMessage: "", waveform: [], cues: [],
                            beatGrid: grid(500, 0, 60000),
                            togglePlay: function() {}, seek: function(ms) {}, stop: function() {}};
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        verify(bar !== null);
        const view = findChild(bar, "playerWaveform");
        verify(view !== null);
        tryVerify(() => view.beatTicks.length > 0, 2000, "the bar's waveform has a grid");
        compare(view.beatData.length, 120);
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
            compare(xs(BeatGridTicks.layout(kept, durationMs, width).ticks), xs(whole), "same ticks at " + width);
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
        compare(view.beatData.length, beats.length, "the page hands the view the whole grid");
        waitForPolish(testCase.Window.window);
        tryVerify(() => view.beatTicks.length > 0, 2000, "and it draws some of it");
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/track-detail-beat-grid.png");
        }
    }
}
