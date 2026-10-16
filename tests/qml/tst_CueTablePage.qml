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
    Component {
        id: playlistEditComponent
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
        // On the fixture copy only OneLibrary is found holding this
        // playlist (playlistWhereabouts), so it is the one library written.
        const log = stickFixture.readText(t.stick + "/Seabass/seabass.log");
        verify(log.indexOf("playlist-move: moved \"" + moving + "\" to position 3 in \"" + playlist + "\" in 1 libraries")
               >= 0, log);
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
