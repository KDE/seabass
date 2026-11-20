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

    // A popup closed and opened again finds its detail already delivered;
    // a write that ends after that still gets it read again, from the
    // library as the write left it, not the copy load() read before it.
    function test_aReopenedPopupGetsTheDetailAgainAfterAWrite() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        player.requestDetailWaveform();
        compare(player.detailWaveform.columns.byteLength, 1024 * 3, "the first opening's detail");
        compare(new Uint8Array(player.detailWaveform.columns)[0], 128, "at the level it was written at");
        player.releaseDetailWaveform();   // closed
        player.requestDetailWaveform();   // and opened again: already there
        compare(player.detailWaveform.columns.byteLength, 1024 * 3);
        player.libraryBusy = true;
        // The level, not the count: libdjinterop writes an overview of its
        // own length whatever it is handed.
        verify(engineGridFixture.setWaveform(lib.path, 1024, 64), "the write changes the waveform");
        player.libraryBusy = false;
        const level = () => player.detailWaveform.columns && player.detailWaveform.columns.byteLength > 0
            ? new Uint8Array(player.detailWaveform.columns)[0] : -1;
        tryVerify(() => level() === 64, 10000, "read again after the write, as the write left it");
        player.stop();
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
        const whole = player.beatGridFor("engine", lib.path, lib.id);
        for (const width of [beatPx * 120, beatPx * 120 - 1, barPx * 30, barPx * 30 - 1]) {
            const kept = at(width);
            compare(kept.map((beat) => beat.timeMs * width / 60000),
                    BeatGridTicks.layout(whole, 60000, width).ticks.map((tick) => tick.x),
                    "a row is handed the ticks of the whole grid at " + width);
        }
    }

    // A waveform blob libdjinterop must not be handed (#104 review, as the
    // cue writer's since 347a95da): three bytes, and a stream cut short,
    // which libdjinterop's decoder would loop on for ever. The waveform
    // reads give nothing for it and come back; the grid of the same row
    // is read all the same.
    function test_anUndecodableWaveformBlobIsNotDecoded_data() {
        return [{tag: "three bytes", hex: "000102"},
                {tag: "a stream cut short", hex: "00000040789c4b4c4a"}];
    }
    function test_anUndecodableWaveformBlobIsNotDecoded(data) {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setOverviewBlob(lib.path, data.hex), "planted");
        const player = createTemporaryObject(playerComponent, testCase);
        compare(player.waveformFor("engine", lib.path, lib.id).length, 0, "no waveform from it");
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 121, "the grid all the same");
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        compare(player.waveform.length, 0, "nor on load");
        player.requestDetailWaveform();
        compare(player.detailWaveform.columns.byteLength, 0, "nor for the popup");
        player.stop();
    }

    // trackData, where the sample rate and count are, unchecked was the
    // same hang (#104, second review): a three-byte and a cut-short
    // trackData. The waveform still shows; there is no grid and no
    // recorded rate; the popup's detail has no spacing of its own.
    function test_anUndecodableTrackDataIsNotDecoded_data() {
        return [{tag: "three bytes", hex: "000102"},
                {tag: "a stream cut short", hex: "00000040789c4b4c4a"}];
    }
    function test_anUndecodableTrackDataIsNotDecoded(data) {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        verify(engineGridFixture.setBlob(lib.path, "trackData", data.hex), "planted");
        const player = createTemporaryObject(playerComponent, testCase);
        verify(player.waveformFor("engine", lib.path, lib.id).length > 0, "the waveform still shows");
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 0, "no grid");
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        verify(player.waveform.length > 0, "on load too");
        compare(player.beatTicksAt(60000, 1200).length, 0, "and no grid there");
        player.requestDetailWaveform();
        compare(player.detailWaveform.columns.byteLength, 1024 * 3, "the popup's detail");
        compare(player.detailWaveform.msPerColumn, 0, "without a spacing of its own");
        player.stop();
    }
}
