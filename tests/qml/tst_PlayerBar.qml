// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

TestCase {
    id: testCase
    name: "PlayerBar"
    width: 820
    height: 200
    visible: true
    when: windowShown

    Component {
        id: barComponent
        PlayerBar { width: 800 }
    }

    function fakeController() {
        return {title: "Major Tom", artist: "DJ Amador", artworkPath: "", position: 0, duration: 372000,
                playing: false, errorMessage: "", waveform: [], cues: [], calls: [],
                togglePlay: function() { this.calls.push("togglePlay"); },
                seek: function(ms) { this.calls.push("seek"); },
                stop: function() { this.calls.push("stop"); }};
    }

    // Out of the way until the pointer is over the player, in its top
    // right corner, and closing stops playback.
    function test_closeAppearsOnHoverAndStopsPlayback() {
        var bar = createTemporaryObject(barComponent, testCase, {controller: fakeController()});
        verify(bar !== null);
        waitForRendering(bar);
        var close = findChild(bar, "closePlayerButton");
        verify(close !== null, "the player must have a close button");
        compare(close.visible, false, "hidden until the pointer is over the player");

        mouseMove(bar, bar.width / 2, bar.height / 2);
        tryCompare(close, "visible", true);
        var corner = close.mapToItem(bar, close.width, 0);
        compare(Math.round(corner.x), Math.round(bar.width), "flush with the right edge");
        compare(Math.round(corner.y), 0, "flush with the top edge");
        // And clear of the time beside it.
        var time = findChild(bar, "playerTime");
        verify(time !== null);
        var timeRight = time.mapToItem(bar, time.width, 0).x;
        var closeLeft = close.mapToItem(bar, 0, 0).x;
        verify(timeRight <= closeLeft, "the close button must not cover the time (" + timeRight + " vs " + closeLeft + ")");

        mouseClick(close);
        verify(bar.controller.calls.indexOf("stop") >= 0, "closing must stop playback");
    }

    // A cover the stick does not have: the fallback where there is one,
    // and where there is none no square at all, not an empty one.
    function test_aMissingCoverFallsBackOrLeavesNoGap() {
        const controller = fakeController();
        controller.artworkPath = browseFixture.missingArtworkUrl();
        controller.fallbackArtworkPath = browseFixture.presentArtworkUrl();
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        verify(bar !== null);
        const art = findChild(bar, "playerArtwork");
        tryCompare(art, "showing", "fallback");
        compare(art.visible, true);
        bar.destroy();
        wait(0);

        const bare = fakeController();
        bare.artworkPath = browseFixture.missingArtworkUrl();
        const noArt = createTemporaryObject(barComponent, testCase, {controller: bare});
        const missing = findChild(noArt, "playerArtwork");
        tryCompare(missing, "sourceFailed", true);
        compare(missing.showing, "");
        compare(missing.visible, false, "no empty square before the title");
        waitForRendering(noArt);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(noArt).save(screenshotDir + "/player-bar-missing-art.png");
        }
    }

    // A controller whose properties notify, for the tests of the play
    // key: it follows the loaded track's catalog and the playing state
    // through bindings, which a plain JS object never tells about.
    Component {
        id: liveController
        QtObject {
            property string currentFormat: "rekordbox"
            property bool playing: false
            property string title: "Major Tom"
            property string artist: "DJ Amador"
            property string artworkPath: ""
            property string fallbackArtworkPath: ""
            property int position: 0
            property int duration: 372000
            property string errorMessage: ""
            property var waveform: []
            property var cues: []
            property var calls: []
            function togglePlay() { playing = !playing; }
            function seek(ms) { calls.push("seek"); }
            function stop() { calls.push("stop"); }
        }
    }

    function sameColor(a, b) {
        return Qt.colorEqual(String(a), String(b));
    }

    // The rim's pixel at the middle of the key's left edge, one pixel
    // inside the face's inset: on the straight side of either form. The
    // bar is grabbed, not the key: a grab of the key alone came back
    // shifted by the Frame's padding, twelve columns of window before
    // the key's own first pixel.
    function rimPixel(bar, key) {
        const image = grabImage(bar);
        const at = key.mapToItem(bar, Theme.transportFaceInset + 1, key.height / 2);
        return PixelScale.pixel(image, bar, at.x, at.y);
    }
    function isGreenish(c) {
        return c.g > c.r + 0.25 && c.g > c.b + 0.25;
    }
    function isGrey(c) {
        return Math.abs(c.r - c.g) < 0.08 && Math.abs(c.g - c.b) < 0.08;
    }

    // The key takes the form of the deck the track plays on: a wide key
    // for either Rekordbox catalog, a round pad for Engine, and it
    // morphs from one to the other when the library changes.
    function test_theKeyTakesTheDecksForm() {
        const controller = createTemporaryObject(liveController, testCase, {currentFormat: "rekordbox"});
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        verify(bar !== null);
        waitForRendering(bar);
        const key = findChild(bar, "playButton");
        verify(key !== null, "the player must have a play key");
        compare(key.width, Theme.transportKeyWidth, "DeviceLibrary: the wide key");
        compare(key.height, Theme.transportPadSize);
        compare(key.radius, Theme.transportKeyRadius, "with the key's corner");

        controller.currentFormat = "onelibrary";
        wait(Theme.arrivalTransitionDuration + 50);
        compare(key.width, Theme.transportKeyWidth, "OneLibrary plays on the same decks: the same key");

        controller.currentFormat = "engine";
        let sawBetween = false;
        const seen = function() {
            if (key.width < Theme.transportKeyWidth - 1 && key.width > Theme.transportPadSize + 1) {
                sawBetween = true;
            }
        };
        key.widthChanged.connect(seen);
        tryCompare(key, "width", Theme.transportPadSize);
        key.widthChanged.disconnect(seen);
        verify(sawBetween, "the change of form is a movement, not a snap");
        tryCompare(key, "radius", Theme.transportPadSize / 2, 1000, "Engine: a round pad");
        waitForRendering(bar);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(bar).save(screenshotDir + "/player-bar-engine-paused.png");
        }
    }

    // Playing: the rim and the glyph are lit green, and stay lit. The
    // press reaches the controller.
    function test_playingLightsTheRim() {
        const controller = createTemporaryObject(liveController, testCase, {playing: true});
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        waitForRendering(bar);
        const key = findChild(bar, "playButton");
        const rim = findChild(bar, "playRim");
        compare(key.light, 1);
        compare(rim.opacity, 1);
        verify(sameColor(rim.border.color, Theme.transportLit));
        compare(key.morph, 1, "the pause bars");
        verify(isGreenish(rimPixel(bar, key)), "the rim is painted green while playing, got " + rimPixel(bar, key));
        wait(Theme.transportBlinkHalfPeriod + 100);
        compare(key.light, 1, "and does not blink while playing");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(bar).save(screenshotDir + "/player-bar-rekordbox-playing.png");
        }

        mouseClick(key);
        tryCompare(controller, "playing", false);
    }

    // Paused on the Pioneer form the light steps between off and on,
    // once a second in equal halves; on the Denon pad it breathes,
    // never out and never fully on.
    function test_pausedIsTheDecksIdleSignal() {
        const controller = createTemporaryObject(liveController, testCase, {currentFormat: "rekordbox", playing: false});
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        waitForRendering(bar);
        const key = findChild(bar, "playButton");

        let values = [];
        const note = function() { values.push(key.light); };
        key.lightChanged.connect(note);
        tryVerify(function() { return values.indexOf(0) >= 0 && values.indexOf(1) >= 0; },
                  2 * Theme.transportBlinkHalfPeriod + 500, "the Pioneer key blinks off and on");
        verify(values.every(function(v) { return v === 0 || v === 1; }), "in steps, never a fade: " + values);
        key.lightChanged.disconnect(note);
        // Off: the rim is not painted, the face shows through.
        tryCompare(key, "light", 0);
        verify(isGrey(rimPixel(bar, key)), "the rim is dark while the blink is off, got " + rimPixel(bar, key));
        tryCompare(key, "light", 1);
        verify(isGreenish(rimPixel(bar, key)), "and green while it is on, got " + rimPixel(bar, key));

        controller.currentFormat = "engine";
        tryCompare(key, "width", Theme.transportPadSize);
        values = [];
        key.lightChanged.connect(note);
        tryVerify(function() { return values.length > 8; }, 2000, "the Denon pad breathes");
        key.lightChanged.disconnect(note);
        verify(values.every(function(v) { return v > 0 && v < 1; }), "never out, never fully lit: " + values);
        verify(values.some(function(v) { return v !== values[0]; }), "and moving");

        // Playing again ends the idle signal at once, at full light.
        controller.playing = true;
        compare(key.light, 1);
        wait(Theme.transportBreathHalfPeriod / 2);
        compare(key.light, 1);
    }

    // The play triangle folds into the pause bars over the morph
    // duration, passing through the shapes in between, and back.
    function test_theGlyphFoldsRatherThanSwaps() {
        const controller = createTemporaryObject(liveController, testCase, {playing: false});
        const bar = createTemporaryObject(barComponent, testCase, {controller: controller});
        waitForRendering(bar);
        const key = findChild(bar, "playButton");
        compare(key.morph, 0, "the triangle");

        let between = 0;
        const note = function() { if (key.morph > 0.05 && key.morph < 0.95) { ++between; } };
        key.morphChanged.connect(note);
        controller.playing = true;
        compare(key.morph < 1, true, "not there yet at the instant of the press");
        tryCompare(key, "morph", 1, Theme.transportMorphDuration + 500);
        verify(between >= 2, "passed through the fold (" + between + " frames)");
        controller.playing = false;
        tryCompare(key, "morph", 0, Theme.transportMorphDuration + 500);
        key.morphChanged.disconnect(note);
    }
}
