// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// TrackDetailPanel.qml headless with fake controllers: showFor() fills
// the header and playlists, the close button asks the page to close,
// and a click on the waveform opens the add-cue form. Plus the page's
// screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "TrackDetailPanel"
    width: 480
    height: 700
    visible: true
    when: windowShown

    Component {
        id: panelComponent
        TrackDetailPanel { width: 460; height: 680 }
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    function makePanel() {
        var panel = createTemporaryObject(panelComponent, testCase, {
            addCueController: {statusMessage: "", errorMessage: "", busy: false, calls: [],
                               addCue: function() { this.calls.push("addCue"); }},
            playbackController: {waveformFor: function(format, path, id) { return []; }},
            format: "engine",
            libraryPath: "/media/MAIN/Engine Library",
        });
        verify(panel !== null);
        waitForRendering(panel);
        return panel;
    }

    function makeDelegate() {
        return {sourceId: "42", title: "Major Tom (Reworked 2024)", artist: "DJ Amador",
                cues: [{kind: "hot", hotCueNumber: 1, positionMs: 32000, isLoop: false, loopEndMs: 0, color: "#ffcc00", comment: ""}],
                durationSeconds: 372, playlistNames: ["Peaktime", "Warm-up"], streamingSource: "",
                rating: 4, bpm: 126.5, key: "Fm", bitrate: 320, playCount: 17,
                artworkPath: "file:///covers/major-tom.jpg",
                comment: "Big room, drop at 1:04", album: "Sounds From The Deep"};
    }

    // The artist list is built from the track being SHOWN, not the one
    // that was showing before. showFor() assigns trackSourceId first, so
    // refreshing on that property's change read the previous track's
    // artist and listed the wrong artist's tracks under this one's
    // heading -- which is what it did, on real data, until this test.
    function test_artistListFollowsTheTrackBeingShown() {
        var asked = [];
        var panel = makePanel();
        panel.scanController = {
            tracksByArtist: function (artist, excludeSourceId) {
                asked.push(artist);
                return [{sourceId: "99", title: "Another One", durationSeconds: 300, bpm: 124, key: "8A",
                         cueCount: 0}];
            }
        };

        var first = makeDelegate();
        panel.showFor(first);
        compare(asked[asked.length - 1], "DJ Amador");

        var second = makeDelegate();
        second.sourceId = "77";
        second.artist = "Someone Else";
        second.title = "Different Track";
        panel.showFor(second);
        compare(asked[asked.length - 1], "Someone Else");
        compare(panel.artistTracks.length, 1);
    }

    function test_showForFillsThePanel() {
        var panel = makePanel();
        panel.showFor(makeDelegate());
        compare(panel.trackTitle, "Major Tom (Reworked 2024)");
        compare(panel.trackArtist, "DJ Amador");
        compare(panel.trackDurationMs, 372000);
        compare(panel.trackPlaylistNames.length, 2);
        // The fields added for the details panel. Asserted here because
        // showFor() reads each one with a fallback, and a typo in a name
        // would silently leave the field at its "hide me" default rather
        // than failing.
        compare(panel.trackRating, 4);
        compare(panel.trackBpm, 126.5);
        compare(panel.trackKey, "Fm");
        compare(panel.trackBitrate, 320);
        compare(panel.trackPlayCount, 17);
        compare(panel.trackComment, "Big room, drop at 1:04");
        compare(panel.trackAlbum, "Sounds From The Deep");
        // Passed straight through, never re-prefixed: the role already
        // hands over a file:// URL, and prefixing it again produced
        // "file://file:///..." and an image that silently did not load.
        compare(panel.trackArtworkPath, "file:///covers/major-tom.jpg");
        compare(panel.pendingPositionMs, -1);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(panel).save(screenshotDir + "/track-panel.png");
        }
    }

    function test_closeButtonAsksThePage() {
        var panel = makePanel();
        var spy = createTemporaryObject(spyComponent, testCase, {target: panel, signalName: "closeRequested"});
        findChild(panel, "closeTrackPanelButton").clicked();
        compare(spy.count, 1);
    }

    function test_streamingTrackHasNoAddCueHint() {
        var panel = makePanel();
        var delegate = makeDelegate();
        delegate.streamingSource = "TIDAL";
        panel.showFor(delegate);
        compare(panel.trackStreamingSource, "TIDAL");
    }
}
