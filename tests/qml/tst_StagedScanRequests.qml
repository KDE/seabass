// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Sync Cue Points, Duplicate Tracks (both StagedCueEditController), Clean
// Up, Library Health (with its check pages, Junk Cues and Cover Art) and
// Stick Statistics scan a stick the same way, and this runs the rule of
// docs/async-requests.md against all five, with the stick's rekordbox
// read held at a gate (catalogGate). Each page pops on scanCancelled, so
// a cancel that did not end the scan left it on its overlay with Cancel
// pressed.
TestCase {
    id: testCase
    name: "StagedScanRequests"
    when: windowShown

    Component {
        id: syncComponent
        SyncController {}
    }
    Component {
        id: duplicatesComponent
        DuplicatesController {}
    }
    Component {
        id: cleanupComponent
        CleanupController {}
    }
    Component {
        id: healthComponent
        LibraryConsistencyController {}
    }
    Component {
        id: statisticsComponent
        StickStatisticsController {}
    }

    // One entry per controller: how to build it and how to ask it to
    // read a stick.
    function kinds() {
        return [
            {
                name: "sync",
                progress: true,
                component: syncComponent,
                read: (controller, root) => controller.analyze(root + "/PIONEER", root + "/Engine Library", ""),
            },
            {
                name: "duplicates",
                progress: true,
                component: duplicatesComponent,
                read: (controller, root) => controller.scan("rekordbox", root + "/PIONEER"),
            },
            {
                name: "cleanup",
                progress: true,
                component: cleanupComponent,
                read: (controller, root) => controller.scan("rekordbox", root + "/PIONEER", "", ""),
            },
            {
                name: "health",
                progress: true,
                component: healthComponent,
                read: (controller, root) => controller.scan(root + "/PIONEER", root + "/Engine Library", ""),
            },
            {
                name: "statistics",
                progress: true,
                component: statisticsComponent,
                read: (controller, root) => controller.scan("GATED", root + "/PIONEER", root + "/Engine Library"),
            },
        ];
    }

    function fixture() {
        return decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function stick() {
        const root = stickFixture.stickCopy(fixture());
        verify(root.length > 0, "the fixture copy must be made");
        return root;
    }

    function make(kind) {
        const controller = createTemporaryObject(kind.component, testCase);
        verify(controller, kind.name + " must build");
        return controller;
    }

    function verifyIdleOnceNothingReads(controllers) {
        tryVerify(() => catalogGate.waiting() === 0, 5000, "the gated reads must be over");
        verify(browseFixture.waitForScans(), "every worker must have returned");
        for (const controller of controllers) {
            tryVerify(() => !controller.busy, 2000, "nothing is reading, so nothing may say it is");
        }
    }

    function cleanup() {
        // A test that failed while it held a repair lets it end here, or
        // the controller's destructor would wait out the stand-in.
        controllerFixture.restoreFilesystemRepair();
        catalogGate.restore();
        browseFixture.waitForScans();
    }

    function kindRows() {
        return kinds().map(kind => ({tag: kind.name, kind: kind}));
    }

    function test_aScanThatEndsEndsBusy_data() { return kindRows(); }
    function test_aScanThatEndsEndsBusy(data) {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make(data.kind);
        data.kind.read(controller, root);
        verify(controller.busy, "reading");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the read ended, so busy must end");
        compare(controller.errorMessage, "", "without an error");
        verifyIdleOnceNothingReads([controller]);
    }

    function test_cancelLetsGoOfAScanThatCannotNotice_data() { return kindRows(); }
    function test_cancelLetsGoOfAScanThatCannotNotice(data) {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make(data.kind);
        const cancelled = signalSpy.createObject(testCase, {target: controller, signalName: "scanCancelled"});
        data.kind.read(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.cancelScan();
        tryVerify(() => !controller.busy, 500, "cancel must end busy without waiting for the read");
        compare(cancelled.count, 1, "and say so once, so the page can leave");
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
        compare(cancelled.count, 1, "and only once");
    }

    // Another stick asked for mid-scan is read. It used to be dropped
    // while the scope it named was already recorded.
    function test_anotherStickSupersedesTheScan_data() { return kindRows(); }
    function test_anotherStickSupersedesTheScan(data) {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        const controller = make(data.kind);
        data.kind.read(controller, first);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first read must reach the gate");
        data.kind.read(controller, second);
        verify(controller.busy, "reading the second");
        tryVerify(() => catalogGate.passes() === 2, 5000, "the second stick must be read too");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the second scan must end");
        verifyIdleOnceNothingReads([controller]);
    }

    function test_theSameStickAgainIsServedByTheScan_data() { return kindRows(); }
    function test_theSameStickAgainIsServedByTheScan(data) {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make(data.kind);
        data.kind.read(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        data.kind.read(controller, root);
        wait(50);
        compare(catalogGate.passes(), 1, "asking again starts no second read");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the one scan answers both");
        verifyIdleOnceNothingReads([controller]);
        compare(catalogGate.passes(), 1, "one read in all, after it ended as well");
    }

    function test_aPulledStickEndsItsScan_data() { return kindRows(); }
    function test_aPulledStickEndsItsScan(data) {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make(data.kind);
        data.kind.read(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(root);
        tryVerify(() => !controller.busy, 500, "the stick is gone, so the scan is over");
        verify(controller.errorMessage.indexOf("removed") >= 0, "and says why: " + controller.errorMessage);
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
    }

    // Leaving the page mid-scan stops the read. Neither controller had a
    // destructor, so the worker went on reading the stick for a page
    // that was gone, holding the catalog's pass for everyone else.
    function test_leavingMidScanStopsTheRead_data() { return kindRows(); }
    function test_leavingMidScanStopsTheRead(data) {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = data.kind.component.createObject(testCase);
        data.kind.read(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.destroy();
        wait(0);
        tryVerify(() => catalogGate.waiting() === 0, 1000, "the read must be told to stop");
        verifyIdleOnceNothingReads([]);
    }

    Component {
        id: signalSpy
        SignalSpy {}
    }

    // Clean Up's Resolve popup asks for a merge plan for one pair; a plan
    // for another pair asked for before the first lands is the one that
    // is worked out. It used to be dropped, and the first pair's plan
    // landed under the second pair's names.
    function test_aMergePlanForAnotherPairSupersedes() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make(kindRows()[2].kind);
        controller.planManualMerge("rekordbox", root + "/PIONEER", "1", "2");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first plan's read must reach the gate");
        controller.planManualMerge("rekordbox", root + "/PIONEER", "2", "3");
        tryVerify(() => catalogGate.passes() === 2, 5000, "the second pair must be read too");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the second plan must end");
        verifyIdleOnceNothingReads([controller]);
    }

    // A repair, a save or an undo changes the stick, and Library Health
    // reads it again afterwards. With a scan already running that rescan
    // was dropped, and the page kept showing what the scan had read from
    // before the change. It supersedes the running scan now.
    function test_healthRescansAfterARepairEvenMidScan() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make(kindRows()[3].kind);
        controller.scan(root + "/PIONEER", root + "/Engine Library", "");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first scan must reach the gate");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the first scan must end");

        controllerFixture.makeFilesystemRepairSucceedWhenFinished();
        controller.repairStickFilesystem();
        // A scan asked for while the repair runs, held at a fresh gate.
        // The repair ends only once that scan has reached the gate: a
        // scan counts the stick before it reads (#58), and a repair that
        // ended first was answered by a rescan that superseded the scan
        // before its read, so the gate saw one pass and this could not
        // tell a rescan from none.
        catalogGate.hold(3, true);
        controller.scan(root + "/PIONEER", root + "/Engine Library", "");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the scan during the repair must reach the gate");
        compare(catalogGate.passes(), 1, "the scan during the repair is the one at the gate");
        verify(controller.repairingFilesystem, "and the repair is still running");
        controllerFixture.finishFilesystemRepair();
        tryVerify(() => catalogGate.passes() === 2, 5000,
                  "the repair's rescan must read the stick again, not be dropped behind the running scan");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the rescan must end");
        controllerFixture.restoreFilesystemRepair();
        verifyIdleOnceNothingReads([controller]);
    }

    // The same stick asked for again is served by the scan running, and
    // that scan's progress bar stays its own. It froze at 0 of 0: the
    // second request reset the bar and took over its reporter before
    // finding out it was served, so the running scan's reports went
    // nowhere. The total is the scan's whole plan (#58), counted from
    // the fixture's catalogs, so it is only known to be positive here;
    // what matters is that the second request leaves it as it was.
    function test_aServedRequestLeavesTheBarMoving_data() {
        return kindRows().filter(row => row.kind.progress === true);
    }
    function test_aServedRequestLeavesTheBarMoving(data) {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make(data.kind);
        data.kind.read(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        tryVerify(() => controller.scanTotal > 0 && controller.scanCurrent > 0, 2000,
                  "the bar shows the read's progress");
        const total = controller.scanTotal;
        data.kind.read(controller, root);
        wait(50);
        compare(catalogGate.passes(), 1, "served by the running read");
        compare(controller.scanTotal, total, "and the bar still shows that read's progress");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000);
        verifyIdleOnceNothingReads([controller]);
    }
}
