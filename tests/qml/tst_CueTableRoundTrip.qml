// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The OneLibrary cue-table repair (#57) there and back on the page, with
// the real controller on a copy of the anonymized fixture, whose one
// drifted row (content_id 392) is the drift: Repair All, Save, a rescan
// finds none; Undo Last Save, a rescan finds the row again, and the
// catalogs are byte for byte what the copy started as.
//
// All but Engine's m.db, on this fixture. Its import counter is not level
// with export.pdb, and every save ends by bringing it level (the player
// would otherwise offer to import the rekordbox library over Engine's):
// the save does, and so does the undo, after restoring the m.db it backed
// up. So m.db after the undo is m.db after the save, byte for byte. On a
// stick whose counter is level already, as the rig's are, neither writes
// it and it is compared with the start like the rest.
//
// The fixture-sized twin of tst_LiveEditMode's
// test_11_cueTableRepairAllSaveUndo (rig part W11), which does the same
// on the rig's stick B. This one runs in CI.
TestCase {
    id: testCase
    name: "CueTableRoundTrip"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_CueTablePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // The catalog files a cue-table repair could touch, and two it must
    // not: OneLibrary with its log, export.pdb and Engine's m.db.
    readonly property var catalogFiles: ["PIONEER/rekordbox/exportLibrary.db", "PIONEER/rekordbox/exportLibrary.db-wal",
                                         "PIONEER/rekordbox/export.pdb", "Engine Library/Database2/m.db"]

    Component {
        id: pageComponent
        CueTablePage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
        }
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
    }

    function cleanup() {
        EditSessionRegistry.discardAll();
    }

    // Each catalog file's sha256, "absent" for one that is not there.
    function catalogBytes(stick) {
        const bytes = {};
        for (const file of testCase.catalogFiles) {
            const path = stick + "/" + file;
            bytes[file] = stickFixture.fileExists(path) ? stickFixture.sha256(path) : "absent";
        }
        return bytes;
    }

    function settled(controller) {
        return controller.cueTablesChecked && !controller.busy;
    }

    // A fresh read, not the one a save or an undo started on its own.
    function rescanned(page) {
        const controller = page.consistencyController;
        tryVerify(() => settled(controller), 300000, "the scan before finishes");
        page.rescan();
        verify(!settled(controller), "a scan started");
        tryVerify(() => settled(controller), 300000, "the scan finishes");
        return controller.cueTableRowCount;
    }

    function finished(session, run) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        run();
        tryVerify(() => summary !== null, 120000, "the write finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    function test_repairAllSaveRescanUndoRescan() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        // One filesystem, one library id for every copy: start from no
        // session, so no earlier test's save is what Undo restores.
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        verify(!EditSessionRegistry.hasSession(libraryId), "precondition: no session left over");
        const before = catalogBytes(stick);
        verify(before["PIONEER/rekordbox/exportLibrary.db"] !== "absent", "the copy has OneLibrary");

        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        const controller = page.consistencyController;
        tryVerify(() => settled(controller), 300000, "the page's scan finishes");
        compare(controller.cueTableRowCount, 1, "the fixture's one drifted row");
        compare(controller.cueTableRows[0].contentId, "392");

        const repairAll = findChild(page, "repairAllButton");
        verify(repairAll !== null && repairAll.visible && repairAll.enabled, "Repair All is offered");
        compare(repairAll.text, "Repair All");
        repairAll.clicked();
        compare(controller.cueTableStagedCount, 1);
        const session = EditSessionRegistry.sessionFor(libraryId);
        compare(session.pendingCount, 1);
        const saved = finished(session, () => session.save());
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 1, JSON.stringify(saved));
        compare(rescanned(page), 0, "repaired: the rescan finds no drifted row");
        const afterSave = catalogBytes(stick);
        verify(afterSave["PIONEER/rekordbox/exportLibrary.db"] !== before["PIONEER/rekordbox/exportLibrary.db"],
               "the save wrote OneLibrary");
        compare(afterSave["PIONEER/rekordbox/export.pdb"], before["PIONEER/rekordbox/export.pdb"],
                "and not export.pdb");
        const engineDb = "Engine Library/Database2/m.db";
        verify(afterSave[engineDb] !== before[engineDb], "precondition: the save brought the import counter level");
        verify(stickFixture.readText(stick + "/Seabass/seabass.log").indexOf("engine import counter: set to") >= 0,
               "and logged it");

        const undo = findChild(page, "undoLastSaveButton");
        verify(undo !== null);
        tryCompare(undo, "visible", true, 5000, "Undo Last Save is offered after the save");
        tryVerify(() => undo.enabled, 300000, "and can be pressed once the page is idle");
        const undone = finished(session, () => undo.clicked());
        compare(undone.error, "", JSON.stringify(undone));
        compare(undone.unit, "undo steps", JSON.stringify(undone));
        compare(rescanned(page), 1, "undone: the drifted row is back");
        compare(controller.cueTableRows[0].contentId, "392");
        const expected = Object.assign({}, before);
        expected[engineDb] = afterSave[engineDb];
        compare(JSON.stringify(catalogBytes(stick), null, 1), JSON.stringify(expected, null, 1),
                "the catalogs are byte for byte as they were, m.db as the save left it");
    }
}
