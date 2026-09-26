// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The backup advisor's steps, with a stick's rekordbox read held at a
// gate (catalogGate): the rule of docs/async-requests.md for a queue of
// reads. The Backups hub says "Scanning existing backups..." for as long
// as its stick is pending, so a step that cannot end kept that up, and
// every stick queued behind it with it.
TestCase {
    id: testCase
    name: "BackupAdvisorRequests"
    when: windowShown

    Component {
        id: advisorComponent
        BackupAdvisorController {}
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
        const advisor = createTemporaryObject(advisorComponent, testCase);
        verify(advisor, "the advisor must build");
        return advisor;
    }

    function assessRekordbox(advisor, root) {
        advisor.assess("GATED", root, root + "/PIONEER", "");
    }

    // An Engine-only stick: its read is not the gated one.
    function assessEngine(advisor, root) {
        advisor.assess("ENGINE", root, "", root + "/Engine Library");
    }

    function pendingHas(advisor, root) {
        return advisor.pending.indexOf(root) >= 0;
    }

    function verifyIdleOnceNothingReads(advisor) {
        tryVerify(() => catalogGate.waiting() === 0, 5000, "the gated reads must be over");
        verify(browseFixture.waitForScans(), "every worker must have returned");
        if (advisor) {
            tryVerify(() => !advisor.busy && advisor.pending.length === 0, 2000,
                      "nothing is reading, so nothing may be pending");
        }
    }

    function cleanup() {
        catalogGate.restore();
        browseFixture.waitForScans();
    }

    function test_aStepThatEndsGivesAdvice() {
        const root = stick();
        catalogGate.hold(3, true);
        const advisor = make();
        assessRekordbox(advisor, root);
        verify(pendingHas(advisor, root), "pending while read");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.release();
        tryVerify(() => !pendingHas(advisor, root), 10000, "the step ended");
        verify(advisor.advice[root] !== undefined, "with advice");
        verifyIdleOnceNothingReads(advisor);
    }

    // A stick forgotten mid-read is over for the advisor at once, and its
    // read, when it does end, does not bring it back.
    function test_forgettingAStickEndsItsStepAndItStaysGone() {
        const root = stick();
        catalogGate.hold(3, false);
        const advisor = make();
        assessRekordbox(advisor, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        advisor.forget(root);
        tryVerify(() => !pendingHas(advisor, root), 500, "a forgotten stick is not pending");
        verify(!advisor.busy, "and nothing else is");
        catalogGate.release();
        verifyIdleOnceNothingReads(advisor);
        wait(50);
        verify(advisor.advice[root] === undefined, "the read that ended afterwards must not bring it back");
    }

    // A stick queued, then forgotten before its turn, is never read.
    function test_aQueuedStickForgottenIsNeverRead() {
        const first = stick();
        const second = stick();
        catalogGate.hold(3, true);
        const advisor = make();
        assessRekordbox(advisor, first);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the first read must reach the gate");
        assessEngine(advisor, second);
        verify(pendingHas(advisor, second), "queued behind the first");
        advisor.forget(second);
        verify(!pendingHas(advisor, second), "forgotten, so not pending");
        catalogGate.release();
        verifyIdleOnceNothingReads(advisor);
        wait(50);
        verify(advisor.advice[first] !== undefined, "the first stick has its advice");
        verify(advisor.advice[second] === undefined, "the forgotten one has none");
    }

    // One stick stuck on its read holds nobody else up once it is
    // forgotten: the next stick starts at once.
    function test_theNextStickStartsWhenAStuckOneIsForgotten() {
        const stuck = stick();
        const next = stick();
        catalogGate.hold(3, false);
        const advisor = make();
        assessRekordbox(advisor, stuck);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the stuck read must reach the gate");
        assessEngine(advisor, next);
        advisor.forget(stuck);
        tryVerify(() => advisor.advice[next] !== undefined, 5000,
                  "the next stick must be read while the stuck one is still at the gate");
        verify(catalogGate.waiting() === 1, "which it still is");
        catalogGate.release();
        verifyIdleOnceNothingReads(advisor);
    }

    // A stick pulled mid-read ends its step, without anyone calling forget.
    function test_aPulledStickEndsItsStep() {
        const root = stick();
        catalogGate.hold(3, false);
        const advisor = make();
        assessRekordbox(advisor, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.announceStickGone(root);
        tryVerify(() => !pendingHas(advisor, root), 500, "the stick is gone, so its step is over");
        catalogGate.release();
        verifyIdleOnceNothingReads(advisor);
    }

    // Tearing the advisor down mid-read does not wait for a read that
    // cannot stop.
    function test_destroyingMidStepDoesNotWaitForIt() {
        const root = stick();
        catalogGate.hold(3, false);
        const advisor = advisorComponent.createObject(testCase);
        assessRekordbox(advisor, root);
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the read must reach the gate");
        catalogGate.releaseAfter(6000);
        const started = Date.now();
        advisor.destroy();
        wait(0);
        const took = Date.now() - started;
        // A bounded moment for the worker to notice, then it is let go.
        verify(took < 2500, "tearing down took " + took + " ms");
        verifyIdleOnceNothingReads(null);
    }
}
