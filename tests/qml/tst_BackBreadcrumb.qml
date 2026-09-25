// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import SeabassGui

// The header's own regression test. Two things are asserted here that
// were both wrong on a real page: the stick's name was abbreviated on a
// row with room to spare, and clicking it went Home -- the same place
// the crumb to its left already went.
TestCase {
    id: testCase
    name: "BackBreadcrumb"
    when: windowShown
    width: 900
    height: 200
    visible: true

    Component {
        id: rowComponent
        // A page header in miniature: the breadcrumb, then a spacer that
        // eats whatever is left over, exactly as every section page's
        // ToolBar is built.
        RowLayout {
            id: header
            property alias crumb: crumb
            property var fakeStack: null
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12
            BackBreadcrumb {
                id: crumb
                stack: header.fakeStack
                middleLabel: "LONG-STICK-NAME"
                title: "Metadata Backup"
            }
            Item { Layout.fillWidth: true }
        }
    }

    // Four segments at an exact width, outside any layout, so the tests
    // below can say precisely how much the row is short by.
    Component {
        id: fourComponent
        Item {
            id: holder
            property alias crumb: crumb
            property var fakeStack: ({depth: 3})
            property string stick: "LONG-STICK-NAME-FOR-TESTING"
            property string middle: "Housekeeping Extended"
            property string pageTitle: "Clean Up Duplicates Everywhere"
            property real crumbWidth: crumb.implicitWidth
            width: testCase.width
            height: 80
            BackBreadcrumb {
                id: crumb
                x: 16
                width: holder.crumbWidth
                stack: holder.fakeStack
                stickLabel: holder.stick
                middleLabel: holder.middle
                title: holder.pageTitle
            }
        }
    }

    function byName(item, name) {
        if (item.objectName === name) {
            return item;
        }
        for (let i = 0; i < item.children.length; ++i) {
            const found = byName(item.children[i], name);
            if (found) {
                return found;
            }
        }
        return null;
    }

    // The separators actually drawn: a dangling one reads as a bug.
    function visibleSeparators(item) {
        let n = 0;
        function walk(it) {
            for (let i = 0; i < it.children.length; ++i) {
                const child = it.children[i];
                if (child.visible && child.text === "›") {
                    ++n;
                }
                walk(child);
            }
        }
        walk(item);
        return n;
    }

    // Whether a segment is showing its whole text. The link form keeps
    // its Label inside the Crumb's Loader.
    function truncated(segment) {
        return segment.truncated !== undefined ? segment.truncated
            : segment.contentItem.item.truncated;
    }

    // The middle segment's Label, whichever of the two forms it is in.
    // Both forms put the name in a Label -- the clickable one inside an
    // AbstractButton -- so the walk keeps the deepest match, and
    // middleButton() below is what distinguishes them.
    function middleItem(header) {
        var found = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.visible && child.text === "LONG-STICK-NAME") {
                    found = child;
                }
                walk(child);
            }
        }
        walk(header);
        return found;
    }

    // The clickable form only: an item carrying the name that is also a
    // button. Matching on `clicked` rather than on the Label means the
    // "not a link" assertion can actually fail -- a Label has no
    // `clicked` either, so asserting its absence on the Label proves
    // nothing about which form is on screen.
    function middleButton(header) {
        var found = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.visible && child.text === "LONG-STICK-NAME"
                        && child.clicked !== undefined) {
                    found = child;
                }
                walk(child);
            }
        }
        walk(header);
        return found;
    }

    // Every test that narrows the row restores it here instead of on its
    // last line: a failing verify() aborts the function, and a 380px row
    // left behind turns one real failure into three confusing ones in the
    // tests that follow.
    function cleanup() {
        testCase.width = 900;
    }

    function test_stickNameIsNotAbbreviatedWhenTheRowHasRoom() {
        var header = createTemporaryObject(rowComponent, testCase);
        waitForRendering(header);
        var middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        // The row is 900 wide and carries maybe a third of that. Nothing
        // here is short of space, so nothing here should be shortened.
        verify(!middle.truncated,
               "the stick's name was elided on a row with room to spare");
        compare(middle.text, "LONG-STICK-NAME");
    }

    // And it still gives way first when the row genuinely runs out --
    // the priority the component was built with, not lost to the fix.
    function test_stickNameStillElidesWhenTheRowIsTooNarrow() {
        var header = createTemporaryObject(rowComponent, testCase);
        testCase.width = 380;
        waitForRendering(header);
        var middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        verify(middle.truncated,
               "the middle segment must be the one that gives way when squeezed");
    }

    // Depth 2 means the item under this page is Home, so the middle
    // segment's click and the house both land there. It stops being a
    // link and becomes plain context.
    function test_stickNameIsNotAClickWhenItWouldOnlyGoHome() {
        var header = createTemporaryObject(rowComponent, testCase,
                                           {fakeStack: {depth: 2}});
        waitForRendering(header);
        verify(!header.crumb.middleClickable,
               "one below Home, the stick's name must not be a link");
        verify(middleItem(header) !== null, "the middle segment was not found");
        verify(middleButton(header) === null,
               "the context form must not be a button");
    }

    function test_stickNameIsAClickWhenItLeadsSomewhereElse() {
        var header = createTemporaryObject(rowComponent, testCase,
                                           {fakeStack: {depth: 3}});
        waitForRendering(header);
        verify(header.crumb.middleClickable,
               "below a hub, the middle segment is a real destination");
        verify(middleButton(header) !== null,
               "below a hub, the middle segment must be a button");
    }

    function test_homeIsTheBreezeIconNotTheWord() {
        var header = createTemporaryObject(rowComponent, testCase);
        waitForRendering(header);
        var sawWord = false;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.text === "Home") {
                    sawWord = true;
                }
                walk(child);
            }
        }
        walk(header);
        verify(!sawWord, "the Home crumb should draw a house, not the word");

        var crumb = null;
        function findCrumb(item) {
            for (var j = 0; j < item.children.length; ++j) {
                if (item.children[j].objectName === "homeCrumb") {
                    crumb = item.children[j];
                }
                findCrumb(item.children[j]);
            }
        }
        findCrumb(header);
        verify(crumb !== null, "the Home crumb was not found");
        verify(crumb.showsIcon, "the Home crumb should draw an icon");
        // And something was actually drawn. The icon is the only way back
        // on pages with no middle segment, so "it has a size" is not
        // enough -- an earlier version rendered an empty gap of exactly
        // the right size when its effect silently produced no pixels.
        var icon = crumb.contentItem.item;
        verify(icon !== null, "the Home crumb has no icon item");
        verify(icon.width > 0 && icon.height > 0, "the Home icon has no size");
        // A quarter larger than HomeIcon's own 0.7 of a small icon. Asked
        // for at that size; drawn at it rounded up to whole pixels, since
        // the SVG is rasterised to a whole-pixel bitmap.
        compare(icon.size, Theme.iconSizeSmall * 0.875);
        compare(icon.implicitHeight, Math.ceil(Theme.iconSizeSmall * 0.875));
        // Deliberately no assertion that pixels were painted. Two were
        // tried -- grabbing the icon, and scanning its rect inside a grab
        // of the header -- and both passed with the icon's color set to
        // "transparent", i.e. neither could fail. grabImage() of anything
        // inside a Control's contentItem Loader comes back empty here,
        // and widening the region picks up the neighbours' pixels. The
        // screenshot below is what backs the visual claim; look at it.
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(header).save(screenshotDir + "/breadcrumb.png");
        }
    }

    // Home > stick > hub > page, with the page under a hub: the stick is
    // context (Home is the stick list, so its click could only go Home),
    // the hub is the one link back.
    function test_fourSegmentsStickIsContextHubIsALink() {
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        const stick = byName(holder, "stickSegment");
        verify(stick !== null && stick.visible, "the stick segment is not shown");
        compare(stick.text, "LONG-STICK-NAME-FOR-TESTING");
        verify(stick.clicked === undefined, "the stick segment must not be a button");
        const link = byName(holder, "middleLink");
        verify(link.visible, "under a hub, the hub segment must be a link");
        compare(link.text, "Housekeeping Extended");
        verify(!byName(holder, "middleSegment").visible,
               "the hub must not also be drawn as context");
        compare(visibleSeparators(holder), 3);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(holder).save(screenshotDir + "/breadcrumb-four.png");
        }
    }

    // Still context at any depth: the stick is never a level of its own.
    function test_stickIsContextEvenDeepInTheStack() {
        const holder = createTemporaryObject(fourComponent, testCase,
                                             {fakeStack: {depth: 5}});
        waitForRendering(holder);
        verify(byName(holder, "stickSegment").visible);
        verify(byName(holder, "middleLink").visible);
    }

    // A page one below Home that has a stick and a middle segment: both
    // are context, since both would only lead Home.
    function test_oneBelowHomeNeitherIsALink() {
        const holder = createTemporaryObject(fourComponent, testCase,
                                             {fakeStack: {depth: 2}});
        waitForRendering(holder);
        verify(byName(holder, "stickSegment").visible);
        verify(!byName(holder, "middleLink").visible);
        verify(byName(holder, "middleSegment").visible);
    }

    // Existing callers: the stick passed as middleLabel, and nothing at all.
    function test_fewerSegmentsLeaveNoDanglingSeparator() {
        const holder = createTemporaryObject(fourComponent, testCase,
                                             {stick: "", fakeStack: {depth: 2}});
        waitForRendering(holder);
        verify(!byName(holder, "stickSegment").visible);
        compare(visibleSeparators(holder), 2);
        holder.middle = "";
        waitForRendering(holder);
        verify(!byName(holder, "middleSegment").visible);
        verify(!byName(holder, "middleLink").visible);
        compare(visibleSeparators(holder), 1);
        // And the stick alone, without a hub: "Home > STICK > page".
        holder.stick = "STICK";
        waitForRendering(holder);
        verify(byName(holder, "stickSegment").visible);
        compare(visibleSeparators(holder), 2);
    }

    // Who gives way, in order. Each step takes the row short by half of
    // one more segment's slack: the stick goes first, then the hub, then
    // the page's own name, and each earlier one is at its floor by the
    // time the next one starts.
    //
    // A RowLayout on its own shares any shortfall among all of them, so
    // the "stick" step used to elide all three at once.
    function test_elisionOrder_data() {
        return [
            {tag: "room", step: 0, stick: false, middle: false, title: false},
            {tag: "stick", step: 1, stick: true, middle: false, title: false},
            {tag: "hub", step: 2, stick: true, middle: true, title: false},
            {tag: "title", step: 3, stick: true, middle: true, title: true},
        ];
    }

    function test_elisionOrder(data) {
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        const crumb = holder.crumb;
        const stickSlack = crumb.stickNatural - crumb.stickFloor;
        const middleSlack = crumb.middleNatural - crumb.middleFloor;
        const titleSlack = crumb.titleNatural - crumb.titleFloor;
        verify(stickSlack > 20 && middleSlack > 20 && titleSlack > 20,
               "the fixture's names are too short to test the order");
        const short = [0,
                       stickSlack / 2,
                       stickSlack + middleSlack / 2,
                       stickSlack + middleSlack + titleSlack / 2][data.step];
        holder.crumbWidth = crumb.implicitWidth - short;
        waitForRendering(holder);
        const stick = byName(holder, "stickSegment");
        const middle = byName(holder, "middleLink");
        const title = byName(holder, "titleSegment");
        compare(truncated(stick), data.stick, "stick");
        compare(truncated(middle), data.middle, "hub");
        compare(truncated(title), data.title, "title");
        if (data.middle) {
            // The stick was spent down to its floor before the hub began.
            fuzzyCompare(stick.width, crumb.stickFloor, 1);
        }
        if (data.title) {
            fuzzyCompare(middle.width, crumb.middleFloor, 1);
        }
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(holder).save(screenshotDir + "/breadcrumb-elide-" + data.tag + ".png");
        }
    }
}
