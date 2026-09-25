// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Duplicates left in OneLibrary" page (#8), driven by a
// stand-in controller.
TestCase {
    id: testCase
    name: "OneLibraryLeftoversPage"
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
            property bool cleanupLeftoversChecked: false
            property int cleanupLeftoverCount: 0
            property int cleanupLeftoverFixableCount: 0
            property bool cleanupLeftoverFixStaged: false
            property string cleanupLeftoverError: ""
            property var cleanupLeftoversHeldBack: []
            function finishCleanupLeftovers() { cleanupLeftoverFixStaged = true; }
            function unstageCleanupLeftoverFix() { cleanupLeftoverFixStaged = false; }
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
        OneLibraryLeftoversPage {
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
    function test_thePageIsThisCheckAlone() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        waitForRendering(page);

        var crumb = findCrumb(page.header);
        verify(crumb !== null, "the header has a breadcrumb");
        compare(crumb.title, "Duplicates Left in OneLibrary");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");
        // And the stick before it, on screen, as every hub page's children do.
        compare(Breadcrumb.read(page.header).stick, "TESTSTICK");

        verify(findChild(page, "cleanupLeftoverSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "cuesAtZeroSummary", "stagedJunkCuesNote",
                             "importPromptSummary", "markImportedButton", "sampleRateSummary", "fillSampleRatesButton"]) {
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

    // #8. Not claimed on a stick the check did not run on (no OneLibrary):
    // the page says so instead of "every duplicate is gone". Once it has
    // run, the numbers are said, the fix stages and unstages from one button, and
    // every leftover it leaves alone is named with its reason.
    function test_cleanUpLeftoversAreSaidStagedAndTheRestNamed() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var notChecked = findChild(page, "cleanupLeftoverNotChecked");
        var summary = findChild(page, "cleanupLeftoverSummary");
        var button = findChild(page, "finishCleanupButton");
        var note = findChild(page, "stagedCleanupLeftoversNote");
        verify(notChecked !== null && summary !== null && button !== null && note !== null);
        compare(summary.visible, false, "not checked: no finding claimed");
        compare(notChecked.visible, true, "and the page says why there is none");

        controller.cleanupLeftoversChecked = true;
        controller.cleanupLeftoverCount = 284;
        controller.cleanupLeftoverFixableCount = 281;
        controller.cleanupLeftoversHeldBack = [
            {title: "Reflection", artist: "Someone", reason: "The rekordbox library has more than one copy of it."}
        ];
        compare(summary.visible, true);
        compare(notChecked.visible, false);
        verify(summary.text.indexOf("284") >= 0 && summary.text.indexOf("281") >= 0,
               "both numbers belong in the sentence: " + summary.text);
        verify(summary.text.indexOf("\u2014") < 0 && summary.text.indexOf("--") < 0, "no dashes on screen");

        var heldBack = findChild(page, "cleanupLeftoverHeldBack");
        verify(heldBack !== null);
        compare(heldBack.count, 1, "the one left alone is named");
        verify(heldBack.itemAt(0).text.indexOf("Reflection") >= 0
               && heldBack.itemAt(0).text.indexOf("more than one copy") >= 0, heldBack.itemAt(0).text);

        // Looked at, not only asserted: saved when SEABASS_SCREENSHOT_DIR
        // is set.
        if (screenshotDir) {
            page.width = 900;
            page.height = 700;
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/OneLibraryLeftoversPage.png");
        }

        compare(button.visible, true);
        compare(note.visible, false);
        button.clicked();
        compare(controller.cleanupLeftoverFixStaged, true, "the button stages");
        compare(note.visible, true, "and the page says so where the button is");
        compare(button.text, "Unstage");
        button.clicked();
        compare(controller.cleanupLeftoverFixStaged, false);

        page.destroy();
        wait(0);
    }
}
