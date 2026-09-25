// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Cues at 0:00" page, driven by a stand-in controller:
// every row says why it is here and points at its own cue, and what is
// staged is said beside the buttons that staged it.
TestCase {
    id: testCase
    name: "CuesAtZeroPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }

    // Only what the page and its frame (HealthCheckPage) read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property int stagedIssueCount: 0
            property int stagedJunkCueCount: 0
            property int unstagedJunkCueCount: 0
            property var junkCues: ListModel {}
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            property int scanCalls: 0
            function scan(a, b, c) { scanCalls++; }
            function cancelScan() {}
            function undoLastOperation() {}
        }
    }

    Component {
        id: pageComponent
        CuesAtZeroPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
        }
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

    // The hub's controller can go before this page does (a stick pulled
    // and its changes discarded); nothing here may read it afterwards.
    // See tst_LibraryConsistencyPage for the case this came from.
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

    // Its own check and no other: the breadcrumb names it under Library
    // Health, the other checks' summaries and buttons are not here, and
    // the hub's scan is shown rather than run again. The header text and
    // the body share the page's one left line.
    function test_thePageIsThisCheckAlone() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        waitForRendering(page);

        var crumb = findCrumb(page.header);
        verify(crumb !== null, "the header has a breadcrumb");
        compare(crumb.title, "Cues at 0:00");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");
        // And the stick before it, on screen, as every hub page's children do.
        compare(Breadcrumb.read(page.header).stick, "TESTSTICK");

        verify(findChild(page, "cuesAtZeroSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "importPromptSummary", "markImportedButton",
                             "sampleRateSummary", "fillSampleRatesButton", "cleanupLeftoverSummary", "finishCleanupButton"]) {
            compare(findChild(page, other), null, other + " belongs to another check's page");
        }
        compare(controller.scanCalls, 0, "a shared controller is not scanned again");

        var home = findChild(page.header, "homeCrumb");
        var body = findChild(page, "healthCheckBody");
        verify(home !== null && body !== null);
        compare(home.contentItem.mapToItem(page, 0, 0).x, body.mapToItem(page, 0, 0).x,
                "breadcrumb and body must share a left edge");
        page.destroy();
        wait(0);
    }

    function findLabelContaining(item, needle) {
        if (item.text !== undefined && typeof item.text === "string" && item.text.indexOf(needle) >= 0) {
            return item;
        }
        for (const child of item.children) {
            const found = findLabelContaining(child, needle);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    function collectHighlights(item, out) {
        if (item.highlightCuePositionMs !== undefined) {
            out.push(item.highlightCuePositionMs);
        }
        for (const child of item.children) {
            collectHighlights(child, out);
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

    // What is staged is said beside the buttons that staged it: the note
    // counts stray cues, and missing-file repairs staged on their own page
    // are not counted here.
    function test_theStagedCuesAreCountedBesideTheirButtons() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var cues = findChild(page, "stagedJunkCuesNote");
        verify(cues !== null, "the check must have a staged note");
        compare(cues.visible, false, "nothing staged, nothing said");

        controller.stagedJunkCueCount = 29;
        compare(cues.visible, true);
        compare(cues.text, "29 staged, not saved yet");

        controller.stagedJunkCueCount = 0;
        controller.stagedIssueCount = 4;
        compare(cues.visible, false, "a staged repair is not a staged cue removal");
        page.destroy();
        wait(0);
    }

    // A "do all of it" button with everything already staged has nothing
    // behind it.
    function test_removeAllGoesQuietOnceEverythingIsStaged() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var row = {track: {title: "A track", artist: "An artist", filePath: "/nowhere/a.mp3",
                           durationMs: 0, cues: [], side: "engine", sourceId: "1", artworkPath: ""},
                   staged: false, format: "engine", positionMs: 0,
                   reason: "at the very start of the track"};
        controller.junkCues.append(row);
        controller.junkCues.append(row);
        controller.unstagedJunkCueCount = 2;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var removeAll = findButton(page, "Remove All");
        verify(removeAll !== null, "the do-all button must be there");
        compare(removeAll.enabled, true, "with cues waiting it is live");
        controller.unstagedJunkCueCount = 0;
        compare(removeAll.enabled, false, "everything staged: nothing left to press it for");
        page.destroy();
        wait(0);
    }

    // Buttons are found by their label, the words the user reads.
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

    // Two checks feed the accidental-cue list now: a cue at the very
    // start of a track, and one of a crowd of hot cues inside its first
    // two seconds (#41). Every row is an offer to delete somebody's
    // cue, so each says why it is there, and the waveform highlights
    // the cue that Remove would actually take.
    //
    // Highlighting 0:00 while removing a cue at 1.188 s is the exact
    // opposite of the "unambiguous which one Remove kills" the row was
    // built for, and it is what this section did the moment a second
    // check started feeding it.
    function test_eachAccidentalCueRowSaysWhyAndPointsAtItself() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const track = {title: "Too Little Too Late", artist: "Joris Voorn", filePath: "/nowhere/a.mp3",
                       durationMs: 300000, cues: [], side: "rekordbox", sourceId: "1", artworkPath: ""};
        controller.junkCues.append({track: track, staged: false, format: "rekordbox", positionMs: 0,
                                    reason: "at the very start of the track"});
        controller.junkCues.append({track: track, staged: false, format: "rekordbox", positionMs: 1188,
                                    reason: "one of 3 hot cues in the first two seconds, which is not a "
                                            + "pattern anyone plays"});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        // The rows are a ListView's, made when it lays out, not when the
        // page is built.
        const list = findChild(page, "junkCueList");
        verify(list !== null);
        tryCompare(list, "count", 2);
        waitForRendering(page);

        // The section headline can no longer claim every row is at 0:00.
        const headline = findLabelContaining(page, "look accidental");
        verify(headline !== null, "the section says what it found without naming only one of the two checks");
        verify(headline.text.indexOf("0:00") < 0, "and does not describe a cue at 1.188 s as being at 0:00");

        // Both rows carry their own reason, and the two differ.
        const reasons = [];
        collectReasons(page, reasons);
        compare(reasons.length, 2, "one reason per row");
        verify(reasons[0] !== reasons[1], "the two checks do not describe their rows the same way");
        verify(reasons[1].indexOf("first two seconds") >= 0, reasons[1]);

        // And each row's waveform points at its own cue. The second row
        // removes a cue at 1.188 s; a highlight left at 0 would mark a
        // different cue than the button takes.
        // Counted by value rather than by position: the card passes the
        // property down to the waveform inside it, so each row
        // contributes it more than once and the exact depth is not what
        // this is about.
        const highlights = [];
        collectHighlights(page, highlights);
        verify(highlights.indexOf(1188) >= 0,
               "the clustered row points at its own cue, not at 0:00: " + JSON.stringify(highlights));
        verify(highlights.indexOf(0) >= 0, "and the row that really is at the start still points at 0");
        for (const value of highlights) {
            verify(value === 0 || value === 1188, "no row highlights a cue no row is about: " + value);
        }

        page.destroy();
        wait(0);
    }

    // Rendered with a finding on it, and saved to look at when
    // SEABASS_SCREENSHOT_DIR is set.
    function test_rendersWithAFinding() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const track = {title: "Too Little Too Late", artist: "Joris Voorn", filePath: "/nowhere/a.mp3",
                       durationMs: 300000, cues: [{kind: "memory", positionMs: 0, index: -1}],
                       side: "rekordbox", sourceId: "1", artworkPath: ""};
        controller.junkCues.append({track: track, staged: false, format: "rekordbox", positionMs: 0,
                                    reason: "at the very start of the track"});
        controller.junkCues.append({track: track, staged: true, format: "rekordbox", positionMs: 1188,
                                    reason: "one of 3 hot cues in the first two seconds, which is not a "
                                            + "pattern anyone plays"});
        controller.stagedJunkCueCount = 1;
        controller.unstagedJunkCueCount = 1;
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        tryCompare(findChild(page, "junkCueList"), "count", 2);
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/CuesAtZeroPage.png");
        }
        page.destroy();
        wait(0);
    }
}
