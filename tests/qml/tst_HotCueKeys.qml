// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Keys 1 to 8 jump to that hot cue: PlaybackController.jumpToHotCue(), and
// the shortcuts in common/HotCueShortcuts.qml that Main.qml holds.
TestCase {
    id: testCase
    name: "HotCueKeys"
    when: windowShown
    visible: true
    width: 200
    height: 100

    readonly property string aFile: Qt.resolvedUrl("tst_HotCueKeys.qml").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    PlaybackController { id: player }
    SignalSpy { id: seeked; target: player; signalName: "seeked" }

    // Hot cue 1 and 3, a memory cue in between that carries number 0, and a
    // hot loop on 5: a loop's start is where its pad goes.
    readonly property var cues: [
        {kind: "hot", hotCueNumber: 1, positionMs: 1000, isLoop: false},
        {kind: "memory", hotCueNumber: 0, positionMs: 500, isLoop: false},
        {kind: "hot", hotCueNumber: 3, positionMs: 30500.6, isLoop: false},
        {kind: "hot", hotCueNumber: 5, positionMs: 64000, isLoop: true, loopEndMs: 68000},
    ]

    // A stand-in for the controller, so the shortcuts can be pressed
    // without a track to play.
    QtObject {
        id: fakePlayer
        property bool hasTrack: true
        // What the fullscreen ring reads besides.
        property string title: "Rej"
        property bool playing: false
        property bool hasNext: false
        property bool hasPrevious: false
        property var jumps: []
        function jumpToHotCue(number) { jumps = jumps.concat([number]); return true; }
    }
    HotCueShortcuts {
        id: shortcuts
        playbackController: fakePlayer
    }
    TextField { id: field; width: 100 }

    Component {
        id: fullscreenComponent
        TrackRingFullscreen {}
    }

    function init() {
        player.stop();
        seeked.clear();
        fakePlayer.hasTrack = true;
        fakePlayer.jumps = [];
        shortcuts.active = true;
        testCase.forceActiveFocus();
    }

    function load() {
        player.load("rekordbox", "/lib", "a", aFile, "Track a", "Artist", "", cues);
    }

    function test_jumpsToTheHotCueWithThatNumber() {
        load();
        verify(player.jumpToHotCue(3));
        compare(seeked.count, 1);
        compare(seeked.signalArguments[0][0], 30500, "to the cue, in whole milliseconds");
        verify(player.jumpToHotCue(1));
        compare(seeked.signalArguments[1][0], 1000);
        verify(player.jumpToHotCue(5), "a hot loop is a hot cue too");
        compare(seeked.signalArguments[2][0], 64000);
    }

    function test_aMissingCueLeavesThePlayheadAlone() {
        load();
        verify(!player.jumpToHotCue(2), "there is no hot cue 2");
        verify(!player.jumpToHotCue(8));
        verify(!player.jumpToHotCue(0), "the memory cue's 0 is not a hot cue number");
        compare(seeked.count, 0);
    }

    function test_withNothingLoadedThereIsNothingToJumpTo() {
        verify(!player.jumpToHotCue(1));
        compare(seeked.count, 0);
    }

    function test_theNumberKeysPressThePads() {
        keyClick(Qt.Key_3);
        keyClick(Qt.Key_1);
        keyClick(Qt.Key_8);
        compare(fakePlayer.jumps, [3, 1, 8]);
    }

    function test_theKeypadWorksToo() {
        keyClick(Qt.Key_5, Qt.KeypadModifier);
        compare(fakePlayer.jumps, [5]);
    }

    function test_nineAndZeroAreNotPads() {
        keyClick(Qt.Key_9);
        keyClick(Qt.Key_0);
        compare(fakePlayer.jumps, []);
    }

    function test_quietWithoutATrackAndWhileTyping() {
        fakePlayer.hasTrack = false;
        keyClick(Qt.Key_2);
        compare(fakePlayer.jumps, [], "no track: nothing to jump in");
        fakePlayer.hasTrack = true;
        shortcuts.active = false;
        keyClick(Qt.Key_2);
        compare(fakePlayer.jumps, [], "typing: a digit is a digit");
    }

    // The fullscreen ring has a key handler of its own, not these
    // shortcuts: the same pads, through keyPressed().
    function test_theFullscreenRingHasThePadsToo() {
        var ring = createTemporaryObject(fullscreenComponent, testCase, {playbackController: fakePlayer});
        verify(ring);
        verify(ring.keyPressed(Qt.Key_4, false));
        verify(ring.keyPressed(Qt.Key_7, true), "held down, it is still taken");
        verify(!ring.keyPressed(Qt.Key_9, false), "9 is not a pad");
        compare(fakePlayer.jumps, [4], "and a held key jumps once, not over and over");
    }
}
