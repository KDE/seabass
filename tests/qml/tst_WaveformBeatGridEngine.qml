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
    // A file that is there but is no audio: an Engine track's popup detail
    // cannot be made from it, and Engine's stored waveform stands in.
    readonly property string undecodableFile: Qt.resolvedUrl("tst_WaveformBeatGridEngine.qml").toString()
        .replace("file://", "").replace(/^\/([A-Za-z]:)/, "$1")

    Component {
        id: playerComponent
        PlaybackController {}
    }
    Component {
        id: detailSpyComponent
        SignalSpy { signalName: "detailWaveformChanged" }
    }
    Component {
        id: analysisSpyComponent
        SignalSpy { signalName: "loadedAnalysisChanged" }
    }
    // Asks for the loaded track's popup detail and waits for the answer,
    // which comes off the GUI thread.
    function askForDetail(player) {
        const spy = createTemporaryObject(detailSpyComponent, testCase, {target: player});
        player.requestDetailWaveform();
        if (spy.count === 0) {
            spy.wait(30000);
        }
        return player.detailWaveform;
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

    // Nothing is read while a library is being written: Engine's analysis
    // comes out of m.db, and a row reading it in the middle of a save held
    // the lock the save's own writer then failed on ("database is
    // locked", the save rolled back; Clean Up on Windows). What was kept
    // before is still answered; the rest is asked again once the write
    // ends, which analysisRevision tells a binding.
    function test_nothingIsReadWhileALibraryIsWritten() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        const player = createTemporaryObject(playerComponent, testCase);
        const kept = player.beatGridFor("engine", lib.path, lib.id);
        compare(kept.length, 121, "read before the write");
        const other = engineGridFixture.library();
        const otherId = engineGridFixture.trackId(other);
        verify(engineGridFixture.setRow(other, 44100, 120));
        const revision = player.analysisRevision;
        player.libraryBusy = true;
        compare(player.beatGridFor("engine", lib.path, lib.id).length, 121, "what was kept is still answered");
        compare(player.beatGridFor("engine", other, otherId).length, 0, "beatGridFor reads nothing during a write");
        compare(player.beatTicksFor("engine", other, otherId, 60000, 1200).length, 0, "nor beatTicksFor");
        compare(player.waveformFor("engine", other, otherId).length, 0, "nor waveformFor");
        compare(player.analysisRevision, revision, "and nothing was dropped yet");
        player.libraryBusy = false;
        verify(player.analysisRevision > revision, "a binding is told to ask again");
        compare(player.beatGridFor("engine", other, otherId).length, 121, "and then it is read");
    }

    // A row's Play pressed during a write plays the audio but opens no
    // library: load() reading Engine's analysis held m.db's lock the
    // save's writer then failed on, as a row's waveform did. The loaded
    // track's waveform and grid are read when the write ends, for the
    // track loaded last when another one followed during the write.
    function test_aTrackLoadedDuringAWriteGetsItsAnalysisAfter() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const other = engineGridFixture.library();
        const otherId = engineGridFixture.trackId(other);
        verify(engineGridFixture.setRow(other, 44100, 120));
        verify(engineGridFixture.setWaveform(other, 1024, 64));
        const player = createTemporaryObject(playerComponent, testCase);
        player.libraryBusy = true;
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        verify(player.hasTrack, "the track is loaded");
        compare(player.errorMessage, "", "and its audio with it");
        compare(player.waveform.length, 0, "load() reads no waveform during a write");
        compare(player.beatTimesMs.length, 0, "nor beats");
        compare(player.beatTicksAt(60000, 1200).length, 0, "nor the grid it draws");
        verify(player.analysisPending, "it waits for the write");
        player.requestDetailWaveform();

        // Another track before the write ends: its analysis is the one read.
        player.load("engine", other, otherId, testCase.clickFile, "Other", "An Artist", "", [], "", false);
        compare(player.waveform.length, 0, "nor for the next track");
        verify(player.analysisPending);
        player.requestDetailWaveform();

        const spy = createTemporaryObject(analysisSpyComponent, testCase, {target: player});
        player.libraryBusy = false;
        verify(spy.count > 0, "the player bar is told");
        verify(!player.analysisPending);
        verify(player.waveform.length > 0, "the waveform once the write is over");
        compare(JSON.stringify(player.waveform), JSON.stringify(player.waveformFor("engine", other, otherId)),
                "the track loaded last's");
        verify(JSON.stringify(player.waveform) !== JSON.stringify(player.waveformFor("engine", lib.path, lib.id)),
               "not the first one's");
        compare(player.beatTimesMs.length, 121, "its beats");
        compare(player.beatTicksAt(60000, 1200).length, 121, "and its grid");
        tryVerify(() => player.detailWaveform.columns && player.detailWaveform.columns.byteLength > 0,
                  30000, "the popup's detail too");
        player.stop();
        verify(!player.analysisPending);
    }

    // A popup closed and opened again finds its detail already delivered;
    // a write that ends after that still gets it read again, from the
    // library as the write left it, not the copy load() read before it.
    function test_aReopenedPopupGetsTheDetailAgainAfterAWrite() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        player.load("engine", lib.path, lib.id, testCase.undecodableFile, "Grid", "An Artist", "", [], "", false);
        askForDetail(player);
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

    // An Engine track's popup detail is made from its audio, on Engine's
    // timeline, as a Denon player makes its own: 150 columns a second, not
    // the stored overview's 1024 for the whole track. The click file's
    // click is at 1.500 s on Engine's timeline (qt_decoder_timeline_test),
    // so in column 225, with silence a tenth of a second either side.
    function test_anEngineDetailIsMadeFromTheAudio() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        const detail = askForDetail(player);
        compare(detail.columns.byteLength, 450 * 3, "three seconds at 150 columns a second");
        fuzzyCompare(detail.msPerColumn, 1000 / 150, 1e-9);
        compare(detail.startMs, 0, "on Engine's timeline, where its grid is");
        const bytes = new Uint8Array(detail.columns);
        const loudest = (c) => Math.max(bytes[3 * c], bytes[3 * c + 1], bytes[3 * c + 2]);
        let peak = -1;
        for (let c = 0; c < 450; ++c) {
            if (peak < 0 || loudest(c) > loudest(peak)) {
                peak = c;
            }
        }
        verify(Math.abs(peak - 225) <= 1, "the click in column " + peak);
        compare(loudest(225 - 15), 0, "silence before it");
        // After it the bands decay as a swell (their release), and are gone
        // well before the next click at 2.5 s.
        verify(loudest(225 + 15) > 0 && loudest(225 + 15) < loudest(peak), "decaying 100 ms after it");
        compare(loudest(225 + 90), 0, "and gone 600 ms after it");
        player.stop();
    }

    // Where the audio cannot be decoded, Engine's stored waveform is the
    // detail, as it always was: a file that is not audio, and one that is
    // not there (load() then reads no analysis, and the popup's read does).
    function test_withoutAudioTheStoredWaveformStays_data() {
        return [{tag: "not audio", file: testCase.undecodableFile},
                {tag: "missing", file: "/nonexistent/none.mp3"}];
    }
    function test_withoutAudioTheStoredWaveformStays(data) {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        player.load("engine", lib.path, lib.id, data.file, "Grid", "An Artist", "", [], "", false);
        const detail = askForDetail(player);
        compare(detail.columns.byteLength, 1024 * 3, "the stored waveform's 1024 entries");
        compare(new Uint8Array(detail.columns)[0], 128, "at the level it was written at");
        player.stop();
    }

    // The audio detail is kept for the session, per file as it is: asked
    // for again it is not decoded again, a rewritten file (a new time, a
    // new size) is, the last four files are kept and no more, and a file
    // that could not be decoded is not tried again either.
    function test_theAudioDetailIsKeptPerFile() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        const copy = (n) => lib.path + "/click-" + n + ".mp3";
        const detailOf = (file) => {
            player.load("engine", lib.path, lib.id, file, "Grid", "An Artist", "", [], "", false);
            return askForDetail(player);
        };
        verify(stickFixture.copyFile(testCase.clickFile, copy(0)));
        compare(detailOf(copy(0)).columns.byteLength, 450 * 3);
        compare(player.audioDetailDecodes(), 1);
        compare(detailOf(copy(0)).columns.byteLength, 450 * 3, "the same again");
        compare(player.audioDetailDecodes(), 1, "without a decode");
        verify(stickFixture.touch(copy(0)));
        detailOf(copy(0));
        compare(player.audioDetailDecodes(), 2, "a new modification time is decoded again");
        const other = testCase.clickFile.replace("click-D-lame-44100", "click-A-notag-44100");
        verify(stickFixture.copyKeepingTime(other, copy(0)), "another size, the same time");
        detailOf(copy(0));
        compare(player.audioDetailDecodes(), 3, "and so is a new size");

        for (let n = 1; n <= 4; ++n) {
            verify(stickFixture.copyFile(testCase.clickFile, copy(n)));
            detailOf(copy(n));
        }
        compare(player.audioDetailDecodes(), 7, "four more files");
        detailOf(copy(4));
        compare(player.audioDetailDecodes(), 7, "the last one still kept");
        detailOf(copy(0));
        compare(player.audioDetailDecodes(), 8, "the fifth one back is not");

        compare(detailOf(testCase.undecodableFile).columns.byteLength, 1024 * 3, "not audio: the stored waveform");
        compare(player.audioDetailDecodes(), 9);
        compare(detailOf(testCase.undecodableFile).columns.byteLength, 1024 * 3);
        compare(player.audioDetailDecodes(), 9, "a file that could not be decoded is not tried again");
        player.stop();
    }

    // While a library is being written nothing is read, the audio file
    // included: the stored overview load() read is shown, and the detail
    // from the audio comes once the write is over.
    function test_duringAWriteTheOverviewThenTheAudio() {
        const lib = library();
        verify(engineGridFixture.setRow(lib.path, 44100, 120));
        verify(engineGridFixture.setWaveform(lib.path, 1024, 128));
        const player = createTemporaryObject(playerComponent, testCase);
        player.load("engine", lib.path, lib.id, testCase.clickFile, "Grid", "An Artist", "", [], "", false);
        player.libraryBusy = true;
        player.requestDetailWaveform();
        compare(player.detailWaveform.columns.byteLength, 1024 * 3, "the stored overview, at once");
        compare(player.audioDetailDecodes(), 0, "and no decode during the write");
        const spy = createTemporaryObject(detailSpyComponent, testCase, {target: player});
        player.libraryBusy = false;
        tryVerify(() => player.detailWaveform.columns && player.detailWaveform.columns.byteLength === 450 * 3,
                  30000, "the audio's once the write is over");
        compare(player.audioDetailDecodes(), 1);
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
        player.load("engine", lib.path, lib.id, testCase.undecodableFile, "Grid", "An Artist", "", [], "", false);
        compare(player.waveform.length, 0, "nor on load");
        askForDetail(player);
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
        player.load("engine", lib.path, lib.id, testCase.undecodableFile, "Grid", "An Artist", "", [], "", false);
        verify(player.waveform.length > 0, "on load too");
        compare(player.beatTicksAt(60000, 1200).length, 0, "and no grid there");
        askForDetail(player);
        compare(player.detailWaveform.columns.byteLength, 1024 * 3, "the popup's detail");
        compare(player.detailWaveform.msPerColumn, 0, "without a spacing of its own");
        player.stop();
    }
}
