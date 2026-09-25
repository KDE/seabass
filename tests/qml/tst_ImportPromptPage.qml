// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's "The player's import prompt" page, driven by a
// stand-in controller.
TestCase {
    id: testCase
    name: "ImportPromptPage"
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
            property bool playerWillOfferImport: false
            property bool importMarkStaged: false
            function markRekordboxImported() { importMarkStaged = true; }
            function unstageRekordboxImportMark() { importMarkStaged = false; }
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
        ImportPromptPage {
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
        compare(crumb.title, "The Player's Import Prompt");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");

        verify(findChild(page, "importPromptSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "cuesAtZeroSummary", "stagedJunkCuesNote",
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

    // Rendered with a finding on it, and saved to look at when
    // SEABASS_SCREENSHOT_DIR is set.
    function test_rendersWithAFinding() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.playerWillOfferImport = true;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        compare(findChild(page, "markImportedButton").visible, true);
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/ImportPromptPage.png");
        }
        page.destroy();
        wait(0);
    }

    // The player's import prompt: the one check here that is about what a
    // Denon player will do next time the stick is in it, and the only one
    // whose "fix" is telling another program something rather than
    // changing what is on the stick.
    function test_theImportPromptIsSaidAndCanBeStagedAway() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        var summary = findChild(page, "importPromptSummary");
        var button = findChild(page, "markImportedButton");
        var note = findChild(page, "stagedImportMarkNote");
        verify(summary !== null && button !== null && note !== null);

        // Nothing to say when the player will leave the library alone.
        verify(summary.text.indexOf("leave the Engine library alone") >= 0, summary.text);
        compare(button.visible, false, "and nothing to offer");

        controller.playerWillOfferImport = true;
        verify(summary.text.indexOf("overwritten") >= 0,
               "what accepting the prompt costs belongs in the sentence: " + summary.text);
        compare(button.visible, true);
        compare(note.visible, false);

        button.clicked();
        compare(controller.importMarkStaged, true, "the button stages");
        compare(note.visible, true, "and says so beside itself");
        compare(button.text, "Unstage");
        button.clicked();
        compare(controller.importMarkStaged, false);

        page.destroy();
        wait(0);
    }
}
