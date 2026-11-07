// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The player puts an MP3's cues on the timeline it plays on (#50): the
// cues a page hands PlaybackController.load() and takeCues() are on
// rekordbox's, and a LAME file at 44.1 kHz plays 51.179 ms earlier than
// rekordbox counts. A rekordbox beat grid moves the same way; the
// arithmetic for an Engine one is playback_timeline_test's.
TestCase {
    id: testCase
    name: "PlaybackTimeline"

    // A class D click file from tests/fixtures/mp3-click, through the
    // source tree the suite runs from.
    readonly property string lameFile: Qt.resolvedUrl("../fixtures/mp3-click/click-D-lame-44100.mp3").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")
    readonly property string notAnMp3: Qt.resolvedUrl("tst_PlaybackTimeline.qml").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")
    readonly property real lameShiftMs: -2257 * 1000 / 44100

    PlaybackController { id: player }
    AppSettingsController { id: settings }
    SignalSpy { id: seeked; target: player; signalName: "seeked" }

    readonly property var cues: [
        {kind: "hot", hotCueNumber: 1, positionMs: 5051.179, isLoop: false},
        {kind: "hot", hotCueNumber: 2, positionMs: 64000, isLoop: true, loopEndMs: 68000},
    ]

    function init() {
        settings.correctMp3CueTiming = true;
        player.stop();
        seeked.clear();
    }

    function cleanup() {
        settings.correctMp3CueTiming = true;
        player.stop();
    }

    function test_aRekordboxCueIsMovedOntoThePlaybackTimeline() {
        player.load("rekordbox", "/lib", "a", lameFile, "Click", "Artist", "", cues);
        fuzzyCompare(player.cues[0].positionMs, 5051.179 + lameShiftMs, 0.001);
        fuzzyCompare(player.cues[1].positionMs, 64000 + lameShiftMs, 0.001);
        fuzzyCompare(player.cues[1].loopEndMs, 68000 + lameShiftMs, 0.001, "a loop's end moves with it");
        verify(player.jumpToHotCue(1));
        compare(seeked.signalArguments[0][0], 5000, "the pad jumps to the sound, in whole milliseconds");
    }

    function test_anEngineCueIsMovedTheSameWay() {
        // Engine cues reach the pages on rekordbox's timeline too
        // (engine_cue_timeline.hpp), so the same shift takes them back.
        player.load("engine", "/lib", "1", lameFile, "Click", "Artist", "", cues);
        fuzzyCompare(player.cues[0].positionMs, 5051.179 + lameShiftMs, 0.001);
    }

    function test_cuesTakenLaterAreMovedToo() {
        player.load("rekordbox", "/lib", "a", lameFile, "Click", "Artist", "", []);
        compare(player.cues.length, 0);
        verify(player.takeCues("rekordbox", "/lib", "a", cues));
        fuzzyCompare(player.cues[0].positionMs, 5051.179 + lameShiftMs, 0.001);
        verify(player.takeCues("rekordbox", "/lib", "a", cues), "the same list again is still taken");
    }

    function test_withTheCorrectionOffNothingMoves() {
        settings.correctMp3CueTiming = false;
        player.load("rekordbox", "/lib", "a", lameFile, "Click", "Artist", "", cues);
        compare(player.cues[0].positionMs, 5051.179);
        compare(player.cues[1].loopEndMs, 68000);
    }

    // The one stick with two catalogs: its rekordbox track 7 carries a
    // beat grid, read here with the click file standing in as its audio.
    readonly property string rekordboxRoot: Qt.resolvedUrl("../fixtures/one_stick_two_catalogs/rekordbox").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    function test_aRekordboxBeatGridIsMovedWithTheCues() {
        settings.correctMp3CueTiming = false;
        player.load("rekordbox", rekordboxRoot, "7", lameFile, "Click", "Artist", "", []);
        // A copy: a sequence read from a Q_PROPERTY is live and would follow the next load.
        const raw = Array.from(player.beatTimesMs);
        verify(raw.length > 8, "the fixture's track 7 has a grid: " + raw.length + " beats");
        settings.correctMp3CueTiming = true;
        player.load("rekordbox", rekordboxRoot, "7", lameFile, "Click", "Artist", "", []);
        compare(player.beatTimesMs.length, raw.length);
        fuzzyCompare(player.beatTimesMs[0], raw[0] + lameShiftMs, 0.001, "the first beat moves with the cues");
        fuzzyCompare(player.beatTimesMs[8], raw[8] + lameShiftMs, 0.001);
        compare(player.beatNumbers.length, raw.length, "the bar positions are untouched");
    }

    function test_aFileThatIsNotAnMp3IsLeftAlone() {
        player.load("rekordbox", "/lib", "a", notAnMp3, "Not audio", "Artist", "", cues);
        compare(player.cues[0].positionMs, 5051.179);
    }

    // Only a loop's end moves: a cue that is not a loop keeps whatever
    // loopEndMs it came with (0, or a leftover). And nothing moves before
    // the start of the file: a cue inside the lead-in lands at 0.
    function test_onlyALoopsEndMovesAndNothingGoesBelowZero() {
        const given = [
            {kind: "hot", hotCueNumber: 1, positionMs: 5051.179, isLoop: false, loopEndMs: 0},
            {kind: "memory", hotCueNumber: 0, positionMs: 30, isLoop: false, loopEndMs: 0},
            {kind: "hot", hotCueNumber: 3, positionMs: 10, isLoop: true, loopEndMs: 40},
            {kind: "hot", hotCueNumber: 4, positionMs: 20, isLoop: true, loopEndMs: 5000},
        ];
        player.load("rekordbox", "/lib", "a", lameFile, "Click", "Artist", "", given);
        compare(player.cues[0].loopEndMs, 0, "a cue that is not a loop keeps its loopEndMs");
        compare(player.cues[1].positionMs, 0, "a cue inside the lead-in lands at the start, not before it");
        compare(player.cues[1].loopEndMs, 0);
        compare(player.cues[2].positionMs, 0, "a loop inside the lead-in starts at 0");
        compare(player.cues[2].loopEndMs, 0, "and ends there");
        compare(player.cues[3].positionMs, 0, "a loop that starts inside the lead-in starts at 0");
        fuzzyCompare(player.cues[3].loopEndMs, 5000 + lameShiftMs, 0.001, "and keeps its end");
    }

    function test_aMissingFileKeepsTheCuesAsGiven() {
        player.load("rekordbox", "/lib", "a", "/nonexistent/none.mp3", "Gone", "Artist", "", cues);
        compare(player.cues[0].positionMs, 5051.179);
    }
}
