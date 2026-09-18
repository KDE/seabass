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
