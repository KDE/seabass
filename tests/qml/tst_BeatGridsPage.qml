// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Library Health's Beat Grids page with the real controller, on a copy of
// the anonymized fixture: 1161 rekordbox tracks in both libraries, 188 of
// them on Engine rows the player analysed (each with a rekordbox grid),
// the other 973 not analysed. The numbers the command line's
// cli_copy_beatgrids_* tests read from the same fixture.
//
// - what the page shows is what the controller counted, from a preview
//   that wrote nothing; the copy stages one change per track, Save writes
//   them through the edit session, m.db really carries 188 new grids, the
//   ledger records them, and the page's next look finds them already the
//   same, with nothing left to copy;
// - with every analysed row's grid locked on the player, nothing is to
//   copy and the action is off; the box that replaces locked grids moves
//   them over to "can be copied", and back.
//
// The Library Health frame's own controller is a stand-in here, as in
// tst_SampleRatesPage: the page under test does not need the hub's scan.
TestCase {
    id: testCase
    name: "BeatGridsPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_CueTablePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // Only what HealthCheckPage and the page read of the hub's controller.
    Component {
        id: frameControllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
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
        id: pageComponent
        BeatGridsPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
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
    }

    // A page on a fresh copy of the fixture, with no session left over
    // from an earlier test (one filesystem, one library id for every copy).
    function openOn(stick) {
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        const frame = createTemporaryObject(frameControllerComponent, testCase);
        const page = createTemporaryObject(pageComponent, testCase, {
            rekordboxPath: stick + "/PIONEER",
            enginePath: stick + "/Engine Library",
            sharedController: frame,
        });
        verify(page !== null, "the page must instantiate");
        return {page: page, controller: page.beatGridsController, libraryId: libraryId};
    }

    function settled(controller) {
        return controller.analysed && !controller.busy;
    }

    function finished(session, run) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        run();
        tryVerify(() => summary !== null, 300000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    function shown(page, name) {
        const item = findChild(page, name);
        verify(item !== null, name + " is on the page");
        return item.visible ? item.text : null;
    }

    function changedRows(before, after) {
        let changed = 0;
        for (const id in after) {
            if (before[id] !== after[id]) {
                ++changed;
            }
        }
        return changed;
    }

    function test_countsThenCopyThenAlreadyTheSame() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const before = stickFixture.engineBeatDataDigests(stick);
        verify(Object.keys(before).length > 0, "m.db's beat data reads");
        const opened = openOn(stick);
        const page = opened.page;
        const c = opened.controller;
        tryVerify(() => settled(c), 300000, "the analysis finishes");
        compare(c.errorMessage, "");
        compare(c.engineTooOld, false);
        compare(c.ledgerWarning, "", "no ledger yet is not an unreadable one");

        compare(c.matchedCount, 1161);
        compare(c.wouldCopyCount, 188);
        compare(c.notAnalysedCount, 973);
        compare(c.alreadySameCount, 0);
        compare(c.lockedCount, 0);
        compare(c.editedCount, 0);
        compare(c.noRekordboxGridCount, 0);
        compare(c.unreadableCount, 0);
        compare(changedRows(before, stickFixture.engineBeatDataDigests(stick)), 0, "the preview wrote nothing");

        // On screen, what the controller counted, and only the counts that
        // are not zero.
        compare(shown(page, "beatGridWouldCopy"), "188 beat grids can be copied.");
        verify(shown(page, "beatGridNotAnalysed").indexOf("973 tracks have not been analysed") === 0,
               shown(page, "beatGridNotAnalysed"));
        for (const zero of ["beatGridAlreadySame", "beatGridLocked", "beatGridEdited", "beatGridNoRekordboxGrid",
                            "beatGridUnreadable", "beatGridNothingMatched", "beatGridLedgerWarning",
                            "beatGridEngineTooOld", "beatGridError"]) {
            compare(shown(page, zero), null, zero + " says nothing for a zero");
        }
        const list = findChild(page, "beatGridTrackList");
        verify(list !== null && list.visible);
        compare(list.count, 188, "every track a copy would write is listed");

        // One left line: the sentence, the counts, the box itself and the
        // list all start where the body does.
        verify(waitForPolish(page.Window.window), "the page is laid out");
        const body = findChild(page, "healthCheckBody");
        const left = body.mapToItem(page, 0, 0).x;
        for (const name of ["beatGridIntro", "beatGridCounts", "beatGridTrackList", "copyBeatGridsButton"]) {
            compare(findChild(page, name).mapToItem(page, 0, 0).x, left, name + " is on the page's left line");
        }
        const box = findChild(findChild(page, "replaceEditsCheckBox"), "seabassCheckIndicator");
        compare(box.mapToItem(page, 0, 0).x, left, "the check box's box is on the left line");
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/BeatGridsPage.png");
        }

        const button = findChild(page, "copyBeatGridsButton");
        compare(button.text, "Copy Beat Grids");
        verify(button.enabled, "there is something to copy");
        button.clicked();
        compare(c.stagedCount, 188, "one change per track");
        const session = EditSessionRegistry.sessionFor(opened.libraryId);
        compare(session.pendingCount, 188);
        compare(shown(page, "stagedBeatGridsNote"), "staged, not saved yet");
        compare(button.text, "Unstage");
        compare(findChild(page, "replaceEditsCheckBox").enabled, false, "the box waits until the copy is saved or unstaged");
        compare(changedRows(before, stickFixture.engineBeatDataDigests(stick)), 0, "staging wrote nothing");

        const saved = finished(session, () => session.save());
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 188, JSON.stringify(saved));
        compare(saved.total, 188, JSON.stringify(saved));
        compare(saved.skipped ?? 0, 0, JSON.stringify(saved));
        compare(saved.unit, "beat grids");
        compare(saved.verb, "copied");

        // The rows themselves, read straight from m.db.
        compare(changedRows(before, stickFixture.engineBeatDataDigests(stick)), 188, "m.db carries 188 new grids");
        const ledger = stickFixture.readText(stick + "/Seabass/beat-grids.tsv");
        const lines = ledger.split("\n").filter(line => line.length > 0);
        compare(lines[0], "seabass-beat-grids\t1");
        compare(lines.length, 1 + 188, "the ledger records every grid written");

        // The page looks again on its own, and finds them there.
        tryVerify(() => settled(c) && c.alreadySameCount === 188, 300000, "the next look finds them the same");
        compare(c.wouldCopyCount, 0);
        compare(c.stagedCount, 0, "nothing is staged once it landed");
        compare(shown(page, "beatGridAlreadySame"), "188 are already the same in Engine.");
        compare(shown(page, "beatGridWouldCopy"), null);
        compare(button.text, "Copy Beat Grids");
        compare(button.enabled, false, "with nothing to copy the action is off");
        compare(list.visible, false, "and there is no list");
    }

    function test_theBoxReplacesLockedGrids() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        verify(stickFixture.lockAnalysedEngineBeatGrids(stick) > 0, "precondition: the analysed rows are locked");
        const opened = openOn(stick);
        const page = opened.page;
        const c = opened.controller;
        tryVerify(() => settled(c), 300000, "the analysis finishes");
        compare(c.lockedCount, 188, "every grid a copy would write is locked on the player");
        compare(c.wouldCopyCount, 0);
        verify(shown(page, "beatGridLocked").indexOf("188 tracks have their grid locked") === 0,
               shown(page, "beatGridLocked"));
        const button = findChild(page, "copyBeatGridsButton");
        compare(button.enabled, false, "nothing to copy: the action is off");

        const box = findChild(page, "replaceEditsCheckBox");
        compare(box.checked, false, "off by default");
        verify(box.enabled);
        mouseClick(box);
        compare(c.replaceEngineEdits, true, "the box sets the controller's option");
        tryVerify(() => settled(c) && c.wouldCopyCount === 188, 300000, "counted again, the locked grids can be copied");
        compare(c.lockedCount, 0);
        compare(shown(page, "beatGridLocked"), null);
        compare(shown(page, "beatGridWouldCopy"), "188 beat grids can be copied.");
        verify(button.enabled);

        mouseClick(box);
        compare(c.replaceEngineEdits, false);
        tryVerify(() => settled(c) && c.lockedCount === 188, 300000, "and back");
        compare(c.wouldCopyCount, 0);
    }
}
