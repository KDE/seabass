// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The real BackupAdvisorController, as the home's Backup group reads it:
// when it says it is busy, which sticks it names as pending, and the two
// steps it publishes advice in. These lived in tst_BackupsHubPage.qml
// until that page went; they are about the advisor, not the page.
TestCase {
    id: testCase
    name: "BackupAdvisor"
    when: windowShown

    Component {
        id: realAdvisorComponent
        BackupAdvisorController {}
    }

    // tests/qml/ -> tests/fixtures/anonymized_library/rekordbox, the
    // committed library, read only (the advisor never writes).
    readonly property string fixtureRekordbox: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library/rekordbox").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // The real advisor, two sticks carrying the same rekordbox library:
    // each one's first step publishes advice at once, and while either
    // side's cues are still to come the two only match "so far", so both
    // pieces of advice say cuesPending, with neither stick in pending any
    // more and busy still true. The second steps settle it: both false,
    // busy false. Holds whether the cache's stages are real or every stage
    // is read in full: the steps are the advisor's own.
    function test_realAdvisorTwoStepsOverTheFixture() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const a = "/nonexistent/seabass-cues-test/A";
        const b = "/nonexistent/seabass-cues-test/B";
        const seen = [];
        const snapshot = function(signal) {
            const pendingA = advisor.advice[a] ? advisor.advice[a].cuesPending : undefined;
            const pendingB = advisor.advice[b] ? advisor.advice[b].cuesPending : undefined;
            seen.push({signal: signal, a: pendingA, b: pendingB, pending: advisor.pending.slice(), busy: advisor.busy});
        };
        advisor.adviceChanged.connect(function() { snapshot("advice"); });
        advisor.pendingChanged.connect(function() { snapshot("pending"); });
        advisor.assess("A", a, testCase.fixtureRekordbox, "");
        advisor.assess("B", b, testCase.fixtureRekordbox, "");
        tryVerify(function() { return !advisor.busy; }, 60000);
        const trace = JSON.stringify(seen);
        const provisional = seen.filter(function(s) {
            return s.a === true && s.b === true && s.pending.length === 0;
        });
        verify(provisional.length > 0, "both verdicts up, both checking cues, neither pending: " + trace);
        compare(provisional[0].busy, true, "busy covers the second step: " + trace);
        const advice = seen.filter(function(s) { return s.signal === "advice"; });
        compare(advice.length, 4, "two first steps and two second steps: " + trace);
        compare(advice[3].a, false, trace);
        compare(advice[3].b, false, trace);
        compare(advisor.pending.length, 0);
    }

    // The real advisor says it is busy when it says anything: it used to
    // announce the change before the pass was running, so a page asking on
    // the signal heard "not busy" and never heard otherwise.
    function test_realAdvisorAnnouncesBusyOnceItIs() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const seen = [];
        advisor.busyChanged.connect(function() { seen.push(advisor.busy); });
        advisor.assess("GHOST", "/nonexistent/seabass-advisor-test/GHOST", "", "");
        tryVerify(function() { return seen.length >= 2 && !advisor.busy; }, 10000);
        compare(seen[0], true);
        compare(seen[seen.length - 1], false);
    }

    // The real advisor names the sticks it is reading, one at a time: with
    // three queued, the first leaves the list once it has been read while
    // the other two are still in it, and busy covers all three.
    function test_realAdvisorPendingNamesEachStick() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        const seen = [];
        advisor.pendingChanged.connect(function() { seen.push({pending: advisor.pending.slice(), busy: advisor.busy}); });
        advisor.assess("A", "/nonexistent/seabass-advisor-test/A", "", "");
        advisor.assess("B", "/nonexistent/seabass-advisor-test/B", "", "");
        advisor.assess("C", "/nonexistent/seabass-advisor-test/C", "", "");
        compare(advisor.pending.slice(), ["/nonexistent/seabass-advisor-test/A", "/nonexistent/seabass-advisor-test/B",
                                          "/nonexistent/seabass-advisor-test/C"]);
        tryVerify(function() { return !advisor.busy; }, 10000);
        compare(advisor.pending.length, 0);
        const firstDone = seen.filter(function(s) {
            return s.pending.indexOf("/nonexistent/seabass-advisor-test/A") < 0 && s.pending.length === 2;
        });
        verify(firstDone.length > 0, "A left the list while B and C were still waiting: " + JSON.stringify(seen));
        compare(firstDone[0].busy, true);
        compare(seen[seen.length - 1].pending.length, 0);
    }

    // Busy until the pass's result has been handled, not until its worker
    // thread returns. A stick that is not there is assessed in well under
    // a millisecond; the UI thread is held here without turning the event
    // loop, as a busy frame holds it, so the worker is certainly done
    // before its result is taken. "Not busy" in that window is what made
    // the test above flake: the announcement of a pass said it was over.
    function test_realAdvisorIsBusyUntilItsResultLands() {
        const advisor = createTemporaryObject(realAdvisorComponent, testCase);
        advisor.assess("GHOST", "/nonexistent/seabass-advisor-test/GHOST", "", "");
        const until = Date.now() + 200;
        while (Date.now() < until) {
            // Hold the UI thread; the worker finishes meanwhile.
        }
        verify(advisor.busy, "still busy: the result has not been handled yet");
        tryVerify(function() { return !advisor.busy; }, 10000, "and done once it has");
    }
}
