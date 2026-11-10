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
    PlaybackController { id: realPlayback }

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
            playbackController: realPlayback
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
            property bool canUndo: false
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
    // Library Health's whole scan per catalog: the playlist
    // comparison, the cover-art, analysis and sample-rate audits, and a stat of
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
        verify(full.playlistsChecked, "precondition: the playlist comparison runs on this stick");
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
        compare(controller.playlistsChecked, false, "no playlist comparison");
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
        verify(controller.playlistsChecked, "every part of it");
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

    // From Maintain's card on the home, and from Sync Cue Points' stray-cue link.
    function test_breadcrumb_data() {
        return [
            {tag: "maintain", props: {}, below: 1, middle: "", link: false},
            {tag: "sync", props: {hubLabel: "Sync Cue Points"}, below: 2, middle: "Sync Cue Points", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const page = pushOnStack(data.below, data.props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Clean Up Stray Cues");
        saveCrumbShot(page, "junk-cues-" + data.tag);
    }

    // One row is one cue, one change is one track: the change strips every
    // stray cue the track carries and staging marks every row of it. After
    // the save every row of the track has to go too. Only the first went,
    // so a track with two stray cues kept one row listed, unstaged, with
    // its cue already off the stick: the list Sebastian saw still full
    // after Save on 2026-10-17 (TESTSHARK, 409 cues, 132 tracks with two).
    Component {
        id: cuesOnlyControllerComponent
        LibraryConsistencyController { scanDepth: LibraryConsistencyController.CuesOnly }
    }
    Component {
        id: addCueComponent
        AddCueController {}
    }
    readonly property int junkFormatRole: Qt.UserRole + 1
    readonly property int junkTrackRole: Qt.UserRole + 4
    function junkRow(model, i, role) {
        return model.data(model.index(i, 0), role);
    }
    function saveSession(session) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 300000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }
    function test_everyRowOfATrackGoesWhenItsCuesAreWritten() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const rb = stick + "/PIONEER";
        const eng = stick + "/Engine Library";
        // A rekordbox track the fixture lists with one stray cue gets a
        // second one through Add Cue, saved and settled before the page.
        const before = createTemporaryObject(cuesOnlyControllerComponent, testCase);
        before.scan(rb, eng, "");
        tryVerify(() => !before.busy, 300000, "the first scan finishes");
        let planted = null;
        for (let i = 0; i < before.junkCues.count && planted === null; ++i) {
            if (junkRow(before.junkCues, i, testCase.junkFormatRole) === "rekordbox") {
                planted = junkRow(before.junkCues, i, testCase.junkTrackRole);
            }
        }
        verify(planted !== null, "precondition: a rekordbox track with a stray cue");
        const adder = createTemporaryObject(addCueComponent, testCase);
        adder.addCue("rekordbox", rb, planted.sourceId, 450, "memory", 0, "", "", false, 0, planted.title);
        compare(adder.errorMessage, "");
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(rb));
        compare(session.pendingCount, 1, "the second cue is staged");
        const plantedSave = saveSession(session);
        compare(plantedSave.error, "", JSON.stringify(plantedSave));

        const page = createTemporaryObject(stickPageComponent, testCase, {rekordboxPath: rb, enginePath: eng});
        const controller = page.consistencyController;
        const overlay = findChild(page, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 300000, "the page's scan finishes");
        compare(controller.errorMessage, "");
        const rows = controller.junkCues.count;
        const tracks = controller.junkCues.trackCount();
        verify(rows > tracks, "precondition: a track with two stray cues (" + rows + " rows on " + tracks + " tracks)");
        let plantedRows = 0;
        for (let i = 0; i < rows; ++i) {
            if (junkRow(controller.junkCues, i, testCase.junkFormatRole) === "rekordbox"
                && junkRow(controller.junkCues, i, testCase.junkTrackRole).sourceId === planted.sourceId) {
                ++plantedRows;
            }
        }
        compare(plantedRows, 2, "the planted track has two rows");

        controller.removeAllJunkCues();
        verify(controller.statusMessage.indexOf("Staged removing " + rows + " stray cue") === 0, controller.statusMessage);
        const summary = saveSession(session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(controller.junkCues.count, 0, "every row of every track goes with the save");
        // And the stick agrees.
        controller.scan(rb, eng, "");
        tryVerify(() => !controller.busy, 300000, "the second scan finishes");
        compare(controller.errorMessage, "");
        compare(controller.junkCues.count, 0, "nothing left on the stick either");
    }

    // The page on a copy of the fixture, its scan done.
    function openScanned() {
        const page = openOnAFreshCopy();
        const overlay = findChild(page, "junkCueBusyOverlay");
        tryVerify(() => !overlay.visible, 300000, "the page's scan finishes");
        compare(page.consistencyController.errorMessage, "");
        verify(page.consistencyController.junkCues.count > 0, "precondition: the fixture has stray cues");
        return page;
    }
    readonly property int junkStagedRole: Qt.UserRole + 5
    function rowItem(page, i) {
        const list = findChild(page, "junkCueListView");
        list.positionViewAtIndex(i, ListView.Beginning);
        tryVerify(() => list.itemAtIndex(i) !== null, 5000, "row " + i + " is built");
        return list.itemAtIndex(i);
    }
    function unstageEveryRow(controller) {
        for (let i = 0; i < controller.junkCues.count; ++i) {
            if (junkRow(controller.junkCues, i, testCase.junkStagedRole) === true) {
                controller.unstageJunkCue(i);
            }
        }
        compare(controller.stagedJunkCueCount, 0);
    }

    // Every row says which cue it is, to the millisecond: a track with a
    // memory cue at 0:00 and a hot cue at 0:00.450 has two rows alike in
    // every other column, and the tick on either stages both.
    function test_everyRowNamesItsCue() {
        const page = openScanned();
        const controller = page.consistencyController;
        const rows = controller.junkCues.count;
        const byTrack = {};
        for (let i = 0; i < rows; ++i) {
            const row = rowItem(page, i);
            const which = findChild(row, "junkCueWhich").text;
            verify(/^(Memory cue|Hot cue [1-8]) at \d+:\d\d\.\d{3}$/.test(which), "row " + i + ": " + which);
            const key = row.format + ":" + row.track.sourceId;
            byTrack[key] = (byTrack[key] || []).concat([which]);
        }
        const twins = Object.keys(byTrack).filter((key) => byTrack[key].length > 1);
        verify(twins.length > 0, "precondition: the fixture has a track with two stray cues");
        for (const key of twins) {
            const labels = byTrack[key];
            compare(new Set(labels).size, labels.length, key + " rows tell apart: " + labels.join(" / "));
        }
    }

    // A track's rows sit together: the first carries the tick, cover and
    // key, the ones after it are continuation rows with none of their
    // own, and the first row's tick stages them all.
    function test_aTracksRowsSitTogetherUnderOneTick() {
        const page = openScanned();
        const controller = page.consistencyController;
        const rows = controller.junkCues.count;
        let firstTwin = -1;
        const seen = {};
        for (let i = 0; i < rows; ++i) {
            const t = junkRow(controller.junkCues, i, testCase.junkTrackRole);
            const key = junkRow(controller.junkCues, i, testCase.junkFormatRole) + ":" + t.sourceId;
            if (seen[key] !== undefined) {
                compare(seen[key], i - 1, key + ": its rows sit together");
                if (firstTwin < 0) {
                    firstTwin = i - 1;
                }
            }
            seen[key] = i;
        }
        verify(firstTwin >= 0, "precondition: a track with two stray cues");
        // The first row brought into view and ticked before the second is
        // looked at: rowItem scrolls the row it is asked for to the top,
        // which would put the first one out of reach of the click.
        const first = rowItem(page, firstTwin);
        compare(first.firstOfTrack, true);
        mouseClick(findChild(first, "rowStageCheck"));
        tryCompare(first, "staged", true, 5000, "the first row's tick stages it");
        const second = rowItem(page, firstTwin + 1);
        compare(second.firstOfTrack, false);
        compare(second.staged, true, "the first row's tick stages the track, so its other row too");
        compare(findChild(second, "rowStageCheck").checked, true);
        compare(findChild(second, "rowStageCheck").enabled, false, "no tick of its own");
        compare(findChild(second, "rowCell_artwork").visible, false, "no cover of its own");
        unstageEveryRow(controller);
    }

    // A row's tick and Stage Removing All stage at once: nothing is asked
    // first, the row reads Staged as the tick lands, and unticking takes it
    // back. The rows used to carry a Remove button, which read as all or
    // nothing next to Stage Removing All; a tick says a few can be picked.
    function test_aTickStagesWithoutAsking() {
        const page = openScanned();
        const controller = page.consistencyController;
        const row = rowItem(page, 0);
        const tick = findChild(row, "rowStageCheck");
        compare(tick.checked, false);
        mouseClick(tick);
        compare(row.staged, true, "staged as the tick lands");
        compare(tick.checked, true);
        verify(controller.stagedJunkCueCount >= 1);
        mouseClick(tick);
        compare(row.staged, false, "and taken back out");
        compare(tick.checked, false);
        compare(controller.stagedJunkCueCount, 0);

        mouseClick(findChild(page, "stageAllButton"));
        compare(controller.unstagedJunkCueCount, 0, "every row staged, nothing asked");
        tryCompare(tick, "checked", true, 5000, "the tick follows Stage Removing All");
        verify(controller.statusMessage.indexOf("Staged removing") === 0, controller.statusMessage);
        unstageEveryRow(controller);
        tryCompare(tick, "checked", false, 5000, "and follows the unstaging too");
    }

    // A Clean Up that wrote everything it staged is said over the list,
    // in the summary's own words, and opens no summary dialog.
    function test_aFinishedCleanUpIsSaidOverTheList() {
        const page = openScanned();
        const controller = page.consistencyController;
        mouseClick(findChild(page, "stageAllButton"));
        compare(controller.unstagedJunkCueCount, 0);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(page.rekordboxPath));
        const summary = saveSession(session);
        compare(summary.error, "", JSON.stringify(summary));
        verify(summary.written > 0, JSON.stringify(summary));
        const toast = findChild(page, "savedToast");
        tryCompare(toast, "showing", true);
        compare(findChild(toast, "confirmationToastLabel").text, summary.written + " cues removed");
        const dialog = findChild(page, "summaryDialog");
        verify(dialog === null || !dialog.visible, "no summary dialog over a save that landed whole");
        compare(controller.junkCues.count, 0, "the written rows are gone from under it");
    }

    // #78: the page that saved offers the save's undo. One row cleaned
    // up, the button shows; pressed, the cue is back on the stick and
    // the rescan lists it again.
    function test_aCleanUpIsUndoneFromThisPage() {
        const page = openScanned();
        const controller = page.consistencyController;
        const before = controller.junkCues.count;
        const undo = findChild(page.header, "undoLastSaveButton");
        verify(undo !== null, "the page has an Undo Last Save button");
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(page.rekordboxPath));
        compare(undo.visible, session.canUndo, "the button is the session's undo");

        mouseClick(findChild(rowItem(page, 0), "rowStageCheck"));
        verify(controller.stagedJunkCueCount >= 1, "precondition: one row staged");
        const saved = saveSession(session);
        compare(saved.error, "", JSON.stringify(saved));
        verify(saved.written > 0, JSON.stringify(saved));
        tryVerify(() => !controller.busy, 300000, "the save's rescan finishes");
        verify(controller.junkCues.count < before, "precondition: the saved row is gone");
        tryCompare(undo, "visible", true, 5000, "the save is offered for undo here");
        verify(undo.enabled);
        // The save's progress is modal, and a style's exit transition keeps
        // it over the page for a moment: the press goes in once it is gone.
        tryVerify(() => !findChild(page, "writeProgressDialog").visible, 10000, "the save's progress is gone");

        let undone = null;
        const done = (result) => { undone = result; };
        session.saveFinished.connect(done);
        mouseClick(undo);
        tryVerify(() => undone !== null, 300000, "the press ran the undo");
        session.saveFinished.disconnect(done);
        compare(undone.error, "", JSON.stringify(undone));
        tryVerify(() => !controller.busy, 300000, "the undo's rescan finishes");
        compare(controller.junkCues.count, before, "the cue is back");
        tryCompare(undo, "visible", false, 5000, "and there is nothing left to undo");
    }

    // A row is drawn as Browse draws one: the cover, which plays the track
    // while the row is hovered and stays while it is the track playing, and
    // the key beside the title.
    function test_aRowPlaysFromItsCover() {
        const page = openScanned();
        const controller = page.consistencyController;
        let index = -1;
        let track = null;
        for (let i = 0; i < controller.junkCues.count && index < 0; ++i) {
            const t = junkRow(controller.junkCues, i, testCase.junkTrackRole);
            if ((t.filePath || "").length > 0 && (t.streamingSource || "").length === 0) {
                index = i;
                track = t;
            }
        }
        verify(index >= 0, "precondition: a stray cue on a track with a file");
        const item = rowItem(page, index);
        verify(findChild(item, "rowArtwork") !== null, "the cover");
        verify(findChild(item, "rowCell_key") !== null, "the key badge");
        const play = findChild(item, "rowPlayButton");
        const cover = findChild(item, "rowCell_artwork");
        mouseMove(page, 1, 1);
        compare(play.visible, false, "hidden until the cover is hovered");
        mouseMove(item, item.width / 2, item.height / 2);
        wait(50);
        compare(play.visible, false, "the pointer on the row, off the cover, shows no key (Browse shows none)");
        mouseMove(cover, cover.width / 2, cover.height / 2);
        tryCompare(play, "visible", true);
        mouseClick(play);
        tryCompare(realPlayback, "currentSourceId", track.sourceId);
        compare(realPlayback.currentFormat, item.format);
        mouseMove(page, 1, 1);
        verify(play.visible, "shown while it is the track playing");
        tryCompare(item, "isPlaying", true);
        realPlayback.pause();
    }
}
