// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Stick Performance's measurement reads the stick (its catalog for the
// sample, then the files), and falls under the rule of
// docs/async-requests.md like every other page's read. Found by the storm
// (seed 141): leaving the page while the catalog read could not see its
// token froze the window, because the controller's destructor waited for
// the measurement without a bound. The stick's rekordbox read is held at
// catalogGate, as for the other controllers.
TestCase {
    id: testCase
    name: "StickPerformanceRequests"
    when: windowShown

    Component {
        id: controllerComponent
        StickPerformanceController {}
    }
    Component {
        id: signalSpy
        SignalSpy {}
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

    function measure(controller, root) {
        controller.measure("GATED", root + "/PIONEER", root + "/Engine Library", root, false);
    }

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

    function test_aMeasurementThatEndsEndsBusy() {
        const root = stick();
        catalogGate.hold(3, true);
        const controller = createTemporaryObject(controllerComponent, testCase);
        measure(controller, root);
        verify(controller.busy, "measuring");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.release();
        tryVerify(() => !controller.busy, 20000, "the measurement ended, so busy must end");
        verifyIdleOnceNothingReads([controller]);
    }

    function test_cancelLetsGoOfAReadThatCannotNotice() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const cancelled = signalSpy.createObject(testCase, {target: controller, signalName: "cancelled"});
        measure(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        controller.cancel();
        tryVerify(() => !controller.busy, 500, "cancel must end busy without waiting for the read");
        compare(cancelled.count, 1, "and say so once");
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
        compare(cancelled.count, 1, "and only once");
    }

    function test_aPulledStickEndsTheMeasurement() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = createTemporaryObject(controllerComponent, testCase);
        measure(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(root);
        tryVerify(() => !controller.busy, 500, "the stick is gone, so the measurement is over");
        verify(controller.errorMessage.indexOf("removed") >= 0, "and says why: " + controller.errorMessage);
        catalogGate.release();
        verifyIdleOnceNothingReads([controller]);
    }

    // The storm's freeze: the page goes while its read cannot stop.
    function test_leavingMidReadDoesNotWaitForAReadThatCannotStop() {
        const root = stick();
        catalogGate.hold(3, false);
        const controller = controllerComponent.createObject(testCase);
        measure(controller, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        // Opened from another thread in 5 s, so a controller that waits
        // for its read fails this case instead of hanging the suite.
        catalogGate.releaseAfter(5000);
        const started = Date.now();
        controller.destroy();
        wait(0);
        const took = Date.now() - started;
        verify(took < 2500, "leaving must not wait for a read that cannot stop (took " + took + " ms)");
        catalogGate.release();
        verifyIdleOnceNothingReads([]);
    }
}
