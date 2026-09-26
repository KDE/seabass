// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Restore Metadata's read of a stick, held at a gate (catalogGate): the
// rule of docs/async-requests.md, case by case, as for Metadata Backup
// (tst_MetadataBackupRequests.qml). Every case ends by asking that busy
// is off once nothing is reading.
TestCase {
    id: testCase
    name: "MetadataRestoreRequests"
    when: windowShown

    Component {
        id: controllerComponent
        MetadataRestoreController {}
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

    function make() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        verify(controller, "the controller must build");
        return controller;
    }

    // Nothing reading, nothing claimed: the hang this file exists for was
    // a page on "scanning" with no thread doing anything.
    function verifyIdleOnceNothingReads(controllers) {
        tryVerify(() => catalogGate.waiting() === 0, 5000, "the gated reads must be over");
        verify(browseFixture.waitForScans(), "every worker must have returned");
        for (const controller of controllers) {
            tryVerify(() => !controller.busy, 2000, "nothing is reading, so nothing may say it is");
        }
    }

    function cleanup() {
        catalogGate.restore();
        browseFixture.waitForScans();
    }

    function test_aReadThatEndsEndsBusy() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.scan(root + "/PIONEER");
        verify(controller.busy, "reading");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the read ended, so busy must end");
        verify(controller.hasScanned, "with a plan");
        compare(controller.stickTrackCount, 3, "of the three gated tracks");
        verifyIdleOnceNothingReads([controller]);
    }

    // Cancel is over at once for the page, even when the read cannot
    // look at its token. Before the rule, busy stayed on until the read
    // finished on its own, with Cancel pressed and nothing to show for it.
    function test_cancelLetsGoOfAReadThatCannotNotice() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make();
        controller.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.cancelScan();
        tryVerify(() => !controller.busy, 500, "cancel must end busy without waiting for the read");
        verify(!controller.hasScanned, "and there is no plan");

        // Asked again: a fresh request, answered once the stuck read is
        // over, never the cancelled one's answer.
        controller.scan(root + "/PIONEER");
        verify(controller.busy, "the new request is reading");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the new request must end");
        verify(controller.hasScanned, "with a plan");
        verifyIdleOnceNothingReads([controller]);
    }

    // A second stick asked for while the first is read is the one the
    // page shows. It used to be dropped while selectStick said yes, and
    // the first stick's list landed instead.
    function test_anotherStickSupersedesTheRead() {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        catalogGate.setTrackCountFor(second + "/PIONEER", 5);
        const controller = make();
        controller.scan(first + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first read must reach the gate");
        controller.scan(second + "/PIONEER");
        verify(controller.busy, "reading it");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the second read must end");
        tryVerify(() => controller.hasScanned, 10000, "with a plan");
        compare(controller.stickTrackCount, 5, "the list is the second stick's");
        verifyIdleOnceNothingReads([controller]);
    }

    // The same stick again is answered by the read already under way.
    function test_theSameStickAgainIsServedByTheRead() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.scan(root + "/PIONEER");
        wait(50);
        compare(catalogGate.passes(), 1, "and starts no second read");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the one read answers both");
        verify(controller.hasScanned, "with a plan");
        compare(catalogGate.passes(), 1, "one read in all");
        verifyIdleOnceNothingReads([controller]);
    }

    // Two pages on one stick: one read, both answered.
    function test_twoPagesWaitOnOneRead() {
        const root = stick();
        catalogGate.hold(3, true);
        const one = make();
        const two = make();
        one.scan(root + "/PIONEER");
        two.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        wait(150);
        compare(catalogGate.passes(), 1, "the second page waits for the first page's read");
        catalogGate.release();
        tryVerify(() => !one.busy && !two.busy, 10000, "both must end");
        verify(one.hasScanned && two.hasScanned, "both with a plan");
        compare(one.stickTrackCount, two.stickTrackCount, "the same plan");
        verifyIdleOnceNothingReads([one, two]);
    }

    // A stick pulled while it is read ends the read, in error, even when
    // the read is stuck on the device that went away.
    function test_aPulledStickEndsItsRead() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make();
        controller.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(root);
        tryVerify(() => !controller.busy, 500, "the stick is gone, so the read is over");
        verify(controller.errorMessage.indexOf("removed") >= 0, "and says why: " + controller.errorMessage);
        verify(!controller.hasScanned, "with no plan");
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
    }

    // Another stick going away leaves this read alone.
    function test_anotherPulledStickLeavesTheReadAlone() {
        const root = stick();
        const other = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(other);
        wait(50);
        verify(controller.busy, "still reading");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000);
        verify(controller.hasScanned, "with a plan");
        verifyIdleOnceNothingReads([controller]);
    }

    // Leaving the page mid-read does not wait for a read that cannot stop.
    // It used to: the destructor waited for the worker, and a worker stuck
    // where it could not see the cancel froze the window with it.
    function test_leavingMidReadDoesNotWaitForIt() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = controllerComponent.createObject(testCase);
        controller.scan(root + "/PIONEER");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        // The safety net if the page does wait: without it the UI thread
        // would be stuck for good and the suite with it.
        catalogGate.releaseAfter(6000);
        const started = Date.now();
        controller.destroy();
        wait(0);
        const took = Date.now() - started;
        // A bounded moment for the worker to notice, then it is let go.
        verify(took < 2500, "leaving took " + took + " ms");
        verifyIdleOnceNothingReads([]);
    }
}
