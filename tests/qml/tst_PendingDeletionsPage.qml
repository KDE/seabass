// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Delete Orphaned Files' old backups, on a real stick copy with real
// records: the page lists the automatic backups nothing in Seabass can
// restore from, never one Undo still needs or one the user made, and its
// prune deletes exactly those.
TestCase {
    id: testCase
    name: "PendingDeletionsPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        PendingDeletionsPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
        }
    }

    // Three automatic records and one of the user's, with an interrupted
    // save's note naming the newest automatic one: two are stale.
    function plantedStick() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const planted = backupFixture.plantRecords(stick, 3, 1);
        compare(planted.automatic.length, 3);
        compare(planted.user.length, 1);
        verify(backupFixture.noteInProgress(stick, [planted.automatic[0]]), "the note is written");
        return {stick: stick, planted: planted};
    }

    function makePage(stick) {
        const page = createTemporaryObject(pageComponent, testCase,
                                           {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "the page must instantiate");
        return page;
    }

    function listedIds(list) {
        const ids = [];
        for (const entry of list.model) {
            ids.push(entry.id);
        }
        return ids;
    }

    function test_a_listsOnlyWhatUndoCannotUse() {
        const s = plantedStick();
        const page = makePage(s.stick);
        const list = findChild(page, "staleBackupsList");
        tryCompare(list, "count", 2, 10000);
        compare(findChild(page, "staleBackupCountLabel").text,
                "2 backup(s) from earlier saves are no longer needed");
        const ids = listedIds(list);
        verify(ids.indexOf(s.planted.automatic[1]) >= 0, "the middle automatic one is listed");
        verify(ids.indexOf(s.planted.automatic[2]) >= 0, "the oldest automatic one is listed");
        verify(ids.indexOf(s.planted.automatic[0]) < 0, "the one the interrupted save names is not");
        verify(ids.indexOf(s.planted.user[0]) < 0, "the user's own backup is not");
        verify(!findChild(page, "staleBackupsEmptyState").visible);
        tryVerify(() => findChild(page, "pruneBackupsButton").enabled, 10000);
    }

    function test_b_pruneDeletesExactlyTheStaleOnes() {
        const s = plantedStick();
        const page = makePage(s.stick);
        const list = findChild(page, "staleBackupsList");
        tryCompare(list, "count", 2, 10000);
        const button = findChild(page, "pruneBackupsButton");
        tryVerify(() => button.enabled, 10000);

        mouseClick(button);
        const confirm = findChild(page, "confirmPruneBackupsDialog");
        tryVerify(() => confirm.opened, 5000, "the prune asks first");
        confirm.accept();

        const toast = findChild(page, "prunedToast");
        tryVerify(() => toast.showing, 30000, "a whole prune is said in the toast");
        compare(findChild(toast, "confirmationToastLabel").text, "2 backups deleted");
        verify(!findChild(page, "summaryDialog").opened, "and no summary dialog opens");

        verify(!backupFixture.recordExists(s.stick, s.planted.automatic[1]), "the middle one is gone");
        verify(!backupFixture.recordExists(s.stick, s.planted.automatic[2]), "the oldest one is gone");
        verify(backupFixture.recordExists(s.stick, s.planted.automatic[0]), "the noted one is still there");
        verify(backupFixture.recordExists(s.stick, s.planted.user[0]), "the user's is still there");

        tryCompare(list, "count", 0, 10000);
        tryVerify(() => findChild(page, "staleBackupsEmptyState").visible, 10000);
        verify(!button.enabled);
    }

    // The list is the stick's: a file orphaned by a rekordbox cleanup and
    // one by an Engine cleanup are both listed, each saying which, with
    // no format to choose on the page.
    function test_d_listsFilesOrphanedByEitherCleanup() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        verify(backupFixture.orphanFile(stick, "rekordbox", "Contents/orphan-rb.mp3", "Left Behind", "RB"));
        verify(backupFixture.orphanFile(stick, "engine", "Music/orphan-engine.mp3", "Left Behind Too", "EN"));
        const page = makePage(stick);
        const list = findChild(page, "pendingListView");
        tryCompare(list, "count", 2, 10000);
        const seen = [];
        for (let r = 0; r < list.count; ++r) {
            const row = list.itemAtIndex(r);
            verify(row !== null, "row " + r + " is built");
            seen.push(findChild(row, "orphanedByLabel").text);
        }
        seen.sort();
        compare(seen, ["Engine cleanup", "rekordbox cleanup"]);
    }

    function test_c_nothingStaleNothingToPrune() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = makePage(stick);
        const list = findChild(page, "staleBackupsList");
        tryVerify(() => findChild(page, "staleBackupsEmptyState").visible, 10000);
        compare(list.count, 0);
        verify(!findChild(page, "pruneBackupsButton").enabled);
        compare(findChild(page, "staleBackupCountLabel").text,
                "0 backup(s) from earlier saves are no longer needed");
    }
}
