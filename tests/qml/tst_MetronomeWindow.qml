// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The metronome window (`seabass --metronome`): what it says before a
// file is dropped on it, and what it says of a file it cannot read.
TestCase {
    id: testCase
    name: "MetronomeWindow"
    width: 200
    height: 200
    visible: true
    when: windowShown

    Component {
        id: windowComponent
        MetronomeWindow {}
    }

    // A path as a file: address, on Windows too ("file://" + "C:/..."
    // would name a computer called c).
    function fileUrl(path) {
        return path.startsWith("/") ? "file://" + path : "file:///" + path;
    }

    function test_itAsksForAFileAndShowsNoControlsWithoutOne() {
        const metronome = createTemporaryObject(windowComponent, testCase);
        verify(metronome !== null, "the window must instantiate");
        tryCompare(metronome, "visible", true);
        compare(findChild(metronome, "metronomeTitle").text, "Drop a music file on me");
        compare(findChild(metronome, "metronomeSummary").text, "I jump on every beat Seabass finds");
        // Nothing to play, so nothing to play it with: the strips and
        // the buttons come with a track.
        compare(findChild(metronome, "metronomeLower").visible, false);
        compare(findChild(metronome, "metronomeTiming").visible, false);
        const track = findChild(metronome, "metronomeTrack");
        compare(track.beatCount, 0);
        compare(track.phaseAt(1000), -1);
        compare(track.levels(0, 1000, 4).length, 4);
    }

    function test_aFileThatIsNoMusicIsSaidSoAndTheRobotStays() {
        const metronome = createTemporaryObject(windowComponent, testCase, {
            // This test's own source: a file that is there and is no audio.
            firstFile: Qt.resolvedUrl("tst_MetronomeWindow.qml")
        });
        verify(metronome !== null);
        const track = findChild(metronome, "metronomeTrack");
        // Named at once, while it is listened to.
        compare(track.title, "tst_MetronomeWindow");
        tryCompare(track, "busy", false, 30000);
        // This error and no other: "no decoder on this computer" would
        // be a lane that tests nothing here.
        compare(track.error, "This file could not be decoded");
        compare(findChild(metronome, "metronomeSummary").text, track.error);
        compare(track.beatCount, 0);
        compare(findChild(metronome, "metronomeLower").visible, false);
        verify(findChild(metronome, "metronomeRobot") !== null);
    }

    // The whole of it on a file with a beat: found, summed up, timed,
    // and what the robot and the strips ask is answered.
    function test_aFileWithABeatIsAnalysedAndItsBeatsAreThere() {
        const file = stickFixture.scratchDirectory() + "/metronome/two beats a second.wav";
        verify(artworkFixture.writeBeatWav(file, 24, 120));
        const metronome = createTemporaryObject(windowComponent, testCase, { firstFile: fileUrl(file) });
        verify(metronome !== null);
        const track = findChild(metronome, "metronomeTrack");
        tryCompare(track, "busy", false, 60000);
        compare(track.error, "");
        compare(track.title, "two beats a second");
        // 24 s at 120 BPM from 0.25 s: 48 beats, give or take the ends.
        verify(track.beatCount >= 44 && track.beatCount <= 50, "beats: " + track.beatCount);
        verify(track.summary.indexOf("120.0") === 0 || track.summary.indexOf("119.9") === 0, track.summary);
        verify(track.summary.indexOf("steady tempo") > 0, track.summary);
        verify(/^Analysed in \d+\.\d\d s, \d+\.\d\d s with decoding$/.test(track.timing), track.timing);
        compare(findChild(metronome, "metronomeTiming").text, track.timing);
        compare(findChild(metronome, "metronomeTitle").text, "two beats a second");
        compare(findChild(metronome, "metronomeLower").visible, true);
        verify(Math.abs(track.lengthMs - 24000) < 200, "length: " + track.lengthMs);
        // The beats lie half a second apart, on the thumps (the first at
        // 250 ms), and each is handed out with its number.
        const beats = track.beatsBetween(5000, 7000);
        compare(beats.length, 8, "four beats in two seconds, each with its number: " + beats);
        for (let i = 0; i < beats.length; i += 2) {
            const off = ((beats[i] - 250) % 500 + 500) % 500;
            verify(Math.min(off, 500 - off) < 25, "a beat " + off + " ms off the thump: " + beats);
            compare(track.beatAt(beats[i] + 1), beats[i + 1]);
        }
        compare(beats[2] - beats[0] > 480 && beats[2] - beats[0] < 520, true);
        const phase = track.phaseAt(beats[0] + (beats[2] - beats[0]) / 4);
        verify(Math.abs(phase - 0.25) < 0.01, "a quarter of the way to the next beat: " + phase);
        // Loud where a thump is, quiet between two.
        const loud = track.levels(beats[0] - 20, beats[0] + 60, 1)[0];
        const quiet = track.levels(beats[0] + 300, beats[0] + 400, 1)[0];
        verify(loud > 0.3 && quiet < loud / 4, "thump " + loud + ", between " + quiet);
    }
}
