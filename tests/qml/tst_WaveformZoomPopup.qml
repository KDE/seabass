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
    function grid(fromMs, toMs) {
        const beats = [];
        let index = 0;
        for (let t = fromMs; t < toMs - 1e-6; t += 500) {
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
            function detailWaveform() { return detail; }
            function beatTicksAt(durationMs, widthPx) {
                return BeatGridTicks.layout(testCase.grid(0, durationMs), durationMs, widthPx).ticks;
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
        player.playing = true;
        const start = popup.centerMs;
        const xStart = strip(popup).x;
        wait(400);
        const advanced = popup.centerMs - start;
        verify(advanced > 150 && advanced < 1500, "moved on by itself: " + advanced + " ms in 400");
        verify(strip(popup).x < xStart, "and the strip with it");
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
        compare(popup.attached, false);
        verify(Math.abs(popup.centerMs - (60000 - 200 / popup.pxPerMs)) <= 3 / popup.pxPerMs,
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
        verify(barView.rulerHeight >= 12, "the bar's lane, " + barView.rulerHeight);
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
        const detail = player.detailWaveform();
        verify(detail.columns instanceof ArrayBuffer, "an ArrayBuffer, got " + typeof detail.columns);
        compare(detail.columns.byteLength, 0, "no track, no columns");
        compare(detail.msPerColumn, 0);
    }
}
