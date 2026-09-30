// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

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

    // Only what the page reads, and a scan that goes where the test says.
    Component {
        id: fakeControllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
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
        }
    }

    // Every step of the scan shows a counted bar and says which step it
    // is. The steps the page used to sit through (the audits it no longer
    // runs) never reported at all, so the bar swept for minutes and the
    // page looked hung; the catalog reads it keeps each report a total.
    function test_theOverlayCountsEveryStepAndNamesIt() {
        const fake = createTemporaryObject(fakeControllerComponent, testCase);
        const page = createTemporaryObject(pageComponent, testCase, {controllerForTesting: fake});
        verify(page.consistencyController === fake, "precondition: the page reads the stand-in");
        const overlay = findChild(page, "junkCueBusyOverlay");
        const report = findChild(overlay, "progressReport");
        fake.busy = true;
        verify(overlay.visible, "scanning");
        const steps = [
            {format: "rekordbox", phase: "Scanning rekordbox tracks", total: 1501},
            {format: "rekordbox", phase: "Reading rekordbox cues", total: 1161},
            {format: "engine", phase: "Scanning Engine tracks", total: 1564},
            {format: "onelibrary", phase: "Reading OneLibrary", total: 1644},
        ];
        for (const step of steps) {
            fake.scanningFormat = step.format;
            fake.scanPhase = step.phase;
            fake.scanTotal = step.total;
            fake.scanCurrent = Math.floor(step.total / 3);
            verify(!report.indeterminate, step.phase + ": a counted bar, not a sweeping one");
            compare(findChild(report, "unitsLabel").text, Math.floor(step.total / 3) + " / " + step.total + " tracks");
            compare(findChild(report, "currentItemLabel").text, step.phase, "the step is named under the bar");
            verify(overlay.label.indexOf(page.formatLabel(step.format)) > 0, "and the catalog above it: "
                   + overlay.label);
        }
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/JunkCuePage-counted-step.png");
        }
        fake.busy = false;
        verify(!overlay.visible);
    }

    Component {
        id: fullControllerComponent
        LibraryConsistencyController {}
    }

    // The stray-cue page reads cues and nothing else. It used to run
    // Library Health's whole scan per catalog: the Clean Up leftover
    // check, the cover-art, analysis and sample-rate audits, and a stat of
    // every track's file. On a full USB stick the sample-rate audit alone
    // (one audio file opened per Engine row without a rate) held the
    // overlay up for minutes, for results this page never shows.
    //
    // The same library through Library Health's depth first, on its own
    // copy so neither scan is answered from the other's catalog cache: it
    // shows each check that the page skips does find something here, so
    // the page's zeroes below are the checks not running, not a library
    // with nothing to find.
    function test_theStrayCueScanReadsCuesAndNothingElse() {
        const fullStick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(fullStick.length > 0, "the fixture must copy");
        const full = createTemporaryObject(fullControllerComponent, testCase);
        compare(full.scanDepth, LibraryConsistencyController.Full, "a controller scans fully unless told otherwise");
        const fullStart = Date.now();
        full.scan(fullStick + "/PIONEER", fullStick + "/Engine Library", "");
        verify(full.busy, "scanning");
        tryVerify(() => !full.busy, 300000, "the full scan finishes");
        const fullMs = Date.now() - fullStart;
        compare(full.errorMessage, "", "the full scan read everything");
        verify(full.issues.count > 0, "precondition: the fixture has no audio, so every row is a missing file");
        verify(full.artworkTracksWithArt > 0, "precondition: the cover-art audit finds art here");
        verify(full.analysisLibraryPresent, "precondition: the analysis audit finds the Engine library");
        verify(full.cleanupLeftoversChecked, "precondition: the Clean Up leftover check runs on this stick");
        verify(full.junkCues.count > 0, "precondition: the fixture has stray cues");

        const page = openOnAFreshCopy();
        const controller = page.consistencyController;
        compare(controller.scanDepth, LibraryConsistencyController.CuesOnly, "the page asks for cues only");
        const cuesStart = Date.now();
        const overlay = findChild(page, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 300000, "the page's scan finishes");
        const cuesMs = Date.now() - cuesStart;
        compare(controller.errorMessage, "", "the page's scan read everything");

        compare(controller.junkCues.count, full.junkCues.count, "the same stray cues either way");
        compare(controller.playlistNames, full.playlistNames, "and the same playlists for the picker");
        compare(controller.issues.count, 0, "no file presence check");
        compare(controller.artworkTracksWithArt, 0, "no cover-art audit");
        compare(controller.artworkError, "", "no cover-art audit");
        compare(controller.analysisLibraryPresent, false, "no analysis audit");
        compare(controller.sampleRateMissingCount, 0, "no sample-rate audit");
        compare(controller.sampleRateError, "", "no sample-rate audit");
        compare(controller.cleanupLeftoversChecked, false, "no Clean Up leftover check");
        console.log("stray-cue scan on the committed fixture (local disk): full " + fullMs + " ms, cues only "
                    + cuesMs + " ms");
    }

    // The depth is part of what was asked. A scan for the same paths is
    // answered by the one already running (tst_StagedScanRequests), but a
    // cues-only scan does not answer a request for the whole check: it
    // would leave Library Health's checks never run and read as clean.
    function test_aFullScanIsNotAnsweredByACuesOnlyOne() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const controller = createTemporaryObject(fullControllerComponent, testCase);
        controller.scanDepth = LibraryConsistencyController.CuesOnly;
        controller.scan(stick + "/PIONEER", stick + "/Engine Library", "");
        verify(controller.busy, "precondition: the cues-only scan is still running");
        controller.scanDepth = LibraryConsistencyController.Full;
        controller.scan(stick + "/PIONEER", stick + "/Engine Library", "");
        tryVerify(() => !controller.busy, 300000, "the scan finishes");
        verify(controller.issues.count > 0, "the full check ran after all");
        verify(controller.cleanupLeftoversChecked, "every part of it");
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

    // A playlist the failed scan did not list is not a playlist the
    // library lacks: the catalog that holds it may be the one that could
    // not be read. The pick stays, and the page does not turn round and
    // rescan the whole library on a stick that just failed.
    function test_aFailedScanKeepsThePickedPlaylist() {
        const page = createTemporaryObject(pageComponent, testCase);
        const overlay = findChild(page, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 30000, "the first scan ends");
        let scansStarted = 0;
        overlay.visibleChanged.connect(() => { if (overlay.visible) scansStarted++; });

        page.selectedPlaylistName = "Only On The Unreadable Catalog";
        page.rescan();
        tryVerify(() => !overlay.visible, 30000, "the playlist's scan ends");
        verify(page.scanFailed, "precondition: the scan failed, it was not stopped");
        compare(page.scanStopped, false);
        wait(200);  // past the Qt.callLater that checks for a missing playlist
        compare(page.selectedPlaylistName, "Only On The Unreadable Catalog", "the user's pick survives a failed scan");
        compare(scansStarted, 1, "no whole-library rescan after the failure");
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

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from a hub on
    // top of Home). The breadcrumb reads the stack's depth to decide
    // whether its middle segment is a link, so a page on its own cannot
    // show that.
    Component {
        id: crumbStackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: crumbFillerComponent
        Item {}
    }
    function pushOnStack(levelsBelow, props) {
        const stack = createTemporaryObject(crumbStackComponent, testCase);
        for (let i = 0; i < levelsBelow; ++i) {
            stack.push(crumbFillerComponent, {}, StackView.Immediate);
        }
        return stack.push(pageComponent, props, StackView.Immediate);
    }

    function saveCrumbShot(page, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/crumb-" + name + ".png");
        }
    }

    // From Housekeeping's card, and from Sync Cue Points' stray-cue link.
    function test_breadcrumb_data() {
        return [
            {tag: "housekeeping", props: {}, middle: "Housekeeping"},
            {tag: "sync", props: {hubLabel: "Sync Cue Points"}, middle: "Sync Cue Points"},
        ];
    }

    function test_breadcrumb(data) {
        const page = pushOnStack(2, data.props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        verify(crumb.middleIsLink);
        compare(crumb.title, "Clean Up Stray Cues");
        saveCrumbShot(page, "junk-cues-" + data.tag);
    }
}
