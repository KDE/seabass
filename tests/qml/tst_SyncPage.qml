// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Sync Cue Points, filled with a fixed analysis (SyncPageFixture in
// qml_test_main.cpp) so its layout can be measured, and looked at, without
// a stick.
//
// What the redesign was for, held in numbers: every title in one column
// across both sections; an expanded panel lined up with that title rather
// than hanging off a label column; a decision's two waveforms the same
// size side by side, and stacked on a narrow window; Select All/None and
// Stage Selected confined to what the search shows, the way Clean Up is;
// and a list that can always scroll its last row clear of Save.
TestCase {
    id: testCase
    name: "SyncPage"
    width: 1100
    height: 720
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        SyncPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    SignalSpy {
        id: junkSpy
        signalName: "junkCueCleanupRequested"
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/is not a function/);
        failOnWarning(/Unable to assign/);
        failOnWarning(/Cannot read property/);
    }

    function openPage(pageWidth, pageHeight) {
        var page = createTemporaryObject(pageComponent, testCase, {width: pageWidth, height: pageHeight});
        verify(page !== null, "page did not instantiate");
        var controller = findChild(page, "syncController");
        verify(controller !== null, "the page's controller was not found");
        // The page analyzes on open; with no catalogs that finishes at once
        // and empty. Filled only after, or it would wipe the fixture.
        tryCompare(controller, "busy", false, 5000);
        verify(syncPageFixture.fill(controller), "the fixture did not take");
        waitForRendering(page);
        return page;
    }

    function listOf(page) {
        var list = findChild(page, "plansList");
        verify(list !== null, "the list was not found");
        return list;
    }

    function collect(item, name, found) {
        found = found || [];
        if (item.objectName === name && item.visible) {
            found.push(item);
        }
        for (var i = 0; i < item.children.length; ++i) {
            collect(item.children[i], name, found);
        }
        return found;
    }

    function xIn(page, item) {
        return item.mapToItem(page, 0, 0).x;
    }

    function test_decisionsComeFirstAndEveryTitleLinesUp() {
        var page = openPage(1100, 720);
        var list = listOf(page);
        compare(list.count, 7, "two decisions and five tracks ready to sync");

        var titleX = -1;
        for (var r = 0; r < 4; ++r) {
            var row = list.itemAtIndex(r);
            verify(row !== null, "row " + r + " was not built");
            compare(row.needsDecision, r < 2, "decisions come first");
            var x = xIn(page, findChild(row, "rowTitle"));
            if (titleX < 0) {
                titleX = x;
            } else {
                compare(x, titleX, "row " + r + "'s title is out of line with the first row's");
            }
        }

        var decisionBox = findChild(list.itemAtIndex(0), "selectCheckBox");
        compare(decisionBox.enabled, false, "a decision cannot be ticked");
        compare(decisionBox.opacity, 0, "and shows no tick box, only its space");
        compare(findChild(list.itemAtIndex(2), "selectCheckBox").enabled, true);
    }

    function test_rightHandColumnsLineUp() {
        var page = openPage(1100, 720);
        var list = listOf(page);
        var names = ["directionColumn", "cueSummaryColumn", "statusColumn"];
        for (var n = 0; n < names.length; ++n) {
            var first = xIn(page, findChild(list.itemAtIndex(0), names[n]));
            for (var r = 1; r < 4; ++r) {
                compare(xIn(page, findChild(list.itemAtIndex(r), names[n])), first,
                        names[n] + " moves on row " + r);
            }
        }
    }

    function test_expandedSidesLineUpWithTheTitle_data() {
        return [
            {tag: "wide decision", pageWidth: 1100, row: 0, sideBySide: true},
            {tag: "wide ready track", pageWidth: 1100, row: 2, sideBySide: true},
            {tag: "narrow decision", pageWidth: 700, row: 0, sideBySide: false},
            {tag: "narrow ready track", pageWidth: 700, row: 2, sideBySide: false},
        ];
    }

    function test_expandedSidesLineUpWithTheTitle(data) {
        var page = openPage(data.pageWidth, 720);
        var list = listOf(page);
        var row = list.itemAtIndex(data.row);
        row.expanded = true;
        waitForRendering(page);

        var extra = findChild(row, "expandedExtra");
        verify(extra.visible, "the expanded content is not showing");
        compare(xIn(page, extra), xIn(page, findChild(row, "rowTitle")),
                "the panel must start where the title does");

        var cards = collect(row, "sideCard");
        compare(cards.length, 2, "both copies of the track are shown");
        compare(cards[0].width, cards[1].width, "the two sides are not the same width");
        var waves = collect(row, "sideWaveform");
        compare(waves.length, 2);
        compare(waves[0].width, waves[1].width, "the two waveforms are not the same width");
        compare(waves[0].height, waves[1].height, "the two waveforms are not the same height");
        if (data.sideBySide) {
            compare(cards[0].mapToItem(page, 0, 0).y, cards[1].mapToItem(page, 0, 0).y, "side by side");
            verify(xIn(page, cards[1]) > xIn(page, cards[0]));
        } else {
            compare(xIn(page, cards[0]), xIn(page, cards[1]), "stacked");
            verify(cards[1].mapToItem(page, 0, 0).y > cards[0].mapToItem(page, 0, 0).y);
        }
    }

    function test_theKeptCueIsCountedAsKept() {
        var page = openPage(1100, 720);
        var row = listOf(page).itemAtIndex(2);
        compare(row.cueSummary, "+3 hot, keeps 1");
        row.expanded = true;
        waitForRendering(page);
        compare(findChild(row, "panelSentence").text,
                "Adds 3 hot cues from Engine and keeps the 1 cue DeviceLibrary already has. "
                + "Nothing is written until Save.");
    }

    function test_selectionAndStagingStayInsideTheSearch() {
        var page = openPage(1100, 720);
        var controller = findChild(page, "syncController");
        compare(controller.selectedCount, 5, "every track ready to sync starts ticked");

        findChild(page, "searchField").text = "kollektiv";
        compare(controller.visibleConflictCount, 1);
        compare(controller.visiblePlanCount, 1);

        mouseClick(findChild(page, "selectNoneButton"));
        compare(controller.selectedVisibleCount, 0);
        compare(controller.selectedCount, 4, "Select None reached tracks the search hides");
        mouseClick(findChild(page, "selectAllButton"));
        compare(controller.selectedVisibleCount, 1);

        var stage = findChild(page, "stageSelectedButton");
        compare(stage.text, "Stage 1 Selected", "the button counts what the search shows");
        mouseClick(stage);
        var dialog = findChild(page, "confirmStageDialog");
        tryCompare(dialog, "opened", true, 2000);
        verify(dialog.searchHidesSome);
        compare(dialog.acceptText, "Stage 1 Track Matching the Search", "the default stages only what is shown");
        compare(dialog.alternateText, "Stage All 5 Tracks Selected");
        compare(dialog.directions.length, 1, "directions are those of the shown track only");
        dialog.close();
        tryCompare(dialog, "opened", false, 2000);
    }

    function test_theJunkCueNoteLinksToStrayCueCleanUp() {
        var page = openPage(1100, 720);
        var row = listOf(page).itemAtIndex(0);
        row.expanded = true;
        waitForRendering(page);
        var cards = collect(row, "sideCard");
        compare(collect(cards[0], "junkCueNote").length, 0, "the side without a 0:00 memory cue has no note");
        var notes = collect(cards[1], "junkCueLink");
        compare(notes.length, 1, "the side with one does");
        junkSpy.target = page;
        junkSpy.clear();
        notes[0].linkActivated("clean-up-stray-cues");
        tryCompare(junkSpy, "count", 1, 2000);
        compare(junkSpy.signalArguments[0][0], "TESTSTICK");
    }

    function test_theListScrollsClearOfSave() {
        var page = openPage(1100, 720);
        var overlay = findChild(page, "saveOverlay");
        verify(overlay !== null);
        verify(overlay.implicitHeight > 0);
        verify(listOf(page).bottomMargin >= overlay.implicitHeight + 2 * overlay.anchors.margins,
               "the last row could end up behind the Save button");
    }

    function test_rowsAndControlsFitANarrowWindow_data() {
        return [
            {tag: "700", pageWidth: 700},
            {tag: "560", pageWidth: 560},
        ];
    }

    function test_rowsAndControlsFitANarrowWindow(data) {
        var page = openPage(data.pageWidth, 720);
        var names = ["filterRow", "scopeRow", "introRow"];
        for (var n = 0; n < names.length; ++n) {
            var item = findChild(page, names[n]);
            var right = item.mapToItem(page, item.width, 0).x;
            verify(right <= page.width + 0.5, names[n] + " reaches " + right + " in a " + page.width + " page");
        }
        var row = listOf(page).itemAtIndex(2);
        var status = findChild(row, "statusColumn");
        verify(xIn(page, status) + status.width <= page.width + 0.5, "the status column runs off the page");
        verify(findChild(row, "rowTitle").width > 60, "the title has been squeezed out");
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = openPage(1100, 720);
        var list = listOf(page);
        list.itemAtIndex(0).expanded = true;
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/SyncPage.png");

        list.itemAtIndex(0).expanded = false;
        list.itemAtIndex(2).expanded = true;
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/SyncPage-ready.png");

        var narrow = openPage(640, 720);
        listOf(narrow).itemAtIndex(0).expanded = true;
        waitForRendering(narrow);
        wait(100);
        grabImage(narrow).save(screenshotDir + "/SyncPage-narrow.png");
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from another
    // page on top of Home). The breadcrumb reads the stack's depth to
    // decide whether its middle segment is a link.
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

    // From Home the stick is all that stands between the house and this
    // page; opened from Library Statistics it names that page too, as the way back,
    // rather than putting the stick's name on a link to it.
    function test_breadcrumb_data() {
        return [
            {tag: "home", below: 1, hubLabel: "", middle: "", link: false},
            {tag: "nested", below: 2, hubLabel: "Library Statistics", middle: "Library Statistics", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const props = {};
        props.hubLabel = data.hubLabel;
        const page = pushOnStack(data.below, props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Sync Cue Points");
    }
}
