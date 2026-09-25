// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Clean Up Stray Cues: the floating save button says what this page's
// save does.
//
// The page stages one kind of change and nothing else, so "Save" told
// the user less than it could -- the button is the moment a stack of
// stray cues stops being a list and gets written to the stick.
//
// Paths point nowhere on purpose, the same as tst_PagesCompile: the page
// must build without a stick, and the label is not a property of one.
TestCase {
    id: testCase
    name: "JunkCuePage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        JunkCuePage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            appSettingsController: realAppSettings
        }
    }

    function test_theSaveButtonSaysCleanUp() {
        var page = createTemporaryObject(pageComponent, testCase);
        verify(page !== null, "the page must instantiate");
        var overlay = findChild(page, "saveOverlay");
        verify(overlay, "the standard save overlay must exist");
        compare(overlay.label, "Clean Up", "the save button says what this page's save does");
    }

    // tests/qml/ -> tests/fixtures/anonymized_library, the committed
    // real-scale library (about 1,400 tracks, 214 cues the scan lists).
    // A local path, not a URL (see tst_SettingsPage.qml for why the naive
    // strip is wrong).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // The page on a real stick, so the controller behind it runs its real
    // scan. Paths are handed in at creation, per copy.
    Component {
        id: stickPageComponent
        JunkCuePage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            appSettingsController: realAppSettings
        }
    }

    function openOnAFreshCopy() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        return createTemporaryObject(stickPageComponent, testCase,
                                     {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
    }

    // The scan runs off the UI thread, behind the same overlay Duplicates
    // and Library Health put over theirs. It used to announce itself with
    // a spinner in the header's corner only.
    //
    // The first check is the one that says "not on this thread": the page
    // is complete, scan() has been called and has returned, and the scan
    // is still running. A scan done on the UI thread would be over, its
    // result applied, before the page ever existed to look at.
    function test_theScanRunsBehindAnOverlayAndTheWindowKeepsDrawing() {
        // A 10 ms timer on the UI thread. How long it ever goes without
        // firing is how long the window went without drawing.
        let ticks = 0;
        let lastTick = Date.now();
        let longestGap = 0;
        const ticker = createTemporaryQmlObject("import QtQuick; Timer { interval: 10; repeat: true; running: true }",
                                                testCase);
        ticker.triggered.connect(() => {
            const now = Date.now();
            longestGap = Math.max(longestGap, now - lastTick);
            lastTick = now;
            ticks++;
        });

        const page = openOnAFreshCopy();
        verify(page !== null, "the page must instantiate");
        const overlay = findChild(page, "junkCueBusyOverlay");
        verify(overlay !== null, "the page has a scan overlay");
        verify(overlay.visible, "the scan is still running once the page is up, behind the overlay");
        verify(overlay.label.indexOf("Looking for stray cues") === 0, "the overlay says what it is doing: "
               + overlay.label);
        verify(overlay.cancellable, "a read-only scan can be stopped");
        compare(findChild(page, "junkCueNoneFound").visible, false, "no verdict while scanning");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/JunkCuePage-scanning.png");
        }

        const ticksAtStart = ticks;
        const scanStart = Date.now();
        lastTick = scanStart;
        longestGap = 0;
        tryVerify(() => !overlay.visible, 180000, "the scan finishes");
        const scanMs = Date.now() - scanStart;
        verify(ticks - ticksAtStart >= 3, "the UI thread kept turning while the scan ran: "
               + (ticks - ticksAtStart) + " ticks");
        // Reading a catalog on this thread stalls it for most of the scan;
        // merging a finished catalog's rows into the list is the longest
        // it may stall now (about 0.1 s here, measured).
        verify(longestGap < scanMs / 2, "the UI thread stalled for " + longestGap + " ms of a " + scanMs
               + " ms scan");

        // And its result arrived: the fixture's cues, the whole library,
        // no hedge.
        const summary = findChild(page, "junkCueSummary");
        verify(summary !== null && summary.visible, "the result reaches the page");
        verify(summary.text.indexOf("across the whole library") > 0, summary.text);
        verify(summary.text.indexOf("so far") < 0, "a finished scan is not called incomplete: " + summary.text);
        compare(findChild(page, "junkCueScanStopped").visible, false);
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/JunkCuePage-scanned.png");
        }
    }

    // Stopped from the overlay, the page stays and says the list is not
    // the whole answer. Above all it must not say "No cues are sitting
    // at 0:00" about a library it never finished reading.
    function test_aStoppedScanSaysSoRatherThanNone() {
        const page = openOnAFreshCopy();
        const overlay = findChild(page, "junkCueBusyOverlay");
        verify(overlay.visible, "scanning");
        overlay.cancelRequested();
        tryVerify(() => !overlay.visible, 180000, "the scan stops");
        verify(findChild(page, "junkCueScanStopped").visible, "the page says the scan was stopped");
        compare(findChild(page, "junkCueNoneFound").visible, false, "a stopped scan found nothing, it did not find none");
        const summary = findChild(page, "junkCueSummary");
        if (summary.visible) {
            verify(summary.text.indexOf("so far") > 0, "a partial count says it is partial: " + summary.text);
        }
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/JunkCuePage-stopped.png");
        }
    }

    // A stick that cannot be read (here: never there; in life, pulled
    // mid-scan) is an error on the page and no count at all.
    function test_aScanThatCouldNotReadTheStickDoesNotSayNone() {
        const page = createTemporaryObject(pageComponent, testCase);
        const overlay = findChild(page, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 30000, "the scan ends");
        compare(findChild(page, "junkCueNoneFound").visible, false,
                "an unreadable stick is not a stick without stray cues");
    }

    // Leaving while the scan runs destroys the page and its controller
    // under a task still reading the stick. Nothing may reach the page
    // afterwards, and the next page on the same stick scans as normal.
    function test_leavingMidScanLeavesNothingBehind() {
        failOnWarning(/TypeError|Cannot read property|Unable to assign/);
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        const props = {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"};
        const first = stickPageComponent.createObject(testCase, props);
        verify(findChild(first, "junkCueBusyOverlay").visible, "scanning");
        first.destroy();
        wait(200);

        const second = createTemporaryObject(stickPageComponent, testCase, props);
        const overlay = findChild(second, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 180000, "the next page's scan finishes");
        verify(findChild(second, "junkCueSummary").visible, "and shows what it found");
    }
}
