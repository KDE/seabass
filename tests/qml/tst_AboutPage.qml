// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// About, headless. The page is prose and two buttons, so what is worth
// pinning is that the prose stays inside the page at a narrow width and
// that the two ways out of it are actually there -- the website link and
// the route to the donation page were the point of rewriting it, and a
// button that stopped being wired would look exactly the same in a
// screenshot as one that works.
TestCase {
    id: testCase
    name: "AboutPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        AboutPage {}
    }

    function make(w, h) {
        return createTemporaryObject(pageComponent, testCase, {width: w, height: h});
    }

    function test_links_are_present() {
        var page = make(700, 620);
        waitForRendering(page);
        var website = findChild(page, "aboutWebsiteButton");
        verify(website !== null, "the website button exists");
        verify(page.websiteUrl.indexOf("http") === 0, "the website URL is a URL: " + page.websiteUrl);
        verify(findChild(page, "aboutSupportButton") !== null, "the support button exists");
    }

    // Clicking Support must reach the page's own signal. Checked by
    // listening for it rather than by pushing a StackView, so the test
    // fails on a disconnected button and not on anything about
    // navigation.
    function test_support_button_emits() {
        // Tall enough that the buttons are actually on screen: they sit
        // near the bottom of a long page, and mouseClick() on an item
        // scrolled out of view lands outside the window and does
        // nothing, which reads exactly like a disconnected button.
        var page = make(700, 1400);
        waitForRendering(page);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "donationRequested"});
        mouseClick(findChild(page, "aboutSupportButton"));
        compare(spy.count, 1);
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    // Nothing may reach past the right edge. Same check tst_CleanupPage
    // makes, and for the same reason: a long unwrapped line is invisible
    // at a comfortable window size and obvious at a narrow one.
    function test_nothing_overflows_a_narrow_page() {
        var page = make(420, 700);
        waitForRendering(page);
        wait(50);
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
        check(page);
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make(700, 900);
        waitForRendering(page);
        wait(150);
        grabImage(page).save(screenshotDir + "/AboutPage.png");
    }
}
