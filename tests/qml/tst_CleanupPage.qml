// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Clean Up's filter row, at the widths a real window actually gets.
//
// Nothing covered this before, which is why the row could run off the
// right edge for as long as it did and why it took a screenshot to find:
// the suite renders at whatever width the test asks for, and every test
// asked for a wide one.
TestCase {
    id: testCase
    name: "CleanupPage"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        CleanupPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    function findByObjectName(item, name) {
        if (item.objectName === name) {
            return item;
        }
        for (var i = 0; i < item.children.length; ++i) {
            var found = findByObjectName(item.children[i], name);
            if (found) {
                return found;
            }
        }
        return null;
    }

    // Widths worth caring about: a half-screen window, a narrow one, and
    // one narrower than any single control's natural width.
    function test_filterRowStaysInsideThePage_data() {
        return [
            {tag: "960", pageWidth: 960},
            {tag: "700", pageWidth: 700},
            {tag: "520", pageWidth: 520},
            {tag: "380", pageWidth: 380},
        ];
    }

    function test_filterRowStaysInsideThePage(row) {
        var page = createTemporaryObject(pageComponent, testCase,
                                          {width: row.pageWidth, height: 660});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        var filterRow = findByObjectName(page, "filterRow");
        verify(filterRow, "the filter row was not found");

        // The row itself must fit, and so must every child in it. A Flow
        // wraps, so the failure this catches is a single control wider
        // than the space rather than too many of them side by side.
        verify(filterRow.width <= page.width,
               "filter row is " + filterRow.width + " wide in a " + page.width + " page");
        for (var i = 0; i < filterRow.children.length; ++i) {
            var child = filterRow.children[i];
            if (!child.visible || child.width === 0) {
                continue;
            }
            var right = child.mapToItem(page, child.width, 0).x;
            verify(right <= page.width + 0.5,
                   "child " + i + " (" + child + ") reaches " + right + " in a " + page.width + " page");
        }
    }

    // The breadcrumb row is the other half of the same problem: it is
    // what pinned the column, and letting it shrink is what unpinned it.
    // So it has to stay inside the page too, at the same widths.
    // At this system's font size and at macOS's 13pt, where every scaled
    // length is 1.3 times as long (round 9: "Clean Up Duplicates" reached
    // 452 in a 380 page on the Mac).
    function test_breadcrumbStaysInsideThePage_data() {
        const rows = [];
        for (const pointSize of [0, 13]) {
            for (const row of test_filterRowStaysInsideThePage_data()) {
                rows.push({tag: row.tag + (pointSize ? " at " + pointSize + "pt" : ""),
                           pageWidth: row.pageWidth, pointSize: pointSize});
            }
        }
        return rows;
    }

    function cleanup() {
        SystemFontMetrics.generalPointSizeOverride = 0;
    }

    function test_breadcrumbStaysInsideThePage(row) {
        SystemFontMetrics.generalPointSizeOverride = row.pointSize;
        var page = createTemporaryObject(pageComponent, testCase,
                                          {width: row.pageWidth, height: 660});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        var filterRow = findByObjectName(page, "filterRow");
        verify(filterRow, "the filter row was not found");
        var crumbRow = filterRow.parent.children[0];
        verify(crumbRow, "the breadcrumb row was not found");

        var right = crumbRow.mapToItem(page, crumbRow.width, 0).x;
        verify(right <= page.width + 0.5,
               "breadcrumb row reaches " + right + " in a " + page.width + " page");

        // And the text inside it, which is what a reader actually sees
        // run off the edge.
        function check(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.visible && child.text !== undefined && child.width > 0) {
                    var edge = child.mapToItem(page, child.width, 0).x;
                    verify(edge <= page.width + 0.5,
                           "\"" + child.text + "\" reaches " + edge + " in a " + page.width + " page");
                }
                check(child);
            }
        }
        check(crumbRow);
    }

    // #66: a group says which copy it keeps and which it removes, by
    // file, and why -- before anything is staged. The fixture's two rows
    // are "Duplicate Song.mp3" in folderA and in folderB: the same name,
    // so the folder is what tells them apart, and nothing else differs,
    // so the reason has to own up to a tie broken on the path.
    readonly property string fixtureEngineRoot: {
        const url = Qt.resolvedUrl("../fixtures/duplicate_engine_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: enginePageComponent
        CleanupPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    function findWhere(item, predicate) {
        if (predicate(item)) {
            return item;
        }
        const kids = item.children ? item.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findWhere(kids[i], predicate);
            if (found) {
                return found;
            }
        }
        return null;
    }

    function shownText(page, text) {
        return findWhere(page, (item) => item.visible && item.text !== undefined && String(item.text) === text);
    }

    function test_aGroupNamesTheCopyItKeepsAndWhy() {
        const engineCopy = artworkFixture.libraryCopy(testCase.fixtureEngineRoot);
        verify(engineCopy.length > 0, "the fixture copy must be made");
        const page = createTemporaryObject(enginePageComponent, testCase,
                                           {width: 960, height: 660, enginePath: engineCopy});
        verify(page, "page did not instantiate");
        tryVerify(() => shownText(page, "Keep") !== null, 5000, "the scan never showed the fixture's group");
        waitForRendering(page);

        verify(shownText(page, "Remove") !== null, "the other copy is named as removed");
        verify(shownText(page, "folderA/Duplicate Song.mp3") !== null, "the kept copy, by folder: the names are the same");
        verify(shownText(page, "folderB/Duplicate Song.mp3") !== null, "the removed copy, by folder");
        verify(shownText(page, "Same length and size, in as many playlists; keeps the copy whose path sorts first.")
               !== null, "the tie is said to be one");

        // Keep and its path share a row: the survivor is folderA's copy.
        const keep = shownText(page, "Keep");
        const keptPath = shownText(page, "folderA/Duplicate Song.mp3");
        compare(keep.mapToItem(page, 0, 0).y, keptPath.mapToItem(page, 0, 0).y);

        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/CleanupPage-group.png");
        }
    }

    // Scrolling the groups folds the header's description away and keeps
    // its controls. Twelve more groups than the fixture's one, at a
    // typical window, so the list has somewhere to scroll to.
    readonly property var headerControls: ["selectAllButton", "deselectAllButton", "stageButton",
                                           "playlistPicker", "searchField", "duplicateInfo"]

    function scrollablePage() {
        const library = artworkFixture.libraryWithDuplicateGroups(testCase.fixtureEngineRoot, 12);
        verify(library.length > 0, "the fixture copy must be made");
        const page = createTemporaryObject(enginePageComponent, testCase,
                                           {width: 1000, height: 700, enginePath: library});
        verify(page, "page did not instantiate");
        const list = findByObjectName(page, "plansList");
        verify(list, "the group list was not found");
        tryCompare(list, "count", 13, 10000, "the scan never listed the fixture's groups");
        waitForRendering(page);
        verify(list.contentHeight > list.height * 2, "the list must have room to scroll");
        return page;
    }

    // Shown, usable, and inside the header: a control the fold clipped
    // or pushed under the list would still read visible and enabled.
    function verifyInHeader(page, item, name) {
        verify(item, name + " was not found");
        verify(item.visible, name + " is hidden");
        const top = item.mapToItem(page, 0, 0).y;
        verify(top >= 0 && top + item.height <= page.header.height + 0.5,
               name + " spans " + top + " to " + (top + item.height) + " in a " + page.header.height + " header");
    }

    function scrollDown(list, notches) {
        for (let i = 0; i < notches; ++i) {
            mouseWheel(list, list.width / 2, list.height / 2, 0, -120);
            wait(30);
        }
    }

    function test_scrollingFoldsTheHeaderAndKeepsItsControls() {
        const page = scrollablePage();
        const list = findByObjectName(page, "plansList");
        const details = findByObjectName(page, "headerDetails");
        verify(details && details.visible, "the description shows before any scroll");
        const fullHeader = page.header.height;
        const enabled = {};
        for (const name of headerControls) {
            const control = findByObjectName(page, name);
            verifyInHeader(page, control, name);
            enabled[name] = control.enabled;
        }

        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/CleanupPage-header-full.png");
        }

        scrollDown(list, 3);
        tryVerify(() => !details.visible, 2000, "scrolling never folded the description");
        const topRow = list.indexAt(0, list.contentY + 1);
        const contentY = list.contentY;
        waitForRendering(page);
        verify(page.header.height < fullHeader - 50,
               "folded header is " + page.header.height + ", was " + fullHeader);
        // Within a pixel: the wheel's own scroll can still be easing out.
        fuzzyCompare(list.contentY, contentY, 1, "folding moved the list");
        compare(list.indexAt(0, list.contentY + 1), topRow, "folding changed the row at the top");
        for (const name of headerControls) {
            const control = findByObjectName(page, name);
            verifyInHeader(page, control, name);
            compare(control.enabled, enabled[name], name + " changed whether it is usable");
        }
        verifyInHeader(page, findByObjectName(page, "crumbRow"), "the breadcrumb");
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/CleanupPage-header-folded.png");
        }

        // Up to the very top again: unfolded, as it was.
        for (let i = 0; i < 60 && !list.atYBeginning; ++i) {
            mouseWheel(list, list.width / 2, list.height / 2, 0, 120);
            wait(30);
        }
        verify(list.atYBeginning, "the wheel never got back to the top");
        tryVerify(() => !list.moving, 2000, "the list never came to rest");
        tryCompare(page.header, "height", fullHeader, 2000, "the header did not unfold at the top");
        verify(details.visible);
        verifyInHeader(page, findByObjectName(page, "duplicateInfo"), "duplicateInfo");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/CleanupPage-header-unfolded.png");
        }
    }

    // A keypress moves contentY and nothing else; one notch past the
    // point where it folds, it folds and stays folded.
    function test_foldedHeaderIsStableJustPastTheThreshold() {
        const page = scrollablePage();
        const list = findByObjectName(page, "plansList");
        const details = findByObjectName(page, "headerDetails");
        list.contentY = list.originY + 40;
        tryVerify(() => !details.visible, 2000, "the header did not fold");
        const heights = [];
        for (let i = 0; i < 10; ++i) {
            wait(50);
            heights.push(page.header.height);
        }
        verify(heights.every((h) => h === heights[0]), "the header kept resizing: " + heights);
        verify(!details.visible, "the header unfolded by itself");
    }

    // A look at the narrow case, since the failure this file exists for
    // was found in a screenshot and not in a number.
    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = createTemporaryObject(pageComponent, testCase, {width: 520, height: 660});
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/CleanupPage-narrow.png");
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

    // From Maintain's card on the home, and from Cues on Duplicate
    // Copies' suggestion, which the crumb leads back to.
    function test_breadcrumb_data() {
        return [
            {tag: "home", props: {}, below: 1, middle: "", link: false},
            {tag: "duplicates", props: {hubLabel: "Cues on Duplicate Copies"}, below: 2,
             middle: "Cues on Duplicate Copies", link: true},
        ];
    }
    function test_breadcrumb(data) {
        const page = pushOnStack(data.below, data.props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Clean Up Duplicates");
        saveCrumbShot(page, "cleanup-" + data.tag);
    }
}
