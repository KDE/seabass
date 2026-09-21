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
                         cueCount: 0, artworkPath: "", playlistNames: []}];
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

    // The artist's other tracks are there to read, not to click: each
    // names its key and its length to the second, and says on hover where
    // it can be found.
    function test_artistTracksAreInformativeRows() {
        var panel = makePanel();
        panel.scanController = {
            tracksByArtist: function () {
                return [{sourceId: "99", title: "Another One", durationSeconds: 372, bpm: 124, key: "8A",
                         cueCount: 0, artworkPath: "", playlistNames: ["Peaktime", "Warm-up"]}];
            }
        };
        panel.showFor(makeDelegate());
        var heading = findChild(panel, "artistTracksHeading");
        verify(heading !== null, "the list must have its subheader");
        compare(heading.text, "1 more track by " + panel.trackArtist);
        var row = findChild(panel, "artistTrackRow");
        verify(row !== null, "the artist's other track must be listed");
        compare(row.tooltipText, "In Peaktime, Warm-up");
        // The row's own popup, which the key badge's tooltip cannot take over.
        var tip = findChild(row, "artistTrackTip");
        verify(tip !== null, "the row must carry its own tooltip");
        compare(tip.text, "In Peaktime, Warm-up");
        compare(findChild(row, "artistTrackDuration").text, "6:12", "to the second, not rounded");
        verify(findChild(row, "artistTrackArtwork") !== null, "each row has room for its cover art");
        verify(row.clicked === undefined, "the row must not be a button");
    }

    // The key is a label beside the sleeve, not a row of the facts table.
    function test_keyIsBesideTheArtworkNotInTheFacts() {
        var panel = makePanel();
        panel.showFor(makeDelegate());
        var badge = findChild(panel, "trackKeyBadge");
        verify(badge !== null, "the key label must exist");
        compare(badge.visible, panel.trackKey.length > 0);
        verify(panel.trackKey.length > 0, "the test delegate must carry a key");
        var facts = findChild(panel, "trackFacts");
        verify(facts !== null);
        for (var p = badge.parent; p; p = p.parent) {
            verify(p !== facts, "the key must not sit in the facts table");
        }
        compare(badge.parent, findChild(panel, "trackArtwork").parent, "the key sits beside the artwork");
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

    function makeLoadedPlayer(format, sourceId, libraryPath) {
        return {waveformFor: function() { return []; }, hasTrack: true, currentFormat: format,
                currentLibraryPath: libraryPath || "/media/MAIN/Engine Library",
                currentSourceId: sourceId, playing: true, position: 93000, duration: 372000,
                seek: function() {}, togglePlay: function() {}};
    }

    function test_thePanelKnowsWhenItsTrackIsTheLoadedOne_data() {
        return [
            {tag: "the same track", player: makeLoadedPlayer("engine", "42"), expected: true},
            {tag: "another track", player: makeLoadedPlayer("engine", "7"), expected: false},
            // rekordbox and Engine number their tracks independently.
            {tag: "the same id in the other format", player: makeLoadedPlayer("rekordbox", "42"), expected: false},
            // Nor is an id a track's name across two sticks of one format.
            {tag: "the same id on another stick", player: makeLoadedPlayer("engine", "42", "/media/OTHER/Engine Library"), expected: false},
            {tag: "nothing loaded", player: {waveformFor: function() { return []; }}, expected: false},
        ];
    }
    function test_thePanelKnowsWhenItsTrackIsTheLoadedOne(data) {
        var panel = makePanel();
        panel.playbackController = data.player;
        panel.showFor(makeDelegate());
        compare(panel.isLoadedTrack, data.expected);
        compare(findChild(panel, "trackRing") !== null, data.expected, "the ring exists only for the loaded track");
    }

    // This suite's platform cannot run the ring's shader. The panel must
    // then keep the sleeve where it was and open no empty row above it --
    // tests/qml-shader has the case where the ring can be drawn.
    // The loaded track gets the ring where a shader can draw it, and its
    // sleeve where one cannot. One case for both, keyed on the row's own
    // decision, so neither platform is the one nobody checks.
    function test_theLoadedTrackGetsTheRingOrItsSleeve() {
        const panel = makePanel();
        panel.playbackController = makeLoadedPlayer("engine", "42");
        panel.showFor(makeDelegate());
        compare(panel.isLoadedTrack, true);
        const row = findChild(panel, "trackRingRow");
        // The ring arrives through a Loader, so it is not there on the
        // frame the page is built: waited for rather than sampled, or
        // this reads "no shader here" on a machine that has one.
        tryVerify(function() { return findChild(panel, "trackRing") !== null || !row.showsRing; }, 2000);
        const ring = findChild(panel, "trackRing");
        const canDraw = ring !== null && ring.available;
        if (typeof shaderExpected !== "undefined" && shaderExpected) {
            verify(canDraw, "this run is on a display with a shader, so the ring must be drawable");
        }
        compare(row.showsRing, canDraw);
        // The row OPENS to the ring's size rather than appearing at it:
        // `side` is animated, and `visible` follows `side > 0`, so on
        // the first frame after showFor the row is still shut. Waited
        // for, not sampled.
        tryCompare(row, "visible", canDraw, 2000);
        tryCompare(findChild(panel, "trackArtwork"), "visible", !canDraw, 2000,
                   canDraw ? "the ring carries the cover, so the sleeve steps aside"
                           : "no ring, so the sleeve stays");
    }
}
