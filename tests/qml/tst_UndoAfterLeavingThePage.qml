// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// #79: Undo Last Save is gone once the page that saved is left.
//
// A save on Library Health's cue-table page (the anonymized fixture's one
// drifted row, repaired), then the page is left and opened again on the
// same library. The undo should still be offered: the save's backups are
// on the stick and nothing has changed since. It is not, because
// EditSessionRegistry::closeSession drops the idle session, and the
// backups of its last save with it, when the page goes. An expected
// failure until #79 is fixed; it turns into an XPASS then.
//
// Keeping the session is not all the fix needs. With closeSession made to
// keep it, the session still offered the undo, and so did the new page's
// controller, but the button stayed hidden: the controller does not say
// canUndo changed when it attaches to a session that can already undo.
// Both changes together turned this into the XPASS.
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

        // Leave, as Back does: the page and its edit host go.
        first.destroy();
        wait(0);

        const again = openScanned(stick);
        compare(again.consistencyController.cueTableRowCount, 0, "the repair is on the stick");
        const reopened = EditSessionRegistry.sessionFor(libraryId);
        verify(reopened !== null);
        const undo = findChild(again, "undoLastSaveButton");
        verify(undo !== null);
        expectFail("", "#79: leaving the page drops the session, and the last save's backups with it");
        verify(reopened.canUndo && undo.visible, "the save is still offered for undo on the page opened again");
    }
}
