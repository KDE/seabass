// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Cue Lists" page (#55, #60), driven by a stand-in
// controller.
TestCase {
    id: testCase
    name: "MemoryCueListsPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // Only what the page and its frame (HealthCheckPage) read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property bool legacyMemoryListsChecked: false
            property string legacyMemoryListError: ""
            property int legacyMemoryListCount: 0
            property int legacyMemoryListFixableCount: 0
            property int legacyMemoryListDebrisCount: 0
            property bool legacyMemoryListFixStaged: false
            property var legacyMemoryListTracks: []
            property int cueListDisagreementCount: 0
            property int cueListDisagreementFixableCount: 0
            property var cueListDisagreementTracks: []
            property int cueListMalformedCount: 0
            property var cueListUnreadableTracks: []
            property int cueListFindingCount: legacyMemoryListCount + cueListDisagreementCount
            property int cueListFixableCount: legacyMemoryListFixableCount + cueListDisagreementFixableCount
            property var cueListCounts: ({examined: 12, unreadable: 0})
            property bool keepPlayerCueLists: true
            function repairLegacyMemoryLists() { legacyMemoryListFixStaged = true; }
            function unstageLegacyMemoryListFix() { legacyMemoryListFixStaged = false; }
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
        MemoryCueListsPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
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
    function test_theCountIsSaidAndTheButtonStages() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.legacyMemoryListsChecked = true;
        controller.legacyMemoryListCount = 3;
        controller.legacyMemoryListFixableCount = 3;
        controller.legacyMemoryListDebrisCount = 1;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        var summary = findChild(page, "memoryCueListsSummary");
        verify(summary.text.indexOf("3 tracks have") === 0, summary.text);
        verify(summary.text.indexOf("removes the file left behind") >= 0, summary.text);
        var button = findChild(page, "repairMemoryCueListsButton");
        compare(button.text, "Repair");
        button.clicked();
        compare(controller.legacyMemoryListFixStaged, true);
        compare(button.text, "Unstage");
        compare(findChild(page, "stagedMemoryCueListsNote").visible, true);
        button.clicked();
        compare(controller.legacyMemoryListFixStaged, false);
    }

    function test_thePageIsThisCheckAlone() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        waitForRendering(page);

        var crumb = findCrumb(page.header);
        verify(crumb !== null, "the header has a breadcrumb");
        compare(crumb.title, "Cue Lists");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");
        // And the stick before it, on screen, as every hub page's children do.
        compare(Breadcrumb.read(page.header).stick, "TESTSTICK");

        verify(findChild(page, "memoryCueListsSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "cuesAtZeroSummary", "stagedJunkCuesNote",
                             "importPromptSummary", "markImportedButton", "sampleRateSummary", "fillSampleRatesButton",
                             "cleanupLeftoverSummary", "hiddenCuesSummary"]) {
            compare(findChild(page, other), null, other + " belongs to another check's page");
        }
        compare(controller.scanCalls, 0, "a shared controller is not scanned again");

        // The Back button's ring is the header's edge on the page's line.
        const home = findChild(page.header, "backButton");
        const body = findChild(page, "healthCheckBody");
        verify(home !== null && body !== null);
        compare(home.mapToItem(page, 0, 0).x, body.mapToItem(page, 0, 0).x,
                "breadcrumb and body must share a left edge");
        page.destroy();
        wait(0);
    }

    // #55. Not claimed on a stick the check did not run on (no rekordbox
    // library): the page says so instead of "every list is in shape". Once
    // it has run, each track is named with what is wrong, the ones left
    // alone are counted, and an error from the leg is shown as such.
    function test_notCheckedIsSaidAndEachTrackIsNamed() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var notChecked = findChild(page, "memoryCueListsNotChecked");
        var summary = findChild(page, "memoryCueListsSummary");
        var error = findChild(page, "memoryCueListsError");
        var button = findChild(page, "repairMemoryCueListsButton");
        verify(notChecked !== null && summary !== null && error !== null && button !== null);
        compare(summary.visible, false, "not checked: no finding claimed");
        compare(notChecked.visible, true, "and the page says why there is none");
        compare(button.visible, false);

        controller.legacyMemoryListsChecked = true;
        compare(notChecked.visible, false);
        compare(summary.visible, true);
        verify(summary.text.indexOf("Every memory cue list") === 0, summary.text);
        compare(button.visible, false, "nothing to repair, no button");

        controller.legacyMemoryListCount = 2;
        controller.legacyMemoryListFixableCount = 1;
        controller.legacyMemoryListDebrisCount = 0;
        controller.legacyMemoryListTracks = [
            {title: "Codec", artist: "Aender", what: "a memory cue list of 1 entry with a header that says it is empty", fixable: true},
            {title: "Galaxy Phase", artist: "", what: "a memory cue list Seabass cannot read (len_tag 136 for a section of 134 bytes), left alone", fixable: false}
        ];
        verify(summary.text.indexOf("2 tracks have") === 0, summary.text);
        verify(summary.text.indexOf("1 listed below is left alone") >= 0, summary.text);
        verify(summary.text.indexOf("\u2014") < 0 && summary.text.indexOf("--") < 0, "no dashes on screen");
        var tracks = findChild(page, "memoryCueListTracks");
        verify(tracks !== null);
        compare(tracks.count, 2, "every track is named");
        verify(tracks.itemAt(0).text.indexOf("Codec") >= 0 && tracks.itemAt(0).text.indexOf("says it is empty") >= 0,
               tracks.itemAt(0).text);
        verify(tracks.itemAt(1).text.indexOf("Galaxy Phase") >= 0 && tracks.itemAt(1).text.indexOf("left alone") >= 0,
               tracks.itemAt(1).text);
        compare(button.visible, true);

        // Looked at, not only asserted: saved when SEABASS_SCREENSHOT_DIR
        // is set.
        if (screenshotDir) {
            page.width = 900;
            page.height = 700;
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/MemoryCueListsPage.png");
        }

        controller.legacyMemoryListError = "export.pdb could not be opened";
        compare(error.visible, true, "the leg's error is shown");
        compare(error.text, "export.pdb could not be opened");

        page.destroy();
        wait(0);
    }

    // #60. Tracks whose two lists disagree are named with what the player
    // shows and what Seabass reads; the choice defaults to the player's,
    // says so, and is held while a repair is staged.
    function test_disagreeingListsAreNamedAndTheChoiceDefaultsToThePlayer() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        controller.legacyMemoryListsChecked = true;
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        const summary = findChild(page, "cueListDisagreementSummary");
        const choice = findChild(page, "cueListChoice");
        const keepPlayer = findChild(page, "keepPlayerCueListsButton");
        const keepSeabass = findChild(page, "keepSeabassCueListsButton");
        const button = findChild(page, "repairMemoryCueListsButton");
        verify(summary !== null && choice !== null && keepPlayer !== null && keepSeabass !== null);
        verify(summary.text.indexOf("the player and Seabass read the same cues") >= 0, summary.text);
        compare(choice.visible, false, "nothing to choose between");

        controller.cueListDisagreementCount = 1;
        controller.cueListDisagreementFixableCount = 1;
        controller.cueListDisagreementTracks = [
            {title: "Too Little Too Late", artist: "", fixable: true,
             what: "the player shows 1 pad (A 0:30.8); Seabass reads 5 pads (A 0:30.3, B 1:07.8, C 1:15.3, D 2:00.3, E 2:15.3)"}
        ];
        verify(summary.text.indexOf("1 track shows different cues on the player than in Seabass") === 0, summary.text);
        verify(summary.text.indexOf("\u2014") < 0 && summary.text.indexOf("--") < 0, "no dashes on screen");
        compare(choice.visible, true);
        compare(keepPlayer.checked, true, "the player's list is the default");
        verify(keepPlayer.text.indexOf("the default") >= 0, keepPlayer.text);
        compare(keepSeabass.checked, false);
        compare(button.visible, true, "a disagreement alone is something to repair");
        const tracks = findChild(page, "cueListDisagreementTracks");
        compare(tracks.count, 1);
        verify(tracks.itemAt(0).text.indexOf("the player shows 1 pad") >= 0, tracks.itemAt(0).text);

        keepSeabass.toggle();
        keepSeabass.toggled();
        compare(controller.keepPlayerCueLists, false, "the choice reaches the controller");
        button.clicked();
        compare(controller.legacyMemoryListFixStaged, true);
        compare(keepPlayer.enabled, false, "held while staged");

        if (screenshotDir) {
            page.width = 900;
            page.height = 700;
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/CueListsPage-disagreement.png");
        }
        page.destroy();
        wait(0);
    }

    // Never a clean bill over nothing: a check that examined no file
    // says so, and the sentences that would claim all is well are not
    // shown.
    function test_nothingExaminedIsNotClean() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        controller.legacyMemoryListsChecked = true;
        controller.cueListCounts = {examined: 0, unreadable: 3};
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        const counts = findChild(page, "cueListCounts");
        verify(counts.visible);
        verify(counts.text.indexOf("no cue list was checked") >= 0 && counts.text.indexOf("3 are missing") >= 0,
               counts.text);
        compare(findChild(page, "memoryCueListsSummary").visible, false);
        compare(findChild(page, "cueListDisagreementSummary").visible, false);

        controller.cueListCounts = {examined: 40, unreadable: 1};
        controller.cueListUnreadableTracks = [{title: "Codec", artist: "", what: "could not be read: ANLZ0000.EXT is missing"}];
        verify(counts.text.indexOf("Checked 40 analysis files. 1 could not be read") === 0, counts.text);
        const unreadable = findChild(page, "cueListUnreadableTracks");
        compare(unreadable.count, 1, "the file that could not be read is named");
        verify(unreadable.itemAt(0).text.indexOf("ANLZ0000.EXT is missing") >= 0, unreadable.itemAt(0).text);

        // A memory list found from the .DAT while its .EXT is missing:
        // nothing examined, and still the finding is said.
        controller.cueListCounts = {examined: 0, unreadable: 1};
        controller.legacyMemoryListCount = 1;
        controller.legacyMemoryListFixableCount = 1;
        compare(findChild(page, "memoryCueListsSummary").visible, true, "a finding is said even when nothing was examined");
        page.destroy();
        wait(0);
    }
}
