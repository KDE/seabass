// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Every page that saves through an EditSessionHost offers Undo Last Save:
// one button, named undoLastSaveButton, hidden while there is nothing to
// undo, shown once there is, and a press runs the controller's undo.
//
// Pages that take a controller from outside (Library Health's check
// pages, Sync after Rekordbox Export, Clean Up Stray Cues) get a stand-in.
// Clean Up, Clean Up Duplicates and Sync Cue Points make their own and
// take none, so they run their real controller on a copy of a small
// fixture stick: an interrupted save's note planted on the copy is what
// gives the session something to undo (LibraryEditSession adopts it when
// it learns the stick's paths), and the press runs a real undo of it.
TestCase {
    id: testCase
    name: "UndoOffered"
    width: 1200
    height: 800
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    // tests/qml/ -> tests/fixtures/one_stick_two_catalogs: a hundred
    // tracks a side, so the pages' own scans are quick.
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/one_stick_two_catalogs").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/is not a function/);
    }

    function cleanup() {
        EditSessionRegistry.discardAll();
    }

    function findWhere(item, predicate) {
        if (!item) {
            return null;
        }
        if (predicate(item)) {
            return item;
        }
        const kids = item.children ? item.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findWhere(kids[i], predicate);
            if (found) {
                return found;
            }
        }
        return null;
    }

    // The control by its name; else any button saying "Undo Last Save",
    // so a page that gains one without the name is still found (and the
    // check below then says the name is missing). Not EditSessionHost's
    // own, which says the same on the notice of an interrupted save and
    // is shown only then.
    function undoControl(page) {
        const named = findChild(page, "undoLastSaveButton") || findChild(page.header, "undoLastSaveButton");
        if (named) {
            return named;
        }
        const byText = (item) => item.text === "Undo Last Save" && item.clicked !== undefined
            && item.objectName !== "undoInterruptedSaveButton";
        return findWhere(page, byText) || findWhere(page.header, byText);
    }

    // ---- Pages that take their controller from outside: a stand-in ----

    Component {
        id: cueTableControllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property int undoCalls: 0
            property bool stickReadOnly: false
            property bool cueTablesChecked: true
            property string cueTablesError: ""
            property var cueTableRows: []
            property int cueTableRowCount: 0
            property int cueTableStagedCount: 0
            property int cueTableLeftAloneCount: 0
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            function scan(a, b, c) {}
            function cancelScan() {}
            function repairCueTables(ids) {}
            function unstageCueTable(id) {}
            function unstageCueTables() {}
            function undoLastOperation() { undoCalls++; }
        }
    }
    Component {
        id: cueTablePageComponent
        CueTablePage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    Component {
        id: exportSyncControllerComponent
        QtObject {
            property var rows: null
            readonly property var sectionCounts: ({})
            readonly property var sectionCheckedCounts: ({})
            readonly property var categoryCounts: ({})
            property bool analyzed: true
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property int undoCalls: 0
            property bool onlyCues: false
            property bool proposalEmpty: true
            property bool hasBaseline: true
            property bool scanCancellable: false
            property int checkedCount: 0
            property int conflictCount: 0
            property int stagedCount: 0
            property int scanCurrent: 0
            property int scanTotal: 0
            property string scanLabel: ""
            property string errorMessage: ""
            property string statusMessage: ""
            property string introText: ""
            readonly property var legendTexts: ({})
            signal scanCancelled()
            function analyze() {}
            function cancelScan() {}
            function setIncluded() {}
            function setSectionIncluded() {}
            function resolveConflict() {}
            function clearConflictResolution() {}
            function resolveAllConflicts() {}
            function clearAllConflictResolutions() {}
            function stageSelected() {}
            function unstageAll() {}
            function undoLastOperation() { undoCalls++; }
        }
    }
    Component {
        id: exportSyncPageComponent
        RekordboxExportSyncPage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            appSettingsController: realAppSettings
            playbackController: realPlayback
        }
    }

    // What JunkCuePage reads (see tst_JunkCuePage's stand-in), and an undo.
    Component {
        id: junkControllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property int undoCalls: 0
            property bool scanCancellable: true
            property int scanCurrent: 0
            property int scanTotal: 0
            property string scanningFormat: ""
            property string scanPhase: ""
            property int unstagedJunkCueCount: 0
            property var junkCues: ListModel {}
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            property string errorMessage: ""
            property string statusMessage: ""
            signal scanCancelled()
            function scan(a, b, c) {}
            function cancelScan() {}
            function undoLastOperation() { undoCalls++; }
        }
    }
    Component {
        id: junkPageComponent
        JunkCuePage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            appSettingsController: realAppSettings
        }
    }

    // The control is there, named, hidden with nothing to undo, shown
    // and usable once there is, and a press is the controller's undo.
    function verifyOfferedThrough(page, fake) {
        const undo = undoControl(page);
        verify(undo !== null, "the page has an Undo Last Save control");
        compare(undo.objectName, "undoLastSaveButton");
        compare(undo.text, "Undo Last Save");
        compare(undo.visible, false, "nothing saved, nothing to undo");
        fake.canUndo = true;
        compare(undo.visible, true, "a save to undo is offered");
        verify(undo.enabled, "and can be pressed");
        undo.clicked();
        compare(fake.undoCalls, 1, "the press is the controller's undo");
        fake.canUndo = false;
        compare(undo.visible, false, "and goes once it is done");
    }

    // Library Health's check pages share HealthCheckPage, and its button.
    function test_aHealthCheckPageOffersUndo() {
        const fake = createTemporaryObject(cueTableControllerComponent, testCase);
        const page = createTemporaryObject(cueTablePageComponent, testCase, {sharedController: fake});
        verify(page !== null);
        verify(page.consistencyController === fake, "precondition: the page reads the stand-in");
        verifyOfferedThrough(page, fake);
    }

    function test_syncAfterRekordboxExportOffersUndo() {
        const fake = createTemporaryObject(exportSyncControllerComponent, testCase,
                                           {rows: stickFixture.emptyExportSyncRows()});
        const page = createTemporaryObject(exportSyncPageComponent, testCase, {controller: fake});
        verify(page !== null);
        verifyOfferedThrough(page, fake);
    }

    // #78: Clean Up Stray Cues saves through its own host and has no
    // Undo Last Save; the undo is only reachable from another page. The
    // day the button is added this turns into an XPASS: take the
    // expectFail out then, and the rest of the check runs.
    function test_cleanUpStrayCuesOffersUndo() {
        // Not a temporary: the page outlives a temporary stand-in by a
        // moment at teardown, and reads it then.
        const fake = junkControllerComponent.createObject(testCase);
        const page = createTemporaryObject(junkPageComponent, testCase, {controllerForTesting: fake});
        verify(page !== null);
        verify(page.consistencyController === fake, "precondition: the page reads the stand-in");
        expectFail("", "#78: Clean Up Stray Cues has no Undo Last Save button");
        verify(undoControl(page) !== null, "the page has an Undo Last Save control");
        verifyOfferedThrough(page, fake);
    }

    // ---- Pages that make their own controller: the real one ----

    Component {
        id: cleanupPageComponent
        CleanupPage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }
    Component {
        id: duplicatesPageComponent
        DuplicatesPage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }
    Component {
        id: syncPageComponent
        SyncPage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    function test_aPageWithItsOwnControllerOffersTheSessionsUndo_data() {
        return [
            {tag: "CleanupPage", page: cleanupPageComponent},
            {tag: "DuplicatesPage", page: duplicatesPageComponent},
            {tag: "SyncPage", page: syncPageComponent},
        ];
    }

    function test_aPageWithItsOwnControllerOffersTheSessionsUndo(data) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const pioneer = stick + "/PIONEER";
        const engine = stick + "/Engine Library";
        const libraryId = EditSessionRegistry.libraryIdForPath(pioneer);
        verify(libraryId.length > 0);
        // Every copy in the scratch folder is on one filesystem, so one
        // library id: whatever an earlier test left in that session (the
        // backups of its last save) is not this page's to offer.
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        verify(!EditSessionRegistry.hasSession(libraryId), "precondition: no session left over");

        // Nothing saved on this stick: the page's scan finishes (the
        // button is usable only then) and Undo stays hidden.
        const props = {rekordboxPath: pioneer, enginePath: engine};
        const quiet = createTemporaryObject(data.page, testCase, props);
        verify(quiet !== null, "page did not instantiate");
        const hidden = undoControl(quiet);
        verify(hidden !== null, "the page has an Undo Last Save control");
        compare(hidden.objectName, "undoLastSaveButton");
        tryVerify(() => hidden.enabled, 300000, "the page's scan finishes");
        compare(hidden.visible, false, "nothing saved, nothing to undo");
        quiet.destroy();
        wait(0);

        // A save that never finished, as a pulled stick leaves it: its
        // backup and the note naming it.
        const planted = backupFixture.plantRecords(stick, 1, 0);
        compare(planted.automatic.length, 1);
        verify(backupFixture.noteInProgress(stick, planted.automatic), "the note is written");

        const page = createTemporaryObject(data.page, testCase, props);
        verify(page !== null, "page did not instantiate");
        const session = EditSessionRegistry.sessionFor(libraryId);
        verify(session !== null);
        tryVerify(() => session.canUndo, 5000, "precondition: the session adopted the interrupted save");
        const undo = undoControl(page);
        verify(undo !== null);
        tryCompare(undo, "visible", true, 5000, "a save to undo is offered");
        tryVerify(() => undo.enabled, 300000, "the page's scan finishes");

        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        undo.clicked();
        tryVerify(() => summary !== null, 120000, "the press ran the session's undo");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.unit, "undo steps", JSON.stringify(summary));
        verify(!session.canUndo, "undone, nothing left to undo");
        tryCompare(undo, "visible", false, 5000, "and the button goes");
    }
}
