// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// #79: Undo Last Save outlives the page that saved.
//
// A save on Library Health's cue-table page (the anonymized fixture's one
// drifted row, repaired), then the page is left and opened again on the
// same library. The undo is still offered: the save's backups are on the
// stick and nothing has changed since. EditSessionRegistry::closeSession
// drops the idle session when the page goes, and used to drop the last
// save's backups with it; it keeps them for the library's next session
// now, and Free Up Space counts them as still needed meanwhile.
//
// Keeping them is not all it takes: the new page's controller attaches to
// a session that can already undo, and has to say so (canUndoChanged on
// attach), or the button stays hidden.
TestCase {
    id: testCase
    name: "UndoAfterLeavingThePage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_CueTablePage).
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
        }
    }

    function cleanup() {
        EditSessionRegistry.discardAll();
    }

    function openScanned(stick) {
        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        const controller = page.consistencyController;
        tryVerify(() => controller.cueTablesChecked && !controller.busy, 300000, "the page's scan finishes");
        return page;
    }

    // Library Health's hub hands its own controller to each check page.
    Component {
        id: hubControllerComponent
        LibraryConsistencyController {}
    }

    function freshStick() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        verify(!EditSessionRegistry.hasSession(libraryId), "precondition: no session left over");
        return stick;
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

    // #78's comment: the cue-table page opened from the hub, on the hub's
    // controller, saved and showed no Undo Last Save while still open.
    // Not reproduced here: the page that saved offers it.
    function test_aCheckPageOnTheHubsControllerOffersItsOwnSave() {
        const stick = freshStick();
        const paths = {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"};
        const hub = createTemporaryObject(hubControllerComponent, testCase);
        hub.scan(paths.rekordboxPath, paths.enginePath);
        tryVerify(() => hub.cueTablesChecked && !hub.busy, 300000, "the hub's scan finishes");
        compare(hub.cueTableRowCount, 1, "precondition: the fixture's drifted row");

        const page = createTemporaryObject(pageComponent, testCase,
                                           Object.assign({sharedController: hub}, paths));
        verify(page !== null);
        verify(page.consistencyController === hub, "precondition: the page runs on the hub's controller");
        const undo = findChild(page, "undoLastSaveButton");
        compare(undo.visible, false, "nothing saved yet");
        hub.repairCueTables([]);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(paths.rekordboxPath));
        compare(session.pendingCount, 1);
        const summary = saveAndWait(session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        verify(session.canUndo, "precondition: the session can undo it");
        tryVerify(() => !hub.busy && hub.cueTablesChecked, 300000, "the save's rescan finishes");
        compare(hub.cueTableRowCount, 0, "the repair is on the stick");
        tryCompare(undo, "visible", true, 5000, "the page that saved offers the undo");
    }

    function test_theUndoOutlivesThePageThatSaved() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        // One filesystem, one library id for every copy: start from no
        // session, so nothing an earlier test saved is offered here.
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        verify(!EditSessionRegistry.hasSession(libraryId), "precondition: no session left over");

        const first = openScanned(stick);
        const controller = first.consistencyController;
        compare(controller.cueTableRowCount, 1, "precondition: the fixture's drifted row");
        controller.repairCueTables([]);
        const session = EditSessionRegistry.sessionFor(libraryId);
        compare(session.pendingCount, 1);
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        verify(session.canUndo, "precondition: the save can be undone on the page that made it");
        tryVerify(() => !controller.busy && controller.cueTablesChecked, 300000, "the save's rescan finishes");
        tryCompare(findChild(first, "undoLastSaveButton"), "visible", true, 5000,
                   "precondition: the page offers it");

        // Free Up Space's old backups while the page is open: not the
        // records the undo needs.
        const listedWithThePage = staleBackupCount(stick);

        // Leave, as Back does: the page and its edit host go.
        first.destroy();
        wait(0);
        verify(!EditSessionRegistry.hasSession(libraryId), "precondition: the session went with the page");

        // Nor once it is gone: they used to be listed then, and deleted.
        compare(staleBackupCount(stick), listedWithThePage,
                "Free Up Space still leaves the last save's records alone");

        const again = openScanned(stick);
        compare(again.consistencyController.cueTableRowCount, 0, "the repair is on the stick");
        const reopened = EditSessionRegistry.sessionFor(libraryId);
        verify(reopened !== null);
        const undo = findChild(again, "undoLastSaveButton");
        verify(undo !== null);
        verify(reopened.canUndo, "the save is still the session's to undo");
        tryCompare(undo, "visible", true, 5000, "and is offered on the page opened again");

        let undone = null;
        const undoDone = (result) => { undone = result; };
        reopened.saveFinished.connect(undoDone);
        undo.clicked();
        tryVerify(() => undone !== null, 120000, "the press ran the undo");
        reopened.saveFinished.disconnect(undoDone);
        compare(undone.error, "", JSON.stringify(undone));
        tryVerify(() => !again.consistencyController.busy && again.consistencyController.cueTablesChecked, 300000,
                  "the undo's rescan finishes");
        compare(again.consistencyController.cueTableRowCount, 1, "the drifted row is back");
        tryCompare(undo, "visible", false, 5000, "and nothing is left to undo");
    }

    Component {
        id: cleanupControllerComponent
        CleanupController {}
    }
    // How many backups Free Up Space offers to delete as old on this stick.
    function staleBackupCount(stick) {
        const cleanup = createTemporaryObject(cleanupControllerComponent, testCase);
        cleanup.loadPendingDeletionsOnly("engine", stick + "/Engine Library");
        tryVerify(() => !cleanup.listingStaleBackups, 30000, "the listing lands");
        const count = cleanup.staleBackupCount;
        cleanup.destroy();
        return count;
    }
}
