// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The beat grid (#40) through the real PlaybackController, on a scratch
// Engine library of one analysed track (engineGridFixture): which grids
// are drawn, that every path a view takes agrees, that a library written
// since is read again, and that the loaded track's grid goes with it.
TestCase {
    id: testCase
    name: "WaveformBeatGridEngine"
    when: windowShown

    readonly property string clickFile: Qt.resolvedUrl("../fixtures/mp3-click/click-D-lame-44100.mp3").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    Component {
        id: playerComponent
        PlaybackController {}
    }

    function library() {
        const path = engineGridFixture.library();
        verify(path.length > 0, "a scratch Engine library");
        const id = engineGridFixture.trackId(path);
        verify(id.length > 0, "with its one track");
        return {path: path, id: id};
    }

    // Every way a view asks for a grid, for one track: the whole of it,
    // a row's, and the loaded track's.
    function grids(player, lib) {
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", []);
        return {
            whole: player.beatGridFor("engine", lib.path, lib.id),
            row: player.beatTicksFor("engine", lib.path, lib.id, 60000, 1200),
            loaded: player.beatTicksAt(60000, 1200),
        };
    }

    // A rate the row records: drawn, a beat every 500 ms, on every path.
    // A minute holds 121 of them: the one at 1:00 is inside the track.
    function test_aRecordedRateIsDrawn() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        const player = createTemporaryObject(playerComponent, testCase);
        const got = grids(player, lib);
        compare(got.whole.length, 121);
        fuzzyCompare(got.whole[2].timeMs, 1000, 0.01);
        compare(got.row.length, 121, "1200 px: every beat");
        // On the playback timeline: an Engine grid is on it already.
        compare(got.loaded.length, 121);
        player.stop();
    }

    // No rate recorded: the beat reader works one out from the markers and
    // the tempo, for the player to keep time by, but a worked-out rate is
    // not drawn as fact -- with the tempo stored at double the grid's it is
    // twice the true one. Withheld on every path a view takes.
    function test_aWorkedOutRateIsNotDrawn_data() {
        return [{tag: "the tempo fits the markers", bpm: 120}, {tag: "the tempo stored at double", bpm: 240}];
    }
    function test_aWorkedOutRateIsNotDrawn(data) {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 0, data.bpm));
        const player = createTemporaryObject(playerComponent, testCase);
        const got = grids(player, lib);
        compare(got.whole.length, 0, "beatGridFor");
        compare(got.row.length, 0, "beatTicksFor");
        compare(got.loaded.length, 0, "beatTicksAt, the loaded track");
        verify(player.beatTimesMs.length > 0, "while the player still keeps time by the reader's beats");
        player.stop();
    }

    // No rate recorded and a tempo the markers do not fit: there is no
    // rate to know, and a default one would be a guess. Withheld on every
    // path a view takes -- the whole grid, a row's and the loaded track's.
    function test_noKnowableRateIsWithheldEverywhere() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 0, 100));
        const player = createTemporaryObject(playerComponent, testCase);
        const got = grids(player, lib);
        compare(got.whole.length, 0, "beatGridFor");
        compare(got.row.length, 0, "beatTicksFor");
        compare(got.loaded.length, 0, "beatTicksAt, the loaded track");
        compare(player.beatTimesMs.length, 0, "and the player has none to keep time by either");
        player.stop();
    }

    // A library written since the last read is read again once the write
    // is over (libraryBusy going false is EditSessionRegistry.anyWriting
    // ending, Main.qml), and when told the sticks changed.
    function test_aWrittenLibraryIsReadAgain() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 0, 100));
        const player = createTemporaryObject(playerComponent, testCase);
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 0, "no rate to know");
        verify(engineGridFixture.setRow(lib.path, 44100, 100));
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 0, "still the cached answer while nothing said");
        player.libraryBusy = true;
        player.libraryBusy = false;
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 121, "read again after the write");

        verify(engineGridFixture.setRow(lib.path, 0, 100));
        compare(player.beatTicksFor("engine", lib.path, lib.id, 60000, 1200).length, 121, "cached");
        player.forgetAnalyses();
        compare(player.beatTicksFor("engine", lib.path, lib.id, 60000, 1200).length, 0, "read again after the sticks changed");
    }

    // Stopping, a recording and a load that fails each leave no grid.
    function test_theLoadedGridGoesWithTheTrack() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        const player = createTemporaryObject(playerComponent, testCase);

        const loaded = function() {
            player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", []);
            compare(player.beatTicksAt(60000, 1200).length, 121, "loaded with its grid");
        };
        loaded();
        let serial = player.trackSerial;
        player.stop();
        compare(player.beatTicksAt(60000, 1200).length, 0, "stop()");
        verify(player.trackSerial !== serial);

        loaded();
        serial = player.trackSerial;
        player.loadFile("engine", lib.path, testCase.clickFile, "A recording", "");
        compare(player.beatTicksAt(60000, 1200).length, 0, "loadFile()");
        verify(player.trackSerial !== serial);

        loaded();
        serial = player.trackSerial;
        player.load("engine", lib.path, lib.id, "/nonexistent/not-there.mp3", "Gone", "", "", []);
        verify(player.errorMessage.length > 0, "the load failed");
        compare(player.beatTicksAt(60000, 1200).length, 0, "a failed load()");
        verify(player.trackSerial !== serial);
        player.stop();
    }

    // A row handed its beats at a width exactly on each threshold: a beat
    // every 500 ms over a minute (121 beats, 31 bars) is width / 120 px a
    // beat and width / 30 a bar.
    function test_theThinnedPathAtEachThreshold() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        const player = createTemporaryObject(playerComponent, testCase);
        const beatPx = BeatGridTicks.minimumBeatSpacing();
        const barPx = BeatGridTicks.minimumDownbeatSpacing();
        const at = (width) => player.beatTicksFor("engine", lib.path, lib.id, 60000, width);
        compare(at(beatPx * 120).length, 121, "exactly the beat spacing: every beat");
        compare(at(beatPx * 120 - 1).length, 31, "a pixel less: the downbeats");
        compare(at(barPx * 30).length, 31, "exactly the downbeat spacing: every downbeat");
        compare(at(barPx * 30 - 1).length, 16, "a pixel less: every second one");
        for (const width of [beatPx * 120, beatPx * 120 - 1, barPx * 30, barPx * 30 - 1]) {
            const kept = at(width);
            compare(BeatGridTicks.layout(kept, 60000, width).ticks.length, kept.length,
                    "the view draws every beat it is handed at " + width);
        }
    }
}
