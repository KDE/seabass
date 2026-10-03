// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Cues the Player Hides" page (#56), driven by a
// stand-in controller.
TestCase {
    id: testCase
    name: "HiddenCuesPage"
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
            property bool hiddenCuesChecked: false
            property int hiddenCueCount: 0
            property int hiddenCueTrackCount: 0
            property bool hiddenCueFixStaged: false
            property var hiddenCueTracks: []
            function recolourHiddenCues() { hiddenCueFixStaged = true; }
            function unstageHiddenCueFix() { hiddenCueFixStaged = false; }
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
        HiddenCuesPage {
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
        controller.hiddenCuesChecked = true;
        controller.hiddenCueCount = 9;
        controller.hiddenCueTrackCount = 3;
        controller.hiddenCueTracks = [{title: "Desert Queen", artist: "Adam Beyer", hotCues: 0, loops: 2}];
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        waitForRendering(page);
        var summary = findChild(page, "hiddenCuesSummary");
        verify(summary.text.indexOf("9 cues on 3 tracks") === 0, summary.text);
        var button = findChild(page, "recolourHiddenCuesButton");
        compare(button.text, "Give Them a Colour");
        button.clicked();
        compare(controller.hiddenCueFixStaged, true);
        compare(button.text, "Unstage");
        compare(findChild(page, "stagedHiddenCuesNote").visible, true);
        button.clicked();
        compare(controller.hiddenCueFixStaged, false);
    }

    function test_thePageIsThisCheckAlone() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        waitForRendering(page);

        var crumb = findCrumb(page.header);
        verify(crumb !== null, "the header has a breadcrumb");
        compare(crumb.title, "Cues the Player Hides");
        compare(crumb.middleLabel, "Library Health", "one level up is the hub");
        // And the stick before it, on screen, as every hub page's children do.
        compare(Breadcrumb.read(page.header).stick, "TESTSTICK");

        verify(findChild(page, "hiddenCuesSummary") !== null, "its own check is here");
        for (const other of ["missingFilesSummary", "stagedIssuesNote", "cuesAtZeroSummary", "stagedJunkCuesNote",
                             "importPromptSummary", "markImportedButton", "sampleRateSummary", "fillSampleRatesButton",
                             "cleanupLeftoverSummary"]) {
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

    // #56. Not claimed on a stick the check did not run on (no Engine
    // library): the page says so instead of "every cue has a colour". Once
    // it has run, each track is named with what the player hides on it.
    function test_notCheckedIsSaidAndEachTrackIsNamed() {
        var controller = createTemporaryObject(controllerComponent, testCase);
        var page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null, "the page must instantiate");
        var notChecked = findChild(page, "hiddenCuesNotChecked");
        var summary = findChild(page, "hiddenCuesSummary");
        var button = findChild(page, "recolourHiddenCuesButton");
        verify(notChecked !== null && summary !== null && button !== null);
        compare(summary.visible, false, "not checked: no finding claimed");
        compare(notChecked.visible, true, "and the page says why there is none");
        compare(button.visible, false);

        controller.hiddenCuesChecked = true;
        compare(notChecked.visible, false);
        compare(summary.visible, true);
        verify(summary.text.indexOf("Every hot cue and saved loop") === 0, summary.text);
        compare(button.visible, false, "nothing to recolour, no button");

        controller.hiddenCueCount = 3;
        controller.hiddenCueTrackCount = 2;
        controller.hiddenCueTracks = [
            {title: "Desert Queen", artist: "Adam Beyer", hotCues: 0, loops: 2},
            {title: "Buggy", artist: "", hotCues: 1, loops: 0}
        ];
        verify(summary.text.indexOf("3 cues on 2 tracks") === 0, summary.text);
        verify(summary.text.indexOf("\u2014") < 0 && summary.text.indexOf("--") < 0, "no dashes on screen");
        var tracks = findChild(page, "hiddenCueTracks");
        verify(tracks !== null);
        compare(tracks.count, 2, "every track is named");
        verify(tracks.itemAt(0).text.indexOf("Desert Queen") >= 0 && tracks.itemAt(0).text.indexOf("2 loops hidden") >= 0,
               tracks.itemAt(0).text);
        verify(tracks.itemAt(1).text.indexOf("Buggy") >= 0 && tracks.itemAt(1).text.indexOf("1 hot cue hidden") >= 0,
               tracks.itemAt(1).text);
        compare(button.visible, true);

        // Looked at, not only asserted: saved when SEABASS_SCREENSHOT_DIR
        // is set.
        if (screenshotDir) {
            page.width = 900;
            page.height = 700;
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/HiddenCuesPage.png");
        }

        page.destroy();
        wait(0);
    }
}
