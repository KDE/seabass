// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The track details page where the ring can be drawn: the loaded track
// gets it, any other track keeps its plain cover art.
TestCase {
    id: testCase
    name: "TrackDetailPageRing"
    width: 1200
    height: 800
    visible: true
    when: windowShown

    property var tracks: [
        {title: "One", artist: "A", key: "8A", bpm: 120, durationSeconds: 240, artworkPath: "", cues: [],
         filePath: "/tmp/a.mp3", sourceId: "a"},
        {title: "Two", artist: "B", key: "8A", bpm: 124, durationSeconds: 200, artworkPath: "",
         cues: [{positionMs: 50000, color: "#ff0000"}], filePath: "/tmp/b.mp3", sourceId: "b"},
    ]
    property var scanner: ({
        trackAt: function(index) { return (index >= 0 && index < tracks.length) ? tracks[index] : {}; },
        trackCount: function() { return tracks.length; },
        keyRelation: function() { return {label: "", relation: "unknown"}; },
    })
    property var seeks: []
    property var player: ({
        waveformFor: function() {
            var columns = [];
            for (var i = 0; i < 400; ++i) columns.push({low: 0.8, mid: 0.4, high: 0.1});
            return columns;
        },
        load: function() {},
        seek: function(ms) { testCase.seeks.push(ms); },
        hasTrack: true, currentFormat: "rekordbox", currentSourceId: "b",
        playing: true, position: 50000, duration: 200000,
    })

    Component { id: pageComponent; TrackDetailPage { width: 1200; height: 800 } }
    // In the app the window behind every page is Theme.background. A bare
    // Page under test takes the style's window colour instead, which need
    // not agree with Theme -- and the ring fades toward Theme's, so a
    // screenshot on the style's would show it against the wrong page.
    Component { id: pageBackground; Rectangle { color: Theme.background } }

    function makePage(trackIndex) {
        var page = createTemporaryObject(pageComponent, testCase, {
            scanController: testCase.scanner, trackIndex: trackIndex, format: "rekordbox",
            libraryPath: "/tmp/library", playbackController: testCase.player,
            appSettingsController: {keyNotation: "camelot"},
        });
        verify(page !== null);
        page.background = pageBackground.createObject(page);
        return page;
    }

    function test_theLoadedTrackIsShownAsARing() {
        var page = makePage(1);
        var tile = findChild(page, "artTile");
        var ring = findChild(page, "trackRing");
        verify(ring !== null && ring.available, "this suite exists to run the shader");
        compare(tile.showsRing, true);
        tryCompare(tile, "side", Theme.iconSizeLarge * 6);
        fuzzyCompare(ring.progress, 0.25, 0.0001);
        compare(ring.playing, true);
        compare(ring.cueData.length, 1);

        // And a click on it seeks the player: three o'clock is a quarter in.
        testCase.seeks = [];
        mouseClick(ring, ring.width / 2 + ring.width * 0.4, ring.height / 2);
        compare(testCase.seeks.length, 1);
        fuzzyCompare(testCase.seeks[0], 50000, 400);

        if (screenshotDir && screenshotDir.length > 0) {
            wait(400);
            grabImage(page).save(screenshotDir + "/track-detail-ring.png");
        }
    }

    function test_anyOtherTrackKeepsItsPlainArt() {
        var page = makePage(0);
        var tile = findChild(page, "artTile");
        compare(tile.showsRing, false);
        compare(findChild(page, "trackRing"), null);
        compare(tile.side, Theme.iconSizeLarge * 2);
    }
}
