// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
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
        verify(browse.deletePlaylist(t.pioneer, t.stick + "/Engine Library", "Playlist 000"),
               "Browse stages a playlist deletion: " + browse.errorMessage);
        compare(t.session.pendingCount, 1);
        t.controller.repairCueTables(["392"]);
        compare(t.session.pendingCount, 1, "only Browse's change");
        compare(t.controller.cueTableStagedCount, 0);
        verify(t.controller.errorMessage.length > 0, "the page says the repair was not staged");
        browse.keepPlaylist("Playlist 000");
        compare(t.session.pendingCount, 0);
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
