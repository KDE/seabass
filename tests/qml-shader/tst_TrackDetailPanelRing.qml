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
    property var player: ({
        waveformFor: function() {
            var columns = [];
            for (var i = 0; i < 400; ++i) columns.push({low: 0.8, mid: 0.4, high: 0.1});
            return columns;
        },
        seek: function(ms) { testCase.seeks.push(ms); },
        togglePlay: function() { testCase.toggles += 1; },
        hasTrack: true, currentFormat: "engine", currentSourceId: "42",
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

        if (screenshotDir && screenshotDir.length > 0) {
            wait(400);
            grabImage(stage).save(screenshotDir + "/track-panel-ring.png");
        }
    }

    // A click on the ring puts it alone on the whole screen; a click
    // there goes back, to the window as it was.
    function test_aClickOnTheRingGoesFullscreenAndAClickThereGoesBack() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var panel = stage.panel;
        panel.showFor(track("42"));
        var ring = findChild(panel, "trackRing");
        var host = testCase.Window.window;
        var before = host.visibility;
        verify(before !== Window.FullScreen);
        tryVerify(function() { return ring.width > 200; }, 2000);

        mouseClick(ring, ring.width / 2, ring.height / 2);
        var big = null;
        tryVerify(function() { big = findChild(Overlay.overlay, "fullscreenRing"); return big !== null && big.visible; },
                  2000, "the fullscreen ring opens");
        compare(host.visibility, Window.FullScreen);
        verify(big.width > ring.width, "and is the larger of the two");
        compare(findChild(Overlay.overlay, "fullscreenTitle").text, "Major Tom (Reworked 2024)");
        compare(findChild(Overlay.overlay, "fullscreenArtist").text, "DJ Amador");
        fuzzyCompare(big.progress, 0.25, 0.0001);
        compare(big.waveformData.length, 2, "it draws what the PLAYER has loaded");

        if (screenshotDir && screenshotDir.length > 0) {
            wait(500);
            grabImage(Overlay.overlay).save(screenshotDir + "/track-ring-fullscreen.png");
        }

        mouseClick(big, big.width / 2, big.height / 2);
        tryVerify(function() { return findChild(Overlay.overlay, "fullscreenRing") === null
                                   || !findChild(Overlay.overlay, "fullscreenRing").visible; }, 2000, "a click there closes it");
        compare(host.visibility, before, "and the window is as it was");
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
