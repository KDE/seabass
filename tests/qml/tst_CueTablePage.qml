// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Library Health's "OneLibrary cue tables" page (#57), driven by a
// stand-in controller, and the real controller's staging across a rescan
// on a copy of the anonymized fixture.
TestCase {
    id: testCase
    name: "CueTablePage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property bool cueTablesChecked: true
            property string cueTablesError: ""
            property var cueTableRows: [
                {contentId: "392", title: "Alpha", artist: "Someone", filePath: "/S/Contents/a.mp3",
                 extra: "pad B at 1:07.751, pad E at 2:15.251",
                 fileCues: "pad A at 0:00.247, pad C at 0:01.188, pad D at 0:01.657, memory cue at 0:04.399", staged: false},
                {contentId: "17", title: "", artist: "", filePath: "/S/Contents/b.mp3",
                 extra: "pad A at 0:30.000", fileCues: "none", staged: false}]
            property int cueTableRowCount: cueTableRows.length
            property int cueTableStagedCount: 0
            property int cueTableLeftAloneCount: 0
            property var calls: []
            function restage(ids, on) {
                cueTableRows = cueTableRows.map(r => Object.assign({}, r, {staged: ids.indexOf(r.contentId) >= 0 ? on : r.staged}));
                cueTableStagedCount = cueTableRows.filter(r => r.staged).length;
            }
            function repairCueTables(ids) {
                calls.push("repair " + ids.join(","));
                restage(ids.length === 0 ? cueTableRows.map(r => r.contentId) : ids, true);
            }
            function unstageCueTable(id) {
                calls.push("unstage " + id);
                restage([id], false);
            }
            function unstageCueTables() {
                calls.push("unstage all");
                restage(cueTableRows.map(r => r.contentId), false);
            }
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            function scan(a, b, c) {}
            function cancelScan() {}
            function undoLastOperation() {}
        }
    }

    Component {
        id: realControllerComponent
        LibraryConsistencyController {}
    }
    // The anonymized fixture spells its paths differently per library, so
    // most of its tracks are in OneLibrary alone, and an add asks first
    // (additionNeedsLibraries). The tests not about that answer "add
    // anyway", as someone would who knows; askingEditComponent does not.
    Component {
        id: playlistEditComponent
        PlaylistEditController {
            onAdditionNeedsLibraries: (playlist, filePath) => addAnyway(playlist, filePath)
        }
    }
    Component {
        id: askingEditComponent
        PlaylistEditController {}
    }
    Component {
        id: scanControllerComponent
        ScanController {}
    }

    // A real controller on a fresh copy of the fixture, scanned. `plant`
    // runs on the copy first.
    function scannedCopy(plant) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const planted = plant ? plant(stick) : null;
        const controller = createTemporaryObject(realControllerComponent, testCase);
        const pioneer = stick + "/PIONEER";
        controller.scan(pioneer, stick + "/Engine Library");
        tryCompare(controller, "cueTablesChecked", true, 120000);
        tryCompare(controller, "busy", false, 120000);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(pioneer));
        verify(session !== null);
        return {stick: stick, pioneer: pioneer, controller: controller, session: session, planted: planted};
    }

    // A playlist edit is looked up on the stick off the GUI thread and
    // staged when that is done: whether it was, once it is.
    function edited(browse, accepted) {
        if (!accepted) {
            return false;
        }
        tryCompare(browse, "busy", false, 60000);
        return browse.errorMessage.length === 0;
    }

    function saveAndWait(session) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_JunkCuePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        CueTablePage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    // Built from character codes, so this file holds no dash literal.
    function noDashes(text) {
        return text.indexOf(String.fromCharCode(0x2014)) < 0 && text.indexOf("-".repeat(2)) < 0;
    }

    function makePage(props) {
        const controller = createTemporaryObject(controllerComponent, testCase, props || {});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null);
        waitForRendering(page);
        return {page: page, controller: controller};
    }

    // Sixty tracks with a disagreeing cue table do not fit the page: the
    // list scrolls, with room under the last row for the floating Save
    // button, and the last row can be brought on screen.
    function test_aLongListOfRowsScrolls() {
        const many = [];
        for (let i = 0; i < 60; ++i) {
            many.push({contentId: String(1000 + i), title: "Track " + i, artist: "Someone",
                       filePath: "/S/Contents/t" + i + ".mp3", extra: "pad B at 1:07.751",
                       fileCues: "pad A at 0:00.247", staged: false});
        }
        const t = makePage({cueTableRows: many});
        const page = t.page;
        const view = findChild(page, "cueTableRowsView");
        verify(view !== null, "the list is a view");
        tryVerify(() => view.contentHeight > view.height, 5000, "more than fits");
        verify(view.interactive, "and so it scrolls");
        verify(page.saveClearance > 0);
        verify(view.bottomMargin >= page.saveClearance - 0.5, "room for the Save button under the last row");
        view.contentY = view.contentHeight + view.bottomMargin - view.height;
        const rows = findChild(page, "cueTableRows");
        const last = rows.itemAt(59);
        verify(last !== null);
        const bottom = last.mapToItem(view, 0, last.height).y;
        verify(bottom <= view.height + 0.5 && bottom > 0, "the last row is on screen at the end: " + bottom);
        verify(bottom <= view.height - page.saveClearance + 0.5, "and clear of the Save button: " + bottom);
    }

    function test_oneRowStagesThatRowAndUnstagesIt() {
        const t = makePage();
        const summary = findChild(t.page, "cueTableSummary");
        compare(summary.text, "2 tracks' OneLibrary cue table holds cues its analysis file does not.");
        verify(findChild(t.page, "cueTableExplanation").visible);
        compare(findChild(t.page, "extra_392").text, "Only in the table: pad B at 1:07.751, pad E at 2:15.251");
        // Looked up again after each press: a new list of rows makes new
        // delegates.
        compare(findChild(t.page, "repair_392").text, "Repair");
        findChild(t.page, "repair_392").clicked();
        compare(t.controller.calls[0], "repair 392", "only the row pressed is staged");
        tryCompare(findChild(t.page, "repair_392"), "text", "Unstage");
        compare(findChild(t.page, "repair_17").text, "Repair", "the other row stays as it was");
        compare(findChild(t.page, "repairAllButton").text, "Repair All");
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(t.page);
            grabImage(t.page).save(screenshotDir + "/cue-table-page.png");
        }
        findChild(t.page, "repair_392").clicked();
        compare(t.controller.calls[1], "unstage 392");
        tryCompare(findChild(t.page, "repair_392"), "text", "Repair");
        for (const label of [summary, findChild(t.page, "cueTableExplanation"), findChild(t.page, "extra_392")]) {
            verify(noDashes(label.text), "no dashes on screen");
        }
    }

    function test_repairAllStagesEveryRowAndUnstageAllTakesThemBack() {
        const t = makePage();
        const all = findChild(t.page, "repairAllButton");
        all.clicked();
        compare(t.controller.calls[0], "repair ", "an empty list means every listed row");
        compare(t.controller.cueTableStagedCount, 2);
        compare(all.text, "Unstage All");
        tryCompare(findChild(t.page, "repair_17"), "text", "Unstage");
        all.clicked();
        compare(t.controller.calls[1], "unstage all");
        compare(t.controller.cueTableStagedCount, 0);
    }

    function test_aReadOnlyStickOffersNoRepair() {
        const t = makePage({stickReadOnly: true});
        compare(findChild(t.page, "repairAllButton").enabled, false);
        compare(findChild(t.page, "repair_392").enabled, false);
    }

    function test_nothingListedSaysSo() {
        const t = makePage({cueTableRows: []});
        compare(findChild(t.page, "cueTableSummary").text,
                "No OneLibrary cue table on this stick holds cues its analysis file does not.");
        compare(findChild(t.page, "cueTableExplanation").visible, false);
        compare(findChild(t.page, "repairAllButton").visible, false);
    }

    function test_rowsLeftAloneAreCountedNotListed() {
        const t = makePage({cueTableLeftAloneCount: 3});
        const note = findChild(t.page, "cueTableLeftAlone");
        verify(note.visible);
        compare(note.text, "3 tracks are left alone: Seabass could not read the analysis file, or the table holds "
                + "cues of a kind Seabass does not know.");
        verify(noDashes(note.text), "no dashes on screen");
        verify(findChild(t.page, "repair_392") !== null && findChild(t.page, "repair_17") !== null,
               "only the listed rows have a button");
    }

    // The fixture's one damaged row, staged through Repair All, then a
    // rescan: the rows are read again, and a staged repair must not
    // outlive its row on screen (nor stay pending in the session).
    function test_aRescanLeavesNothingStagedWithoutARow() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const controller = createTemporaryObject(realControllerComponent, testCase);
        const pioneer = stick + "/PIONEER";
        controller.scan(pioneer, stick + "/Engine Library");
        tryCompare(controller, "cueTablesChecked", true, 120000);
        tryCompare(controller, "busy", false, 120000);
        compare(controller.cueTableRowCount, 1, "content_id 392 alone");
        compare(controller.cueTableRows[0].contentId, "392");
        compare(controller.cueTableRows[0].extra, "pad B at 1:07.751, pad E at 2:15.251");
        compare(controller.cueTableLeftAloneCount, 0);
        controller.repairCueTables([]);
        compare(controller.cueTableStagedCount, 1);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(pioneer));
        verify(session !== null);
        compare(session.pendingCount, 1, "one change for every staged row");
        controller.scan(pioneer, stick + "/Engine Library");
        compare(controller.cueTableStagedCount, 0, "nothing staged while no row is listed");
        compare(session.pendingCount, 0, "the change went with its rows");
        tryCompare(controller, "cueTablesChecked", true, 120000);
        tryCompare(controller, "busy", false, 120000);
        compare(controller.cueTableRowCount, 1);
        compare(controller.cueTableRows[0].staged, false);
        compare(controller.cueTableStagedCount, 0);
        compare(session.pendingCount, 0);

        // And through to the stick: staged again and saved, the save's
        // rescan lists nothing.
        controller.repairCueTables(["392"]);
        compare(session.pendingCount, 1);
        let summary = null;
        session.saveFinished.connect((result) => { summary = result; });
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        compare(summary.error, "", "the save wrote it: " + JSON.stringify(summary));
        tryVerify(() => !controller.busy && controller.cueTablesChecked, 300000, "the rescan finishes");
        compare(controller.cueTableRowCount, 0, "the repaired row is no longer listed");
        compare(controller.cueTableStagedCount, 0);
    }

    // While a save runs nothing staged moves: taking a row back is
    // refused and said, and a rescan asked for then leaves the staged
    // repair to the save, which writes it.
    function test_aSaveRunningKeepsWhatIsStaged() {
        const t = scannedCopy(null);
        t.controller.repairCueTables(["392"]);
        compare(t.session.pendingCount, 1);
        let summary = null;
        const done = (result) => { summary = result; };
        t.session.saveFinished.connect(done);
        t.session.save();
        verify(t.session.writing, "the save is running");
        t.controller.unstageCueTable("392");
        verify(t.controller.errorMessage.length > 0, "refused, and said");
        compare(t.controller.cueTableStagedCount, 1, "still staged");
        compare(t.session.pendingCount, 1);
        t.controller.scan(t.pioneer, t.stick + "/Engine Library");
        tryVerify(() => summary !== null, 120000, "the save finishes");
        t.session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, "the staged repair was written: " + JSON.stringify(summary));
        tryVerify(() => !t.controller.busy && t.controller.cueTablesChecked, 300000, "the rescan finishes");
        compare(t.controller.cueTableRowCount, 0);
        compare(t.controller.cueTableStagedCount, 0);
        compare(t.session.pendingCount, 0);
    }

    // Another page's edit holds the session: staging is refused, nothing
    // is marked staged, and the page says why.
    function test_aRefusedStageMarksNothingAndSaysWhy() {
        const t = scannedCopy(null);
        const browse = createTemporaryObject(playlistEditComponent, testCase);
        verify(edited(browse, browse.deletePlaylist(t.pioneer, t.stick + "/Engine Library", "Playlist 000")),
               "Browse stages a playlist deletion: " + browse.errorMessage);
        compare(t.session.pendingCount, 1);
        t.controller.repairCueTables(["392"]);
        compare(t.session.pendingCount, 1, "only Browse's change");
        compare(t.controller.cueTableStagedCount, 0);
        verify(t.controller.errorMessage.length > 0, "the page says the repair was not staged");
        browse.keepPlaylist("Playlist 000");
        compare(t.session.pendingCount, 0);
    }

    // Browse's "Create new playlist" and "Add to" with the real
    // controller: names it refuses; the one it takes is staged, and so is
    // a track added to it, which Browse's list shows in it, flagged, before
    // anything is written; Save makes both in every library on the copy
    // (so asking again is refused, naming them), and Browse lists the
    // playlist in each library, an empty one included.
    function test_aNewPlaylistAndItsTracksShowUntilSaveWritesThem() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const browse = createTemporaryObject(playlistEditComponent, testCase);
        verify(!edited(browse, browse.createPlaylist(t.pioneer, engine, "Playlist 000")), "a name the stick has");
        verify(browse.errorMessage.indexOf("already a playlist") >= 0, browse.errorMessage);
        verify(!edited(browse, browse.createPlaylist(t.pioneer, engine, "  ")), "no name");
        verify(!edited(browse, browse.createPlaylist(t.pioneer, engine, "AC/DC")), "a folder path");
        compare(t.session.pendingCount, 0);
        verify(edited(browse, browse.createPlaylist(t.pioneer, engine, "  Fresh Picks ")), browse.errorMessage);
        verify(!t.session.writing, "nothing written before Save");
        compare(JSON.stringify(browse.newPlaylists), JSON.stringify(["Fresh Picks"]), "trimmed");
        verify(browse.isNewPlaylistStaged("Fresh Picks"));
        verify(!edited(browse, browse.createPlaylist(t.pioneer, engine, "Fresh Picks")), "asked twice");
        verify(edited(browse, browse.createPlaylist(t.pioneer, engine, "Left Empty")), browse.errorMessage);

        // A track into it, shown there by Browse's list before Save.
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("onelibrary", t.pioneer);
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "onelibrary scanned");
        scan.filterByPlaylist("Fresh Picks");
        compare(scan.tracks.trackCount(), 0, "nothing in it yet");
        const file = t.stick + "/Contents/795079.mp3";
        // A playlist with tracks that does not hold the file yet.
        scan.filterByPlaylist("");
        let fileRow = null;
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            if (scan.tracks.trackAt(i).filePath === file) {
                fileRow = scan.tracks.trackAt(i);
            }
        }
        verify(fileRow !== null, "OneLibrary has a row for " + file);
        const full = scan.playlistNames.filter(n => scan.playlistTrackCounts[n] > 0
                                                    && fileRow.playlistNames.indexOf(n) < 0)[0];
        verify(full !== undefined, JSON.stringify(scan.playlistTrackCounts));
        scan.filterByPlaylist(full);
        const inIt = scan.tracks.trackCount();
        verify(inIt > 0);
        verify(scan.tracks.trackAt(0).pendingAddition === false);
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, "Fresh Picks", file, "A Track")), browse.errorMessage);
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, full, file, "A Track")), browse.errorMessage);
        compare(browse.lastAddPlaylist, full);
        compare(createTemporaryObject(playlistEditComponent, testCase).lastAddPlaylist, full,
                "remembered by a fresh controller (a restart)");
        compare(t.session.pendingCount, 4);
        compare(browse.pendingAdditions["Fresh Picks"].join(), file);
        compare(browse.trackCountChange("Fresh Picks"), 1);
        verify(browse.isPlaylistEdited(full));
        verify(!browse.isPlaylistEdited("Left Empty"));
        const other = scan.playlistNames.filter(n => n !== full && scan.playlistTrackCounts[n] > 0)[0];
        scan.filterByPlaylist(other);
        const outFile = scan.tracks.trackAt(0).filePath;
        verify(edited(browse, browse.removeFromPlaylist(t.pioneer, engine, other, outFile, "Out")), browse.errorMessage);
        compare(browse.trackCountChange(other), -1, "one staged out");
        verify(browse.isPlaylistEdited(other));
        browse.keepInPlaylist(other, outFile);
        verify(!browse.isPlaylistEdited(other));
        compare(browse.trackCountChange(other), 0);
        scan.filterByPlaylist(full);
        compare(browse.pendingAdditions[full].join(), file);
        scan.setPendingEdits(browse.pendingAdditions, browse.pendingMoves);
        compare(scan.tracks.trackCount(), inIt + 1, "added at the end of " + full);
        const added = scan.tracks.trackAt(inIt);
        compare(added.filePath, file);
        verify(added.pendingAddition === true, "flagged");
        scan.filterByPlaylist("Fresh Picks");
        compare(scan.tracks.trackCount(), 1, "and shown in the new playlist");
        verify(scan.tracks.trackAt(0).pendingAddition === true);
        scan.filterByPlaylist("");
        verify(scan.tracks.trackCount() > 1);
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            verify(scan.tracks.trackAt(i).pendingAddition === false, "no flag outside the playlist");
        }
        browse.keepOutOfPlaylist(full, file);
        scan.setPendingEdits(browse.pendingAdditions, browse.pendingMoves);
        scan.filterByPlaylist(full);
        compare(scan.tracks.trackCount(), inIt, "taken back");

        // A new playlist dropped takes the tracks staged into it along.
        // Asked back to back: the add waits for the create's turn, and
        // sees it staged.
        verify(browse.createPlaylist(t.pioneer, engine, "Gone Again"));
        verify(browse.addToPlaylist(t.pioneer, engine, "Gone Again", file, "A Track"));
        verify(browse.busy, "looked up off the GUI thread");
        verify(edited(browse, true), browse.errorMessage);
        compare(t.session.pendingCount, 5);
        browse.dropNewPlaylist("Gone Again");
        compare(t.session.pendingCount, 3, "the create and its add both gone");
        verify(!browse.isAdditionStaged("Gone Again", file));
        compare(Object.keys(browse.pendingAdditions).join(), "Fresh Picks");

        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 3, JSON.stringify(summary));
        compare(JSON.stringify(browse.pendingAdditions), "{}", "written, so no longer pending");
        compare(browse.lastSavedSummary, "2 playlists saved: 2 created, 1 track added");
        compare(browse.newPlaylists.length, 0);
        // The anonymized fixture spells its paths differently per library,
        // so only OneLibrary has a row for this file: the log says why the
        // other two were left alone.
        const log = stickFixture.readText(t.stick + "/Seabass/seabass.log");
        verify(log.indexOf("playlist-create: created \"Fresh Picks\" in 3 libraries") >= 0, log);
        verify(log.indexOf("playlist-create: created \"Left Empty\" in 3 libraries") >= 0, log);
        verify(log.indexOf("to \"Fresh Picks\" in 1 libraries") >= 0, log);
        verify(log.indexOf("not added to \"Fresh Picks\" there: rekordbox has no row for the file") >= 0, log);
        verify(log.indexOf("not added to \"Fresh Picks\" there: Engine has no row for the file") >= 0, log);
        verify(!edited(browse, browse.createPlaylist(t.pioneer, engine, "Fresh Picks")), "the stick has it now");
        compare(browse.errorMessage, "There is already a playlist \"Fresh Picks\" (in rekordbox, OneLibrary, Engine).");

        // Browse lists both in every library, the empty one too.
        for (const [format, path] of [["rekordbox", t.pioneer], ["onelibrary", t.pioneer], ["engine", engine]]) {
            const again = createTemporaryObject(scanControllerComponent, testCase);
            again.scan(format, path);
            tryVerify(() => !again.busy && again.playlistNames.length > 0, 120000, format + " scanned");
            for (const name of ["Fresh Picks", "Left Empty", "Playlist 000"]) {
                verify(again.playlistNames.indexOf(name) >= 0, format + " lists " + name + ": "
                       + JSON.stringify(again.playlistNames));
            }
        }
    }

    // Browse's move within a playlist with the real controllers: staged,
    // the list shows the track at its new place, flagged, the others closed
    // up around it; moved again, the one change is replaced; taken back,
    // the old order; Save writes it, and a rescan reads the new order off
    // the stick.
    function test_aTrackMovedWithinAPlaylistShowsThereUntilSaveWritesIt() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const browse = createTemporaryObject(playlistEditComponent, testCase);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("onelibrary", t.pioneer);
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "onelibrary scanned");
        const order = () => {
            const files = [];
            for (let i = 0; i < scan.tracks.trackCount(); ++i) {
                files.push(scan.tracks.trackAt(i).filePath);
            }
            return files;
        };
        // A playlist of four or more different files.
        const playlist = scan.playlistNames.filter(n => {
            if (scan.playlistTrackCounts[n] < 4) {
                return false;
            }
            scan.filterByPlaylist(n);
            return new Set(order()).size === order().length;
        })[0];
        verify(playlist !== undefined, JSON.stringify(scan.playlistTrackCounts));
        scan.filterByPlaylist(playlist);
        const before = order();
        const moving = before[0];
        const moved = (files, from, to) => {
            const copy = files.slice();
            const file = copy.splice(from, 1)[0];
            copy.splice(to, 0, file);
            return copy;
        };
        const refresh = () => scan.setPendingEdits(browse.pendingAdditions, browse.pendingMoves);

        verify(edited(browse, browse.moveInPlaylist(t.pioneer, engine, playlist, moving, "Moving", 2)), browse.errorMessage);
        compare(t.session.pendingCount, 1);
        verify(browse.isMoveStaged(playlist, moving));
        verify(browse.isPlaylistEdited(playlist), "a move edits the playlist");
        compare(browse.trackCountChange(playlist), 0, "a move changes no count");
        compare(JSON.stringify(browse.pendingMoves[playlist]), JSON.stringify([{filePath: moving, toIndex: 2}]));
        refresh();
        compare(JSON.stringify(order()), JSON.stringify(moved(before, 0, 2)), "shown at its new place");
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            compare(scan.tracks.trackAt(i).pendingMove, i === 2, "only the moved row is flagged: row " + i);
            verify(scan.tracks.trackAt(i).pendingAddition === false);
        }
        scan.filterByPlaylist("");
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            verify(scan.tracks.trackAt(i).pendingMove === false, "no flag outside the playlist");
        }
        scan.filterByPlaylist(playlist);

        verify(edited(browse, browse.moveInPlaylist(t.pioneer, engine, playlist, moving, "Moving", 1)), browse.errorMessage);
        compare(t.session.pendingCount, 1, "moved again: the one change replaced");
        compare(JSON.stringify(browse.pendingMoves[playlist]), JSON.stringify([{filePath: moving, toIndex: 1}]));
        refresh();
        compare(JSON.stringify(order()), JSON.stringify(moved(before, 0, 1)));

        browse.keepPositionInPlaylist(playlist, moving);
        compare(t.session.pendingCount, 0);
        verify(!browse.isMoveStaged(playlist, moving));
        verify(!browse.isPlaylistEdited(playlist));
        refresh();
        compare(JSON.stringify(order()), JSON.stringify(before), "taken back: the old order");
        verify(scan.tracks.trackAt(0).pendingMove === false);

        verify(edited(browse, browse.moveInPlaylist(t.pioneer, engine, playlist, moving, "Moving", 2)), browse.errorMessage);
        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        compare(JSON.stringify(browse.pendingMoves), "{}", "written, so no longer pending");

        const again = createTemporaryObject(scanControllerComponent, testCase);
        again.scan("onelibrary", t.pioneer);
        tryVerify(() => !again.busy && again.playlistNames.length > 0, 120000, "onelibrary scanned again");
        again.filterByPlaylist(playlist);
        const written = [];
        for (let i = 0; i < again.tracks.trackCount(); ++i) {
            written.push(again.tracks.trackAt(i).filePath);
            verify(again.tracks.trackAt(i).pendingMove === false);
        }
        compare(JSON.stringify(written), JSON.stringify(moved(before, 0, 2)), "the stick holds the new order");
        // All three libraries have the playlist, but the anonymized fixture
        // spells its paths differently per library, so only OneLibrary's
        // copy holds the file: it is the one written, the other two say why.
        const log = stickFixture.readText(t.stick + "/Seabass/seabass.log");
        verify(log.indexOf("playlist-move: moved \"" + moving + "\" to position 3 in \"" + playlist + "\" in 1 libraries")
               >= 0, log);
        for (const library of ["rekordbox", "Engine"]) {
            verify(log.indexOf("not moved in \"" + playlist + "\" there: " + library + "'s playlist does not hold the file")
                   >= 0, log);
        }
    }

    // A track added at a place (dropped between two rows) with the real
    // controllers: one change, previewed at its place, flagged as added
    // and not moved; Save writes it there, and a rescan reads it there.
    function test_aTrackAddedAtAPlaceShowsAndSavesThere() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const browse = createTemporaryObject(playlistEditComponent, testCase);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("onelibrary", t.pioneer);
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "onelibrary scanned");
        const order = (s) => {
            const files = [];
            for (let i = 0; i < s.tracks.trackCount(); ++i) {
                files.push(s.tracks.trackAt(i).filePath);
            }
            return files;
        };
        const file = t.stick + "/Contents/795079.mp3";
        // A playlist of three or more that does not hold the file.
        const playlist = scan.playlistNames.filter(n => {
            if (scan.playlistTrackCounts[n] < 3) {
                return false;
            }
            scan.filterByPlaylist(n);
            return order(scan).indexOf(file) < 0;
        })[0];
        verify(playlist !== undefined, JSON.stringify(scan.playlistTrackCounts));
        scan.filterByPlaylist(playlist);
        const before = order(scan);

        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, playlist, file, "Placed", 1)), browse.errorMessage);
        compare(t.session.pendingCount, 1, "one change, not an add and a move");
        compare(t.session.playlistsEdited, 1);
        compare(browse.trackCountChange(playlist), 1);
        verify(t.session.pendingDescriptions[0].indexOf("at position 2") > 0, t.session.pendingDescriptions[0]);
        scan.setPendingEdits(browse.pendingAdditions, browse.pendingMoves);
        const wanted = before.slice();
        wanted.splice(1, 0, file);
        compare(JSON.stringify(order(scan)), JSON.stringify(wanted), "previewed at its place");
        verify(scan.tracks.trackAt(1).pendingAddition === true, "flagged as added");
        verify(scan.tracks.trackAt(1).pendingMove === false, "not as moved");

        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        const again = createTemporaryObject(scanControllerComponent, testCase);
        again.scan("onelibrary", t.pioneer);
        tryVerify(() => !again.busy && again.playlistNames.length > 0, 120000, "onelibrary scanned again");
        again.filterByPlaylist(playlist);
        compare(JSON.stringify(order(again)), JSON.stringify(wanted), "the stick holds it at its place");
    }

    // Browse's rename with the real controllers on a stick copy. Staged,
    // the list names the playlist anew and shows its tracks under the new
    // name; a track then added to it is looked up by the old name (the
    // stick's until Save) and staged under the new, so one Save writes the
    // rename first and the add after it, and a rescan finds both. A new
    // playlist renamed is renamed in place, its track staged into it along;
    // a name the stick has is refused, and taking a rename back takes the
    // edits made under the new name with it.
    function test_aRenameAndAnAddAfterItLandInOneSave() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const browse = createTemporaryObject(playlistEditComponent, testCase);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("onelibrary", t.pioneer);
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "onelibrary scanned");
        const oldName = "Playlist 001";
        const newName = "Renamed 001";
        verify(scan.playlistNames.indexOf(oldName) >= 0, JSON.stringify(scan.playlistNames));
        const count = scan.playlistTrackCounts[oldName];
        verify(count > 0);
        scan.filterByPlaylist(oldName);
        const before = [];
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            before.push(scan.tracks.trackAt(i).filePath);
        }
        // A file the playlist does not hold, OneLibrary has a row for.
        scan.filterByPlaylist("");
        let file = "";
        for (let i = 0; i < scan.tracks.trackCount() && file.length === 0; ++i) {
            const row = scan.tracks.trackAt(i);
            if (row.playlistNames.indexOf(oldName) < 0 && row.streamingSource.length === 0) {
                file = row.filePath;
            }
        }
        verify(file.length > 0);

        verify(!edited(browse, browse.renamePlaylist(t.pioneer, engine, oldName, "Playlist 000")), "a name the stick has");
        verify(browse.errorMessage.indexOf("already a playlist") >= 0, browse.errorMessage);
        verify(!edited(browse, browse.renamePlaylist(t.pioneer, engine, oldName, "A/B")), "a folder path");
        compare(t.session.pendingCount, 0);

        verify(edited(browse, browse.renamePlaylist(t.pioneer, engine, oldName, "  " + newName + " ")), browse.errorMessage);
        compare(t.session.pendingCount, 1);
        compare(JSON.stringify(browse.pendingRenames), JSON.stringify({"Playlist 001": newName}), "trimmed");
        verify(browse.isRenameStaged(newName) && browse.isRenameStaged(oldName), "by either name");
        verify(browse.isPlaylistEdited(newName));
        scan.setPlaylistAliases(browse.pendingRenames);
        verify(scan.playlistNames.indexOf(newName) >= 0, "listed under the new name");
        verify(scan.playlistNames.indexOf(oldName) < 0, "and not the old");
        compare(scan.playlistTrackCounts[newName], count, "its count unchanged");
        compare(scan.currentPlaylistName(oldName), newName);
        scan.filterByPlaylist(newName);
        compare(scan.tracks.trackCount(), before.length, "the old playlist's tracks");
        verify(scan.tracks.trackAt(0).playlistNames.indexOf(newName) >= 0, "the rows name it anew");

        // Taken back, with an add made under the new name meanwhile.
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, newName, file, "Added")), browse.errorMessage);
        compare(t.session.pendingCount, 2);
        browse.keepPlaylistName(newName);
        compare(t.session.pendingCount, 0, "the rename and the add under its name both taken back");
        compare(JSON.stringify(browse.pendingRenames), "{}");
        scan.setPlaylistAliases(browse.pendingRenames);
        verify(scan.playlistNames.indexOf(oldName) >= 0 && scan.playlistNames.indexOf(newName) < 0);
        compare(scan.currentPlaylistName(newName), oldName, "a page showing it follows it back");

        // A new playlist with a track in it, renamed in place.
        verify(edited(browse, browse.createPlaylist(t.pioneer, engine, "Fresh A")), browse.errorMessage);
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, "Fresh A", file, "Added")), browse.errorMessage);
        verify(edited(browse, browse.renamePlaylist(t.pioneer, engine, "Fresh A", "Fresh B")), browse.errorMessage);
        compare(JSON.stringify(browse.newPlaylists), JSON.stringify(["Fresh B"]));
        compare(JSON.stringify(browse.pendingRenames), "{}", "no rename of a playlist the stick does not have");
        compare(browse.pendingAdditions["Fresh B"].join(), file, "its track along");
        verify(browse.isAdditionStaged("Fresh B", file) && !browse.isAdditionStaged("Fresh A", file));
        compare(t.session.pendingCount, 2);

        // The rename, then the add under the new name: one Save.
        verify(edited(browse, browse.renamePlaylist(t.pioneer, engine, oldName, newName)), browse.errorMessage);
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, newName, file, "Added")), browse.errorMessage);
        verify(browse.isAdditionStaged(newName, file));
        compare(t.session.pendingCount, 4);
        verify(!edited(browse, browse.renamePlaylist(t.pioneer, engine, newName, "Again")),
               "renamed again with an edit under the new name");
        verify(browse.errorMessage.indexOf("edits staged under its new name") >= 0, browse.errorMessage);
        scan.setPendingEdits(browse.pendingAdditions, browse.pendingMoves);
        scan.setPlaylistAliases(browse.pendingRenames);
        scan.filterByPlaylist(newName);
        compare(scan.tracks.trackCount(), before.length + 1, "the add shown in it under the new name");
        compare(scan.tracks.trackAt(before.length).filePath, file);

        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 4, JSON.stringify(summary));
        compare(browse.lastSavedSummary, "2 playlists saved: 1 created, 1 renamed, 2 tracks added",
                "the add after the rename counted with it, as one playlist");
        compare(JSON.stringify(browse.pendingRenames), "{}", "written, so no longer pending");
        const log = stickFixture.readText(t.stick + "/Seabass/seabass.log");
        verify(log.indexOf("playlist-rename: renamed \"" + oldName + "\" to \"" + newName + "\" in 3 libraries") >= 0, log);
        // The add after it, by the new name: written after the rename.
        verify(log.indexOf("to \"" + newName + "\" in 2 libraries") > log.indexOf("playlist-rename: renamed"), log);
        verify(log.indexOf("playlist-create: created \"Fresh B\" in 3 libraries") >= 0, log);

        const again = createTemporaryObject(scanControllerComponent, testCase);
        again.scan("onelibrary", t.pioneer);
        tryVerify(() => !again.busy && again.playlistNames.length > 0, 120000, "onelibrary scanned again");
        verify(again.playlistNames.indexOf(newName) >= 0, JSON.stringify(again.playlistNames));
        verify(again.playlistNames.indexOf(oldName) < 0, "the old name is gone");
        verify(again.playlistNames.indexOf("Fresh B") >= 0 && again.playlistNames.indexOf("Fresh A") < 0);
        again.filterByPlaylist(newName);
        const written = [];
        for (let i = 0; i < again.tracks.trackCount(); ++i) {
            written.push(again.tracks.trackAt(i).filePath);
        }
        compare(JSON.stringify(written), JSON.stringify(before.concat([file])), "its tracks, and the add last");
        again.filterByPlaylist("Fresh B");
        compare(again.tracks.trackCount(), 1);

        // A rename alone, saved: said by the name it had.
        verify(edited(browse, browse.renamePlaylist(t.pioneer, engine, "Fresh B", "Fresh C")), browse.errorMessage);
        const renamedOnly = saveAndWait(t.session);
        compare(renamedOnly.error, "", JSON.stringify(renamedOnly));
        compare(browse.lastSavedSummary, "\"Fresh B\" saved: renamed");
    }

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }
    Component {
        id: browseStackComponent
        StackView {
            width: 1000
            height: 760
            initialItem: Item {}
        }
    }
    Component {
        id: browseComponent
        ScanPage { stickLabel: "TESTSTICK" }
    }

    // Playlist edits staged in Browse with the real controller are unsaved
    // changes like any other: quitting asks (the window asks when
    // EditSessionRegistry.anyDirty, see Main.qml's onClosing), and so does
    // leaving the page, whose Back goes through EditSessionHost's
    // requestLeave. Meanwhile the Save button names what Save writes: the
    // playlist, or the playlists.
    function test_stagedPlaylistEditsAreUnsavedChanges() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const stack = createTemporaryObject(browseStackComponent, testCase);
        const page = stack.push(browseComponent, {rekordboxPath: t.pioneer, enginePath: engine,
                                                  playbackController: realPlayback,
                                                  appSettingsController: realAppSettings});
        tryCompare(stack, "busy", false);
        compare(stack.depth, 2);
        const browse = page.playlistEditController;
        verify(!EditSessionRegistry.anyDirty, "nothing staged yet");
        verify(edited(browse, browse.createPlaylist(t.pioneer, engine, "Dragged Here")), browse.errorMessage);
        verify(edited(browse, browse.addToPlaylist(t.pioneer, engine, "Dragged Here", t.stick + "/Contents/795079.mp3",
                                                   "A Track")), browse.errorMessage);
        // The track is in OneLibrary alone on this fixture: the page asks.
        const ask = findChild(page, "missingLibrariesDialog");
        tryCompare(ask, "opened", true);
        compare(t.session.pendingCount, 1, "the add waits for the answer");
        findChild(ask, "addAnywayButton").clicked();
        tryCompare(ask, "visible", false);
        compare(t.session.pendingCount, 2);
        compare(t.session.playlistsEdited, 1);
        verify(EditSessionRegistry.anyDirty, "quitting would ask");
        const label = findChild(page, "saveButtonLabel");
        verify(label !== null);
        tryCompare(label, "text", "Save Playlist");
        verify(edited(browse, browse.deletePlaylist(t.pioneer, engine, "Playlist 000")), browse.errorMessage);
        tryCompare(label, "text", "Save Playlists");

        mouseClick(findChild(page, "backButton"));
        const dialog = findChild(page, "unsavedDialog");
        tryCompare(dialog, "opened", true);
        compare(stack.depth, 2, "not left before the answer");
        compare(dialog.pendingCount, 3);
        findChild(dialog, "discardButton").clicked();
        tryCompare(stack, "depth", 1);
        compare(t.session.pendingCount, 0, "discarded");
        verify(!EditSessionRegistry.anyDirty);
    }

    // Browse's Cancel beside Save, with the real controllers: two playlist
    // edits staged, Cancel asks, Discard drops both, the session is clean,
    // nothing is said and Back leaves without asking.
    function test_cancelDiscardsStagedPlaylistEdits() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const stack = createTemporaryObject(browseStackComponent, testCase);
        const page = stack.push(browseComponent, {rekordboxPath: t.pioneer, enginePath: engine,
                                                  playbackController: realPlayback,
                                                  appSettingsController: realAppSettings});
        tryCompare(stack, "busy", false);
        const browse = page.playlistEditController;
        verify(edited(browse, browse.createPlaylist(t.pioneer, engine, "Cancelled One")), browse.errorMessage);
        verify(edited(browse, browse.deletePlaylist(t.pioneer, engine, "Playlist 000")), browse.errorMessage);
        compare(t.session.pendingCount, 2);
        const cancel = findChild(page, "cancelOverlayButton");
        tryCompare(cancel, "visible", true);
        mouseClick(cancel);
        const dialog = findChild(page, "discardDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "Discard 2 staged changes?");
        findChild(dialog, "confirmDiscardButton").clicked();
        tryCompare(dialog, "opened", false);
        // Gone, not just closing: a style's exit transition keeps the
        // modal's dimmer over the page until it ends.
        tryCompare(dialog, "visible", false);
        compare(t.session.pendingCount, 0, "both gone");
        verify(!t.session.dirty, "the session is clean");
        verify(!EditSessionRegistry.anyDirty);
        compare(browse.newPlaylists.length, 0);
        verify(!browse.isPlaylistStaged("Playlist 000"));
        compare(cancel.visible, false);
        compare(findChild(page, "savedToast").visible, false, "nothing said");
        let asked = false;
        findChild(page, "unsavedDialog").aboutToShow.connect(() => { asked = true; });
        mouseClick(findChild(page, "backButton"));
        tryCompare(stack, "depth", 1);
        verify(!asked, "left without asking");
    }

    // Library Health's "Engine playlist entries the player cannot see",
    // with the real controller on a stick copy: a playlist's entries given
    // a library uuid their tracks do not name as their origin (as Seabass
    // wrote 120 on WHALESHARK3) are found, counted per playlist, staged,
    // and written back to the tracks' own uuid by Save; the rescan finds
    // none, and the playlist still holds its tracks in their order.
    function test_engineEntriesThePlayerCannotSeeAreRepaired() {
        const t = scannedCopy((stick) => stickFixture.plantUnresolvedEngineEntries(
            stick, "Playlist 000", "52f6b8cd-3bf1-44c9-ba8f-4b04244a3f60"));
        compare(t.planted, 100, "Playlist 000's entries planted");
        compare(stickFixture.unresolvedEngineEntries(t.stick), 100);
        compare(t.controller.unresolvedPlaylistEntryCount, 100);
        compare(t.controller.unresolvedPlaylists.length, 1);
        compare(t.controller.unresolvedPlaylists[0].playlist, "Playlist 000");
        compare(t.controller.unresolvedPlaylists[0].entries, 100);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("engine", t.stick + "/Engine Library");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "engine scanned");
        scan.filterByPlaylist("Playlist 000");
        const before = [];
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            before.push(scan.tracks.trackAt(i).sourceId);
        }
        compare(before.length, 100, "Seabass's reader lists them whatever their uuid");

        t.controller.repairUnresolvedPlaylistEntries();
        verify(t.controller.unresolvedFixStaged);
        compare(t.session.pendingCount, 1);
        t.controller.unstageUnresolvedPlaylistEntries();
        compare(t.session.pendingCount, 0);
        verify(!t.controller.unresolvedFixStaged);
        t.controller.repairUnresolvedPlaylistEntries();
        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 100, "counted in entries: " + JSON.stringify(summary));
        tryVerify(() => !t.controller.busy && t.controller.playlistsChecked, 300000, "the rescan finishes");
        compare(stickFixture.unresolvedEngineEntries(t.stick), 0, "every entry resolves now, on the stick");
        compare(t.controller.unresolvedPlaylistEntryCount, 0, "and the rescan says so");
        verify(!t.controller.unresolvedFixStaged);
        const log = stickFixture.readText(t.stick + "/Seabass/seabass.log");
        verify(log.indexOf("repointed 100 Engine playlist entr(ies) the player could not see") >= 0, log);

        const again = createTemporaryObject(scanControllerComponent, testCase);
        again.scan("engine", t.stick + "/Engine Library");
        tryVerify(() => !again.busy && again.playlistNames.length > 0, 120000, "engine scanned again");
        again.filterByPlaylist("Playlist 000");
        const after = [];
        for (let i = 0; i < again.tracks.trackCount(); ++i) {
            after.push(again.tracks.trackAt(i).sourceId);
        }
        compare(JSON.stringify(after), JSON.stringify(before), "the same tracks in the same order");
    }

    Component { id: addSpyComponent; SignalSpy { signalName: "additionNeedsLibraries" } }

    // An add of a track some library holding the playlist has no row for
    // (on the anonymized fixture only OneLibrary has 795079.mp3): nothing
    // is staged, the page is asked with the libraries left out and the
    // ones reached, and only "add anyway" stages the add; a move waits
    // with it, both halves or neither.
    function test_anAddToALibraryWithoutTheTrackWaitsForAnAnswer() {
        const t = scannedCopy(null);
        const engine = t.stick + "/Engine Library";
        const browse = createTemporaryObject(askingEditComponent, testCase);
        const spy = createTemporaryObject(addSpyComponent, testCase, {target: browse});
        const file = t.stick + "/Contents/795079.mp3";
        verify(browse.addToPlaylist(t.pioneer, engine, "Playlist 000", file, "A Track", 3));
        tryCompare(browse, "busy", false, 60000);
        compare(spy.count, 1, "asked");
        const args = spy.signalArguments[0];
        compare(args[0], "Playlist 000");
        compare(args[1], file);
        compare(args[2], "A Track");
        compare(JSON.stringify(args[3]), JSON.stringify(["rekordbox", "Engine"]), "the libraries left out");
        compare(args[4], 3, "the place asked for");
        compare(JSON.stringify(args[5]), JSON.stringify(["OneLibrary"]), "the one it would reach");
        compare(t.session.pendingCount, 0, "nothing staged while the page asks");
        verify(!browse.isAdditionStaged("Playlist 000", file));
        browse.dropAddition("Playlist 000", file);
        browse.addAnyway("Playlist 000", file);
        compare(t.session.pendingCount, 0, "dropped: an answer after it does nothing");

        verify(browse.addToPlaylist(t.pioneer, engine, "Playlist 000", file, "A Track"));
        tryCompare(browse, "busy", false, 60000);
        compare(spy.count, 2);
        browse.addAnyway("Playlist 000", file);
        compare(t.session.pendingCount, 1, "added anyway: one add");
        verify(browse.isAdditionStaged("Playlist 000", file));
        browse.keepOutOfPlaylist("Playlist 000", file);
        compare(t.session.pendingCount, 0);

        // A move: the removal from the source waits for the answer too.
        verify(browse.addToPlaylist(t.pioneer, engine, "Playlist 000", file, "A Track", -1, "Playlist 001"));
        tryCompare(browse, "busy", false, 60000);
        compare(spy.count, 3);
        compare(t.session.pendingCount, 0, "neither half staged");
        browse.dropAddition("Playlist 000", file);
        compare(t.session.pendingCount, 0, "neither, on don't");
        verify(browse.addToPlaylist(t.pioneer, engine, "Playlist 000", file, "A Track", -1, "Playlist 001"));
        tryCompare(browse, "busy", false, 60000);
        browse.addAnyway("Playlist 000", file);
        tryCompare(browse, "busy", false, 60000);
        compare(t.session.pendingCount, 2, "both, on add anyway");
        verify(browse.isAdditionStaged("Playlist 000", file) && browse.isRemovalStaged("Playlist 001", file));
        // Left clean for the cases after this one: anyDirty is the whole
        // registry's.
        t.session.discard();
        compare(t.session.pendingCount, 0);
    }

    // The page picks its playlist again as a scan's names arrive (the one
    // it showed before the rescan a save causes): the pick stands, the
    // list shows that playlist and not every track.
    function test_aPlaylistPickedAsTheNamesArriveStaysShown() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("onelibrary", stick + "/PIONEER");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 120000, "scanned");
        const all = scan.tracks.trackCount();
        const pick = scan.playlistNames.filter(n => scan.playlistTrackCounts[n] > 0
                                                    && scan.playlistTrackCounts[n] < all)[0];
        verify(pick !== undefined, JSON.stringify(scan.playlistTrackCounts));
        let picked = 0;
        scan.playlistNamesChanged.connect(() => { scan.filterByPlaylist(pick); ++picked; });
        scan.scan("onelibrary", stick + "/PIONEER");
        tryVerify(() => !scan.busy && picked > 0, 120000, "scanned again, the playlist picked meanwhile");
        compare(scan.tracks.trackCount(), scan.playlistTrackCounts[pick], "the pick stands: " + pick);
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            verify(scan.tracks.trackAt(i).playlistNames.indexOf(pick) >= 0, "every row is in " + pick);
        }
    }

    // Two rows staged; one's analysis file stops reading before the save.
    // The summary counts that row skipped and the other written.
    function test_aRowLeftAloneAtSaveTimeIsCountedSkipped() {
        const t = scannedCopy((stick) => stickFixture.plantCueTableRow(stick, -1, 3, 200000));
        verify(t.planted && t.planted.dat, "a second row was planted");
        compare(t.controller.cueTableRowCount, 2);
        t.controller.repairCueTables([]);
        compare(t.controller.cueTableStagedCount, 2);
        verify(stickFixture.replaceFileInCopy(t.planted.dat, "not an analysis file"));
        const committed = testCase.fixtureRoot + "/rekordbox" + t.planted.dat.substring((t.stick + "/PIONEER").length);
        verify(stickFixture.readText(committed) !== "not an analysis file", "the committed fixture is untouched");
        const summary = saveAndWait(t.session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        compare(summary.skipped, 1, JSON.stringify(summary));
        compare(summary.total, 2, JSON.stringify(summary));
        tryVerify(() => !t.controller.busy && t.controller.cueTablesChecked, 300000, "the rescan finishes");
        compare(t.controller.cueTableRowCount, 0);
        compare(t.controller.cueTableLeftAloneCount, 1, "the row whose file stopped reading is left alone");
    }
}
