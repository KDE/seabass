// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// The waveform zoomed in (#104): opened from the track details pane it
// loads the track into the player (paused when nothing played, playing
// when something did), a fixed position line at the golden-ratio point
// with the waveform moving under it, zoom from the whole track to four
// bars, scrolling by hand that lets go of the playhead and play, a seek
// or the follow button that takes hold again, and the ruler and the beat
// lines drawn at a zoom where every beat fits. A fake controller with the
// player's properties and calls throughout: no audio is played.
TestCase {
    id: testCase
    name: "WaveformZoomPopup"
    width: 1400
    height: 800
    visible: true
    when: windowShown

    // 120 BPM: a beat every 500 ms, a bar every 2 s, the first beat of
    // every four a downbeat, over four minutes.
    readonly property real trackMs: 240000
    readonly property string fixtureRoot: Qt.resolvedUrl("../fixtures/anonymized_library").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")
    readonly property string clickFile: Qt.resolvedUrl("../fixtures/mp3-click/click-D-lame-44100.mp3").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")
    readonly property string otherClickFile: clickFile.replace("click-D-lame-44100", "click-D-lame-48000")
    function grid(fromMs, toMs, beatMs) {
        const beats = [];
        let index = 0;
        for (let t = fromMs; t < toMs - 1e-6; t += beatMs || 500) {
            beats.push({timeMs: t, downbeat: index % 4 === 0});
            ++index;
        }
        return beats;
    }

    Component {
        id: fakePlayer
        QtObject {
            property bool hasTrack: true
            property string currentFormat: "rekordbox"
            property string currentSourceId: "7"
            property string currentLibraryPath: "/fixture/PIONEER"
            property string title: "Major Tom"
            property string artist: "DJ Amador"
            property real duration: testCase.trackMs
            property real position: 0
            property bool playing: false
            property var waveform: []
            property var cues: [{kind: "hot", hotCueNumber: 1, positionMs: 64000, color: "#ff0000", isLoop: false}]
            property int trackSerial: 1
            // The real player's: its length is known for this track. Bound,
            // so always known unless a test sets it.
            property int durationSerial: trackSerial
            property var beatTimesMs: testCase.grid(0, testCase.trackMs).map((beat) => beat.timeMs)
            property var calls: []
            property var loads: []
            // What the details pane asks for, set by the tests that show one.
            property var waveformFor: null
            property var beatTicksFor: null
            function cuesOnGridTimeline(format, path, cues) { return cues; }
            // Quiet columns, a fifth of the height, 150 a second: the lines
            // behind them show above and below.
            property var detail: {
                const n = Math.floor(testCase.trackMs * 150 / 1000);
                const buffer = new ArrayBuffer(n * 3);
                const bytes = new Uint8Array(buffer);
                for (let i = 0; i < n * 3; i += 3) {
                    bytes[i] = 51;
                    bytes[i + 1] = 20;
                    bytes[i + 2] = 10;
                }
                return {columns: buffer, msPerColumn: 1000 / 150, startMs: 0};
            }
            signal seeked(real positionMs)
            signal trackChanged()
            // The player's own: empty until asked for, then (autoDetail)
            // handed over at once, or when a test sets it.
            property var detailWaveform: ({})
            property bool autoDetail: true
            property int detailRequests: 0
            property int detailReleases: 0
            function releaseDetailWaveform() { ++detailReleases; }
            property real beatMs: 500
            property bool noGrid: false
            property bool libraryBusy: false
            function requestDetailWaveform() {
                ++detailRequests;
                if (autoDetail) {
                    detailWaveform = detail;
                }
            }
            function beatTicksAt(durationMs, widthPx) {
                return noGrid ? [] : BeatGridTicks.layout(testCase.grid(0, durationMs, beatMs), durationMs, widthPx).ticks;
            }
            // Another track loaded, as the queue moving on does it.
            signal trackAboutToChange()
            function changeTrack(newTitle, newArtist, newBeatMs, newDetail) {
                trackAboutToChange();
                title = newTitle;
                artist = newArtist;
                beatMs = newBeatMs;
                beatTimesMs = testCase.grid(0, duration, newBeatMs).map((beat) => beat.timeMs);
                position = 0;
                detail = newDetail;
                detailWaveform = ({});
                ++trackSerial;
                trackChanged();
            }
            function load(format, libraryPath, sourceId, filePath, title, artist, artworkPath, cues,
                          fallbackArtworkPath, startPlaying) {
                loads.push({sourceId: sourceId, filePath: filePath, startPlaying: startPlaying});
                calls.push("load");
                hasTrack = true;
                currentFormat = format;
                currentLibraryPath = libraryPath;
                currentSourceId = sourceId;
                position = 0;
                playing = startPlaying !== false;
                ++trackSerial;
                trackChanged();
            }
            function togglePlay() { calls.push("togglePlay"); playing = !playing; }
            function play() { calls.push("play"); playing = true; }
            function pause() { calls.push("pause"); playing = false; }
            function stop() { calls.push("stop"); playing = false; hasTrack = false; }
            function seek(ms) { calls.push("seek"); position = ms; seeked(ms); }
        }
    }

    Component {
        id: popupComponent
        WaveformZoomPopup {}
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    function makePopup(player) {
        const popup = createTemporaryObject(popupComponent, testCase, {playbackController: player});
        verify(popup !== null);
        return popup;
    }

    function openLoaded(player) {
        const popup = makePopup(player);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        waitForPolish(testCase.Window.window);
        const area = findChild(popup.contentItem, "zoomArea");
        verify(area !== null);
        tryVerify(() => area.width > 0 && popup.pxPerMs > 0, 2000, "laid out");
        return popup;
    }

    function area(popup) {
        return findChild(popup.contentItem, "zoomArea");
    }

    function strip(popup) {
        return findChild(popup.contentItem, "waveformStrip");
    }

    function tiles(popup) {
        const out = [];
        const s = strip(popup);
        for (let i = 0; i < s.children.length; ++i) {
            if (s.children[i].objectName === "zoomTile") {
                out.push(s.children[i]);
            }
        }
        return out;
    }

    // Every tile painted, every one of its three canvases: asked to
    // paint once more, and seen to, so that a paint already done before
    // the spies were made is not waited for in vain.
    function waitForTiles(popup) {
        tryVerify(() => tiles(popup).length > 0, 2000, "tiles");
        const spies = [];
        for (const tile of tiles(popup)) {
            for (const canvas of [tile, findChild(tile, "beatGridLayer"), findChild(tile, "waveformOverlay")]) {
                spies.push(createTemporaryObject(spyComponent, testCase, {target: canvas, signalName: "painted"}));
                canvas.requestPaint();
            }
        }
        tryVerify(() => spies.every((spy) => spy.count > 0), 5000, "every tile painted");
        waitForPolish(testCase.Window.window);
    }

    // ---- Opening from the track details pane ------------------------

    Component {
        id: panelComponent
        TrackDetailPanel { width: 460; height: 700 }
    }

    function openFromPanel(player) {
        const panel = createTemporaryObject(panelComponent, testCase, {
            addCueController: {statusMessage: "", errorMessage: "", busy: false},
            playbackController: player,
            format: "rekordbox",
            libraryPath: "/fixture/PIONEER",
        });
        verify(panel !== null);
        player.waveformFor = function() { return []; };
        player.beatTicksFor = function() { return []; };
        panel.showFor({sourceId: "42", title: "Shown Track", artist: "Someone", cues: [], durationSeconds: 240,
                       playlistNames: [], streamingSource: "", filePath: "/fixture/Contents/shown.mp3"});
        waitForPolish(testCase.Window.window);
        const view = findChild(panel, "trackDetailPanelWaveform");
        verify(view !== null);
        mouseClick(view, view.width / 2, view.height / 2);
        const popup = findChild(panel, "waveformZoomPopup");
        verify(popup !== null, "the pane has the popup");
        tryCompare(popup, "opened", true);
        return popup;
    }

    function makeFakePlayer(props) {
        const player = createTemporaryObject(fakePlayer, testCase, props || {});
        verify(player !== null);
        // Room for the two the pane asks for.
        return player;
    }

    Component {
        id: paneFake
        QtObject {
            property bool hasTrack: false
            property string currentFormat: ""
            property string currentSourceId: ""
            property string currentLibraryPath: ""
            property string title: ""
            property string artist: ""
            property real duration: 240000
            property real position: 0
            property bool playing: false
            property var waveform: []
            property var cues: []
            property int trackSerial: 1
            property var calls: []
            property var loads: []
            property var waveformFor: null
            property var beatTicksFor: null
            function cuesOnGridTimeline(format, path, cues) { return cues; }
            signal seeked(real positionMs)
            function beatTicksAt(durationMs, widthPx) { return []; }
            function load(format, libraryPath, sourceId, filePath, title, artist, artworkPath, cues,
                          fallbackArtworkPath, startPlaying) {
                loads.push({format: format, libraryPath: libraryPath, sourceId: sourceId, filePath: filePath,
                            title: title, startPlaying: startPlaying});
                calls.push("load");
                hasTrack = true;
                currentFormat = format;
                currentLibraryPath = libraryPath;
                currentSourceId = sourceId;
                position = 0;
                playing = startPlaying !== false;
            }
            function togglePlay() { calls.push("togglePlay"); playing = !playing; }
            function seek(ms) { calls.push("seek"); position = ms; }
            function stop() { calls.push("stop"); }
        }
    }

    // The pane's track is its library's row: the page can switch to
    // another library while the pane still shows it (Browse, Engine to
    // Device Library), and the same id is another track there. Engine's
    // 786 is "Discotech" and export.pdb's 786 is "1985": opened as
    // rekordbox's 786, the popup drew 1985's waveform and grid under
    // Discotech's title and audio (Sebastian's screenshot, 2026-11-24).
    // Everything the pane reads, plays or opens names the track by the
    // library it was shown from.
    function test_thePaneKeepsItsTracksLibrary() {
        const player = createTemporaryObject(paneFake, testCase);
        const asked = [];
        player.waveformFor = function(format, path, id) { asked.push(format + "|" + path + "|" + id); return []; };
        player.beatTicksFor = function(format, path, id) { asked.push(format + "|" + path + "|" + id); return []; };
        const cueAsks = [];
        const panel = createTemporaryObject(panelComponent, testCase, {
            addCueController: {statusMessage: "", errorMessage: "", busy: false, pendingRevision: 0,
                               pendingCuesFor: function(format, path, id) {
                                   cueAsks.push(format + "|" + path + "|" + id);
                                   return [];
                               }},
            playbackController: player,
            format: "engine",
            libraryPath: "/fixture/Engine Library",
        });
        panel.showFor({sourceId: "786", title: "Discotech", artist: "Huntemann", cues: [], durationSeconds: 346,
                       playlistNames: [], streamingSource: "", filePath: "/fixture/Contents/discotech.m4a"});
        // The page switches to the Device Library on the same stick.
        panel.format = "rekordbox";
        panel.libraryPath = "/fixture/PIONEER";
        waitForPolish(testCase.Window.window);
        const engine = "engine|/fixture/Engine Library|786";
        verify(asked.length > 0 && asked.every((key) => key === engine), "the pane reads Engine's 786: " + asked);
        verify(cueAsks.length > 0 && cueAsks.every((key) => key === engine), "its staged cues: " + cueAsks);
        const view = findChild(panel, "trackDetailPanelWaveform");
        mouseClick(view, view.width / 2, view.height / 2);
        const popup = findChild(panel, "waveformZoomPopup");
        tryCompare(popup, "opened", true);
        compare(player.loads.length, 1);
        compare(player.loads[0].format, "engine", "Engine's 786, not export.pdb's");
        compare(player.loads[0].libraryPath, "/fixture/Engine Library");
        compare(player.loads[0].sourceId, "786");
        verify(panel.isLoadedTrack, "and the pane knows it is the track loaded");
        popup.close();
    }

    // Nothing playing: the track goes into the player paused, at its start.
    function test_openingFromThePaneLoadsPausedWhenNothingPlays() {
        const player = createTemporaryObject(paneFake, testCase);
        const popup = openFromPanel(player);
        compare(player.loads.length, 1, "loaded once");
        compare(player.loads[0].sourceId, "42");
        compare(player.loads[0].filePath, "/fixture/Contents/shown.mp3");
        compare(player.loads[0].startPlaying, false, "paused");
        compare(player.playing, false);
        popup.close();
    }

    // Something playing: the shown track plays.
    function test_openingFromThePaneLoadsPlayingWhenSomethingPlays() {
        const player = createTemporaryObject(paneFake, testCase, {hasTrack: true, currentFormat: "rekordbox",
            currentSourceId: "9", currentLibraryPath: "/fixture/PIONEER", playing: true});
        openFromPanel(player);
        compare(player.loads.length, 1);
        compare(player.loads[0].sourceId, "42");
        compare(player.loads[0].startPlaying, true, "playing");
        compare(player.playing, true);
    }

    // The track already in the player: nothing is loaded again, nothing
    // restarts, playing or paused.
    function test_openingTheLoadedTrackLeavesItAsItIs() {
        for (const playing of [true, false]) {
            const player = createTemporaryObject(paneFake, testCase, {hasTrack: true, currentFormat: "rekordbox",
                currentSourceId: "42", currentLibraryPath: "/fixture/PIONEER", playing: playing, position: 90000});
            const popup = openFromPanel(player);
            compare(player.loads.length, 0, "not loaded again");
            compare(player.calls.length, 0, "nothing called: " + player.calls);
            compare(player.playing, playing);
            compare(player.position, 90000);
            popup.close();
            // Gone, its dimmer with it, before the next pane is clicked.
            tryCompare(popup, "visible", false);
        }
    }

    // ---- The position line and the waveform under it -----------------

    function test_thePositionLineIsAtTheGoldenRatio() {
        const popup = openLoaded(makeFakePlayer());
        const line = findChild(popup.contentItem, "positionLine");
        verify(line !== null);
        const a = area(popup);
        const middle = line.x + line.width / 2;
        verify(Math.abs(middle - a.width * 0.382) <= 1, "the line at " + middle + " of " + a.width);
        fuzzyCompare(popup.lineRatio, 0.381966, 1e-6);
    }

    // Paused, the position rests under the line; moved on by a known
    // time, the waveform moves left by that time at this zoom.
    function test_theWaveformMovesUnderTheLine() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        const s = strip(popup);
        const a = area(popup);
        const lineX = popup.lineX;
        // 0:00 is the strip's left edge: 60 s is under the line.
        verify(Math.abs(s.x + 60000 * popup.pxPerMs - lineX) <= 1, "60 s under the line");
        const before = s.x;
        player.position = 62500;
        waitForPolish(testCase.Window.window);
        const moved = before - s.x;
        verify(Math.abs(moved - 2500 * popup.pxPerMs) <= 1,
               "moved " + moved + " px for 2.5 s at " + popup.pxPerMs + " px/ms");
        // Zoomed in, the same time is further.
        popup.zoomIn();
        const at = s.x;
        player.position = 65000;
        waitForPolish(testCase.Window.window);
        verify(Math.abs((at - s.x) - 2500 * popup.pxPerMs) <= 1, "at the new zoom too");
        // Near the start the waveform keeps moving: the line stays put.
        player.position = 1000;
        waitForPolish(testCase.Window.window);
        verify(s.x > 0, "ground left of 0:00, the strip starts at " + s.x);
    }

    // Playing, it moves between the player's reports, every frame, and
    // a report close to where it is does not make it jump.
    function test_playingMovesSmoothlyBetweenReports() {
        const player = makeFakePlayer({position: 30000});
        const popup = openLoaded(player);
        // The frames driven by hand, not by the clock on the wall.
        popup.frameClock = false;
        player.playing = true;
        const start = popup.centerMs;
        const xStart = strip(popup).x;
        for (let i = 0; i < 24; ++i) {
            popup.advanceClock(1 / 60);
        }
        fuzzyCompare(popup.centerMs - start, 400, 1e-6, "24 frames of 1/60 s moved it 400 ms");
        verify(Math.abs((xStart - strip(popup).x) - 400 * popup.pxPerMs) <= 1, "and the strip with it");
        const here = popup.centerMs;
        player.position = here + 40;
        verify(Math.abs(popup.centerMs - here) < 20, "a near report nudges, " + (popup.centerMs - here));
        player.seek(120000);
        compare(popup.centerMs, 120000, "a seek jumps");
        player.playing = false;
    }

    // ---- Zoom ----------------------------------------------------------

    // From four bars across to the whole track, by keys; the slider's
    // handle goes with them, to its right and left ends.
    function test_zoomStopsAtFourBarsAndTheWholeTrack() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const slider = findChild(popup.contentItem, "zoomSlider");
        compare(popup.barMs, 2000);
        compare(popup.shownSpanMs, 16 * 2000, "opens at 16 bars");
        for (let i = 0; i < 12; ++i) {
            keyClick(Qt.Key_Plus);
        }
        compare(popup.shownSpanMs, 4 * 2000, "no closer than four bars");
        fuzzyCompare(slider.value, 1, 1e-9, "the handle at the right end");
        for (let i = 0; i < 20; ++i) {
            keyClick(Qt.Key_Minus);
        }
        compare(popup.shownSpanMs, testCase.trackMs, "no further than the whole track");
        fuzzyCompare(slider.value, 0, 1e-9, "the handle at the left end");
        keyClick(Qt.Key_Minus);
        compare(popup.shownSpanMs, testCase.trackMs);
        // Zooming while following keeps the position under the line.
        keyClick(Qt.Key_Plus);
        verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "still under the line");
    }

    // Two popups on one player (the player bar's and the details pane's):
    // one that never asked for the detail lets nothing go when it closes
    // or is destroyed, so the other one's read goes on.
    function test_aPopupLetsGoOnlyOfWhatItAskedFor() {
        const player = makeFakePlayer({position: 60000});
        const asking = openLoaded(player);
        const other = makePopup(player);
        compare(player.detailRequests, 1);
        other.open();
        other.close();
        tryCompare(other, "visible", false);
        compare(player.detailReleases, 1, "the other one asked when it opened, and let go when it closed");
        const idle = makePopup(player);
        idle.destroy();
        wait(0);
        compare(player.detailReleases, 1, "a popup that never opened lets nothing go");
        verify(asking.visible, "and the open one still has its detail");
        asking.close();
        tryCompare(asking, "visible", false);
        compare(player.detailReleases, 2, "the open one lets go of its own");
    }

    // Until the player has reported this track's length, the popup draws
    // no tiles and lays nothing out to a length: `duration` may still be
    // the previous track's, and a tile with no length would spread the
    // whole track over its own width. Once the length is in, it draws to
    // that one.
    function test_nothingIsLaidOutToAnotherTracksLength() {
        const player = makeFakePlayer({position: 0});
        const popup = openLoaded(player);
        verify(tiles(popup).length > 0, "drawn while the length is known");
        compare(popup.durationMs, testCase.trackMs);
        player.durationSerial = player.trackSerial;   // the length stops following
        player.changeTrack("Space Oddity", "David Bowie", 500, player.detail);
        compare(popup.lengthKnown, false, "the old track's length is not this one's");
        compare(popup.durationMs, 0);
        tryCompare(popup, "tilesShown", 0);
        player.duration = 180000;
        player.durationSerial = player.trackSerial;
        compare(popup.durationMs, 180000, "the new track's length once reported");
        tryVerify(() => tiles(popup).length > 0, 5000, "and drawn again");
        tiles(popup).forEach((t) => compare(t.trackDurationMs, 180000));
    }

    // The slider zooms: its right end is four bars, its left end the
    // whole track, its middle halfway between them on a log scale. It
    // zooms around the line, as a key does, and after a drag it follows
    // the keys again.
    function test_theSliderZooms() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const slider = findChild(popup.contentItem, "zoomSlider");
        verify(slider.enabled);
        const y = slider.height / 2;
        mouseClick(slider, slider.width - 1, y);
        compare(popup.shownSpanMs, 4 * 2000, "the right end: four bars");
        verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "still under the line");
        mouseClick(slider, 1, y);
        compare(popup.shownSpanMs, testCase.trackMs, "the left end: the whole track");
        mouseClick(slider, slider.width / 2, y);
        const middle = Math.sqrt(4 * 2000 * testCase.trackMs);
        verify(Math.abs(popup.shownSpanMs - middle) / middle < 0.05,
               "the middle: " + popup.shownSpanMs + " against " + middle);
        verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "still under the line");
        const before = slider.value;
        keyClick(Qt.Key_Plus);
        verify(slider.value > before, "a key moves the handle after the slider was used");
        fuzzyCompare(slider.value, popup.zoomLevel, 1e-9);
    }

    // A mark at 4, 8, 16, 32 and 64 bars and one at the whole track (120
    // bars here), left to right from the whole track, inside the slider.
    function test_theSliderHasAMarkAtEveryDoubling() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const slider = findChild(popup.contentItem, "zoomSlider");
        compare(popup.zoomTicks.length, 6);
        for (let i = 0; i < 5; ++i) {
            fuzzyCompare(popup.spanAtLevel(popup.zoomTicks[i]), (4 << i) * 2000, 1e-6);
        }
        compare(popup.zoomTicks[5], 0);
        const marks = [];
        for (let i = 0; i < slider.children.length; ++i) {
            if (slider.children[i].objectName === "zoomTick") {
                marks.push(slider.children[i]);
            }
        }
        compare(marks.length, 6);
        marks.sort((a, b) => a.x - b.x);
        verify(marks[0].x >= 0 && marks[5].x + marks[5].width <= slider.width, "inside the slider");
        for (let i = 1; i < marks.length; ++i) {
            verify(marks[i].x > marks[i - 1].x, "apart");
        }
    }

    // A drag follows the handle at most once per interval, so a fast drag
    // costs a few zooms rather than one per mouse event; the handle stays
    // under the pointer meanwhile, and letting go lands on where it is.
    function test_aDragZoomsAtMostOncePerInterval() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const slider = findChild(popup.contentItem, "zoomSlider");
        const spy = createTemporaryObject(spyComponent, testCase, {target: popup, signalName: "shownSpanMsChanged"});
        const y = slider.height / 2;
        const from = slider.width * 0.5;
        mousePress(slider, from, y);
        const zoomsAtPress = spy.count;
        const moves = 30;
        for (let i = 1; i <= moves; ++i) {
            mouseMove(slider, from + (slider.width * 0.45) * i / moves, y);
        }
        verify(spy.count - zoomsAtPress <= 2, "a burst of " + moves + " moves zooms at once and once more at most, not "
               + (spy.count - zoomsAtPress) + " times");
        verify(slider.value > 0.9, "the handle stays under the pointer: " + slider.value);
        mouseRelease(slider, from + slider.width * 0.45, y);
        fuzzyCompare(popup.zoomLevel, slider.value, 1e-9, "letting go lands where the handle is");
        verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "still under the line");
    }

    // Ctrl and the wheel zoom; detached, around the pointer.
    function test_ctrlWheelZoomsAroundThePointer() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        const a = area(popup);
        // Let go first: a drag.
        mouseDrag(a, popup.lineX, a.height / 2, -100, 0);
        findChild(popup.contentItem, "zoomFlick").cancelFlick();
        compare(popup.attached, false);
        const x = popup.lineX + 300;
        const before = popup.centerMs + (x - popup.lineX) / popup.pxPerMs;
        const span = popup.shownSpanMs;
        mouseWheel(a, x, a.height / 2, 0, 120, Qt.NoButton, Qt.ControlModifier);
        verify(popup.shownSpanMs < span, "closer");
        const after = popup.centerMs + (x - popup.lineX) / popup.pxPerMs;
        verify(Math.abs(after - before) < 1, "the time under the pointer stays: " + before + " to " + after);
    }

    // ---- Letting go and taking hold ----------------------------------

    function test_scrollingLetsGoAndFollowTakesHold() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        const a = area(popup);
        const follow = findChild(popup.contentItem, "followButton");
        verify(!follow.visible, "no follow button while following");

        // A drag right shows earlier music.
        mouseDrag(a, popup.lineX, a.height / 2, 200, 0);
        findChild(popup.contentItem, "zoomFlick").cancelFlick();
        compare(popup.attached, false);
        // Back by about the drag (the Flickable starts after its drag
        // threshold).
        verify(popup.centerMs < 60000 - 100 / popup.pxPerMs && popup.centerMs > 60000 - 300 / popup.pxPerMs,
               "the drag moved the view to " + popup.centerMs);
        tryCompare(follow, "visible", true);
        waitForPolish(testCase.Window.window);
        player.position = 61000;
        verify(popup.centerMs < 60000, "it no longer follows the playhead");
        mouseClick(follow);
        compare(popup.attached, true);
        compare(popup.centerMs, 61000);

        // The wheel lets go too; play takes hold again.
        mouseWheel(a, a.width / 2, a.height / 2, 0, -120);
        compare(popup.attached, false);
        verify(popup.centerMs > 61000, "the wheel turned down moves on through the track");
        mouseClick(findChild(popup.contentItem, "zoomPlayButton"));
        compare(player.playing, true);
        compare(popup.attached, true, "play takes hold");
        player.playing = false;

        // And a seek: a click on the waveform puts that moment on the line.
        mouseWheel(a, a.width / 2, a.height / 2, 0, 120);
        compare(popup.attached, false);
        const target = popup.centerMs + 150 / popup.pxPerMs;
        mouseClick(a, popup.lineX + 150, a.height / 2);
        compare(popup.attached, true);
        verify(Math.abs(player.position - target) <= 1, "seeked to the clicked moment: " + player.position
               + " for " + target);
        compare(popup.centerMs, player.position);
    }

    // ---- Closing ---------------------------------------------------------

    function test_escapeClosesAndPlaybackIsUntouched() {
        const player = makeFakePlayer({position: 60000, playing: true});
        const popup = openLoaded(player);
        player.calls = [];
        keyClick(Qt.Key_Escape);
        tryCompare(popup, "opened", false);
        compare(player.calls.length, 0, "nothing called: " + player.calls);
        compare(player.playing, true);
        compare(player.hasTrack, true);
    }

    // Space plays and pauses while the popup is open: the app's own
    // shortcut, which no key handling in the popup takes from it.
    Component {
        id: spaceComponent
        PlayPauseShortcut {}
    }
    function test_spaceTogglesPlayWhileOpen() {
        const player = makeFakePlayer({position: 60000});
        createTemporaryObject(spaceComponent, testCase, {playbackController: player});
        const popup = openLoaded(player);
        keyClick(Qt.Key_Space);
        compare(player.playing, true, "playing");
        compare(popup.opened, true, "and still open");
        keyClick(Qt.Key_Space);
        compare(player.playing, false, "paused");
    }

    // ---- What it draws -------------------------------------------------

    // Each library's detail in its own player's colours. A rekordbox
    // track in rekordbox's three-band colours ("vivid"); an Engine track
    // as a Denon player draws it ("engine"): the bands as layers mirrored
    // around the centre line, each as tall as its own band, low under mid
    // under high, in Engine's blue, green and white.
    function columnsOf(low, mid, high) {
        const n = Math.floor(testCase.trackMs * 150 / 1000);
        const buffer = new ArrayBuffer(n * 3);
        const bytes = new Uint8Array(buffer);
        for (let i = 0; i < n * 3; i += 3) {
            bytes[i] = low;
            bytes[i + 1] = mid;
            bytes[i + 2] = high;
        }
        return {columns: buffer, msPerColumn: 1000 / 150, startMs: 0};
    }
    function test_eachLibraryInItsOwnColours_data() {
        return [{tag: "rekordbox", format: "rekordbox", scheme: "vivid"},
                {tag: "engine", format: "engine", scheme: "engine"}];
    }
    function test_eachLibraryInItsOwnColours(data) {
        const player = makeFakePlayer({position: 60000, currentFormat: data.format});
        player.detail = columnsOf(230, 128, 51);
        const popup = openLoaded(player);
        compare(popup.colorScheme, data.scheme);
        waitForTiles(popup);
        for (const tile of tiles(popup)) {
            compare(tile.colorScheme, data.scheme, "every tile");
        }
        if (data.format !== "engine") {
            return;
        }
        const a = area(popup);
        const x = Math.round(popup.lineX + 40);
        const tile = tiles(popup).find((t) => {
            const p = t.mapFromItem(a, x, 0);
            return p.x >= 0 && p.x < t.width;
        });
        verify(tile !== undefined, "a tile right of the position line");
        const image = grabImage(testCase);
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-zoom-popup-engine-colours.png");
        }
        const tileX = tile.mapFromItem(a, x, 0).x;
        const centre = tile.waveTop + tile.waveHeight / 2;
        // Columns are drawn a pixel apart at most zooms: of a few pixels
        // side by side at one height, the one nearest the colour wanted.
        const distance = (c, want) => Math.max(Math.abs(c.r - want.r), Math.abs(c.g - want.g), Math.abs(c.b - want.b));
        const near = (dy, want, what) => {
            let best = null;
            for (let dx = 0; dx <= 3; dx += 0.5) {
                const p = tile.mapToItem(testCase, tileX + dx, centre + dy);
                const c = PixelScale.pixel(image, testCase, p.x, p.y);
                if (best === null || distance(c, want) < distance(best, want)) {
                    best = c;
                }
            }
            verify(distance(best, want) < 0.04, what + ": got " + best.r + "," + best.g + "," + best.b);
        };
        // Half-heights: high 0.1, mid 0.25, low 0.45 of the band.
        const h = tile.waveHeight;
        near(0.05 * h, Theme.waveformEngineHigh, "the core is the high band");
        near(-0.18 * h, Theme.waveformEngineMid, "around it the mid band");
        near(0.38 * h, Theme.waveformEngineLow, "outside it the low band");
    }

    // Engine's bands are drawn as antialiased outlines through the
    // columns' values, not as a rectangle a column: a kick is a straight
    // attack at its column's left edge and a sloped decay after it. 50 ms
    // columns of a sawtooth in the high band only: every tenth column at
    // 250, then 25 less each column.
    function sawtooth() {
        const n = Math.floor(testCase.trackMs / 50);
        const buffer = new ArrayBuffer(n * 3);
        const bytes = new Uint8Array(buffer);
        for (let i = 0; i < n; ++i) {
            bytes[3 * i + 2] = 250 - 25 * (i % 10);
        }
        return {columns: buffer, msPerColumn: 50, startMs: 0};
    }
    function test_engineBandsAreOutlinesWithStraightAttacks() {
        const player = makeFakePlayer({position: 60000, currentFormat: "engine"});
        player.detail = sawtooth();
        const popup = openLoaded(player);
        for (let i = 0; i < 12; ++i) {
            popup.zoomIn();
        }
        waitForTiles(popup);
        const a = area(popup);
        const pxPerMs = popup.pxPerMs;
        verify(50 * pxPerMs >= 6, "columns several pixels wide: " + 50 * pxPerMs);
        const image = grabImage(testCase);
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-zoom-popup-engine-outline.png");
        }
        const tileAt = (x) => tiles(popup).find((t) => {
            const p = t.mapFromItem(a, x, 0);
            return p.x >= 0 && p.x < t.width;
        });
        const want = Theme.waveformEngineHigh;
        const ground = (x) => {
            const t = tileAt(x);
            const p = t.mapToItem(testCase, t.mapFromItem(a, x, 0).x, t.waveTop + 1);
            return PixelScale.pixel(image, testCase, p.x, p.y);
        };
        const distance = (c, d) => Math.max(Math.abs(c.r - d.r), Math.abs(c.g - d.g), Math.abs(c.b - d.b));
        // The half-height of the high layer at area x: from the top of the
        // band down, the first pixel nearer the layer's colour than the
        // ground's. And whether a pixel on the way was neither (a blend:
        // the edge is antialiased).
        const edgeAt = (x) => {
            const t = tileAt(x);
            const local = t.mapFromItem(a, x, 0).x;
            const centre = t.waveTop + t.waveHeight / 2;
            const g = ground(x);
            let blended = false;
            for (let y = t.waveTop; y <= centre; y += 0.5) {
                const p = t.mapToItem(testCase, local, y);
                const c = PixelScale.pixel(image, testCase, p.x, p.y);
                if (distance(c, want) > 0.08 && distance(c, g) > 0.08) {
                    blended = true;
                }
                if (distance(c, want) < distance(c, g)) {
                    return {half: (centre - y) / (t.waveHeight / 2), blended: blended};
                }
            }
            return {half: 0, blended: blended};
        };
        const xOf = (ms) => popup.stripX + ms * pxPerMs;
        // The first rise (a column at 250) well right of the position line.
        let k = Math.ceil((popup.lineX + 40 - popup.stripX) / pxPerMs / 50);
        k += (10 - k % 10) % 10;
        const rise = xOf(k * 50);
        verify(rise + 9 * 50 * pxPerMs < a.width, "the tooth on screen");
        const tolerance = 2 / tileAt(rise).waveHeight;
        // Straight up at the column's left edge, within a device pixel.
        const dpr = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1;
        fuzzyCompare(edgeAt(rise - 1.5 / dpr).half, 25 / 255, 0.03, "just before the attack: the last decay");
        const colPx = 50 * pxPerMs;
        const after = 1.5 / dpr;
        fuzzyCompare(edgeAt(rise + after).half, (250 - 25 * after / colPx) / 255, tolerance + 0.01,
                     "just after it: the attack's height, already sloping down");
        // Halfway through a falling column, halfway between its value and
        // the next one's: a slope, not a step.
        let anyBlend = false;
        for (let j = 1; j <= 7; ++j) {
            const mid = edgeAt(rise + (j + 0.5) * 50 * pxPerMs);
            fuzzyCompare(mid.half, (250 - 25 * j - 12.5) / 255, tolerance + 0.01,
                         "halfway through column " + j + " of the decay");
            anyBlend = anyBlend || mid.blended;
        }
        verify(anyBlend, "the sloped edge is antialiased");
    }

    // Engine's layers are one continuous envelope, as on the player: no
    // gap between columns where a column is wider than a pixel (rekordbox's
    // three-band columns keep theirs). Columns of 50 ms at the closest
    // zoom are several pixels wide.
    function test_engineColumnsMeetWithoutAGap() {
        const player = makeFakePlayer({position: 60000, currentFormat: "engine"});
        const wide = columnsOf(230, 128, 51);
        const n = Math.floor(testCase.trackMs / 50);
        wide.columns = wide.columns.slice(0, n * 3);
        wide.msPerColumn = 50;
        player.detail = wide;
        const popup = openLoaded(player);
        for (let i = 0; i < 12; ++i) {
            popup.zoomIn();
        }
        waitForTiles(popup);
        const barPx = 50 * popup.pxPerMs;
        verify(barPx >= 4, "columns several pixels wide: " + barPx);
        const a = area(popup);
        const image = grabImage(testCase);
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-zoom-popup-engine-no-gap.png");
        }
        const want = Theme.waveformEngineHigh;
        // Across three columns, right of the position line, on the centre
        // line, every half pixel.
        const fromX = popup.lineX + 30;
        for (let x = fromX; x < fromX + 3 * barPx; x += 0.5) {
            const tile = tiles(popup).find((t) => {
                const p = t.mapFromItem(a, x, 0);
                return p.x >= 0 && p.x < t.width;
            });
            verify(tile !== undefined);
            const local = tile.mapFromItem(a, x, 0);
            const p = tile.mapToItem(testCase, local.x, tile.waveTop + tile.waveHeight / 2);
            const c = PixelScale.pixel(image, testCase, p.x, p.y);
            verify(Math.max(Math.abs(c.r - want.r), Math.abs(c.g - want.g), Math.abs(c.b - want.b)) < 0.04,
                   "no gap at " + (x - fromX) + " px: got " + c.r + "," + c.g + "," + c.b);
        }
    }

    // At a zoom where every beat fits: a faint line at every beat behind
    // the waveform, a tall tick with its number at every bar in the lane.
    function test_theRulerAndBeatLinesAtFullZoom() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        for (let i = 0; i < 12; ++i) {
            popup.zoomIn();
        }
        compare(popup.shownSpanMs, 8000);
        waitForTiles(popup);
        const tile = tiles(popup)[0];
        verify(tile.rulerHeight >= 12, "a ruler lane, " + tile.rulerHeight);
        const ticks = popup.ticks;
        verify(ticks.length === 480, "every beat of four minutes has a mark: " + ticks.length);
        verify(ticks.every((tick) => tick.strong === tick.downbeat), "every bar a stronger mark");

        const a = area(popup);
        // The popup is in the window's overlay: grab the window, and read
        // it at the area's points.
        const image = grabImage(testCase);
        if (screenshotDir && screenshotDir.length > 0) {
            image.save(screenshotDir + "/waveform-zoom-popup.png");
        }
        const pixelAt = (x, y) => {
            const p = a.mapToItem(testCase, x, y);
            return PixelScale.pixel(image, testCase, p.x, p.y);
        };
        const xOf = (ms) => popup.stripX + ms * popup.pxPerMs;
        const lane = tile.rulerHeight;
        const ground = pixelAt(xOf(60250), lane + 4);   // between beats
        // A line is one whole pixel, rounded within its tile: the most
        // ink a pixel either side of the exact place.
        const ink = (x, y) => {
            let most = 0;
            for (let dx = -1; dx <= 1; ++dx) {
                const c = pixelAt(x + dx, y);
                most = Math.max(most, Math.abs(c.r - ground.r) + Math.abs(c.g - ground.g) + Math.abs(c.b - ground.b));
            }
            return most;
        };
        // A beat at 60.5 s (not a downbeat) and bar 32 at 62 s, both right
        // of the position line.
        const beatX = Math.round(xOf(60500));
        const barX = Math.round(xOf(62000));
        verify(beatX > popup.lineX + 4 && barX < a.width, "both on screen");
        verify(ink(beatX, lane + 4) > 0.03, "a faint line at the beat, ink " + ink(beatX, lane + 4));
        verify(ink(barX, lane + 4) > ink(beatX, lane + 4) + 0.05, "a stronger one at the bar");
        const laneGround = pixelAt(xOf(60250), 2);
        const laneInk = (x, y) => {
            let most = 0;
            for (let dx = -1; dx <= 1; ++dx) {
                const c = pixelAt(x + dx, y);
                most = Math.max(most, Math.abs(c.r - laneGround.r) + Math.abs(c.g - laneGround.g)
                                + Math.abs(c.b - laneGround.b));
            }
            return most;
        };
        verify(laneInk(barX, 2) > 0.5, "bar 32's tall tick");
        verify(laneInk(beatX, 2) < 0.02, "no tall tick at a beat");
        let number = 0;
        for (let x = barX + 3; x < barX + 20; ++x) {
            for (let y = 2; y < lane - 4; ++y) {
                number = Math.max(number, laneInk(x, y));
            }
        }
        verify(number > 0.4, "bar 32 has its number, ink " + number);
        compare(ticks.filter((tick) => tick.timeMs === 62000)[0].bar, 32);
    }

    // A frame of playback moves the strip and paints nothing, but the
    // tile that scrolls into view.
    function test_playbackRepaintsNoTile() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        waitForTiles(popup);
        const spies = [];
        for (const tile of tiles(popup)) {
            for (const canvas of [tile, findChild(tile, "beatGridLayer"), findChild(tile, "waveformOverlay")]) {
                spies.push(createTemporaryObject(spyComponent, testCase, {target: canvas, signalName: "paint"}));
            }
        }
        const firstTiles = [popup.firstTile];
        const xBefore = strip(popup).x;
        player.playing = true;
        for (let i = 0; i < 25; ++i) {
            wait(20);
            if (firstTiles.indexOf(popup.firstTile) < 0) {
                firstTiles.push(popup.firstTile);
            }
        }
        const xAfter = strip(popup).x;
        player.playing = false;
        verify(xAfter < xBefore, "the strip moved: " + xBefore + " to " + xAfter);
        const paints = spies.reduce((sum, spy) => sum + spy.count, 0);
        verify(paints <= 3 * (firstTiles.length - 1),
               paints + " canvas paints for " + (firstTiles.length - 1) + " tiles scrolling in");
    }

    // ---- The pane and the bar, as Browse shows them --------------------

    // Music-like columns: a kick on every beat, a quiet intro and break,
    // loud blocks between; `perSecond` columns a second over `ms`.
    function music(ms, perSecond) {
        const n = Math.floor(ms * perSecond / 1000);
        const out = [];
        for (let i = 0; i < n; ++i) {
            const t = i * 1000 / perSecond;
            const bar = t / 2000;
            const energy = bar < 16 ? 0.25 : bar < 48 ? 0.85 : bar < 64 ? 0.3 : bar < 112 ? 0.9 : 0.2;
            const kick = Math.exp(-((t % 500) / 500) * 5);
            const hat = 0.5 + 0.5 * Math.sin(t / 37) * Math.sin(t / 113);
            out.push({low: Math.min(1, energy * (0.35 + 0.65 * kick)), mid: energy * 0.55 * hat,
                      high: energy * 0.35 * (1 - kick) * hat});
        }
        return out;
    }
    function asBytes(columns) {
        const buffer = new ArrayBuffer(columns.length * 3);
        const bytes = new Uint8Array(buffer);
        for (let i = 0; i < columns.length; ++i) {
            bytes[3 * i] = Math.round(columns[i].low * 255);
            bytes[3 * i + 1] = Math.round(columns[i].mid * 255);
            bytes[3 * i + 2] = Math.round(columns[i].high * 255);
        }
        return buffer;
    }

    Component {
        id: browseScene
        Rectangle {
            id: scene
            required property var player
            property alias panel: scenePanel
            property alias bar: sceneBar
            width: 1300
            height: 800
            color: Theme.background
            TrackDetailPanel {
                id: scenePanel
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: sceneBar.top
                width: 460
                addCueController: ({statusMessage: "", errorMessage: "", busy: false})
                playbackController: scene.player
                format: "rekordbox"
                libraryPath: "/fixture/PIONEER"
            }
            PlayerBar {
                id: sceneBar
                controller: scene.player
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }
        }
    }

    // The details pane's waveform and the player bar's both draw the
    // ruler lane, and the bar opens the same popup for the loaded track.
    function test_thePaneAndTheBarDrawTheRuler() {
        const player = makeFakePlayer({position: 61000, title: "Major Tom", artist: "DJ Amador"});
        const overview = music(testCase.trackMs, 400 / (testCase.trackMs / 1000));
        player.waveform = overview;
        player.detail = {columns: asBytes(music(testCase.trackMs, 150)), msPerColumn: 1000 / 150, startMs: 0};
        player.waveformFor = function() { return overview; };
        player.beatTicksFor = function(format, path, id, durationMs, widthPx) {
            return BeatGridTicks.layout(testCase.grid(0, durationMs), durationMs, widthPx).ticks;
        };
        const scene = createTemporaryObject(browseScene, testCase, {player: player});
        verify(scene !== null);
        scene.panel.showFor({sourceId: "7", title: "Major Tom", artist: "DJ Amador", cues: player.cues,
                             durationSeconds: testCase.trackMs / 1000, playlistNames: ["Peaktime"],
                             streamingSource: "", filePath: "/fixture/Contents/major-tom.mp3", bpm: 120, key: "8A"});
        waitForPolish(testCase.Window.window);
        const paneView = findChild(scene.panel, "trackDetailPanelWaveform");
        const barView = findChild(scene.bar, "playerWaveform");
        tryVerify(() => paneView.beatTicks.length > 0 && barView.beatTicks.length > 0, 2000, "both have a grid");
        verify(paneView.rulerHeight >= 12, "the pane's lane, " + paneView.rulerHeight);
        compare(barView.gridStyle, "sliced", "the bar's grid is cut out of its bars");
        compare(barView.rulerHeight, 0, "no lane in the bar");
        verify(paneView.beatTicks.some((tick) => tick.strong && tick.bar > 1), "numbered marks in the pane");
        waitForRendering(scene);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(scene).save(screenshotDir + "/browse-pane-and-player-bar.png");
        }

        const button = findChild(scene.bar, "openWaveformPopupButton");
        verify(button !== null, "the bar has a button for the popup");
        mouseClick(button);
        const popup = findChild(scene.bar, "waveformZoomPopup");
        tryCompare(popup, "opened", true);
        compare(player.loads.length, 0, "the loaded track, as it is");
        waitForPolish(testCase.Window.window);
        waitForTiles(popup);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(testCase).save(screenshotDir + "/waveform-zoom-popup-music.png");
            for (let i = 0; i < 3; ++i) {
                popup.zoomIn();
            }
            waitForTiles(popup);
            grabImage(testCase).save(screenshotDir + "/waveform-zoom-popup-four-bars.png");
            for (let i = 0; i < 8; ++i) {
                popup.zoomOut();
            }
            waitForTiles(popup);
            grabImage(testCase).save(screenshotDir + "/waveform-zoom-popup-whole-track.png");
        }
        popup.close();
    }

    // The real controller hands its finer waveform over as an ArrayBuffer
    // (three bytes a column), and with no track loaded there is none.
    Component {
        id: realPlayer
        PlaybackController {}
    }
    function test_theControllersDetailWaveformIsAnArrayBuffer() {
        const player = createTemporaryObject(realPlayer, testCase);
        const detail = player.detailWaveform;
        verify(detail.columns instanceof ArrayBuffer, "an ArrayBuffer, got " + typeof detail.columns);
        compare(detail.columns.byteLength, 0, "no track, no columns");
        compare(detail.msPerColumn, 0);
    }

    // ---- Review of 2026-11-20 -------------------------------------------

    // The queue moves on while the popup is open: it shows the new track
    // (title, artist, waveform, grid), keeps the zoom as a number of bars
    // across, at the new track's tempo, and follows the playhead again.
    function test_aNewTrackKeepsTheZoomInBarsAndIsFollowed() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        popup.zoomIn();
        const bars = popup.shownSpanMs / popup.barMs;
        verify(Math.abs(bars - 16 / 1.5) < 1e-6, "zoomed in to " + bars + " bars");
        mouseWheel(area(popup), area(popup).width / 2, 50, 0, -120);
        compare(popup.attached, false);
        const requests = player.detailRequests;
        const other = new ArrayBuffer(300);
        player.changeTrack("Space Oddity", "Another Artist", 600, {columns: other, msPerColumn: 10, startMs: 0});
        compare(popup.trackTitle, "Space Oddity");
        compare(findChild(popup.contentItem, "zoomTitle").text, "Space Oddity");
        compare(findChild(popup.contentItem, "zoomArtist").text, "Another Artist");
        compare(popup.barMs, 2400, "the new track's bar");
        fuzzyCompare(popup.shownSpanMs / popup.barMs, bars, 1e-6);
        compare(popup.attached, true, "following again");
        compare(popup.centerMs, 0);
        verify(player.detailRequests > requests, "its waveform asked for");
        tryVerify(() => tiles(popup).length > 0 && tiles(popup).every((tile) => tile.columnBytes === other),
                  2000, "and drawn");
        const ticks = popup.ticks;
        fuzzyCompare(ticks[1].timeMs - ticks[0].timeMs, 600, 1e-6, "its own grid");
    }

    // The overview until the finer waveform arrives, then the finer one.
    function test_theOverviewStandsInUntilTheDetailArrives() {
        const player = makeFakePlayer({position: 60000, autoDetail: false});
        player.waveform = [{low: 0.5, mid: 0.2, high: 0.1}, {low: 0.6, mid: 0.2, high: 0.1}];
        const popup = openLoaded(player);
        tryVerify(() => tiles(popup).length > 0, 2000);
        compare(player.detailRequests, 1, "asked for once");
        verify(tiles(popup).every((tile) => tile.columnBytes === null && tile.waveformData.length === 2),
               "the overview meanwhile");
        player.detailWaveform = player.detail;
        tryVerify(() => tiles(popup).every((tile) => tile.columnBytes === player.detail.columns), 2000,
                  "then the detail");
    }

    // Opening, the popup listens from the first moment: a report that
    // arrives while it is still fading in is taken, not missed.
    Component {
        id: slowPopupComponent
        WaveformZoomPopup {
            enter: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 300 }
            }
        }
    }
    function test_aReportWhileOpeningIsTaken() {
        const player = makeFakePlayer({position: 30000, playing: true});
        const popup = createTemporaryObject(slowPopupComponent, testCase, {playbackController: player});
        popup.frameClock = false;
        popup.openLoaded();
        compare(popup.opened, false, "still opening");
        player.position = 90000;
        tryCompare(popup, "opened", true);
        compare(popup.centerMs, 90000, "the report was taken");
        player.playing = false;
    }

    // A track whose grid is not drawn: no lane, no lines, the waveform
    // has the whole height.
    function test_aTrackWithoutAGridHasNoLaneInThePopup() {
        const player = makeFakePlayer({position: 60000, noGrid: true});
        const popup = openLoaded(player);
        for (let i = 0; i < 12; ++i) {
            popup.zoomIn();
        }
        waitForTiles(popup);
        compare(popup.ticks.length, 0);
        for (const tile of tiles(popup)) {
            compare(tile.rulerHeight, 0, "no lane");
            compare(tile.waveHeight, tile.height, "the waveform has the whole height");
        }
        const a = area(popup);
        const image = grabImage(testCase);
        const pixelAt = (x, y) => {
            const p = a.mapToItem(testCase, x, y);
            return PixelScale.pixel(image, testCase, p.x, p.y);
        };
        // Above the quiet bars, right of the line: ground all along, where
        // a beat would have had a line every 500 ms.
        // The hot cue at 1:04 is drawn there too: its marker is no line of
        // the grid, so its few pixels are left out.
        const cueX = popup.lineX + (64000 - popup.centerMs) * popup.pxPerMs;
        const ground = pixelAt(popup.lineX + 20, 10);
        for (let x = popup.lineX + 20; x < a.width - 20; x += 3) {
            if (Math.abs(x - cueX) <= Theme.scaled(10)) {
                continue;
            }
            const c = pixelAt(x, 10);
            const d = Math.abs(c.r - ground.r) + Math.abs(c.g - ground.g) + Math.abs(c.b - ground.b);
            verify(d < 0.01, "no line at " + x + ": " + d);
        }
    }

    // Shift and the wheel zoom too, as ctrl does, however the system
    // sends a shifted wheel: up as it was turned, or turned sideways. The
    // slider's tooltip says so.
    function test_shiftWheelZooms() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const a = area(popup);
        const x = popup.lineX + 300;
        for (const delta of [{x: 0, y: 120}, {x: 120, y: 0}]) {
            const span = popup.shownSpanMs;
            mouseWheel(a, x, a.height / 2, delta.x, delta.y, Qt.NoButton, Qt.ShiftModifier);
            verify(popup.shownSpanMs < span, "closer, wheel " + JSON.stringify(delta));
            // Following the player, as ctrl does: around the line.
            compare(popup.attached, true, "still following");
            verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "60 s still under the line");
        }
        const span = popup.shownSpanMs;
        mouseWheel(a, x, a.height / 2, 0, -120, Qt.NoButton, Qt.ShiftModifier);
        verify(popup.shownSpanMs > span, "turned back, further out");
        const slider = findChild(popup.contentItem, "zoomSlider");
        verify(slider !== null);
        verify(slider.ToolTip.text.indexOf("Shift+scroll") >= 0, "the tooltip says so: " + slider.ToolTip.text);
    }

    // Ctrl and the wheel while following: zoomed around the line, the
    // playhead stays under it, and it still follows.
    function test_ctrlWheelWhileFollowingZoomsAroundTheLine() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const a = area(popup);
        const span = popup.shownSpanMs;
        mouseWheel(a, popup.lineX + 400, a.height / 2, 0, 120, Qt.NoButton, Qt.ControlModifier);
        verify(popup.shownSpanMs < span, "closer");
        compare(popup.attached, true, "still following");
        verify(Math.abs(strip(popup).x + 60000 * popup.pxPerMs - popup.lineX) <= 1, "60 s still under the line");
    }

    // The player bar's own waveform still seeks; only its button opens
    // the popup.
    Component {
        id: barComponent
        PlayerBar { width: 1000 }
    }
    function test_thePlayerBarWaveformStillSeeks() {
        const player = makeFakePlayer({position: 0});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player});
        const view = findChild(bar, "playerWaveform");
        verify(view !== null);
        waitForPolish(testCase.Window.window);
        mouseClick(view, view.width / 2, view.height - 5);
        compare(player.calls[player.calls.length - 1], "seek");
        verify(Math.abs(player.position - testCase.trackMs / 2) < testCase.trackMs / view.width * 2,
               "seeked to the middle: " + player.position);
        compare(findChild(bar, "waveformZoomPopup").visible, false, "no popup");
    }

    // Engine: where the audio cannot be decoded (the detail is made from
    // it otherwise, tst_WaveformBeatGridEngine), the waveform load() read
    // is all there is (2.x and 3.x keep only the overview), so the popup
    // is handed it without the library being opened again: changed in
    // the database after the load, the popup still gets what was loaded.
    function test_anEngineTracksDetailIsWhatLoadRead() {
        const lib = engineGridFixture.library();
        verify(lib.length > 0);
        const id = engineGridFixture.trackId(lib);
        verify(engineGridFixture.setWaveform(lib, 1024, 128));
        const player = createTemporaryObject(realPlayer, testCase);
        const notAudio = Qt.resolvedUrl("tst_WaveformZoomPopup.qml").toString()
            .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1");
        player.load("engine", lib, id, notAudio, "Grid", "An Artist", "", [], "", false);
        verify(engineGridFixture.setWaveform(lib, 512, 64));
        const answered = createTemporaryObject(spyComponent, testCase,
                                               {target: player, signalName: "detailWaveformChanged"});
        player.requestDetailWaveform();
        if (answered.count === 0) {
            answered.wait(30000);
        }
        const detail = player.detailWaveform;
        compare(detail.columns.byteLength, 1024 * 3, "the 1024 entries the load read");
        fuzzyCompare(detail.msPerColumn, 60000 / 1024, 1e-6);
        compare(new Uint8Array(detail.columns)[0], 128);
        player.stop();
    }

    // rekordbox: read off the GUI thread, and not while a library is being
    // written; nothing is kept from a refused read, and the read comes
    // once the write is over.
    function test_aRekordboxDetailWaitsForTheWriteToEnd() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0);
        const pioneer = stick + "/PIONEER";
        const id = stickFixture.rekordboxTrackIds(pioneer, 1)[0];
        verify(stickFixture.plantScrollWaveform(pioneer, id, 300), "a PWV7 of 300 columns planted");
        const player = createTemporaryObject(realPlayer, testCase);
        player.libraryBusy = true;
        player.load("rekordbox", pioneer, id, testCase.clickFile, "Click", "Artist", "", [], "", false);
        player.requestDetailWaveform();
        wait(200);
        compare(player.detailWaveform.columns.byteLength, 0, "nothing while a library is written");
        player.libraryBusy = false;
        tryVerify(() => player.detailWaveform.columns.byteLength === 300 * 3, 5000, "read once the write is over");
        player.stop();
    }

    // ---- The owner's changes of 2026-11-20 -------------------------------

    function flickOf(popup) {
        return findChild(popup.contentItem, "zoomFlick");
    }

    // Title and artist at the left of the top row, the controls right of
    // them with close in the top right corner, the time under the
    // waveform at the right.
    function test_theLayout() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const content = popup.contentItem;
        const close = findChild(content, "zoomCloseButton");
        const zoomIn = findChild(content, "zoomSlider");
        const play = findChild(content, "zoomPlayButton");
        const title = findChild(content, "zoomTitle");
        const time = findChild(content, "zoomTime");
        const a = area(popup);
        const right = (item) => item.mapToItem(content, item.width, 0).x;
        const left = (item) => item.mapToItem(content, 0, 0).x;
        const top = (item) => item.mapToItem(content, 0, 0).y;
        verify(Math.abs(right(close) - right(a)) <= 1, "close at the right edge");
        verify(top(close) < top(a), "above the waveform");
        verify(right(zoomIn) <= left(close) && right(play) <= left(zoomIn), "the controls left of it");
        verify(right(title) <= left(play), "the title left of them");
        verify(top(time) >= top(a) + a.height, "the time under the waveform");
        verify(Math.abs(right(time) - right(a)) <= 1, "at the right");
        compare(findChild(content, "zoomTitle").text, "Major Tom");
        compare(findChild(content, "zoomArtist").text, "DJ Amador");
    }

    // The lane is low and its numbers small: 20 px at most, numbers of
    // at most 10.
    function test_theLaneIsLow() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        waitForTiles(popup);
        const tile = tiles(popup)[0];
        verify(tile.rulerHeight >= 18 && tile.rulerHeight <= 20, "a lane of " + tile.rulerHeight);
        verify(Theme.beatRulerNumberMaximumSize <= 10);
    }

    // The zoom, in bars, is kept: reopened, and by a fresh settings
    // controller (an app restart).
    Component {
        id: settingsComponent
        AppSettingsController {}
    }
    function test_theZoomIsKept() {
        const settings = createTemporaryObject(settingsComponent, testCase);
        settings.waveformZoomBars = 0;
        const player = makeFakePlayer({position: 60000});
        const popup = createTemporaryObject(popupComponent, testCase, {playbackController: player, settings: settings});
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        waitForPolish(testCase.Window.window);
        compare(popup.spanBars, 16, "the first time ever: 16 bars");
        popup.zoomIn();
        popup.zoomIn();
        const bars = popup.spanBars;
        verify(Math.abs(bars - 16 / 2.25) < 1e-6, "zoomed to " + bars);
        popup.close();
        tryCompare(popup, "visible", false);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        compare(popup.spanBars, bars, "reopened at the same zoom");
        const fresh = createTemporaryObject(settingsComponent, testCase);
        fuzzyCompare(fresh.waveformZoomBars, bars, 1e-9, "and after a restart");
        // Another tempo: the same bars, another time span.
        player.changeTrack("Slower", "Someone", 750, player.detail);
        fuzzyCompare(popup.shownSpanMs, bars * 3000, 1e-6);
        settings.waveformZoomBars = 0;
        popup.close();
    }

    // Flung, the waveform glides on and slows to a stop; a press stops a
    // glide at once; it never goes past the track's ends.
    function test_aFlickGlidesAndAPressStopsIt() {
        const player = makeFakePlayer({position: 120000});
        const popup = openLoaded(player);
        const a = area(popup);
        const flick = flickOf(popup);
        // A quick drag to the left, let go while moving.
        const y = a.height / 2;
        mousePress(a, 900, y);
        for (let x = 880; x >= 600; x -= 40) {
            mouseMove(a, x, y, 8);
        }
        mouseRelease(a, 600, y, Qt.LeftButton, Qt.NoModifier, 8);
        compare(popup.attached, false, "a flick lets go of the playhead");
        verify(flick.flicking, "gliding after the release");
        const atRelease = popup.centerMs;
        tryVerify(() => !flick.moving, 10000, "and comes to rest");
        verify(popup.centerMs > atRelease, "it went on through the track: " + atRelease + " to " + popup.centerMs);

        // Again, and a press during the glide stops it there.
        mousePress(a, 900, y);
        for (let x = 880; x >= 600; x -= 40) {
            mouseMove(a, x, y, 8);
        }
        mouseRelease(a, 600, y, Qt.LeftButton, Qt.NoModifier, 8);
        verify(flick.flicking);
        mousePress(a, 700, y);
        tryVerify(() => !flick.flicking, 1000, "the press stopped it");
        const held = popup.centerMs;
        wait(100);
        compare(popup.centerMs, held, "it stays");
        mouseRelease(a, 700, y);

        // Flung hard towards the end from near it: it stops at the end.
        popup.follow();
        player.position = testCase.trackMs - 1000;
        popup.flingAt(1e6);
        tryVerify(() => !flick.moving, 10000);
        verify(Math.abs(popup.centerMs - testCase.trackMs) < 1, "at the end: " + popup.centerMs);
        // Follow takes hold again and stops any glide.
        popup.flingAt(-5000);
        mouseClick(findChild(popup.contentItem, "followButton"));
        compare(popup.attached, true);
        verify(!flick.moving, "the glide stopped");
        compare(popup.centerMs, player.position);
    }

    // A click on the time switches between the time played and the time
    // left, in the popup and the bar alike, through the one setting, and
    // with the keyboard too.
    function test_theTimeSwitchesEverywhere() {
        const settings = createTemporaryObject(settingsComponent, testCase);
        settings.showRemainingTime = false;
        const player = makeFakePlayer({position: 61000});
        const popup = createTemporaryObject(popupComponent, testCase, {playbackController: player, settings: settings});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player, settings: settings});
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        waitForPolish(testCase.Window.window);
        const popupTime = findChild(popup.contentItem, "zoomTime");
        const barTime = findChild(bar, "playerTime");
        compare(popupTime.text, "1:01 / 4:00");
        compare(barTime.text, "1:01 / 4:00");
        verify(popupTime.Accessible.name.indexOf("Time played") === 0);
        mouseClick(popupTime);
        compare(settings.showRemainingTime, true);
        compare(popupTime.text, "-2:59 / 4:00");
        compare(barTime.text, "-2:59 / 4:00", "the bar follows");
        const fresh = createTemporaryObject(settingsComponent, testCase);
        compare(fresh.showRemainingTime, true, "kept");
        popup.close();
        tryCompare(popup, "visible", false);
        mouseClick(barTime);
        compare(barTime.text, "1:01 / 4:00", "the bar's click switches back");
        barTime.forceActiveFocus();
        keyClick(Qt.Key_Space);
        compare(barTime.text, "-2:59 / 4:00", "and the keyboard");
        settings.showRemainingTime = false;
    }

    // The bar blends from one track to the next: the old face held until
    // the new track's length is known, both during the blend, only the
    // new one after it; ten changes in a row end with one face.
    function faces(bar) {
        const holder = findChild(bar, "playerFaces");
        const out = [];
        for (let i = 0; i < holder.children.length; ++i) {
            if (holder.children[i].objectName === "playerFace" && !holder.children[i].retired) {
                out.push(holder.children[i]);
            }
        }
        return out;
    }
    function test_theBarBlendsToTheNextTrack() {
        const player = makeFakePlayer({position: 61000});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player, blendAnimated: false});
        waitForPolish(testCase.Window.window);
        compare(faces(bar).length, 1);
        const height = bar.height;
        // The new track's length not known yet: the old face is held.
        player.duration = 0;
        player.changeTrack("Space Oddity", "Another Artist", 600, player.detail);
        let both = faces(bar);
        compare(both.length, 2, "old and new");
        const old = both.filter((face) => face.isLeaving)[0];
        const next = both.filter((face) => !face.isLeaving)[0];
        compare(old.title, "Major Tom", "the old face keeps its track");
        compare(next.title, "Space Oddity");
        compare(old.opacity, 1, "held");
        compare(next.opacity, 0, "the new one waits for its data");
        player.duration = testCase.trackMs;
        compare(bar.blendWaiting, false, "its length came: the blend may start");
        bar.blend = 0.5;
        fuzzyCompare(old.opacity, 0.5, 1e-9);
        fuzzyCompare(next.opacity, 0.5, 1e-9);
        verify(old.layer.enabled && next.layer.enabled, "each fades as one picture");
        compare(bar.height, height, "no layout jump");
        bar.blend = 1;
        tryVerify(() => faces(bar).length === 1, 2000, "the old face gone");
        compare(faces(bar)[0].title, "Space Oddity");
        compare(faces(bar)[0].opacity, 1);

        // Ten in a row, animated: one face at the end, the last track's.
        bar.blendAnimated = true;
        for (let i = 0; i < 10; ++i) {
            player.changeTrack("Track " + i, "Artist", 500, player.detail);
        }
        verify(faces(bar).length <= 2, "never more than two faces");
        tryVerify(() => faces(bar).length === 1 && bar.blend === 1, 3000, "settles");
        compare(faces(bar)[0].title, "Track 9");
    }

    // ---- Second review of 2026-11-20 -----------------------------------

    function plantedStick(columnsById) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0);
        const pioneer = stick + "/PIONEER";
        const ids = stickFixture.rekordboxTrackIds(pioneer, columnsById.length);
        for (let i = 0; i < columnsById.length; ++i) {
            verify(stickFixture.plantScrollWaveform(pioneer, ids[i], columnsById[i]), "planted " + ids[i]);
        }
        return {pioneer: pioneer, ids: ids};
    }
    function detailColumns(player) {
        return player.detailWaveform.columns.byteLength / 3;
    }

    // A detail asked for track A that lands after B was loaded is dropped.
    function test_anEarlierTracksDetailIsDropped() {
        const stick = plantedStick([300, 200]);
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("rekordbox", stick.pioneer, stick.ids[0], testCase.clickFile, "A", "", "", [], "", false);
        player.requestDetailWaveform();
        player.load("rekordbox", stick.pioneer, stick.ids[1], testCase.clickFile, "B", "", "", [], "", false);
        wait(300);
        compare(detailColumns(player), 0, "A's columns are not B's");
        player.requestDetailWaveform();
        tryVerify(() => detailColumns(player) === 200, 5000, "B's own");
        player.stop();
    }

    // A write that starts while the read runs: nothing taken from it, read
    // again when the write is over.
    function test_aWriteDuringTheReadIsWaitedFor() {
        const stick = plantedStick([300]);
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("rekordbox", stick.pioneer, stick.ids[0], testCase.clickFile, "A", "", "", [], "", false);
        player.requestDetailWaveform();
        player.libraryBusy = true;
        wait(300);
        compare(detailColumns(player), 0, "nothing while the write runs");
        player.libraryBusy = false;
        tryVerify(() => detailColumns(player) === 300, 5000, "read once it is over");
        player.stop();
    }

    // A write may replace what was delivered: dropped when it ends and
    // read again while it is wanted; once released, not read for nobody.
    function test_aDeliveredDetailIsReadAgainAfterAWrite() {
        const stick = plantedStick([300]);
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("rekordbox", stick.pioneer, stick.ids[0], testCase.clickFile, "A", "", "", [], "", false);
        player.requestDetailWaveform();
        tryVerify(() => detailColumns(player) === 300, 5000);
        verify(stickFixture.plantScrollWaveform(stick.pioneer, stick.ids[0], 500), "the write");
        player.libraryBusy = true;
        player.libraryBusy = false;
        tryVerify(() => detailColumns(player) === 500, 5000, "the new one");
        player.releaseDetailWaveform();
        player.libraryBusy = true;
        player.libraryBusy = false;
        wait(300);
        compare(detailColumns(player), 0, "released: dropped and not read again");
        player.stop();
    }

    // Touchpad phases: a begin and an end without deltas do nothing; the
    // fingers move it; let go with speed it glides; the system's own
    // momentum (macOS) is followed with no glide of ours on top.
    function test_touchpadPhases() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const a = area(popup);
        const flick = flickOf(popup);
        const y = a.height / 2;
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 1);
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 3);
        compare(popup.attached, true, "a begin and an end with nothing between: nothing");
        verify(!flick.moving);
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 1);
        const before = popup.centerMs;
        for (let i = 0; i < 3; ++i) {
            stickFixture.touchpadWheel(a, 600, y, -30, 0, 2);
        }
        compare(popup.attached, false);
        verify(Math.abs(popup.centerMs - (before + 90 / popup.pxPerMs)) < 1, "followed the fingers");
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 3);
        verify(flick.moving, "let go with speed: a glide");
        const atMomentum = popup.centerMs;
        stickFixture.touchpadWheel(a, 600, y, -20, 0, 4);
        verify(!flick.moving, "the system's momentum stops ours");
        verify(Math.abs(popup.centerMs - (atMomentum + 20 / popup.pxPerMs)) < 1, "and moves it itself");
    }

    // Grabbing the waveform while it plays holds it where it was: no jump
    // back when the press becomes a drag.
    function test_aGrabWhilePlayingHoldsTheWaveform() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        popup.frameClock = false;
        player.playing = true;
        const a = area(popup);
        const y = a.height / 2;
        const grabbed = popup.centerMs;
        mousePress(a, 700, y);
        popup.advanceClock(1.0);
        compare(popup.centerMs, grabbed, "held under the hand");
        for (let x = 680; x >= 500; x -= 20) {
            mouseMove(a, x, y);
        }
        verify(popup.centerMs >= grabbed, "no jump back: " + grabbed + " to " + popup.centerMs);
        mouseRelease(a, 500, y);
        flickOf(popup).cancelFlick();
        player.playing = false;
    }

    // A press that stops a glide does not seek on its release. Real time:
    // the glide must still be running when the press lands, which a very
    // loaded machine could prevent; the velocity is high and the press
    // comes at once, so it is not a close thing.
    function test_aPressThatStopsAGlideDoesNotSeek() {
        const player = makeFakePlayer({position: 60000});
        const popup = openLoaded(player);
        const a = area(popup);
        popup.flingAt(2500);
        verify(flickOf(popup).moving, "gliding");
        player.calls = [];
        mousePress(a, 700, a.height / 2);
        mouseRelease(a, 700, a.height / 2);
        compare(player.calls.indexOf("seek"), -1, "no seek: " + player.calls);
        compare(popup.attached, false, "still let go");
        verify(!flickOf(popup).moving, "stopped");
        // A plain click after that seeks as ever.
        mouseClick(a, 700, a.height / 2);
        verify(player.calls.indexOf("seek") >= 0, "a click seeks");
    }

    // The time with the keyboard: Return switches it; Space stays the
    // play key, with the app's own shortcut there.
    function test_theTimeKeys() {
        const player = makeFakePlayer({position: 61000});
        createTemporaryObject(spaceComponent, testCase, {playbackController: player});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player});
        waitForPolish(testCase.Window.window);
        const time = findChild(bar, "playerTime");
        time.forceActiveFocus();
        compare(time.text, "1:01 / 4:00");
        keyClick(Qt.Key_Space);
        compare(player.playing, true, "Space plays");
        compare(time.text, "1:01 / 4:00", "and leaves the time");
        keyClick(Qt.Key_Return);
        compare(time.text, "-2:59 / 4:00", "Return switches it");
        keyClick(Qt.Key_Enter);
        compare(time.text, "1:01 / 4:00", "and Enter");
        verify(time.Accessible.description.indexOf("Return") >= 0, time.Accessible.description);
        player.playing = false;
    }

    // The time left adds up with the time played to the length shown, and
    // without a length only the time played is shown.
    Component {
        id: timeComponent
        TrackTimeLabel {}
    }
    function test_theTimeLeftAddsUp() {
        const time = createTemporaryObject(timeComponent, testCase, {positionMs: 61500, durationMs: 240400,
                                                                     localRemaining: true});
        compare(time.text, "-2:59 / 4:00");
        time.localRemaining = false;
        compare(time.text, "1:01 / 4:00");
        time.localRemaining = true;
        time.durationMs = 0;
        compare(time.text, "1:01 / 0:00", "no length: the time played");
    }

    // The zoom is written once it has been left alone, not at each notch.
    function test_theZoomIsWrittenOnceSettled() {
        const settings = createTemporaryObject(settingsComponent, testCase);
        settings.waveformZoomBars = 0;
        const popup = createTemporaryObject(popupComponent, testCase,
                                            {playbackController: makeFakePlayer({position: 60000}), settings: settings});
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        waitForPolish(testCase.Window.window);
        const writes = createTemporaryObject(spyComponent, testCase, {target: settings, signalName: "waveformZoomBarsChanged"});
        for (let i = 0; i < 3; ++i) {
            mouseWheel(area(popup), 600, 50, 0, 120, Qt.NoButton, Qt.ControlModifier);
        }
        compare(writes.count, 0, "not at every notch");
        tryCompare(writes, "count", 1, 3000, "once, when it has settled");
        fuzzyCompare(settings.waveformZoomBars, popup.spanBars, 1e-9);
        settings.waveformZoomBars = 0;
        popup.close();
    }

    // The real controller announces every change of track before it makes
    // it: load, a missing file, loadFile, stop. A missing file leaves no
    // old track behind: no length, and Play does not play the old file.
    function test_theControllerAnnouncesEveryChange() {
        const player = createTemporaryObject(realPlayer, testCase);
        const order = [];
        const about = () => order.push("about:" + player.trackSerial);
        const changed = () => order.push("changed:" + player.trackSerial);
        player.trackAboutToChange.connect(about);
        player.trackChanged.connect(changed);
        player.load("rekordbox", "", "1", testCase.clickFile, "Click", "", "", [], "", false);
        tryVerify(() => player.duration > 0 && player.durationSerial === player.trackSerial, 5000, "a length");
        const clickLength = player.duration;
        // Settled by running (2026-11-20, Qt 6.11, FFmpeg backend, under
        // xvfb): loading another file reads duration 0 at once; loading
        // the same file again changes nothing and still reads its length
        // (3030 ms), with no new report to come. durationSerial says whose
        // length it is either way: the same file's is taken as known at
        // once.
        player.load("rekordbox", "", "2", testCase.otherClickFile, "Another click", "", "", [], "", false);
        console.log("OBSERVED duration right after loading another file:", player.duration,
                    "durationSerial", player.durationSerial, "trackSerial", player.trackSerial);
        verify(player.durationSerial !== player.trackSerial, "not known yet for the new serial");
        tryVerify(() => player.durationSerial === player.trackSerial, 5000);
        player.load("rekordbox", "", "2b", testCase.otherClickFile, "The same again", "", "", [], "", false);
        tryVerify(() => player.durationSerial === player.trackSerial, 5000, "the same file again");
        player.load("rekordbox", "", "3", "/nonexistent/missing.mp3", "Missing", "", "", [], "", false);
        compare(player.duration, 0, "no length left of the old file");
        compare(player.durationSerial, player.trackSerial, "known: none will come");
        verify(player.errorMessage.length > 0);
        player.togglePlay();
        wait(200);
        compare(player.playing, false, "Play does not play the old file");
        player.loadFile("rekordbox", "", testCase.clickFile, "Recording", "");
        player.stop();
        compare(order.length, 12, JSON.stringify(order));
        for (let i = 0; i < order.length; i += 2) {
            verify(order[i].indexOf("about:") === 0 && order[i + 1].indexOf("changed:") === 0, JSON.stringify(order));
            verify(order[i] !== "about:" + order[i + 1].split(":")[1], "announced before the serial moved");
        }
        verify(clickLength > 0);
    }

    // The old face keeps its own cues, grid, position and length while the
    // new track's come in; after the blend no face is layered.
    function test_theOldFaceKeepsItsOwnTrack() {
        const player = makeFakePlayer({position: 61000});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player, blendAnimated: false});
        waitForPolish(testCase.Window.window);
        const first = faces(bar)[0];
        const cues = first.cues;
        const beats = first.beats;
        verify(beats.length > 0);
        player.changeTrack("Space Oddity", "Another Artist", 600, player.detail);
        player.cues = [];
        player.position = 5000;
        player.duration = 180000;
        const old = faces(bar).filter((face) => face.isLeaving)[0];
        compare(old, first);
        compare(old.cues, cues, "its cues");
        compare(old.beats, beats, "its grid");
        compare(old.positionMs, 61000, "its position");
        compare(old.durationMs, testCase.trackMs, "its length");
        bar.blend = 1;
        tryVerify(() => faces(bar).length === 1, 2000);
        compare(faces(bar)[0].layer.enabled, false, "not layered once whole");
    }

    // A change during a blend goes on from what shows: no pop.
    function test_anInterruptedBlendDoesNotPop() {
        const player = makeFakePlayer({position: 61000});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player, blendAnimated: false});
        waitForPolish(testCase.Window.window);
        player.changeTrack("Second", "", 500, player.detail);
        bar.blend = 0.7;
        const second = faces(bar).filter((face) => !face.isLeaving)[0];
        fuzzyCompare(second.opacity, 0.7, 1e-9);
        player.changeTrack("Third", "", 500, player.detail);
        compare(second.isLeaving, true, "the face mostly showing stays");
        fuzzyCompare(second.opacity, 0.7, 1e-9, "at the opacity it had");
        const third = faces(bar).filter((face) => !face.isLeaving)[0];
        compare(third.title, "Third");
        verify(third.opacity < 0.01, "the new one starts from nothing");
        bar.blend = 1;
        tryVerify(() => faces(bar).length === 1 && faces(bar)[0].title === "Third", 2000);
    }

    // While the new track's length is awaited, the old face's popup button
    // still opens the popup for what is loaded.
    function test_theHeldFaceStillOpensThePopup() {
        const player = makeFakePlayer({position: 61000});
        const bar = createTemporaryObject(barComponent, testCase, {controller: player, blendAnimated: false});
        waitForPolish(testCase.Window.window);
        player.duration = 0;
        player.durationSerial = player.trackSerial;   // the new length not reported yet
        player.changeTrack("Space Oddity", "", 600, player.detail);
        verify(bar.blendWaiting, "held");
        const old = faces(bar).filter((face) => face.isLeaving)[0];
        mouseClick(findChild(old, "openWaveformPopupButton"));
        const popup = findChild(bar, "waveformZoomPopup");
        tryCompare(popup, "opened", true);
        compare(popup.trackTitle, "Space Oddity", "for the loaded track");
        popup.close();
    }

    // ---- Third review of 2026-11-20 -------------------------------------

    // A touchpad lift flings only when the fingers were moving at the
    // lift: held still a second (Wayland sends nothing meanwhile), then
    // lifted, nothing glides.
    function test_aStaleSwipeDoesNotFling() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const a = area(popup);
        const flick = flickOf(popup);
        const y = a.height / 2;
        popup.clockNow = 10000;
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 1);
        for (let i = 0; i < 3; ++i) {
            popup.clockNow += 10;
            stickFixture.touchpadWheel(a, 600, y, -30, 0, 2);
        }
        popup.clockNow += 1000;
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 3);
        verify(!flick.moving, "held still before the lift: no glide");
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 1);
        for (let i = 0; i < 3; ++i) {
            popup.clockNow += 10;
            stickFixture.touchpadWheel(a, 600, y, -30, 0, 2);
        }
        popup.clockNow += 10;
        stickFixture.touchpadWheel(a, 600, y, 0, 0, 3);
        verify(flick.moving, "lifted while moving: a glide");
        flick.cancelFlick();
        popup.clockNow = -1;
    }

    // Opening the popup after a load reads export.pdb no more: the index
    // the load built is handed to the read.
    function test_theDetailReadUsesTheLoadsIndex() {
        const stick = plantedStick([300]);
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("rekordbox", stick.pioneer, stick.ids[0], testCase.clickFile, "A", "", "", [], "", false);
        const parses = stickFixture.trackDatabaseParses();
        player.requestDetailWaveform();
        tryVerify(() => detailColumns(player) === 300, 5000);
        compare(stickFixture.trackDatabaseParses(), parses, "export.pdb not parsed again");
        player.stop();
    }

    // A file that does not decode, loaded again: no new status comes, and
    // its length is known (none) at once, so the bar does not wait.
    function test_aBrokenFileAgainIsKnownAtOnce() {
        const broken = stickFixture.scratchDirectory() + "/not-audio.mp3";
        verify(stickFixture.writeText(broken, "this is not an MP3 file, only text"));
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("rekordbox", "", "1", broken, "Broken", "", "", [], "", false);
        tryVerify(() => player.durationSerial === player.trackSerial, 5000, "the first load: InvalidMedia reported");
        player.load("rekordbox", "", "2", broken, "Broken again", "", "", [], "", false);
        compare(player.durationSerial, player.trackSerial, "the same broken file again: known at once");
        player.stop();
    }

    // ---- The two sides of a track (#131) ------------------------------

    // The Beat Grids & Waveforms page hands the popup both copies of a
    // track: rekordbox's and Engine's.
    function sidesFor(engineAvailable) {
        return [
            {label: "rekordbox", available: true, scheme: "vivid",
             columns: columnsOf(230, 128, 51).columns, msPerColumn: 1000 / 150, startMs: 0,
             ticksAt: (durationMs, widthPx) => BeatGridTicks.layout(testCase.grid(0, durationMs), durationMs, widthPx).ticks,
             track: {format: "rekordbox", libraryPath: "/fixture/PIONEER", sourceId: "7",
                     filePath: "/fixture/Contents/major-tom.mp3", title: "Major Tom", artist: "DJ Amador"}},
            {label: "Engine", available: engineAvailable, scheme: "engine",
             columns: columnsOf(120, 90, 60).columns, msPerColumn: 1000 / 150, startMs: 0,
             ticksAt: (durationMs, widthPx) => BeatGridTicks.layout(testCase.grid(10, durationMs), durationMs, widthPx).ticks,
             track: {format: "engine", libraryPath: "/fixture/Engine Library", sourceId: "e7",
                     filePath: "/fixture/Engine Library/Music/major-tom.mp3", title: "Major Tom", artist: "DJ Amador"}},
        ];
    }
    function lanesOf(popup) {
        const out = [];
        const a = area(popup);
        for (let i = 0; i < a.children.length; ++i) {
            if (a.children[i].objectName === "zoomLane") {
                out.push(a.children[i]);
            }
        }
        return out.sort((x, y) => x.y - y.y);
    }
    function tilesOf(lane) {
        const s = findChild(lane, "waveformStrip");
        const out = [];
        for (let i = 0; i < s.children.length; ++i) {
            if (s.children[i].objectName === "zoomTile") {
                out.push(s.children[i]);
            }
        }
        return out;
    }

    // Elsewhere the popup has one side and no box to switch.
    function test_noSideBoxWithoutTwoSides() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        const box = findChild(popup.contentItem, "zoomSideBox");
        verify(box !== null, "the popup has the box");
        compare(box.visible, false, "hidden: there is only one side");
    }

    // The box shows the side that is loaded, in its own colours; picking
    // the other loads that copy and keeps the zoom and the place.
    function test_theSideBoxSwitchesKeepingZoomAndPlace() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        const box = findChild(popup.contentItem, "zoomSideBox");
        compare(box.visible, true, "both sides: the box is shown");
        compare(box.count, 3, "rekordbox, Engine and Both");
        compare(box.currentIndex, 0, "rekordbox's copy is loaded");
        compare(box.currentText, "rekordbox");
        compare(popup.colorScheme, "vivid", "in rekordbox's colours");

        popup.zoomBy(2, popup.lineX);
        const span = popup.shownSpanMs;
        popup.scrollBy(400);
        compare(popup.attached, false);
        const at = popup.manualMs;

        box.activated(1);
        compare(player.loads.length, 1, "Engine's copy is loaded");
        compare(player.loads[0].sourceId, "e7");
        compare(player.currentFormat, "engine");
        tryCompare(player, "position", 60000, 2000, "the player goes back to where it was");
        compare(box.currentIndex, 1);
        compare(popup.colorScheme, "engine", "in Engine's colours");
        compare(popup.shownSpanMs, span, "at the same zoom");
        compare(popup.attached, false, "still held where it was scrolled to");
        compare(popup.manualMs, at, "at the same place");

        // And back, following the playhead: it still follows after.
        popup.follow();
        box.activated(0);
        compare(player.currentFormat, "rekordbox");
        tryCompare(player, "position", 60000, 2000);
        compare(popup.attached, true, "following, as before the switch");
        compare(popup.colorScheme, "vivid");
        compare(popup.shownSpanMs, span);
    }

    // A side with nothing to show is in the box, switched off.
    function test_aSideWithoutDataIsDisabled() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(false);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        const box = findChild(popup.contentItem, "zoomSideBox");
        compare(box.visible, true);
        box.popup.open();
        tryCompare(box.popup, "opened", true);
        const entry = findChild(box.popup.contentItem, "zoomSideEntry_1");
        verify(entry !== null, "Engine's entry");
        compare(entry.enabled, false, "Engine has nothing: its entry is off");
        compare(findChild(box.popup.contentItem, "zoomSideEntry_0").enabled, true);
        compare(findChild(box.popup.contentItem, "zoomSideEntry_2").enabled, false, "nor Both");
        box.popup.close();
        box.activated(1);
        compare(player.loads.length, 0, "nothing loaded");
        compare(box.currentIndex, 0, "the box stays on rekordbox");
        box.activated(2);
        compare(popup.showingBoth, false, "Both needs both sides");
        compare(box.currentIndex, 0);
        compare(lanesOf(popup).length, 1, "one lane");
    }

    // The popup's own margins are Theme's tight spacing, so the waveform
    // has the width (#132).
    function test_theMarginsAreTight() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        for (const side of ["leftPadding", "rightPadding", "topPadding", "bottomPadding"]) {
            compare(popup[side], Theme.tightSpacing, side);
        }
        const a = area(popup);
        compare(a.width, popup.availableWidth, "the waveform is as wide as the popup allows");
        compare(a.mapToItem(popup.contentItem, 0, 0).x, 0);
        if (screenshotDir && screenshotDir.length > 0) {
            waitForTiles(popup);
            grabImage(testCase).save(screenshotDir + "/waveform-zoom-popup-margins.png");
        }
    }

    // Both: rekordbox's and Engine's stacked, each in its own look, on one
    // timeline: zoomed and scrolled together.
    function test_bothSidesStackedAndInStep() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.wantBoth = true;
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        const box = findChild(popup.contentItem, "zoomSideBox");
        compare(box.currentText, "Both");
        compare(popup.showingBoth, true);
        const lanes = lanesOf(popup);
        compare(lanes.length, 2, "two lanes");
        verify(lanes[0].y + lanes[0].height <= lanes[1].y, "one above the other");
        const a = area(popup);
        tryVerify(() => a.mapToItem(popup.contentItem, 0, a.height).y <= popup.availableHeight, 2000,
                  "the popup grows to hold both: " + a.height + " in " + popup.availableHeight);
        compare(lanes[0].height, lanes[1].height);
        tryVerify(() => tilesOf(lanes[0]).length > 0 && tilesOf(lanes[1]).length > 0, 2000, "tiles in both");
        for (const tile of tilesOf(lanes[0])) {
            compare(tile.colorScheme, "vivid", "rekordbox's half in rekordbox's look");
        }
        for (const tile of tilesOf(lanes[1])) {
            compare(tile.colorScheme, "engine", "Engine's half in Engine's look");
        }
        verify(tilesOf(lanes[0]).every((t) => t.byteColumns > 0), "rekordbox's columns drawn");
        verify(tilesOf(lanes[1]).every((t) => t.byteColumns > 0), "Engine's columns drawn");
        const inStep = (why) => {
            compare(findChild(lanes[0], "waveformStrip").x, findChild(lanes[1], "waveformStrip").x, why);
            const top = tilesOf(lanes[0]);
            const bottom = tilesOf(lanes[1]);
            compare(top.length, bottom.length, why);
            for (const t of top) {
                const twin = bottom.find((b) => b.tile === t.tile);
                verify(twin !== undefined, why + ": tile " + t.tile + " in both");
                compare(twin.viewStartMs, t.viewStartMs, why);
                compare(twin.viewSpanMs, t.viewSpanMs, why);
                compare(twin.x, t.x, why);
            }
            tryVerify(() => tilesOf(lanes[1]).some((b) => b.beatTicks.length > 0), 2000, why + ": Engine's grid");
            // The same moment under the playhead line in both.
            for (const lane of lanes) {
                const s = findChild(lane, "waveformStrip");
                const tile = tilesOf(lane).find((t) => s.x + t.x <= popup.lineX && s.x + t.x + t.width > popup.lineX);
                verify(tile !== undefined, why + ": a tile under the line");
                const ms = tile.viewStartMs + (popup.lineX - s.x - tile.x) / popup.pxPerMs;
                verify(Math.abs(ms - popup.centerMs) <= 1 / popup.pxPerMs, why + ": " + ms + " under the line, the playhead at "
                       + popup.centerMs);
            }
        };
        inStep("opened");
        popup.zoomBy(2, popup.lineX);
        inStep("zoomed");
        popup.scrollBy(700);
        inStep("scrolled");
        player.seek(90000);
        inStep("seeked");
        if (screenshotDir && screenshotDir.length > 0) {
            waitForPolish(testCase.Window.window);
            waitForRendering(popup.contentItem);
            grabImage(testCase).save(screenshotDir + "/waveform-zoom-popup-both.png");
        }

        // One side picked: one lane, that side's copy, the same place.
        box.activated(1);
        compare(popup.showingBoth, false);
        compare(player.currentFormat, "engine");
        tryCompare(player, "position", 90000, 2000);
        compare(lanesOf(popup).length, 1);
        compare(popup.colorScheme, "engine");
        box.activated(2);
        compare(popup.showingBoth, true, "and Both again");
        compare(lanesOf(popup).length, 2);
    }

    // The player moving on to another track (the queue, a media key) while
    // Both shows: the popup follows it as one view, with no box, and draws
    // nothing of the page's track.
    function test_bothFollowsThePlayerOntoAnotherTrack() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.wantBoth = true;
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        compare(popup.showingBoth, true);
        compare(lanesOf(popup).length, 2);
        player.load("rekordbox", "/fixture/PIONEER", "99", "/fixture/Contents/next.mp3", "Next", "", "", [], "", false);
        compare(popup.loadedSide, -1, "the page's track is no longer loaded");
        compare(popup.showingBoth, false, "Both stops");
        compare(lanesOf(popup).length, 1, "one lane, the player's");
        compare(popup.lanes[0].columns, popup.hasDetail ? popup.detail.columns : null, "drawn from the player");
        compare(findChild(popup.contentItem, "zoomSideBox").visible, false, "and no box for a track it is not");
    }

    // A side switch still waiting for the length when the popup closes is
    // dropped: the next opening, on another track, gets no seek and no
    // zoom of it.
    function test_aSwitchLeftUnfinishedIsDropped() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        popup.zoomBy(2, popup.lineX);
        const box = findChild(popup.contentItem, "zoomSideBox");
        // The length of what is loaded next is not known yet.
        player.durationSerial = 0;
        box.activated(1);
        compare(player.currentFormat, "engine");
        verify(popup.pendingSideMs >= 0, "the switch waits for the length");
        popup.close();
        tryCompare(popup, "visible", false);

        // Another track, of another tempo, opened as a page opens one.
        popup.sides = [];
        player.beatTimesMs = testCase.grid(0, testCase.trackMs, 400).map((beat) => beat.timeMs);
        const calls = player.calls.length;
        popup.openFor({format: "rekordbox", libraryPath: "/fixture/PIONEER", sourceId: "42",
                       filePath: "/fixture/Contents/other.mp3", title: "Other", artist: ""});
        tryCompare(popup, "opened", true);
        player.durationSerial = Qt.binding(() => player.trackSerial);
        tryVerify(() => popup.lengthKnown, 2000);
        wait(50);
        compare(player.calls.slice(calls).indexOf("seek"), -1, "no seek carried over: " + player.calls.slice(calls));
        compare(player.position, 0, "the new track at its start");
        compare(popup.spanBars, popup.localZoomBars, "at the popup's own zoom in bars");
        compare(popup.attached, true, "following the playhead");
    }

    // A side switch whose copy never gets a length (the load ends with
    // none) is given up: the popup follows the player again, and a length
    // that comes later seeks nowhere.
    function test_aSwitchWithoutALengthIsGivenUp() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        popup.scrollBy(400);
        compare(popup.attached, false);
        player.durationSerial = 0;
        findChild(popup.contentItem, "zoomSideBox").activated(1);
        verify(popup.pendingSideMs >= 0, "the switch waits for the length");
        const calls = player.calls.length;
        // The load ends: its length is reported, and it is none.
        player.duration = 0;
        player.durationSerial = player.trackSerial;
        compare(popup.pendingSideMs, -1, "given up");
        compare(popup.attached, true, "following the player again");
        player.duration = testCase.trackMs;
        wait(50);
        compare(player.calls.slice(calls).indexOf("seek"), -1, "nothing seeks: " + player.calls.slice(calls));
    }

    // The player going on to another track (a media key) before the
    // switched side's length came: the switch is given up, and the other
    // track is not moved to the old place.
    function test_aSwitchOvertakenByAnotherTrackIsGivenUp() {
        const player = makeFakePlayer({position: 60000});
        const popup = makePopup(player);
        popup.sides = sidesFor(true);
        popup.openLoaded();
        tryCompare(popup, "opened", true);
        tryVerify(() => popup.pxPerMs > 0, 2000, "laid out");
        player.durationSerial = 0;
        findChild(popup.contentItem, "zoomSideBox").activated(1);
        verify(popup.pendingSideMs >= 0, "the switch waits for the length");
        const calls = player.calls.length;
        player.load("rekordbox", "/fixture/PIONEER", "99", "/fixture/Contents/next.mp3", "Next", "", "", [], "", false);
        compare(popup.pendingSideMs, -1, "given up for the other track");
        player.durationSerial = Qt.binding(() => player.trackSerial);
        tryVerify(() => popup.lengthKnown, 2000);
        wait(50);
        compare(player.calls.slice(calls).indexOf("seek"), -1, "the other track stays at its start");
        compare(player.position, 0);
        compare(popup.attached, true, "and is followed");
    }
}
