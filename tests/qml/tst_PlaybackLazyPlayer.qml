// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// gh#96: the PlaybackController makes its QMediaPlayer the first time a
// track is loaded, not with itself. Main.qml makes the controller before
// the first window, and making a player loads Qt's media backend: on
// Windows the FFmpeg plugin and 19 DLLs behind it, which Defender scans
// the first time after an install. Until a track is loaded the controller
// answers for a player that has nothing loaded, and the transport does
// nothing.
TestCase {
    id: testCase
    name: "PlaybackLazyPlayer"
    when: windowShown
    visible: true
    width: 100
    height: 100

    // The "audio" is this file: it exists, which is all loading asks.
    readonly property string aFile: Qt.resolvedUrl("tst_PlaybackLazyPlayer.qml").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    Component {
        id: controllerComponent
        PlaybackController {}
    }

    function players(controller) {
        return stickFixture.childrenInheriting(controller, "QMediaPlayer");
    }

    // What a player brings with it. The buffer output that meters the
    // levels was always the controller's child, made with the player, so
    // it shows a player made too early even where the player itself was
    // not a child (a plain member, before gh#96).
    function playerParts(controller) {
        return players(controller)
            + stickFixture.childrenInheriting(controller, "QAudioOutput")
            + stickFixture.childrenInheriting(controller, "QAudioBufferOutput");
    }

    function test_thereIsNoPlayerUntilATrackIsLoaded() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        compare(playerParts(controller), 0, "a new controller has made a player");
        compare(controller.hasTrack, false);
        compare(controller.duration, 0);
        compare(controller.position, 0);
        compare(controller.playing, false);

        // The whole transport with nothing loaded: nothing happens, and
        // none of it makes a player.
        controller.play();
        controller.togglePlay();
        controller.pause();
        controller.seek(1000);
        controller.skipBeats(4);
        compare(controller.jumpToHotCue(1), false);
        controller.next();
        controller.previous();
        controller.libraryBusy = true;
        controller.libraryBusy = false;
        controller.stop();
        compare(controller.playing, false);
        compare(controller.position, 0);
        compare(playerParts(controller), 0, "the transport made a player with nothing to play");
    }

    function test_theVolumeSetBeforeAPlayerIsKept() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const spy = createTemporaryObject(signalSpyComponent, testCase, {target: controller, signalName: "volumeChanged"});
        compare(controller.volume, 1.0);
        controller.volume = 0.3;
        fuzzyCompare(controller.volume, 0.3, 0.001);
        compare(spy.count, 1);
        controller.volume = 0.3;
        compare(spy.count, 1, "the same volume again is no change");
        controller.volume = 4;
        compare(controller.volume, 1.0, "the volume is 0 to 1");
        controller.volume = 0.3;
        compare(playerParts(controller), 0, "setting the volume made a player");

        controller.loadFile("engine", "/stick/Engine Library", aFile, "Session-0001.wav", "Sessions");
        compare(players(controller), 1);
        fuzzyCompare(controller.volume, 0.3, 0.001);
        controller.volume = 0.6;
        fuzzyCompare(controller.volume, 0.6, 0.001);
    }

    function test_loadingMakesOnePlayerAndKeepsIt_data() {
        return [
            {tag: "load", plain: false},
            {tag: "loadFile", plain: true},
        ];
    }

    function test_loadingMakesOnePlayerAndKeepsIt(data) {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const load = () => data.plain
            ? controller.loadFile("engine", "/stick/Engine Library", aFile, "Session-0001.wav", "Sessions")
            : controller.load("rekordbox", "/lib", "a", aFile, "Track a", "Artist", "", []);
        load();
        compare(controller.hasTrack, true);
        compare(players(controller), 1);
        load();
        compare(players(controller), 1, "a second track made a second player");
        controller.stop();
        compare(controller.hasTrack, false);
        compare(players(controller), 1, "stop() let go of the player");
    }

    // Nothing to play, so nothing to make a player for: the track is
    // loaded, says what is wrong, and the backend stays where it is.
    function test_aFileThatIsNotThereMakesNoPlayer() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        controller.loadFile("engine", "/stick/Engine Library", "/nonexistent/Session-0009.wav", "Session-0009.wav", "Sessions");
        compare(controller.hasTrack, true);
        verify(controller.errorMessage.length > 0);
        compare(playerParts(controller), 0);
    }

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }
}
