// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

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
    function test_breadcrumbStaysInsideThePage_data() {
        return test_filterRowStaysInsideThePage_data();
    }

    function test_breadcrumbStaysInsideThePage(row) {
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
}
