// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Metadata Backup's read of a stick, held at a gate (catalogGate) so each
// case can act while it is still going: the rule of
// docs/async-requests.md, case by case. The page said "scanning" for as
// long as busy stayed on, so every case ends by asking that busy is off
// once nothing is reading.
TestCase {
    id: testCase
    name: "MetadataBackupRequests"
    when: windowShown

    Component {
        id: controllerComponent
        MetadataBackupController {}
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
        controller.selectStick(root + "/PIONEER", "", "GATED");
        verify(controller.busy, "reading");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the read ended, so busy must end");
        verify(controller.hasScanned, "with a plan");
        compare(controller.proposalCount, 3, "of the three gated tracks");
        verifyIdleOnceNothingReads([controller]);
    }

    // Cancel is over at once for the page, even when the read cannot
    // look at its token. Before the rule, busy stayed on until the read
    // finished on its own, with Cancel pressed and nothing to show for it.
    function test_cancelLetsGoOfAReadThatCannotNotice() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.cancel();
        tryVerify(() => !controller.busy, 500, "cancel must end busy without waiting for the read");
        verify(controller.scanCancelled, "and say there is no plan");

        // Asked again: a fresh request, answered once the stuck read is
        // over, never the cancelled one's answer.
        controller.selectStick(root + "/PIONEER", "", "GATED");
        verify(controller.busy, "the new request is reading");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the new request must end");
        verify(controller.hasScanned, "with a plan");
        verify(!controller.scanCancelled, "and not as cancelled");
        verifyIdleOnceNothingReads([controller]);
    }

    // A second stick asked for while the first is read is the one the
    // page shows. It used to be dropped while selectStick said yes, and
    // the first stick's list landed instead.
    function test_anotherStickSupersedesTheRead() {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.selectStick(first + "/PIONEER", "", "FIRST");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first read must reach the gate");
        verify(controller.selectStick(second + "/PIONEER", "", "SECOND"), "the second stick is accepted");
        compare(controller.sourceLibraryPath, second + "/PIONEER", "and is the source");
        verify(controller.busy, "reading it");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the second read must end");
        compare(controller.sourceLibraryPath, second + "/PIONEER", "the list is the second stick's");
        compare(controller.sourceStickLabel, "SECOND");
        verify(controller.hasScanned, "with a plan");
        verifyIdleOnceNothingReads([controller]);
    }

    // The same stick again is answered by the read already under way.
    function test_theSameStickAgainIsServedByTheRead() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        verify(controller.selectStick(root + "/PIONEER", "", "GATED"), "asking again is accepted");
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
        one.selectStick(root + "/PIONEER", "", "GATED");
        two.selectStick(root + "/PIONEER", "", "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        wait(150);
        compare(catalogGate.passes(), 1, "the second page waits for the first page's read");
        catalogGate.release();
        tryVerify(() => !one.busy && !two.busy, 10000, "both must end");
        verify(one.hasScanned && two.hasScanned, "both with a plan");
        compare(one.proposalCount, two.proposalCount, "the same plan");
        verifyIdleOnceNothingReads([one, two]);
    }

    // A stick pulled while it is read ends the read, in error, even when
    // the read is stuck on the device that went away.
    function test_aPulledStickEndsItsRead() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(root);
        tryVerify(() => !controller.busy, 500, "the stick is gone, so the read is over");
        verify(controller.errorMessage.indexOf("removed") >= 0, "and says why: " + controller.errorMessage);
        verify(controller.scanCancelled, "with no plan");
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
    }

    // Another stick going away leaves this read alone.
    function test_anotherPulledStickLeavesTheReadAlone() {
        const root = stick();
        const other = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "GATED");
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
        controller.selectStick(root + "/PIONEER", "", "GATED");
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

    // Back to the store and then the same stick again while its read was
    // still going: the read was taken as the answer, but the page had
    // left it, and stayed on the store with the stick's plan landing
    // behind it. Going to the store ends the read now, and the stick is
    // read afresh.
    function test_theStoreInBetweenEndsTheRead() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        verify(controller.browseStore(), "back to the store");
        verify(!controller.busy, "and the read is over for the page");
        controller.selectStick(root + "/PIONEER", "", "GATED");
        verify(!controller.browsingStore, "the stick is the source again");
        verify(controller.busy, "being read");
        catalogGate.release();
        tryVerify(() => !controller.busy, 10000, "the read must end");
        verify(controller.hasScanned && !controller.browsingStore, "with the stick's plan, showing the stick");
        verifyIdleOnceNothingReads([controller]);
    }

    // Another stick asked for while a save writes is read once the save is
    // over. It used to be dropped while the page was told yes, and the
    // save's own rescan put the stick just left back on the list.
    function test_aStickAskedForDuringASaveIsReadAfterIt() {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        catalogGate.setTrackCountFor(second + "/PIONEER", 5);
        catalogGate.release();
        const controller = make();
        controller.selectStick(first + "/PIONEER", "", "FIRST");
        tryVerify(() => !controller.busy && controller.hasScanned, 10000, "the first stick must be read");
        verify(controller.proposalCount > 0, "with something to back up");

        catalogGate.holdStore();
        controller.toggleStagedForAdd(0);
        controller.save();
        tryVerify(() => catalogGate.storeWaiting(), 5000, "the save must be writing");
        controller.discardStagingAndSelectStick(second + "/PIONEER", "", "SECOND");
        catalogGate.releaseStore();
        tryVerify(() => !controller.busy && controller.hasScanned
                  && controller.sourceLibraryPath === second + "/PIONEER", 10000,
                  "the stick asked for during the save must be the one read after it, got "
                  + controller.sourceLibraryPath);
        compare(controller.sourceStickLabel, "SECOND");
        verifyIdleOnceNothingReads([controller]);
    }

    // Reads a stick and stages its first proposal, ready to save.
    function readAndStage(root) {
        const controller = make();
        controller.selectStick(root + "/PIONEER", "", "FIRST");
        tryVerify(() => !controller.busy && controller.hasScanned, 10000, "the first stick must be read");
        controller.toggleStagedForAdd(0);
        return controller;
    }

    // A save that fails says so, even when a stick asked for during it is
    // read straight after: the read used to clear the banner as it began.
    function test_aFailedSaveStillSaysSoWhenTheNextStickIsRead() {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        catalogGate.release();
        const controller = readAndStage(first);
        catalogGate.holdStore(true);
        controller.save();
        tryVerify(() => catalogGate.storeWaiting(), 5000, "the save must be writing");
        controller.discardStagingAndSelectStick(second + "/PIONEER", "", "SECOND");
        catalogGate.releaseStore();
        tryVerify(() => !controller.busy && controller.sourceLibraryPath === second + "/PIONEER"
                  && controller.hasScanned, 10000, "the second stick is read after the save");
        verify(controller.errorMessage.indexOf("could not be written") >= 0,
               "and the save's failure is still said: '" + controller.errorMessage + "'");
        verifyIdleOnceNothingReads([controller]);
    }

    // The last choice made during a save wins. Going back to the store, or
    // back to the stick being saved from, drops a stick asked for before.
    function test_theLastChoiceDuringASaveWins_data() {
        return [{tag: "store"}, {tag: "same stick"}];
    }
    function test_theLastChoiceDuringASaveWins(data) {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        catalogGate.release();
        const controller = readAndStage(first);
        catalogGate.holdStore();
        controller.save();
        tryVerify(() => catalogGate.storeWaiting(), 5000, "the save must be writing");
        controller.discardStagingAndSelectStick(second + "/PIONEER", "", "SECOND");
        if (data.tag === "store") {
            verify(controller.browseStore(), "back to the store");
        } else {
            verify(controller.selectStick(first + "/PIONEER", "", "FIRST"), "back to the first stick");
        }
        catalogGate.releaseStore();
        tryVerify(() => !controller.busy, 10000, "the save and whatever follows must end");
        verify(browseFixture.waitForScans(), "every worker must have returned");
        wait(50);
        tryVerify(() => !controller.busy, 5000);
        if (data.tag === "store") {
            verify(controller.browsingStore, "still showing the store, got " + controller.sourceLibraryPath);
        } else {
            compare(controller.sourceLibraryPath, first + "/PIONEER", "still showing the first stick");
        }
        verifyIdleOnceNothingReads([controller]);
    }
}
