// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Sample rates" page, driven by a stand-in controller.
// A row without a sample rate means every cue on that track is placed by
// a guess, and the file itself can say what it really is. The button
// stages, like every other fix in Library Health.
TestCase {
    id: testCase
    name: "SampleRatesPage"
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
            property int sampleRateMissingCount: 0
            property int sampleRateFixableCount: 0
            property bool sampleRateFillStaged: false
            function fillSampleRates() { sampleRateFillStaged = true; }
            function unstageSampleRateFill() { sampleRateFillStaged = false; }
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
        SampleRatesPage {
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
        compare(crumb.title, "Sample Rates");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");
        // And the stick before it, on screen, as every hub page's children do.
        compare(Breadcrumb.read(page.header).stick, "TESTSTICK");

        verify(findChild(page, "sampleRateSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "cuesAtZeroSummary", "stagedJunkCuesNote",
                             "importPromptSummary", "markImportedButton", "cleanupLeftoverSummary", "finishCleanupButton"]) {
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
        controller.sampleRateMissingCount = 43;
        controller.sampleRateFixableCount = 40;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        compare(findChild(page, "fillSampleRatesButton").visible, true);
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/SampleRatesPage.png");
        }
        page.destroy();
        wait(0);
    }

    function test_theSampleRateFixStagesAndSaysSo() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        controller.sampleRateMissingCount = 43;
        controller.sampleRateFixableCount = 40;
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");

        var summary = findChild(page, "sampleRateSummary");
        verify(summary !== null, "the check must say what it found");
        verify(summary.text.indexOf("43") >= 0 && summary.text.indexOf("40") >= 0,
               "both numbers belong in the sentence: " + summary.text);

        var button = findChild(page, "fillSampleRatesButton");
        var note = findChild(page, "stagedSampleRatesNote");
        verify(button !== null && note !== null);
        compare(button.visible, true, "there is something to fix");
        compare(note.visible, false, "and nothing staged yet");

        button.clicked();
        compare(controller.sampleRateFillStaged, true, "the button stages");
        compare(note.visible, true, "and the page says so where the button is");
        compare(button.text, "Unstage", "the same button takes it back");
        button.clicked();
        compare(controller.sampleRateFillStaged, false);

        page.destroy();
        wait(0);
    }
}
