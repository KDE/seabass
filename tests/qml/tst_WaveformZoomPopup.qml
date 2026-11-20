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
            property var beatTimesMs: testCase.grid(0, testCase.trackMs).map((beat) => beat.timeMs)
            property var calls: []
            property var loads: []
            // What the details pane asks for, set by the tests that show one.
            property var waveformFor: null
            property var beatTicksFor: null
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
            signal seeked(real positionMs)
            function beatTicksAt(durationMs, widthPx) { return []; }
            function load(format, libraryPath, sourceId, filePath, title, artist, artworkPath, cues,
                          fallbackArtworkPath, startPlaying) {
                loads.push({sourceId: sourceId, filePath: filePath, title: title, startPlaying: startPlaying});
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

    // From four bars across to the whole track, by keys and buttons.
    function test_zoomStopsAtFourBarsAndTheWholeTrack() {
        const popup = openLoaded(makeFakePlayer({position: 60000}));
        compare(popup.barMs, 2000);
        compare(popup.shownSpanMs, 16 * 2000, "opens at 16 bars");
        for (let i = 0; i < 12; ++i) {
            keyClick(Qt.Key_Plus);
        }
        compare(popup.shownSpanMs, 4 * 2000, "no closer than four bars");
        verify(!findChild(popup.contentItem, "zoomInButton").enabled, "zoom in is off there");
        const zoomOut = findChild(popup.contentItem, "zoomOutButton");
        for (let i = 0; i < 20; ++i) {
            mouseClick(zoomOut);
        }
        compare(popup.shownSpanMs, testCase.trackMs, "no further than the whole track");
        verify(!zoomOut.enabled, "zoom out is off there");
        keyClick(Qt.Key_Minus);
        compare(popup.shownSpanMs, testCase.trackMs);
        // Zooming while following keeps the position under the line.
        keyClick(Qt.Key_Plus);
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
        const ground = pixelAt(popup.lineX + 20, 10);
        for (let x = popup.lineX + 20; x < a.width - 20; x += 3) {
            const c = pixelAt(x, 10);
            const d = Math.abs(c.r - ground.r) + Math.abs(c.g - ground.g) + Math.abs(c.b - ground.b);
            verify(d < 0.01, "no line at " + x + ": " + d);
        }
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

    // Engine: the waveform load() read is all there is (2.x and 3.x keep
    // only the overview), so the popup is handed it without the library
    // being opened again: changed in the database after the load, the
    // popup still gets what was loaded.
    function test_anEngineTracksDetailIsWhatLoadRead() {
        const lib = engineGridFixture.library();
        verify(lib.length > 0);
        const id = engineGridFixture.trackId(lib);
        verify(engineGridFixture.setWaveform(lib, 1024, 128));
        const player = createTemporaryObject(realPlayer, testCase);
        player.load("engine", lib, id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        verify(engineGridFixture.setWaveform(lib, 512, 64));
        player.requestDetailWaveform();
        const detail = player.detailWaveform;
        compare(detail.columns.byteLength, 1024 * 3, "the 1024 entries the load read, at once");
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
        const zoomIn = findChild(content, "zoomInButton");
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
        player.changeTrack("Space Oddity", "", 600, player.detail);
        verify(bar.blendWaiting, "held");
        const old = faces(bar).filter((face) => face.isLeaving)[0];
        mouseClick(findChild(old, "openWaveformPopupButton"));
        const popup = findChild(bar, "waveformZoomPopup");
        tryCompare(popup, "opened", true);
        compare(popup.trackTitle, "Space Oddity", "for the loaded track");
        popup.close();
    }
}
