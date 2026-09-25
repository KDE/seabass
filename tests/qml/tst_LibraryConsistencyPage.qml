// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Library Health's "Tracks and their files" page, driven by a stand-in
// controller: what is staged is said beside the buttons that staged it,
// and nothing from any other check is on it.
//
// It used to be the page for every check at once. One number for the
// page put "29 staged, not saved yet" beside "Stage All Safe Repairs"
// after staging 29 stray cue removals; and every card on the hub opened
// the same long page, so "Review sample rates" landed on a list of
// missing files. The other checks' tests moved with them, to
// tst_CuesAtZeroPage, tst_ImportPromptPage, tst_SampleRatesPage and
// tst_OneLibraryLeftoversPage.
TestCase {
    id: testCase
    name: "LibraryConsistencyPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }

    // Only what the two notes and the rows around them read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property int repairableCount: 0
            property int unstagedRepairableCount: 0
            property int sampleRateMissingCount: 0
            property int sampleRateFixableCount: 0
            property bool sampleRateFillStaged: false
            property bool playerWillOfferImport: false
            property bool importMarkStaged: false
            function markRekordboxImported() { importMarkStaged = true; }
            function unstageRekordboxImportMark() { importMarkStaged = false; }
            function fillSampleRates() { sampleRateFillStaged = true; }
            function unstageSampleRateFill() { sampleRateFillStaged = false; }
            property bool cleanupLeftoversChecked: false
            property int cleanupLeftoverCount: 0
            property int cleanupLeftoverFixableCount: 0
            property bool cleanupLeftoverFixStaged: false
            property string cleanupLeftoverError: ""
            property var cleanupLeftoversHeldBack: []
            function finishCleanupLeftovers() { cleanupLeftoverFixStaged = true; }
            function unstageCleanupLeftoverFix() { cleanupLeftoverFixStaged = false; }
            property int unstagedJunkCueCount: 0
            property int stagedCount: stagedIssueCount + stagedJunkCueCount
            property int stagedIssueCount: 0
            property int stagedJunkCueCount: 0
            property int artworkRepairableCount: 0
            property bool artworkRepairStaged: false
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            property var issues: ListModel {}
            property var junkCues: ListModel {}
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            property int scanCalls: 0
            property int undoCalls: 0
            function scan(a, b, c) { scanCalls++; }
            function cancelScan() {}
            function undoLastOperation() { undoCalls++; }
        }
    }

    Component {
        id: pageComponent
        LibraryConsistencyPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
        }
    }

    function collectReasons(item, out) {
        if (item.objectName === "junkCueReason" && item.text.length > 0) {
            out.push(item.text);
        }
        for (const child of item.children) {
            collectReasons(child, out);
        }
    }

    // The hub hands over its own controller, and the hub can go first: a
    // stick pulled and its changes discarded took both pages down, the
    // hub's controller before this page, and every binding here that
    // read it then logged "Cannot read property ... of null" -- 468 of
    // them on Windows, one per row delegate per binding.
    function test_aSharedControllerThatGoesAwayIsNotReadAfterwards() {
        failOnWarning(/Cannot read property/);
        failOnWarning(/Unable to assign/);
        var controller = controllerComponent.createObject(testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page.consistencyController === controller);
        controller.destroy();
        wait(50);
        verify(page.consistencyController !== null, "the page falls back to a controller of its own");
        compare(page.consistencyController.busy, false);
    }

    // A fake edit session and registry, the shape tst_EditSessionHost
    // uses: only what the leave guard reads.
    Component {
        id: sessionComponent
        QtObject {
            property bool dirty: false
            property bool writing: false
            property int pendingCount: 0
            property var pendingDescriptions: []
            property string editorOwner: "library-health"
            property string libraryId: "EB9F-F032"
            property string stickLabel: "TESTSTICK"
            property string state: "idle"
            property string writeLabel: ""
            property int writeCurrent: 0
            property int writeTotal: 0
            property bool cancelRequested: false
            property bool stickPresent: true
            property string stickIdentityStrength: "hardware"
            property bool interruptedSave: false
            signal saveFinished(var summary)
            signal lockRefused(var holder)
            function save() {}
            function discard() { dirty = false; pendingCount = 0; }
            function cancelWrite() {}
        }
    }
    Component {
        id: registryComponent
        QtObject {
            property var session: null
            property bool quitAfterSave: false
            function openSession(id, label, rb, engine) { return session; }
            function closeSession(id) {}
            function removeLock(id) {}
        }
    }
    Component {
        id: stackComponent
        StackView { width: 980; height: 760 }
    }
    Component {
        id: bottomPage
        Item {}
    }

    // A scan cancelled from its overlay leaves the page the way Back does,
    // through the leave guard. It used to pop straight past it: stage a
    // repair, close Resolve... (which rescans the hub's controller), cancel
    // that scan -- and the page was gone with the repair still staged, on
    // the hub, which has no guard to offer Save or Discard.
    function test_aCancelledScanAsksAboutStagedChangesBeforeLeaving() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var session = createTemporaryObject(sessionComponent, testCase, {dirty: true, pendingCount: 1});
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var stack = createTemporaryObject(stackComponent, testCase);
        stack.push(bottomPage);
        var page = stack.push(pageComponent, {sharedController: controller, editSessionRegistry: registry},
                              StackView.Immediate);
        verify(page !== null, "the page must be pushed");
        compare(stack.depth, 2);

        controller.scanCancelled();
        var unsaved = findChild(page, "unsavedDialog");
        verify(unsaved !== null);
        tryCompare(unsaved, "opened", true);
        compare(stack.depth, 2, "the page stays until the staged changes are saved or discarded");
        unsaved.close();

        // With nothing staged it leaves at once, as before.
        session.dirty = false;
        session.pendingCount = 0;
        controller.scanCancelled();
        tryCompare(stack, "depth", 1);
    }

    function test_onlyThisChecksStagedWorkIsCountedHere() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var missing = findChild(page, "stagedIssuesNote");
        verify(missing !== null, "the check must have a staged note");
        compare(missing.visible, false, "nothing staged, nothing said");

        // Stray cues staged elsewhere: this page stays quiet about them.
        controller.stagedJunkCueCount = 29;
        compare(missing.visible, false, "a staged cue removal is not staged repair work");

        controller.stagedJunkCueCount = 0;
        controller.stagedIssueCount = 4;
        compare(missing.visible, true);
        compare(missing.text, "4 staged, not saved yet");

        // The page outlives the stand-in otherwise, and spends teardown
        // reading properties off a destroyed object.
        page.destroy();
        wait(0);
    }

    // The page is this check and nothing else: none of the other checks'
    // summaries, buttons or rows are on it, and the breadcrumb names it
    // under Library Health.
    function test_thePageHoldsThisCheckOnly() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.sampleRateMissingCount = 3;
        controller.sampleRateFixableCount = 3;
        controller.playerWillOfferImport = true;
        controller.cleanupLeftoversChecked = true;
        controller.cleanupLeftoverCount = 2;
        controller.junkCues.append({track: {title: "A track", artist: "An artist", filePath: "/nowhere/a.mp3",
                                            durationMs: 0, cues: [], side: "engine", sourceId: "1",
                                            artworkPath: ""},
                                    staged: false, format: "engine", positionMs: 0,
                                    reason: "at the very start of the track"});
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        verify(findChild(page, "missingFilesSummary") !== null, "its own check is here");
        for (const other of ["sampleRateSummary", "fillSampleRatesButton", "importPromptSummary",
                             "markImportedButton", "cleanupLeftoverSummary", "finishCleanupButton",
                             "stagedJunkCuesNote", "cuesAtZeroSummary"]) {
            compare(findChild(page, other), null, other + " belongs to another check's page");
        }
        var reasons = [];
        collectReasons(page, reasons);
        compare(reasons.length, 0, "no stray-cue rows on the missing-files page");
        compare(findButton(page, "Remove All"), null);

        var crumb = findCrumb(page.header);
        verify(crumb !== null, "the header has a breadcrumb");
        compare(crumb.title, "Tracks and Their Files");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");

        // Handed the hub's controller, it shows that scan rather than
        // making the user wait through the same one again.
        compare(controller.scanCalls, 0, "a shared controller is not scanned again");

        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/LibraryConsistencyPage-missing-files.png");
        }
        page.destroy();
        wait(0);
    }

    // Undo Last Save is the session's undo, and it sits on every check's
    // page: it used to be beside the missing-file repairs, the only place
    // it could be found when every check shared one page.
    function test_undoLastSaveIsOfferedOnceThereIsOne() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        var undo = findChild(page, "undoLastSaveButton");
        verify(undo !== null);
        compare(undo.visible, false, "nothing saved, nothing to undo");
        controller.canUndo = true;
        compare(undo.visible, true);
        undo.clicked();
        compare(controller.undoCalls, 1);
        page.destroy();
        wait(0);
    }

    // The header's text and the body share one left line.
    function test_theBreadcrumbLinesUpWithTheBody() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        var home = findChild(page.header, "homeCrumb");
        var body = findChild(page, "healthCheckBody");
        verify(home !== null && body !== null);
        compare(home.contentItem.mapToItem(page, 0, 0).x, body.mapToItem(page, 0, 0).x,
                "breadcrumb and body must share a left edge");
        compare(body.mapToItem(page, 0, 0).x, Theme.pageMargin);
        page.destroy();
        wait(0);
    }

    function findCrumb(item) {
        if (item === null || item === undefined) {
            return null;
        }
        if (item.middleClickable !== undefined && item.title !== undefined) {
            return item;
        }
        var kids = item.children ? item.children : [];
        for (var i = 0; i < kids.length; ++i) {
            var found = findCrumb(kids[i]);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    // A "do all of it" button with everything already staged has nothing
    // behind it. It used to stay live, because it asked what the check
    // found rather than what it had left to do. (The stray cues' own
    // Remove All is covered in tst_CuesAtZeroPage.)
    function test_theAllButtonGoesQuietOnceEverythingIsStaged() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.repairableCount = 3;
        controller.unstagedRepairableCount = 3;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        var repairAll = findButton(page, "Stage All Safe Repairs");
        verify(repairAll !== null, "the do-all button must be there");
        compare(repairAll.enabled, true, "with repairs waiting it is live");

        controller.unstagedRepairableCount = 0;
        compare(repairAll.enabled, false, "everything staged: nothing left to press it for");
        page.destroy();
        wait(0);
    }

    // Buttons are found by their label: the page has no objectName on
    // them, and adding one only for a test would be a worse thing to
    // pin than the words the user reads.
    function findButton(item, text) {
        if (item === null || item === undefined) {
            return null;
        }
        if (item.text === text && item.enabled !== undefined && item.clicked !== undefined) {
            return item;
        }
        var kids = item.children ? item.children : [];
        for (var i = 0; i < kids.length; ++i) {
            var found = findButton(kids[i], text);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    // Which playlists end up short a track. The data was already in
    // hand -- the same track list the playlist picker is built from --
    // and was being dropped at the GUI boundary, so a missing file told
    // you a track was gone and never which set now has a gap in it.
    //
    // The three answers are different things, and the third is the one
    // worth being careful about: a reader that does not report
    // memberships gives an empty list, which means "not known" and must
    // never be shown as "in no playlist".
    function test_thePlaylistLineSaysWhichSetsLoseATrack() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        const broken = [{playlists: [{name: "Techno/Peak Time", position: 7},
                                     {name: "Warmup", position: 2}]}];

        // Missing: every playlist it was in loses it.
        const missing = page.playlistSentence("missing", broken, null);
        verify(missing.indexOf("2 playlists") >= 0, "both are named as lost: " + missing);
        verify(missing.indexOf("Techno/Peak Time") >= 0);
        verify(missing.indexOf("Warmup") >= 0);

        // Repairable: the kept copy absorbs the row, so only a playlist
        // the kept copy is NOT in actually loses anything.
        const survivorInOne = {playlists: [{name: "Warmup", position: 5}]};
        const partly = page.playlistSentence("repairable", broken, survivorInOne);
        verify(partly.indexOf("1 playlist") >= 0, "only the one the survivor is missing from: " + partly);
        verify(partly.indexOf("Techno/Peak Time") >= 0);
        verify(partly.indexOf("Warmup") < 0, "the survivor covers Warmup, so it is not short a track");

        // And when the kept copy covers all of them, that is worth
        // saying too rather than leaving the row silent.
        const survivorInBoth = {playlists: [{name: "Warmup", position: 5},
                                            {name: "Techno/Peak Time", position: 1}]};
        const none = page.playlistSentence("repairable", broken, survivorInBoth);
        verify(none.indexOf("no set loses a track") >= 0, none);

        // Nothing known: the line is empty, so the row says nothing at
        // all rather than "in no playlist".
        compare(page.playlistsKnown([{playlists: []}], null), false);
        compare(page.playlistSentence("missing", [{playlists: []}], null), "",
                "a missing row with no memberships known says nothing");
        compare(page.playlistsKnown(broken, null), true);

        // Two broken copies of one song in one issue must not name the
        // same playlist twice.
        const twoCopies = [{playlists: [{name: "Warmup", position: 2}]},
                           {playlists: [{name: "Warmup", position: 9}]}];
        compare(page.playlistsLeftShort("missing", twoCopies, null).length, 1);

        page.destroy();
        wait(0);
    }
}
