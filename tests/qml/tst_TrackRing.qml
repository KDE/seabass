// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// TrackRing's own reckoning: what it takes from the waveform and which
// cover file it asks for. What the shader then draws
// is tests/qml-shader's business -- this suite runs on the offscreen
// platform, where no shader draws anything.
TestCase {
    id: testCase
    name: "TrackRing"
    width: 400
    height: 400
    visible: true
    when: windowShown

    Component {
        id: ringComponent
        TrackRing { width: 400; height: 400 }
    }

    function make(properties) {
        var ring = createTemporaryObject(ringComponent, testCase, properties || {});
        verify(ring !== null, "the ring did not instantiate");
        return ring;
    }

    // The suite's platform has the software scene graph. There the ring
    // must say so, and draw nothing, so its caller keeps the plain art.
    function test_itIsUnavailableWhereNoShaderCanRun() {
        var ring = make({cueData: [{positionMs: 1000, color: "#ff0000"}], trackDurationMs: 4000});
        compare(ring.available, false, "the offscreen platform cannot run a shader");
        compare(findChild(ring, "ringShader").visible, false);
        compare(findChild(ring, "ringCueMark"), null, "cue marks belong to a ring that is drawn");
    }

    function test_theBassIsTheLowBandUnderThePlayhead() {
        var waveform = [{low: 0.1, mid: 0, high: 0}, {low: 0.9, mid: 0, high: 0},
                        {low: 0.4, mid: 0, high: 0}, {low: 0.7, mid: 0, high: 0}];
        var ring = make({waveformData: waveform, playing: true, progress: 0.30});
        compare(ring.bass, 0.9, "30% of four columns is the second");
        ring.progress = 0.80;
        compare(ring.bass, 0.7);
        ring.progress = 1.0;
        compare(ring.bass, 0.7, "the very end is still the last column, not one past it");
        ring.playing = false;
        compare(ring.bass, 0, "a paused track holds still");
        ring.playing = true;
        ring.progress = -1;
        compare(ring.bass, 0, "no playhead, no bass");
    }

    // An older rekordbox preview is one number per column, not three.
    function test_aSingleBandWaveformCountsAsItsOwnBass() {
        var ring = make({waveformData: [0.2, 0.6], playing: true, progress: 0.75});
        compare(ring.bass, 0.6);
    }

    // Where the player can measure the audio, that is what the ring
    // moves to: it has the beat in it, and the waveform has not.
    function test_liveLevelsWinOverTheWaveform() {
        var ring = make({waveformData: [{low: 0.1, mid: 0.2, high: 0.3}], playing: true, progress: 0.5,
                         liveLevels: true, liveLow: 0.9, liveMid: 0.6, liveHigh: 0.4});
        compare(ring.bass, 0.9);
        compare(ring.mid, 0.6);
        compare(ring.high, 0.4);
        ring.liveLevels = false;
        compare(ring.bass, 0.1, "without them, the column under the playhead");
        compare(ring.mid, 0.2);
        compare(ring.high, 0.3);
        ring.liveLevels = true;
        ring.playing = false;
        compare(ring.bass + ring.mid + ring.high, 0, "and a paused track holds still either way");
    }

    function test_aBeatStartsARippleOnlyWhilePlaying() {
        var ring = make({playing: true, animated: false, time: 12.5});
        verify(ring.rippleAge > 1, "no beat yet, no ripple: its age is past the shader's one second");
        ring.beatCount = 1;
        compare(ring.rippleAge, 0, "a beat starts one");
        ring.time = 12.8;
        fuzzyCompare(ring.rippleAge, 0.3, 0.0001);
        ring.beatCount = 2;
        compare(ring.rippleAge, 0, "and the next beat the next");

        ring.time = 20;
        ring.playing = false;
        ring.beatCount = 3;
        verify(ring.rippleAge > 1, "a beat that arrives after the pause starts nothing");
    }

    // ---- keeping time by the beat grid ----

    function gridRing(extra) {
        var times = [];
        var numbers = [];
        for (var i = 0; i < 64; ++i) {
            times.push(1000 + i * 500);      // 120 BPM from one second in
            numbers.push(i % 4 + 1);
        }
        var properties = {playing: true, animated: false, visualLeadMs: 0, beatTimesMs: times, beatNumbers: numbers};
        for (var key in extra) {
            properties[key] = extra[key];
        }
        return make(properties);
    }

    // With a grid the ripple is a function of the position and nothing
    // else: no beat has to be noticed, so none can be noticed late.
    function test_theGridSaysWhenTheBeatIs() {
        var ring = gridRing();
        compare(ring.hasBeatGrid, true);
        ring.positionMs = 400;
        verify(ring.rippleAge > 100, "before the first beat there is none to ripple from");
        ring.positionMs = 1000;
        compare(ring.rippleAge, 0, "on the first beat");
        compare(ring.beatInBar, 1);
        ring.positionMs = 2625;
        fuzzyCompare(ring.rippleAge, 0.125, 0.0001, "an eighth of a second after the fourth");
        compare(ring.beatInBar, 4);
        compare(ring.beatLengthMs, 500);
        fuzzyCompare(ring.rippleSpan, 0.425, 0.0001, "the ripple takes most of a beat to cross, whatever the tempo");
        ring.positionMs = 1000 + 63 * 500 + 5000;
        verify(ring.rippleAge > 100, "and the grid ends where it ends");

        // The player's own beat detector is not asked while there is a grid.
        ring.positionMs = 2625;
        ring.beatCount = 5;
        fuzzyCompare(ring.rippleAge, 0.125, 0.0001);
    }

    function test_theDownbeatRipplesHarder() {
        var ring = gridRing({liveLevels: true, liveLow: 1});
        ring.advance(0.1);                  // the music has been loud
        ring.positionMs = 1000;
        var downbeat = ring.rippleStrength;
        ring.positionMs = 1500;
        verify(ring.rippleStrength < downbeat * 0.7, "one is stronger than two: " + downbeat + " against " + ring.rippleStrength);
    }

    // The eye is behind the ear, so the display runs a little ahead.
    function test_theDisplayLeadsTheAudioALittle() {
        var ring = gridRing({visualLeadMs: 25});
        ring.positionMs = 1475;
        compare(ring.rippleAge, 0, "25 ms before the beat is heard, it is shown");
    }

    // The player reports its position in steps. Between them the ring
    // carries it forward itself, and takes a report as a nudge.
    function test_thePositionIsCarriedForwardBetweenReports() {
        var ring = gridRing();
        ring.positionMs = 2000;
        compare(ring.smoothPositionMs, 2000, "the first report, a jump, is taken as it is");
        ring.advance(0.1);
        fuzzyCompare(ring.smoothPositionMs, 2100, 0.001, "a tenth of a second later it is a tenth of a second on");
        fuzzyCompare(ring.rippleAge, 0.1, 0.0001, "and so is the beat");
        ring.positionMs = 2110;
        fuzzyCompare(ring.smoothPositionMs, 2101.5, 0.001, "a report 10 ms off is a nudge, not a jump");
        ring.positionMs = 9000;
        compare(ring.smoothPositionMs, 9000, "a seek is a jump");

        ring.playing = false;
        ring.advance(0.5);
        compare(ring.smoothPositionMs, 9000, "and paused, nothing is carried anywhere");
    }

    // The grid says when; the audio says how hard. A breakdown has a grid
    // all the same and must not pulse as though it had a kick.
    function test_thePulseIsAsStrongAsTheMusicIs() {
        var ring = gridRing({liveLevels: true, liveLow: 1});
        ring.advance(0.1);
        ring.positionMs = 5000;                       // on a beat
        verify(ring.bass > 0.95, "in a drop the beat lands at full strength, got " + ring.bass);
        ring.positionMs = 5250;                       // halfway to the next
        verify(ring.bass < 0.25, "and has died away by half way, got " + ring.bass);

        ring.liveLow = 0;                             // the breakdown
        ring.advance(2.0);
        ring.positionMs = 8000;                       // on a beat again
        verify(ring.bass < 0.05, "with no bass in the music the beat barely shows, got " + ring.bass);
        verify(ring.rippleStrength < 0.3, "nor does its ripple, got " + ring.rippleStrength);
    }

    function test_rekordboxArtAsksForTheLargeCoverAndSettlesForTheSmall() {
        var small = "file:///nowhere/PIONEER/Artwork/00001/a16.jpg";
        var ring = make({artworkSource: small});
        compare(ring.largeArtworkSource, "file:///nowhere/PIONEER/Artwork/00001/a16_m.jpg");
        var art = findChild(ring, "ringArtwork");
        // Neither file exists; what matters is that it gave up on the
        // large one and asked for the one the library named.
        tryCompare(ring, "largeArtworkMissing", true);
        compare(art.source.toString(), small);
    }

    function test_artFromAnywhereElseIsAskedForAsItIs() {
        var ring = make({artworkSource: "file:///nowhere/Engine Library/cover-12.jpg"});
        compare(ring.largeArtworkSource, "");
        compare(findChild(ring, "ringArtwork").source.toString(), "file:///nowhere/Engine Library/cover-12.jpg");
    }
}
