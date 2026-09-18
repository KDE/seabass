// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
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
    property var player: ({
        waveformFor: function() {
            var columns = [];
            for (var i = 0; i < 400; ++i) columns.push({low: 0.8, mid: 0.4, high: 0.1});
            return columns;
        },
        seek: function(ms) { testCase.seeks.push(ms); },
        togglePlay: function() { testCase.toggles += 1; },
        hasTrack: true, currentFormat: "engine", currentSourceId: "42",
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
                bitrate: 320, playCount: 17, artworkPath: "file:///covers/major-tom.jpg", comment: "", album: "",
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
        compare(ring.waveformData.length, 400, "the ring draws the waveform the pane already read");

        testCase.seeks = [];
        testCase.toggles = 0;
        mouseClick(ring, ring.width / 2 + ring.width * 0.4, ring.height / 2);
        compare(testCase.seeks.length, 1, "a click on the ring seeks");
        fuzzyCompare(testCase.seeks[0], 93000, 800);
        mouseClick(ring, ring.width / 2, ring.height / 2);
        compare(testCase.toggles, 1, "a click on the cover plays and pauses");
        compare(testCase.seeks.length, 1);

        if (screenshotDir && screenshotDir.length > 0) {
            wait(400);
            grabImage(stage).save(screenshotDir + "/track-panel-ring.png");
        }
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
