// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Sync's Beat Grids & Waveforms page with the real controller, on scratch
// copies of two fixtures:
//
// - TESTSHARK after the Prime Go+ (tests/fixtures/testshark_t73,
//   m-after-R1b.db, put together as tests/assemble_t73_stick.cmake does):
//   11 tracks in both libraries, 5 analysed by a player (B04, B11, N1, N2,
//   N3), 6 not; rekordbox has a grid and a colour waveform for every one,
//   and the audio heads give each file's rate, so the 6 can be completed.
//   Playlists "T73 Renamed" (B01 to B04, B12), "T73 Order" (B07, B11, B12,
//   B08) and "T73 Folder/T73 New" (N1 to N3): B12 is in two. The numbers
//   are the command line's cli_copy_analysis_* tests' on the same stick.
// - the anonymized library: 1161 tracks in both, 188 analysed, no
//   three-band or colour waveform in its export.
//
// Ids below are rekordbox's (the page ticks rekordbox tracks).
TestCase {
    id: testCase
    name: "BeatGridsWaveformsPage"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    // rekordbox ids on the TESTSHARK stick.
    readonly property string b01: "9"     // not analysed
    readonly property string b02: "10"    // not analysed
    readonly property string b04: "12"    // analysed
    readonly property string b07: "1"     // not analysed
    readonly property string b12: "6"     // not analysed, in two playlists
    readonly property string n1: "13"     // analysed

    // The player a row's waveform loads its track into, as Main.qml hands
    // the page one; a test that opens the zoomed popup passes it.
    PlaybackController { id: realPlayback }

    function fixture(name) {
        const url = Qt.resolvedUrl("../fixtures/" + name).toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }
    function t73() {
        const stick = stickFixture.t73StickCopy(fixture("testshark_t73"), "m-after-R1b.db");
        verify(stick.length > 0, "the TESTSHARK stick must assemble");
        return stick;
    }

    Component {
        id: pageComponent
        BeatGridsWaveformsPage {
            width: 1080
            height: 800
            stickLabel: "TESTSHARK"
        }
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/Cannot read property/);
        failOnWarning(/Unable to assign/);
    }

    function cleanup() {
        EditSessionRegistry.discardAll();
        SystemFontMetrics.generalPointSizeOverride = 0;
    }

    // A page on a stick copy, with no session left over from an earlier
    // test (one filesystem, one library id for every copy).
    function openOn(stick, properties) {
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        const page = createTemporaryObject(pageComponent, testCase, Object.assign({
            rekordboxPath: stick + "/PIONEER",
            enginePath: stick + "/Engine Library",
        }, properties || {}));
        verify(page !== null, "the page must instantiate");
        tryVerify(() => page.controller.loaded && !page.controller.loading, 120000, "the libraries are read");
        compare(page.controller.errorMessage, "");
        return {page: page, c: page.controller, libraryId: libraryId};
    }

    // Every row shown and every ticked track has its state.
    function settled(c) {
        if (c.loading || c.previewBusy || c.tickedPendingCount > 0) {
            return false;
        }
        for (let i = 0; i < c.tracks.count; ++i) {
            if (c.tracks.get(i).pending) {
                return false;
            }
        }
        return true;
    }
    function waitSettled(c, why) {
        tryVerify(() => settled(c), 120000, (why || "every state is worked out") + " " + JSON.stringify({
            loading: c.loading, previewBusy: c.previewBusy, pending: c.tickedPendingCount,
            error: c.errorMessage, first: c.tracks.count > 0 ? c.tracks.get(0) : null}));
    }

    function row(c, id) {
        const i = c.tracks.indexOf(id);
        verify(i >= 0, "track " + id + " is listed");
        return c.tracks.get(i);
    }

    // A click where the item is drawn once the page is laid out. The page
    // lays itself out again when its read lands (the header grows with the
    // options it then shows, and the lists move down under it), and that
    // waits for the next polish: a position read before it is where the
    // item was, and on a loaded Mac the click landed on nothing there,
    // the box ticking no track.
    function clickOn(item) {
        verify(item !== null, "something to click");
        verify(waitForPolish(item.Window.window), "the page is laid out before the click");
        mouseClick(item);
    }

    // A playlist's row in the list ("All tracks" for ""), and its box.
    function playlistRow(page, name) {
        const list = findChild(page, "playlistList");
        verify(list !== null);
        const wanted = name.length === 0 ? "All tracks" : name;
        // The list builds its rows after the read lands and the page is
        // laid out: on a loaded machine that is later than the caller.
        let found = null;
        tryVerify(() => {
            for (let i = 0; i < list.contentItem.children.length; ++i) {
                const item = list.contentItem.children[i];
                if (item.name === wanted) {
                    found = item;
                    return true;
                }
            }
            return false;
        }, 30000, "no playlist row " + wanted);
        return found;
    }
    function clickPlaylistBox(page, name) {
        const box = findChild(playlistRow(page, name), "playlistRowCheck");
        verify(box !== null && box.visible, "the playlist row has a box");
        clickOn(box);
    }
    function showPlaylist(page, name) {
        clickOn(findChild(playlistRow(page, name), "playlistRowName"));
        compare(page.controller.shownPlaylist, name);
    }
    // A track's row delegate, built because it is in view.
    function trackDelegate(page, id) {
        const list = findChild(page, "trackList");
        for (let i = 0; i < list.contentItem.children.length; ++i) {
            const item = list.contentItem.children[i];
            if (item.objectName === "trackRow_" + id) {
                return item;
            }
        }
        return null;
    }
    // The same row, scrolled into view first and waited for. A row the
    // list built ahead of the view (its cache) is no row to click on, and
    // with taller rows (macOS's fonts) or a shorter window one asked for
    // may not be built at all.
    function delegateInView(page, id) {
        const list = findChild(page, "trackList");
        const index = page.controller.tracks.indexOf(id);
        verify(index >= 0, "track " + id + " is listed");
        list.positionViewAtIndex(index, ListView.Contain);
        verify(waitForPolish(page.Window.window));
        let found = null;
        tryVerify(() => {
            found = trackDelegate(page, id);
            if (found === null) {
                return false;
            }
            const top = found.mapToItem(list, 0, 0).y;
            return top >= 0 && top + found.height <= list.height;
        }, 5000, "track " + id + "'s row is in view");
        return found;
    }
    function shownBadges(delegate) {
        const shown = [];
        for (const name of ["gridMark", "waveformMark"]) {
            const badge = findChild(findChild(delegate, name), "stateBadge");
            shown.push(badge !== null && badge.visible ? badge.label : null);
        }
        return shown;
    }
    // Every row one shape, whatever its states say: the same height, the
    // beat grid's badge over the waveform's on two lines, both badges at
    // one x on every row, and the badges clear of the tempo.
    function verifyRowsAlike(page, ids, why) {
        let height = -1;
        let badgeX = -1;
        for (const id of ids) {
            const delegate = delegateInView(page, id);
            if (height < 0) {
                height = delegate.height;
            }
            compare(delegate.height, height, why + ": " + id + " is as tall as the first row");
            const grid = findChild(findChild(delegate, "gridMark"), "stateBadge");
            const waveform = findChild(findChild(delegate, "waveformMark"), "stateBadge");
            verify(grid.visible && waveform.visible, why + ": " + id + " shows both badges");
            const gridX = grid.mapToItem(delegate, 0, 0).x;
            if (badgeX < 0) {
                badgeX = gridX;
            }
            compare(gridX, badgeX, why + ": " + id + "'s grid badge (" + grid.label + ") lines up with the first row's");
            compare(waveform.mapToItem(delegate, 0, 0).x, badgeX,
                    why + ": " + id + "'s waveform badge (" + waveform.label + ") too");
            verify(waveform.mapToItem(delegate, 0, 0).y >= grid.mapToItem(delegate, 0, grid.height).y,
                   why + ": " + id + ": the waveform's line is under the grid's");
            const bpm = findChild(delegate, "bpmLabel");
            for (const badge of [grid, waveform]) {
                verify(badge.mapToItem(delegate, badge.width, 0).x <= bpm.mapToItem(delegate, 0, 0).x,
                       why + ": " + id + "'s " + badge.label + " stops before the tempo");
            }
        }
    }
    // A box's words a gap from Theme's scale off the box itself.
    function verifyBoxLabelGap(page, name) {
        const box = findChild(page, name);
        const indicator = findChild(box, "seabassCheckIndicator");
        const label = box.contentItem;
        const boxRight = indicator.mapToItem(page, indicator.width, 0).x;
        const textLeft = label.mapToItem(page, label.leftPadding, 0).x;
        verify(textLeft - boxRight >= Theme.tightSpacing,
               name + ": its words start " + (textLeft - boxRight) + " px after the box, under " + Theme.tightSpacing);
    }

    function finished(session, run, what) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        run();
        tryVerify(() => summary !== null, 300000, (what || "the save") + " finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // Nothing is copied that the user did not choose: nothing ticked, the
    // action off, the parts both on and the replace box off.
    function test_nothingTickedAtFirstAndTheActionIsOff() {
        const o = openOn(t73());
        waitSettled(o.c);
        compare(o.c.totalTrackCount, 11);
        compare(o.c.matchedCount, 11);
        compare(JSON.stringify(o.c.playlistNames), JSON.stringify(["T73 Folder/T73 New", "T73 Order", "T73 Renamed"]));
        compare(o.c.tickedCount, 0);
        for (let i = 0; i < o.c.tracks.count; ++i) {
            compare(o.c.tracks.get(i).ticked, false, "row " + i + " starts unticked");
        }
        compare(o.c.grids, true);
        compare(o.c.waveforms, true);
        compare(o.c.replaceEngineEdits, false);
        compare(findChild(o.page, "gridsCheckBox").checked, true);
        compare(findChild(o.page, "waveformsCheckBox").checked, true);
        compare(findChild(o.page, "replaceEditsCheckBox").checked, false);
        const button = findChild(o.page, "stageButton");
        compare(button.text, "Stage Selected");
        compare(button.enabled, false, "nothing ticked: nothing to stage");
        compare(o.c.canStage, false);
        compare(findChild(o.page, "summaryLabel").text,
                "Nothing is ticked. Tick a playlist or tracks to copy their beat grids and waveforms.");
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.Unchecked);
        compare(playlistRow(o.page, "").checkState, Qt.Unchecked);
        for (const name of ["errorLabel", "engineTooOldLabel", "readOnlyLabel", "ledgerWarningLabel",
                            "nothingMatchedLabel", "noPlaylistsLabel", "notInEngineLabel"]) {
            compare(findChild(o.page, name).visible, false, name + " says nothing here");
        }
    }

    // Ticking a playlist's box ticks its tracks, and the summary counts
    // what the controller's previews say of them: 5 grids and 5 waveforms,
    // 4 of the 5 completed (B04 is analysed already).
    function test_tickingAPlaylistTicksItsTracks() {
        const o = openOn(t73());
        waitSettled(o.c);
        clickPlaylistBox(o.page, "T73 Renamed");
        compare(o.c.tickedCount, 5);
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.Checked);
        compare(playlistRow(o.page, "").checkState, Qt.PartiallyChecked, "All tracks: some of it");
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked, "B12 is in T73 Order too");
        compare(playlistRow(o.page, "T73 Folder/T73 New").checkState, Qt.Unchecked);
        for (const id of [b01, b02, b04, b12]) {
            compare(row(o.c, id).ticked, true, id + " is ticked");
        }
        compare(row(o.c, b07).ticked, false);
        waitSettled(o.c);
        compare(o.c.gridsToCopy, 5);
        compare(o.c.waveformsToCopy, 5);
        compare(o.c.tracksToComplete, 4);
        compare(o.c.tracksToWrite, 5);
        compare(o.c.leftAloneCount, 0);
        compare(findChild(o.page, "summaryLabel").text,
                "5 tracks ticked: 5 beat grids and 5 waveforms to copy, 4 tracks then load without analysis.");
        compare(findChild(o.page, "leftAloneLabel").visible, false);
        verify(findChild(o.page, "stageButton").enabled, "something to stage");

        // And off again, all of it.
        clickPlaylistBox(o.page, "T73 Renamed");
        compare(o.c.tickedCount, 0);
        compare(playlistRow(o.page, "").checkState, Qt.Unchecked);
        compare(findChild(o.page, "stageButton").enabled, false);
    }

    // A tick is the track's, not the row's: B12 ticked in one playlist is
    // ticked in the other, and survives switching between them.
    function test_aTrackInTwoPlaylistsIsOneTrack() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        compare(o.c.tracks.count, 5);
        verify(waitForPolish(o.page.Window.window));
        const delegate = delegateInView(o.page, b12);
        clickOn(findChild(delegate, "selectCheckBox"));
        compare(o.c.tickedCount, 1);
        compare(row(o.c, b12).ticked, true);

        showPlaylist(o.page, "T73 Order");
        compare(o.c.tracks.count, 4);
        compare(row(o.c, b12).ticked, true, "ticked in the other playlist too");
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked);
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.PartiallyChecked);
        o.c.setTicked(b12, false);
        showPlaylist(o.page, "T73 Renamed");
        compare(row(o.c, b12).ticked, false, "and unticked in both");
        compare(o.c.tickedCount, 0);
    }

    // What each kind of row reads, in the model and on screen: analysed,
    // not analysed, no beat grid in rekordbox (its .DAT taken off), locked
    // on the player.
    function test_perTrackStates() {
        const stick = t73();
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b04), "B04's grid file is taken off");
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b02), "B02's grid file is taken off");
        compare(stickFixture.lockAnalysedEngineBeatGrids(stick), 5, "the 5 analysed rows are locked");
        const o = openOn(stick);
        waitSettled(o.c);
        // N1: analysed, locked.
        compare(row(o.c, n1).gridState, "Locked on the player");
        compare(row(o.c, n1).waveformState, "Replaces Engine's", "a waveform has no lock");
        compare(row(o.c, n1).note, "");
        // B01: not analysed, completed from rekordbox.
        compare(row(o.c, b01).gridState, "New");
        compare(row(o.c, b01).waveformState, "New");
        compare(row(o.c, b01).note, "Loads without analysis");
        // B04: analysed, no grid in rekordbox.
        compare(row(o.c, b04).gridState, "No beat grid in rekordbox");
        compare(row(o.c, b04).waveformState, "Replaces Engine's");
        // B02: not analysed and no grid: cannot be completed.
        compare(row(o.c, b02).gridState, "No beat grid in rekordbox");
        compare(row(o.c, b02).waveformState, "Left for the player");
        compare(row(o.c, b02).note, "Not analysed by the player yet: rekordbox has no beat grid to complete it with");

        // On screen, the delegate says the same.
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        compare(JSON.stringify(shownBadges(trackDelegate(o.page, b01))), JSON.stringify(["New", "New"]));
        compare(findChild(trackDelegate(o.page, b01), "rowNote").text, "Loads without analysis");
        compare(JSON.stringify(shownBadges(trackDelegate(o.page, b02))),
                JSON.stringify(["No beat grid in rekordbox", "Left for the player"]));
        compare(findChild(trackDelegate(o.page, b04), "bpmLabel").text, "108 BPM");
        // The longest states the page shows, beside the shortest, at its
        // widest and at its narrowest: one shape, the badges in one column.
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "wide");
        o.page.width = 720;
        verify(waitForPolish(o.page.Window.window));
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "720 px");
        o.page.width = 1080;
        verify(waitForPolish(o.page.Window.window));

        // Ticked, the ones that would change nothing are said and grouped.
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 4, "B01, B03, B12 completed and B04's waveform");
        compare(o.c.leftAloneCount, 1);
        compare(findChild(o.page, "leftAloneLabel").text, "One ticked track is left alone: 1 left for the player.");
    }

    // The box that replaces grids set on the player: off, a locked grid is
    // left; on, the states are worked out again and it is replaced.
    function test_alsoReplaceBox() {
        const stick = t73();
        compare(stickFixture.lockAnalysedEngineBeatGrids(stick), 5);
        const o = openOn(stick);
        clickPlaylistBox(o.page, "");
        compare(o.c.tickedCount, 11);
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Locked on the player");
        compare(o.c.gridsToCopy, 6, "only the 6 rows the player has not analysed");
        compare(o.c.waveformsToCopy, 11);
        compare(o.c.leftAloneCount, 0, "every row still gets its waveform");

        const box = findChild(o.page, "replaceEditsCheckBox");
        compare(box.checked, false, "off by default");
        clickOn(box);
        compare(o.c.replaceEngineEdits, true);
        compare(row(o.c, n1).pending, true, "worked out again");
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Replaces Engine's");
        compare(o.c.gridsToCopy, 11);

        clickOn(box);
        compare(o.c.replaceEngineEdits, false);
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Locked on the player", "and back");
        compare(o.c.gridsToCopy, 6);
    }

    // A row the player has not analysed is completed with both parts or
    // not at all: with waveforms off it is left for the player, and says
    // why, in the row and in the summary.
    function test_waveformsOffLeavesUnanalysedRowsForThePlayer() {
        const o = openOn(t73());
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        clickOn(findChild(o.page, "waveformsCheckBox"));
        compare(o.c.waveforms, false);
        waitSettled(o.c);
        compare(row(o.c, b01).gridState, "Left for the player");
        compare(row(o.c, b01).waveformState, "Not chosen");
        compare(row(o.c, b01).note, "Not analysed by the player yet: only completed with both beat grids and waveforms");
        compare(row(o.c, b04).gridState, "Replaces Engine's", "an analysed row still gets its grid");
        compare(row(o.c, b04).waveformState, "Not chosen");
        compare(o.c.gridsToCopy, 1);
        compare(o.c.waveformsToCopy, 0);
        compare(o.c.tracksToComplete, 0);
        compare(o.c.leftAloneCount, 4);
        compare(findChild(o.page, "summaryLabel").text, "5 tracks ticked: one beat grid to copy.");
        compare(findChild(o.page, "leftAloneLabel").text, "4 ticked tracks are left alone: 4 left for the player.");

        clickOn(findChild(o.page, "gridsCheckBox"));
        compare(o.c.grids, false);
        compare(findChild(o.page, "summaryLabel").text, "Choose beat grids, waveforms or both.");
        compare(findChild(o.page, "stageButton").enabled, false);
    }

    // Stage and Save on TESTSHARK: the database really holds the grids,
    // overviews and completed rows, the overview files are there, the
    // ledger is written, the toast says what landed, the page then finds
    // everything already the same. Undo puts the database back byte for
    // byte and takes the new overview files and the ledger away.
    function test_stageSaveThenAlreadyTheSameThenUndo() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        const before = stickFixture.sha256(db);
        const rowsBefore = stickFixture.engineAnalysisRows(stick);
        compare(rowsBefore.analysed, 5);
        compare(rowsBefore.overviewFiles, 0);
        const o = openOn(stick);
        clickPlaylistBox(o.page, "");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 11);
        compare(o.c.tracksToComplete, 6);
        compare(stickFixture.sha256(db), before, "the previews wrote nothing");

        const button = findChild(o.page, "stageButton");
        verify(button.enabled);
        clickOn(button);
        compare(o.c.stagedCount, 11, "one change per track");
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        compare(session.pendingCount, 11);
        compare(session.editorOwner, "beat-grids");
        compare(button.text, "Unstage");
        compare(findChild(o.page, "stagedNote").visible, true);
        compare(findChild(o.page, "waveformsCheckBox").enabled, false, "what to copy waits for the save");
        compare(playlistRow(o.page, "").checkEnabled, false, "and so do the ticks");
        compare(stickFixture.sha256(db), before, "staging wrote nothing");

        const saved = finished(session, () => session.save(), "the save");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 11, JSON.stringify(saved));
        compare(saved.total, 11);
        compare(saved.skipped, 0);
        compare(saved.unit, "tracks");

        // What landed, read from the stick.
        const after = stickFixture.engineAnalysisRows(stick);
        compare(after.rows, 11);
        compare(after.analysed, 11, "the 6 unanalysed rows are completed");
        compare(after.gridsSet, 11);
        compare(after.overviews, 11);
        compare(after.overviewFiles, 11, "an overview file for every row");
        compare(after.matchingFiles, 11, "each over its row's own length");
        compare(stickFixture.keptEngineAnalyses(stick), 5, "the 5 analyses of the player kept first");
        const ledger = stickFixture.readText(stick + "/Seabass/beat-grids.tsv");
        const lines = ledger.split("\n").filter(line => line.length > 0);
        compare(lines[0], "seabass-beat-grids\t1");
        compare(lines.length, 1 + 11, "the ledger records every grid written");

        // The toast says what landed, counted from the changes that did.
        compare(o.c.lastSaveGrids, 11);
        compare(o.c.lastSaveWaveforms, 11);
        compare(o.c.lastSaveCompleted, 6);
        compare(o.c.lastSaveTracks, 11);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing, 5000, "the save is said with the toast");
        compare(toast.text, "11 beat grids and 11 waveforms copied, 6 tracks now load without analysis");
        compare(o.c.stagedCount, 0);

        // A second look finds it all there.
        waitSettled(o.c, "worked out again after the save");
        compare(o.c.tickedCount, 11, "the ticks stay");
        compare(o.c.tracksToWrite, 0);
        compare(o.c.leftAloneCount, 11);
        compare(findChild(o.page, "leftAloneLabel").text, "11 ticked tracks are left alone: 11 already the same.");
        for (const id of [b01, b04, n1]) {
            compare(row(o.c, id).gridState, "Already the same", id);
            compare(row(o.c, id).waveformState, "Already the same", id);
        }
        compare(button.text, "Stage Selected");
        compare(button.enabled, false, "nothing left to copy: the action is off");

        // Undo Last Save: the database back byte for byte, the files the
        // save made gone, the ledger with them.
        const undo = findChild(o.page, "undoLastSaveButton");
        tryVerify(() => undo.visible && undo.enabled, 5000);
        // The save's progress is modal, and a style's exit transition keeps
        // it over the page for a moment: the press goes in once it is gone.
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        const undone = finished(session, () => clickOn(undo), "the undo");
        compare(undone.error, "", JSON.stringify(undone));
        // Said as every page says it (#98), not as the summary's "1 undo
        // steps written".
        tryVerify(() => toast.showing && toast.text === "Changes undone.", 5000, toast.text);
        compare(stickFixture.sha256(db), before, "m.db is as it was");
        const restored = stickFixture.engineAnalysisRows(stick);
        compare(restored.analysed, 5);
        compare(restored.overviewFiles, 0, "the overview files the save made are gone");
        compare(stickFixture.fileExists(stick + "/Seabass/beat-grids.tsv"), false, "and the ledger it made");
        compare(stickFixture.keptEngineAnalyses(stick), 0, "and the kept analyses it made");
        waitSettled(o.c, "worked out again after the undo");
        compare(o.c.tracksToWrite, 11);
        compare(row(o.c, b01).gridState, "New");
    }

    // Copies T73 New's three rows (all analysed by a player), so their
    // analysis is kept, and leaves them ticked.
    function copyT73New(o) {
        clickPlaylistBox(o.page, "T73 Folder/T73 New");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 3);
        compare(o.c.keptTickedCount, 0, "nothing kept before a copy");
        compare(findChild(o.page, "restoreButton").enabled, false, "nothing to restore yet");
        compare(findChild(o.page, "letPlayerAnalyseButton").enabled, true, "three analysed rows");
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        waitSettled(o.c, "worked out again after the copy");
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        return session;
    }

    // Restore Engine's Analysis: the kept note on each row, the count, one
    // change per track, the analysis back and the entries gone; Undo puts
    // the copy back, entries and all.
    function test_restoreEngineAnalysisThenUndo() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        const original = stickFixture.sha256(db);
        const o = openOn(stick);
        const session = copyT73New(o);
        compare(stickFixture.keptEngineAnalyses(stick), 3);
        const copied = stickFixture.sha256(db);
        verify(copied !== original);
        compare(row(o.c, n1).note, "Engine's analysis kept", "the row says its analysis is kept");
        compare(o.c.keptTickedCount, 3);
        compare(o.c.restorableCount, 3);
        compare(findChild(o.page, "engineAnalysisLabel").text,
                "Engine's own analysis is kept for 3 ticked tracks, ready to be put back.");
        showPlaylist(o.page, "T73 Folder/T73 New");
        tryVerify(() => trackDelegate(o.page, n1) !== null
                  && findChild(trackDelegate(o.page, n1), "rowNote").text === "Engine's analysis kept", 5000,
                  "and on screen");
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-kept.png");
        }

        const restore = findChild(o.page, "restoreButton");
        compare(restore.text, "Restore Engine's Analysis");
        verify(restore.enabled);
        clickOn(restore);
        compare(o.c.stagedCount, 3, "one change per track");
        compare(o.c.stagedAction, "restore");
        compare(session.pendingCount, 3);
        compare(restore.enabled, false, "staged: the actions wait");
        compare(findChild(o.page, "letPlayerAnalyseButton").enabled, false);
        compare(findChild(o.page, "stageButton").text, "Unstage");
        compare(stickFixture.sha256(db), copied, "staging wrote nothing");
        const saved = finished(session, () => session.save(), "the restore");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        compare(o.c.lastSaveRestored, 3);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing && toast.text === "Engine's analysis restored for 3 tracks", 5000, toast.text);
        compare(stickFixture.keptEngineAnalyses(stick), 0, "the kept entries are used up");
        compare(stickFixture.engineAnalysisRows(stick).overviewFiles, 0, "the player had no overview files");
        waitSettled(o.c, "worked out again after the restore");
        compare(row(o.c, n1).note, "", "nothing kept any more");
        compare(row(o.c, n1).gridState, "Replaces Engine's", "the player's own grid again");
        compare(o.c.keptTickedCount, 0);

        // Undo: the copy as it was, the kept entries back.
        const undo = findChild(o.page, "undoLastSaveButton");
        tryVerify(() => undo.visible && undo.enabled, 5000);
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        const undone = finished(session, () => clickOn(undo), "the undo");
        compare(undone.error, "", JSON.stringify(undone));
        compare(stickFixture.sha256(db), copied, "m.db is the copy's again");
        compare(stickFixture.keptEngineAnalyses(stick), 3, "the kept analyses are back");
        waitSettled(o.c, "worked out again after the undo");
        compare(row(o.c, n1).note, "Engine's analysis kept");
        compare(o.c.restorableCount, 3);
    }

    // Let the Player Analyse Again: the rows read as unanalysed after it,
    // their kept analysis stays.
    function test_letThePlayerAnalyseAgain() {
        const stick = t73();
        const o = openOn(stick);
        const session = copyT73New(o);
        const button = findChild(o.page, "letPlayerAnalyseButton");
        compare(button.text, "Let the Player Analyse Again");
        compare(o.c.analysedTickedCount, 3);
        clickOn(button);
        compare(o.c.stagedCount, 3);
        compare(o.c.stagedAction, "letPlayerAnalyse");
        const saved = finished(session, () => session.save(), "marking");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        compare(o.c.lastSaveMarked, 3);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing && toast.text === "3 tracks left for the player to analyse", 5000, toast.text);
        compare(stickFixture.engineAnalysisRows(stick).analysed, 2, "B04 and B11 only");
        compare(stickFixture.keptEngineAnalyses(stick), 3, "what is kept stays");
        waitSettled(o.c, "worked out again");
        // As for a track the player has not analysed: completed from rekordbox.
        compare(row(o.c, n1).gridState, "New");
        compare(row(o.c, n1).waveformState, "New");
        compare(row(o.c, n1).note, "Loads without analysis. Engine's analysis kept");
        compare(o.c.analysedTickedCount, 0);
        compare(button.enabled, false, "nothing analysed left to mark");
        compare(o.c.restorableCount, 3, "and it can still be put back");
    }

    // A rekordbox track with no Engine row: the row says so, and the page
    // asks for the libraries to be synced first, with a link to Sync.
    function test_tracksNotInTheEngineLibrary() {
        const stick = t73();
        compare(stickFixture.removeEngineTracks(stick, [1, 2]), 2, "B07 and B08 leave the Engine library");
        const o = openOn(stick);
        waitSettled(o.c);
        compare(o.c.matchedCount, 9);
        compare(o.c.notInEngineShown, 2);
        const label = findChild(o.page, "notInEngineLabel");
        compare(label.visible, true);
        compare(label.text, "2 tracks here are not in the Engine library yet, so nothing can be copied to them. "
                + "Sync the libraries first with <a href=\"sync\">Sync after Rekordbox Export</a>, which also copies "
                + "their beat grids and waveforms as it adds them when both are switched on in Preferences, or come "
                + "back here once the libraries are in step.");
        compare(row(o.c, b07).matched, false);
        compare(row(o.c, b07).note, "Not in the Engine library yet: sync the libraries first");
        o.c.setTicked(b07, true);
        compare(o.c.tickedCount, 0, "it cannot be ticked");
        let asked = null;
        o.page.rekordboxExportSyncRequested.connect((stickLabel, rekordboxPath) => { asked = [stickLabel, rekordboxPath]; });
        label.linkActivated("sync");
        compare(JSON.stringify(asked), JSON.stringify(["TESTSHARK", stick + "/PIONEER"]), "the link asks for Sync");
        verify(waitForPolish(o.page.Window.window));
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-not-in-engine.png");
        }
        showPlaylist(o.page, "T73 Order");
        compare(o.c.notInEngineShown, 2, "B07 and B08 are in T73 Order");
        showPlaylist(o.page, "T73 Folder/T73 New");
        compare(o.c.notInEngineShown, 0);
        compare(label.visible, false, "a list with none says nothing");
        showPlaylist(o.page, "T73 Renamed");
        verify(waitForPolish(o.page.Window.window));
        compare(o.c.notInEngineText, "");
    }

    // What is staged holds no copy of rekordbox's analysis: the save reads
    // it again, and a track whose analysis changed in rekordbox since the
    // preview is skipped, saying so, never written with what the page did
    // not show.
    function test_aSaveWritesOnlyWhatThePreviewShowed() {
        const stick = t73();
        const o = openOn(stick);
        clickPlaylistBox(o.page, "T73 Folder/T73 New");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 3);
        clickOn(findChild(o.page, "stageButton"));
        compare(o.c.stagedCount, 3);
        verify(stickFixture.removeRekordboxBeatGridFile(stick, n1), "N1's beat grid leaves rekordbox's export");
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.skipped, 1, "N1 is skipped: " + JSON.stringify(saved));
        compare(stickFixture.keptEngineAnalyses(stick), 2, "and its row untouched, nothing kept of it");
        compare(o.c.lastSaveTracks, 2);
    }

    // Back from Sync after Rekordbox Export, whose save put the missing
    // tracks into the Engine library: the page reads the libraries again
    // when it is in front again (reloadIfStale(), StackView.onActivated),
    // and keeps the ticks of the tracks that can still be ticked. Without
    // another page's save it reads nothing again.
    function test_backFromAnotherPagesSaveReadsAgain() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        verify(stickFixture.copyFile(db, db + ".all"));
        compare(stickFixture.removeEngineTracks(stick, [1, 2]), 2, "B07 and B08 leave the Engine library");
        const o = openOn(stick);
        waitSettled(o.c);
        compare(o.c.notInEngineShown, 2);
        o.c.setTicked(b04, true);
        o.c.setTicked(b12, true);
        compare(o.c.tickedCount, 2);
        o.c.reloadIfStale();
        compare(o.c.loading, false, "nothing saved meanwhile: nothing read again");

        // Another page's save: the rows are back in the Engine library.
        verify(stickFixture.moveFile(db + ".all", db));
        verify(stickFixture.touch(db));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        session.saveFinished({written: 2, total: 2, unit: "tracks", verb: "synced", cancelled: false, error: "",
                              skipped: 0});
        waitSettled(o.c);
        compare(o.c.notInEngineShown, 2, "not read again behind another page");
        o.c.reloadIfStale();
        tryVerify(() => o.c.loaded && !o.c.loading && o.c.notInEngineShown === 0, 120000,
                  "read again: every track is in both libraries " + o.c.notInEngineShown);
        compare(o.c.matchedCount, 11);
        compare(row(o.c, b07).matched, true);
        compare(o.c.tickedCount, 2, "the ticks stay");
        verify(o.c.isTicked(b04) && o.c.isTicked(b12));
        waitSettled(o.c);
        compare(findChild(o.page, "notInEngineLabel").visible, false);
    }

    // A lost ledger: the ticked tracks already the same go into the next
    // copy's record with the ones it copies, as the command line records
    // them, so the grids Seabass wrote are not taken for the player's.
    function test_aLostLedgerLearnsTheTracksAlreadyTheSame() {
        const stick = t73();
        const o = openOn(stick);
        copyT73New(o);
        const ledgerFile = stick + "/Seabass/beat-grids.tsv";
        const ledgerLines = () => stickFixture.readText(ledgerFile).split("\n").filter(line => line.length > 0);
        compare(ledgerLines().length, 1 + 3);
        verify(stickFixture.moveFile(ledgerFile, ledgerFile + ".lost"));
        verify(o.c.ledgerWarning.length === 0, "a missing ledger is no warning");
        clickPlaylistBox(o.page, "");
        waitSettled(o.c);
        compare(o.c.tickedCount, 11);
        compare(o.c.tracksToWrite, 8, "the 3 copied before are already the same");
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 8);
        compare(ledgerLines().length, 1 + 11, "the record has the 8 copied and the 3 already the same");
    }

    // One left line at the narrowest width the page is laid out for, the
    // title and the row's marks never on top of each other, and a picture
    // of it with a playlist shown and some tracks ticked.
    function test_layoutAtTheNarrowestWidth() {
        const o = openOn(t73(), {width: 720, height: 760});
        showPlaylist(o.page, "T73 Renamed");
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        o.c.setTicked(b12, true);
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window), "the page is laid out");
        const body = findChild(o.page, "pageBody");
        const left = body.mapToItem(o.page, 0, 0).x;
        compare(left, Theme.pageMargin);
        for (const name of ["introLabel", "summaryLabel", "playlistList", "breadcrumb"]) {
            compare(findChild(o.page, name).mapToItem(o.page, 0, 0).x, left, name + " is on the page's left line");
        }
        const box = findChild(findChild(o.page, "gridsCheckBox"), "seabassCheckIndicator");
        compare(box.mapToItem(o.page, 0, 0).x, left, "the first box itself is on the left line");
        for (const name of ["gridsCheckBox", "waveformsCheckBox", "replaceEditsCheckBox"]) {
            verifyBoxLabelGap(o.page, name);
        }
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "720 px, three ticked");

        const list = findChild(o.page, "trackList");
        for (const id of [b01, b02, b04, b12]) {
            const delegate = delegateInView(o.page, id);
            const title = findChild(delegate, "rowTitle");
            const bpm = findChild(delegate, "bpmLabel");
            const titleRight = title.mapToItem(delegate, title.width, 0).x;
            verify(titleRight <= bpm.mapToItem(delegate, 0, 0).x,
                   id + ": the title (to " + titleRight + ") stops before the tempo");
            const marks = findChild(delegate, "rowDetailItems");
            const titleBottom = title.mapToItem(delegate, 0, title.height).y;
            verify(marks.mapToItem(delegate, 0, 0).y >= titleBottom, id + ": the marks are under the title");
            verify(marks.mapToItem(delegate, 0, marks.height).y <= delegate.height, id + ": inside the row");
            verify(delegate.mapToItem(list, delegate.width, 0).x <= list.width, id + ": the row fits the list");
        }
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage.png");
        }

        // Narrower than the boxes fit on one line: the last goes onto a
        // line of its own, whole, rather than squeezing beside the others.
        o.page.width = 480;
        verify(waitForPolish(o.page.Window.window));
        const options = findChild(o.page, "optionsRow");
        const first = findChild(o.page, "gridsCheckBox");
        const last = findChild(o.page, "replaceEditsCheckBox");
        verify(last.y >= first.y + first.height, "the third box is on a line of its own");
        compare(last.x, first.x, "and starts it on the left line");
        verify(last.x + last.width <= options.width, "and fits the page");
        verifyBoxLabelGap(o.page, "replaceEditsCheckBox");
    }

    // The state badges are bounded by their row's title column (`room`,
    // the Flow they sit in), and their widths in turn are part of what
    // that column asks the row for: a loop, if the row gave the column
    // less when it asked for less. It does not, and this says so: at the
    // narrowest width, on a Mac-sized font, each change of the page's
    // width moves `room` at most once, `room` is the title column's
    // width as the row laid it out, and nothing moves afterwards.
    function test_stateMarksSettleInOnePass_data() {
        return [
            {tag: "system font", pointSize: 0},
            {tag: "13pt", pointSize: 13},
        ];
    }
    function test_stateMarksSettleInOnePass(data) {
        SystemFontMetrics.generalPointSizeOverride = data.pointSize;
        const o = openOn(t73(), {width: 800, height: 760});
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        const delegate = delegateInView(o.page, b04);
        const marks = findChild(delegate, "stateMarks");
        const flow = findChild(delegate, "rowDetailItems");
        const column = flow.parent;
        const badge = findChild(findChild(delegate, "gridMark"), "stateBadge");
        verify(badge !== null && badge.visible, "B04 has a beat grid badge");
        let roomMoves = 0;
        let badgeMoves = 0;
        const onRoom = () => ++roomMoves;
        const onBadge = () => ++badgeMoves;
        marks.roomChanged.connect(onRoom);
        badge.widthChanged.connect(onBadge);
        try {
            for (const width of [720, 760, 720]) {
                roomMoves = 0;
                badgeMoves = 0;
                o.page.width = width;
                verify(waitForPolish(o.page.Window.window));
                waitForRendering(o.page);
                // Time for frames more, for anything a second pass would move.
                wait(100);
                verify(roomMoves <= 1, width + " px: room moved " + roomMoves + " times");
                verify(badgeMoves <= 1, width + " px: the badge's width moved " + badgeMoves + " times");
                compare(marks.room, flow.width, width + " px: room is the Flow's width");
                compare(flow.width, column.width, width + " px: and the Flow fills the title column");
                verify(badge.mapToItem(delegate, badge.width, 0).x <= column.mapToItem(delegate, column.width, 0).x + 0.5,
                       width + " px: the badge ends inside the title column");
            }
        } finally {
            marks.roomChanged.disconnect(onRoom);
            badge.widthChanged.disconnect(onBadge);
        }
    }

    // The anonymized library: its export has no waveform for the analysed
    // rows, and the preview of every track is timed for the report.
    function test_anonymizedLibraryHasNoWaveforms() {
        const stick = stickFixture.stickCopy(fixture("anonymized_library"));
        verify(stick.length > 0);
        const o = openOn(stick);
        compare(o.c.matchedCount, 1161);
        const started = Date.now();
        waitSettled(o.c, "every track of All tracks is worked out");
        console.log("Beat Grids & Waveforms: preview of " + o.c.tracks.count + " tracks took "
                    + (Date.now() - started) + " ms");
        let analysed = 0;
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const r = o.c.tracks.get(i);
            if (r.matched && r.gridState === "Replaces Engine's") {
                ++analysed;
                compare(r.waveformState, "No waveform in rekordbox", r.title);
            }
        }
        compare(analysed, 188);
    }

    // The edge states, each with its own sentence.
    function test_engine1xLibrary() {
        const stick = t73();
        // Engine 1.x keeps m.db at the library's root.
        verify(stickFixture.moveFile(stick + "/Engine Library/Database2/m.db", stick + "/Engine Library/m.db"));
        const o = openOn(stick);
        compare(o.c.engineTooOld, true);
        compare(findChild(o.page, "engineTooOldLabel").visible, true);
        compare(findChild(o.page, "panes").visible, false, "nothing to tick");
        compare(findChild(o.page, "optionsRow").visible, false);
        compare(findChild(o.page, "summaryRow").visible, false);
    }

    function test_unreadableLedgerIsSaid() {
        const stick = t73();
        verify(stickFixture.makeDirectory(stick + "/Seabass"));
        verify(stickFixture.writeText(stick + "/Seabass/beat-grids.tsv", "not a ledger\n"));
        const o = openOn(stick);
        waitSettled(o.c);
        const label = findChild(o.page, "ledgerWarningLabel");
        compare(label.visible, true);
        verify(label.text.indexOf("Seabass's record of the beat grids it copied before cannot be read") === 0,
               label.text);
    }

    function test_readOnlyStickTicksNothing() {
        const o = openOn(t73(), {stickReadOnly: true});
        waitSettled(o.c);
        compare(findChild(o.page, "readOnlyLabel").visible, true);
        compare(playlistRow(o.page, "T73 Renamed").checkEnabled, false);
        compare(findChild(o.page, "gridsCheckBox").enabled, false);
        o.c.setPlaylistTicked("T73 Renamed", true);
        waitSettled(o.c);
        compare(findChild(o.page, "stageButton").enabled, false, "a read-only stick takes no copy");
    }

    // ---- the rows' waveforms ---------------------------------------------

    function rowWaveformOf(delegate) {
        const view = findChild(delegate, "rowSourceWaveform");
        verify(view !== null, "the row has a waveform slot");
        return view;
    }
    // A waveform's columns as a string, to tell one track's from another's.
    function columnsKey(analysis) {
        if (!analysis || analysis.ready !== true || !analysis.columns) {
            return "";
        }
        return Array.from(new Uint8Array(analysis.columns)).join(",");
    }

    // A row shows rekordbox's waveform with its beat grid as lines; a row
    // whose track rekordbox has no analysis of (its .DAT taken off) shows
    // none and offers no click. The waveform is lower than the row's text,
    // so the row is no taller for it. A click on it opens the zoomed
    // popup with that track loaded, and ticks nothing.
    function test_rowWaveformAndItsPopup() {
        const stick = t73();
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b02), "B02's analysis file is taken off");
        const o = openOn(stick, {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);

        const withRow = delegateInView(o.page, b01);
        const withView = rowWaveformOf(withRow);
        tryVerify(() => withView.visible, 30000, "B01's waveform is drawn");
        compare(withView.trackId, b01);
        verify(withView.byteColumns > 0, "with rekordbox's columns");
        compare(withView.gridStyle, "lines");
        verify(withView.beatTicks.length > 0, "and its beat grid's lines: " + withView.beatTicks.length);
        verify(withView.trackDurationMs > 0, "over the track's length");
        compare(withView.pointerShape, Qt.PointingHandCursor, "a hand: there is a file to play");

        const withoutRow = delegateInView(o.page, b02);
        const withoutView = rowWaveformOf(withoutRow);
        tryVerify(() => o.c.rowAnalysis(b02).ready === true, 30000, "B02's analysis is read");
        verify(waitForPolish(o.page.Window.window));
        compare(withoutView.visible, false, "no waveform for a track rekordbox has not analysed");
        compare(withoutView.opensZoom, false);
        compare(withoutView.pointerShape, Qt.ArrowCursor, "and no hand");

        // The rows keep one shape, and the waveform fits beside the text.
        compare(withRow.height, withoutRow.height, "a row with a waveform is as tall as one without");
        const slot = withView.parent;
        const textColumn = findChild(withRow, "rowTitle").parent;
        verify(slot.height <= textColumn.height,
               "the waveform (" + slot.height + ") is no taller than the row's text (" + textColumn.height + ")");

        // A click on B02's empty slot opens nothing.
        const popup = findChild(o.page, "waveformZoomPopup");
        verify(popup !== null, "the page has one popup");
        clickOn(withoutView.parent);
        compare(popup.visible, false, "nothing to open on a row without a waveform");
        compare(o.c.isTicked(b02), false);

        // A click on B01's opens the popup with B01 in the player.
        compare(o.c.isTicked(b01), false);
        const box = findChild(withRow, "selectCheckBox");
        clickOn(withView);
        tryCompare(popup, "opened", true, 10000, "the zoomed popup opens");
        compare(realPlayback.currentFormat, "rekordbox");
        compare(realPlayback.currentSourceId, b01, "with the clicked row's track");
        compare(realPlayback.currentLibraryPath, stick + "/PIONEER");
        compare(findChild(popup, "zoomTitle").text, row(o.c, b01).title);
        compare(o.c.isTicked(b01), false, "the click ticks nothing");
        compare(box.checked, false, "and the row's box stays as it was");
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // A row's read is the track's it was asked for and lands on that row
    // only: B01's read, under way when its row is replaced by another
    // playlist's B07 in the same place, is kept for B01 and never drawn on
    // B07's row. The other rows of the playlist left before their turn are
    // never read.
    // A page with T73 New shown and only its three rows read. Every track
    // is listed at first, and its row read: the libraries are read again
    // with only T73 New shown, which forgets what was read.
    function openOnT73NewOnly() {
        const o = openOn(t73());
        waitSettled(o.c);
        showPlaylist(o.page, "T73 Folder/T73 New");
        o.c.load(o.page.rekordboxPath, o.page.enginePath);
        tryVerify(() => o.c.loaded && !o.c.loading, 120000, "the libraries are read again");
        waitSettled(o.c);
        compare(o.c.shownPlaylist, "T73 Folder/T73 New");
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const id = o.c.tracks.get(i).rekordboxId;
            tryVerify(() => o.c.rowAnalysis(id).ready === true, 30000, id + " is read");
        }
        return o;
    }
    function test_aLateReadLandsOnItsOwnTrackOnly() {
        const o = openOnT73NewOnly();
        compare(o.c.rowAnalysis(b01).ready, false, "B01 is not read yet");
        const list = findChild(o.page, "trackList");
        const ready = createTemporaryObject(signalSpyComponent, testCase,
                                            {target: o.c, signalName: "rowAnalysisReady"});
        // Built at once, B01 first, so its read starts first; then another
        // playlist before anything has landed.
        const reads = o.c.rowAnalysisReads;
        o.c.shownPlaylist = "T73 Renamed";
        verify(trackDelegate(o.page, b01) !== null, "B01's row is built at once");
        compare(o.c.rowAnalysisReads, reads + 1, "one read at a time: B01's");
        compare(ready.count, 0, "and nothing has landed yet");
        const leftEarly = [];
        for (let i = 1; i < o.c.tracks.count; ++i) {
            leftEarly.push(o.c.tracks.get(i).rekordboxId);
        }
        verify(leftEarly.length > 0);
        o.c.shownPlaylist = "T73 Order";
        compare(o.c.tracks.get(0).rekordboxId, b07, "B07 is first in T73 Order");
        const b07Row = trackDelegate(o.page, b07);
        verify(b07Row !== null, "B07's row is built at once, where B01's was");
        // What B07's row draws the moment B01's read lands (the rows take
        // an answer before this handler runs: they connected first).
        let atB01 = null;
        const onReady = (id) => {
            if (id === b01 && atB01 === null) {
                atB01 = {id: b07Row.analysisId, columns: columnsKey(b07Row.analysis)};
            }
        };
        o.c.rowAnalysisReady.connect(onReady);

        tryVerify(() => {
            for (let i = 0; i < ready.count; ++i) {
                if (ready.signalArguments[i][0] === b01) {
                    return true;
                }
            }
            return false;
        }, 30000, "B01's read lands after its row has gone");
        o.c.rowAnalysisReady.disconnect(onReady);
        verify(atB01 !== null);
        verify(columnsKey(o.c.rowAnalysis(b01)).length > 0, "B01 has a waveform");
        verify(atB01.columns !== columnsKey(o.c.rowAnalysis(b01)), "B01's columns never land on B07's row");
        verify(atB01.id !== b01, "nor is B07's row said to draw B01's");
        tryVerify(() => o.c.rowAnalysis(b07).ready === true, 30000, "B07's is read for its row");
        verify(waitForPolish(o.page.Window.window));
        const first = delegateInView(o.page, b07);
        const view = rowWaveformOf(first);
        tryVerify(() => view.visible, 10000, "B07's row draws its waveform");
        compare(view.trackId, b07, "B07's row draws B07's analysis");
        compare(columnsKey(first.analysis), columnsKey(o.c.rowAnalysis(b07)), "B07's columns");
        verify(columnsKey(first.analysis) !== columnsKey(o.c.rowAnalysis(b01)), "not B01's");
        for (let i = 0; i < list.contentItem.children.length; ++i) {
            const item = list.contentItem.children[i];
            if (item.analysisId !== undefined && item.analysisId.length > 0) {
                compare(item.analysisId, item.rekordboxId, item.objectName + " draws its own track's");
            }
        }
        for (const id of leftEarly) {
            if (o.c.tracks.indexOf(id) < 0) {
                compare(o.c.rowAnalysis(id).ready, false, "T73 Renamed's " + id + " left before its turn: never read");
            }
        }
    }

    // Nothing is read for a row while a library is being written: a row
    // that asks then waits, and is read once the write is over.
    function test_noRowReadWhileWriting() {
        const o = openOnT73NewOnly();
        compare(o.c.rowAnalysis(b07).ready, false, "B07 is not read yet");
        o.c.setTicked(n1, true);
        waitSettled(o.c);
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        let reads = -1;
        let asked = false;
        const saved = finished(session, () => {
            session.save();
            verify(EditSessionRegistry.anyWriting, "the save is writing");
            reads = o.c.rowAnalysisReads;
            // B07 is in no row shown: asked for as a row would.
            o.c.requestRowAnalysis(b07);
            asked = true;
            compare(o.c.rowAnalysisReads, reads, "no read starts while the library is written");
        }, "the copy");
        verify(asked);
        compare(saved.error, "", JSON.stringify(saved));
        tryVerify(() => o.c.rowAnalysis(b07).ready === true, 30000, "read once the write is over");
        verify(o.c.rowAnalysisReads > reads);
        o.c.releaseRowAnalysis(b07);
    }

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }

    // A picture of the rows with their waveforms, at the page's usual width.
    function test_rowWaveformsScreenshot() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        for (const id of [b01, b04, b12]) {
            tryVerify(() => o.c.rowAnalysis(id).ready === true, 30000, id + " is read");
        }
        verify(waitForPolish(o.page.Window.window));
        tryVerify(() => rowWaveformOf(trackDelegate(o.page, b01)).visible, 10000);
        waitForRendering(o.page);
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-row-waveforms.png");
        }
    }
}
