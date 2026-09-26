// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtTest
import SeabassGui

// Browse Library's track details pane where the ring can be drawn: the
// loaded track gets it, any other track keeps its sleeve.
TestCase {
    id: testCase
    name: "TrackDetailPanelRing"
    width: 460
    height: 760
    visible: true
    when: windowShown

    property var seeks: []
    property int toggles: 0
    property var calls: []
    property var player: ({
        waveformFor: function() {
            var columns = [];
            for (var i = 0; i < 400; ++i) columns.push({low: 0.8, mid: 0.4, high: 0.1});
            return columns;
        },
        seek: function(ms) { testCase.seeks.push(ms); },
        togglePlay: function() { testCase.toggles += 1; testCase.calls.push("togglePlay"); },
        skipBeats: function(beats) { testCase.calls.push("skip " + beats); },
        next: function() { testCase.calls.push("next"); },
        previous: function() { testCase.calls.push("previous"); },
        pause: function() { testCase.calls.push("pause"); },
        stop: function() { testCase.calls.push("stop"); },
        hasNext: true, hasPrevious: false,
        hasTrack: true, currentFormat: "engine", currentSourceId: "42", currentLibraryPath: "/media/MAIN/Engine Library",
        errorMessage: "",
        title: "Major Tom (Reworked 2024)", artist: "DJ Amador", artworkPath: "",
        waveform: [{low: 0.8, mid: 0.4, high: 0.1}, {low: 0.2, mid: 0.1, high: 0.0}],
        cues: [{positionMs: 32000, color: "#ffcc00"}],
        playing: true, position: 93000, duration: 372000,
    })

    Component {
        id: stageComponent
        Rectangle {
            width: 460; height: 760
            color: Theme.background
            property alias panel: panelItem
            TrackDetailPanel {
                id: panelItem
                anchors.fill: parent
                addCueController: ({statusMessage: "", errorMessage: "", busy: false, addCue: function() {}})
                playbackController: testCase.player
                format: "engine"
                libraryPath: "/media/MAIN/Engine Library"
            }
        }
    }

    function track(sourceId) {
        return {sourceId: sourceId, title: "Major Tom (Reworked 2024)", artist: "DJ Amador",
                cues: [{kind: "hot", hotCueNumber: 1, positionMs: 32000, isLoop: false, loopEndMs: 0, color: "#ffcc00", comment: ""}],
                durationSeconds: 372, playlistNames: [], streamingSource: "", rating: 4, bpm: 126.5, key: "Fm",
                bitrate: 320, playCount: 17, artworkPath: "file://" + browseFixture.presentArtwork(), comment: "", album: "",
                filePath: "/music/major-tom.mp3"};
    }

    function test_theLoadedTrackIsShownAsARing() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var panel = stage.panel;
        panel.showFor(track("42"));
        var row = findChild(panel, "trackRingRow");
        var ring = findChild(panel, "trackRing");
        verify(ring !== null && ring.available, "this suite exists to run the shader");
        compare(row.showsRing, true);
        tryVerify(function() { return row.side > 200 && ring.width === row.side; }, 2000, "the row opens to the ring's size");
        compare(findChild(panel, "trackArtwork").visible, false, "the ring carries the cover now");
        fuzzyCompare(ring.progress, 0.25, 0.0001);
        compare(ring.cueData.length, 1);
        compare(ring.waveformData.length, 2, "the ring draws what the player has loaded");

        if (screenshotDir && screenshotDir.length > 0) {
            wait(400);
            grabImage(stage).save(screenshotDir + "/track-panel-ring.png");
        }
    }

    // A click on the ring puts it alone on the whole screen, in a window
    // of its own; a click there goes back. The app's own window is not
    // touched: an earlier version took IT fullscreen and back, which is
    // a good way to lose the size and place the user gave it.
    function test_aClickOnTheRingOpensAFullscreenWindowAndAClickThereClosesIt() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var panel = stage.panel;
        panel.showFor(track("42"));
        var ring = findChild(panel, "trackRing");
        var host = testCase.Window.window;
        var hostVisibility = host.visibility;
        var hostWidth = host.width;
        var overlay = findChild(panel, "ringFullscreenWindow");
        verify(overlay !== null);
        compare(overlay.visible, false);
        tryVerify(function() { return ring.width > 200; }, 2000);

        mouseClick(ring, ring.width / 2, ring.height / 2);
        tryCompare(overlay, "visible", true, 2000, "the fullscreen window opens");
        compare(overlay.visibility, Window.FullScreen);
        verify(overlay !== host, "and it is not the app's window");
        compare(host.visibility, hostVisibility, "which is left as it was");
        compare(host.width, hostWidth);

        var big = findChild(overlay.contentItem, "fullscreenRing");
        verify(big !== null);
        tryVerify(function() { return big.width > ring.width; }, 2000, "the ring there is the larger of the two");
        // The title is there with the controls, and nothing else is:
        // the cover says what is playing once they have gone.
        compare(findChild(overlay.contentItem, "fullscreenTitle").text, "Major Tom (Reworked 2024)");
        compare(findChild(overlay.contentItem, "fullscreenArtist"), null, "no artist under it");
        verify(big.width > Math.min(overlay.width, overlay.height) * 0.9, "so the ring has the window: " + big.width + " of "
               + Math.min(overlay.width, overlay.height));
        fuzzyCompare(big.y + big.height / 2, overlay.height / 2, 1, "in the middle of it");
        fuzzyCompare(big.progress, 0.25, 0.0001);
        compare(big.waveformData.length, 2, "it draws what the PLAYER has loaded");

        if (screenshotDir && screenshotDir.length > 0) {
            wait(500);
            grabImage(overlay.contentItem).save(screenshotDir + "/track-ring-fullscreen.png");
        }

        mouseClick(big, big.width / 2, big.height / 2);
        tryCompare(overlay, "visible", false, 2000, "a click there closes it");
        compare(host.visibility, hostVisibility);
    }

    function openFullscreen() {
        var stage = createTemporaryObject(stageComponent, testCase);
        stage.panel.showFor(track("42"));
        var ring = findChild(stage.panel, "trackRing");
        tryVerify(function() { return ring.width > 200; }, 2000);
        mouseClick(ring, ring.width / 2, ring.height / 2);
        var overlay = findChild(stage.panel, "ringFullscreenWindow");
        tryCompare(overlay, "visible", true, 2000);
        testCase.calls = [];
        return overlay;
    }

    // Keys go through the window's one keyPressed(), which is what is
    // pressed here: a test cannot give another window the keyboard focus
    // where there is no window manager to grant it.
    function test_theKeysInFullscreen_data() {
        return [
            {tag: "right skips a bar on", key: Qt.Key_Right, expected: ["skip 4"]},
            {tag: "left skips a bar back", key: Qt.Key_Left, expected: ["skip -4"]},
            {tag: "up is the previous track", key: Qt.Key_Up, expected: ["previous"]},
            {tag: "down is the next track", key: Qt.Key_Down, expected: ["next"]},
            {tag: "space plays and pauses", key: Qt.Key_Space, expected: ["togglePlay"]},
            {tag: "the media play key", key: Qt.Key_MediaTogglePlayPause, expected: ["togglePlay"]},
            {tag: "the media next key", key: Qt.Key_MediaNext, expected: ["next"]},
            {tag: "the media previous key", key: Qt.Key_MediaPrevious, expected: ["previous"]},
            {tag: "the media stop key", key: Qt.Key_MediaStop, expected: ["stop"]},
            // Held down, down would run through the whole list.
            {tag: "down held is not next again", key: Qt.Key_Down, repeat: true, expected: []},
            {tag: "right held keeps skipping", key: Qt.Key_Right, repeat: true, expected: ["skip 4"]},
            {tag: "a letter is not ours", key: Qt.Key_A, expected: [], unhandled: true},
        ];
    }
    function test_theKeysInFullscreen(data) {
        var overlay = openFullscreen();
        compare(overlay.keyPressed(data.key, data.repeat === true), data.unhandled !== true);
        compare(testCase.calls, data.expected);
        overlay.close();
    }

    function test_enterSetsTheCoverSpinningAndStopsItAgain() {
        var overlay = openFullscreen();
        var big = findChild(overlay.contentItem, "fullscreenRing");
        compare(big.spinning, false, "it does not spin until asked");
        overlay.keyPressed(Qt.Key_Return, false);
        compare(big.spinning, true);
        overlay.keyPressed(Qt.Key_Enter, false);
        compare(big.spinning, false, "the keypad's Enter is Enter too");
        compare(findChild(testCase.Window.window.contentItem, "trackRing").spinning, false, "the pane's ring never spins");
        overlay.close();
    }

    function test_escapeClosesFullscreen() {
        var overlay = openFullscreen();
        overlay.keyPressed(Qt.Key_Escape, false);
        tryCompare(overlay, "visible", false, 2000);
    }

    // A click anywhere closes the fullscreen ring -- except on a control,
    // which does what it says and leaves the ring up.
    function test_theControlsWorkAndDoNotCloseTheRing() {
        var overlay = openFullscreen();
        overlay.showControls();
        var next = findChild(overlay.contentItem, "fullscreenNext");
        var previous = findChild(overlay.contentItem, "fullscreenPrevious");
        tryVerify(function() { return findChild(overlay.contentItem, "fullscreenTransport").opacity === 1; }, 2000);
        compare(next.enabled, true);
        compare(previous.enabled, false, "there is no previous track, and the button says so");
        mouseClick(next);
        mouseClick(findChild(overlay.contentItem, "fullscreenForward"));
        mouseClick(findChild(overlay.contentItem, "fullscreenBack"));
        mouseClick(findChild(overlay.contentItem, "fullscreenPlay"));
        compare(testCase.calls, ["next", "skip 4", "skip -4", "togglePlay"]);
        compare(overlay.visible, true, "and the ring is still up");
        overlay.close();
    }

    // The title and the controls are one overlay: they come and go
    // together, on one animation, and it takes its time. An earlier
    // version faded the controls over Theme.shortTransitionDuration,
    // which over a picture reads as a switch rather than a fade.
    function test_theTitleAndTheControlsFadeTogetherAndSlowly() {
        var overlay = openFullscreen();
        var title = findChild(overlay.contentItem, "fullscreenTitle");
        var controls = findChild(overlay.contentItem, "fullscreenTransport");
        overlay.showControls();
        tryVerify(function() { return overlay.controlsOpacity === 1; }, 2000, "both are up");
        compare(title.opacity, controls.opacity, "on one animation, never a frame apart");
        verify(title.visible && controls.visible);

        // How long the fade takes, measured rather than waited out: a
        // wait() of part of it only promises to be no shorter than it
        // asks for, so on a loaded machine it can land past the end and
        // fail a fade that was fine. The clock here is a lower bound,
        // which overshooting only makes safer.
        var startedFading = Date.now();
        overlay.controlsShown = false;
        compare(title.opacity, controls.opacity, "and they leave together");
        tryVerify(function() { return overlay.controlsOpacity === 0; }, Theme.fadeTransitionDuration * 10, "and they go");
        var fadeTook = Date.now() - startedFading;
        verify(fadeTook >= Theme.fadeTransitionDuration / 2,
               "a fade, not a switch: it took " + fadeTook + " ms of " + Theme.fadeTransitionDuration);
        compare(title.visible, false, "gone, the title takes no clicks");
        compare(controls.visible, false);

        // The pointer brings them back, together.
        overlay.showControls();
        tryVerify(function() { return overlay.controlsOpacity === 1; }, Theme.fadeTransitionDuration * 3);
        compare(title.opacity, controls.opacity);
        overlay.close();
    }

    // A track that failed to load keeps its sleeve: the ring would hide
    // it, and the sleeve is the pane's way to try playing it again.
    function test_aTrackThatFailedToLoadKeepsItsSleeve() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var failed = {};
        for (var key in testCase.player) failed[key] = testCase.player[key];
        failed.errorMessage = "audio file not found: /music/major-tom.mp3";
        stage.panel.playbackController = failed;
        stage.panel.showFor(track("42"));
        compare(stage.panel.isLoadedTrack, true);
        compare(findChild(stage.panel, "trackRingRow").showsRing, false);
        compare(findChild(stage.panel, "trackArtwork").visible, true);
    }

    // Fullscreen's "click anywhere to go back" includes the corners of
    // the ring's square, which the ring used to take and then ignore.
    function test_aClickBesideTheRoundRingClosesFullscreenToo() {
        var overlay = openFullscreen();
        var big = findChild(overlay.contentItem, "fullscreenRing");
        tryVerify(function() { return big.width > 300; }, 2000);
        mouseClick(big, 6, 6);
        tryCompare(overlay, "visible", false, 2000);
    }

    // The ring's clock costs a redraw a frame, so it runs only while
    // something moves: not in the fullscreen window while it is shut, and
    // not for a paused platter resting on its brake.
    function test_theClockStopsWhenNothingMoves() {
        var stage = createTemporaryObject(stageComponent, testCase);
        stage.panel.showFor(track("42"));
        var overlay = findChild(stage.panel, "ringFullscreenWindow");
        var big = findChild(overlay.contentItem, "fullscreenRing");
        var clock = findChild(big, "ringClock");
        compare(testCase.player.playing, true);
        compare(clock.running, false, "shut, the fullscreen ring runs nothing though a track plays");

        var ring = findChild(stage.panel, "trackRing");
        tryVerify(function() { return ring.width > 200; }, 2000);
        mouseClick(ring, ring.width / 2, ring.height / 2);
        tryCompare(overlay, "visible", true, 2000);
        tryCompare(clock, "running", true, 2000, "open, it runs");

        // Spinning, then paused: the brake stops the platter askew and it waits.
        overlay.keyPressed(Qt.Key_Return, false);
        wait(900);
        var paused = {};
        for (var key in testCase.player) paused[key] = testCase.player[key];
        paused.playing = false;
        stage.panel.playbackController = paused;
        overlay.playbackController = paused;
        tryCompare(big, "platterSpeed", 0, 3000);
        verify(big.artTurns !== 0, "the brake left it askew");
        tryCompare(clock, "running", false, 1000, "and a platter waiting on its brake runs nothing");
        overlay.close();
    }

    function test_anyOtherTrackKeepsItsSleeve() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var panel = stage.panel;
        panel.showFor(track("7"));
        compare(findChild(panel, "trackRing"), null);
        compare(findChild(panel, "trackRingRow").visible, false);
        compare(findChild(panel, "trackArtwork").visible, true);
    }
}
