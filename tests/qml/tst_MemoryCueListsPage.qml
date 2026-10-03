// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Memory Cue Lists" page (#55), driven by a stand-in
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
        compare(crumb.title, "Memory Cue Lists");
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
}
