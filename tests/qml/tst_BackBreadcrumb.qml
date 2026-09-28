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

    // The ground every header is drawn on, so the screenshots show the
    // title's ink the way a page does and not light text on white.
    Rectangle {
        anchors.fill: parent
        z: -1
        color: Theme.surface
    }

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

    // The chevrons actually drawn: a dangling one reads as a bug.
    function visibleSeparators(item) {
        let n = 0;
        function walk(it) {
            for (let i = 0; i < it.children.length; ++i) {
                const child = it.children[i];
                if (child.visible && child.objectName === "eyebrowSep") {
                    ++n;
                }
                walk(child);
            }
        }
        walk(item);
        return n;
    }

    // The width to give the bar so that its eyebrow is `shortBy` pixels
    // short of its natural width. The eyebrow gets the column beside the
    // Back button, and reaches back into the gap by its pill's padding.
    function widthForEyebrow(crumb, shortBy) {
        return byName(crumb, "backButton").implicitWidth + crumb.spacing
            - crumb.eyebrowPillPadding + crumb.eyebrowNatural - shortBy;
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
        let found = null;
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                let child = item.children[i];
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
        let found = null;
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                let child = item.children[i];
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
        SystemFontMetrics.generalPointSizeOverride = 0;
    }

    function test_stickNameIsNotAbbreviatedWhenTheRowHasRoom() {
        let header = createTemporaryObject(rowComponent, testCase);
        waitForRendering(header);
        let middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        // The row is 900 wide and carries maybe a third of that. Nothing
        // here is short of space, so nothing here should be shortened.
        verify(!middle.truncated,
               "the stick's name was elided on a row with room to spare");
        compare(middle.text, "LONG-STICK-NAME");
    }

    // And it still gives way when the row genuinely runs out: the
    // priority the component was built with, not lost to the fix. Short
    // by twenty pixels of what the eyebrow needs, whatever the fonts.
    function test_stickNameStillElidesWhenTheRowIsTooNarrow() {
        let header = createTemporaryObject(rowComponent, testCase);
        waitForRendering(header);
        // The header's own margins and its gap before the spacer.
        testCase.width = widthForEyebrow(header.crumb, 20) + 2 * 16 + 12;
        waitForRendering(header);
        let middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        verify(middle.truncated,
               "the middle segment must be the one that gives way when squeezed");
    }

    // Depth 2 means the item under this page is Home, so the middle
    // segment's click and the house both land there. It stops being a
    // link and becomes plain context.
    function test_stickNameIsNotAClickWhenItWouldOnlyGoHome() {
        let header = createTemporaryObject(rowComponent, testCase,
                                           {fakeStack: {depth: 2}});
        waitForRendering(header);
        verify(!header.crumb.middleClickable,
               "one below Home, the stick's name must not be a link");
        verify(middleItem(header) !== null, "the middle segment was not found");
        verify(middleButton(header) === null,
               "the context form must not be a button");
    }

    function test_stickNameIsAClickWhenItLeadsSomewhereElse() {
        let header = createTemporaryObject(rowComponent, testCase,
                                           {fakeStack: {depth: 3}});
        waitForRendering(header);
        verify(header.crumb.middleClickable,
               "below a hub, the middle segment is a real destination");
        verify(middleButton(header) !== null,
               "below a hub, the middle segment must be a button");
    }

    function test_homeIsTheBreezeIconNotTheWord() {
        let header = createTemporaryObject(rowComponent, testCase);
        waitForRendering(header);
        let sawWord = false;
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                let child = item.children[i];
                if (child.visible && child.text === "Home") {
                    sawWord = true;
                }
                walk(child);
            }
        }
        walk(header);
        verify(!sawWord, "the Home crumb should draw a house, not the word");

        let crumb = null;
        function findCrumb(item) {
            for (let j = 0; j < item.children.length; ++j) {
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
        let icon = crumb.contentItem.item.children[0];
        verify(icon !== null && icon.iconName === "go-home", "the Home crumb has no icon item");
        verify(icon.width > 0 && icon.height > 0, "the Home icon has no size");
        // The eyebrow's own size, and never taller than the crumb's
        // content: it is a caption's picture, not a button's.
        compare(icon.size, header.crumb.eyebrowIconSize);
        verify(icon.size <= crumb.contentItem.item.height,
               "the house is taller than the eyebrow's line");
        // And it starts on the title's left edge: the eyebrow and the
        // title share one line down the header.
        const title = byName(header, "titleSegment");
        compare(icon.mapToItem(header, 0, 0).x, title.mapToItem(header, 0, 0).x,
                "the house and the title must start on one edge");
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
        // House > stick > hub: two chevrons, none before the title, which
        // is on a line of its own.
        compare(visibleSeparators(holder), 2);
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
        compare(visibleSeparators(holder), 1);
        holder.middle = "";
        waitForRendering(holder);
        verify(!byName(holder, "middleSegment").visible);
        verify(!byName(holder, "middleLink").visible);
        compare(visibleSeparators(holder), 0);
        // The house alone would be a speck above the title: it says
        // "Home" when it is the whole path, and only then.
        verify(byName(holder, "homeWord").visible, "a lone house names itself");
        // And the stick alone, without a hub: "Home > STICK".
        holder.stick = "STICK";
        waitForRendering(holder);
        verify(byName(holder, "stickSegment").visible);
        compare(visibleSeparators(holder), 1);
        verify(!byName(holder, "homeWord").visible, "with a crumb after it, the house is a picture");
    }

    // Who gives way in the eyebrow, in order. The stick drops whole at
    // the first pixel short, and a "…" stands where it was; then the hub
    // elides, down to its floor; past the floor it keeps going rather
    // than overflow. The title is on its own line and is not part of
    // this: it elides when its own line is short (the last step, where
    // the column is a floor's width and the title cannot fit it).
    //
    // A RowLayout on its own shares any shortfall among all of them, so
    // the stick and the hub used to elide at once.
    function test_elisionOrder_data() {
        return [
            {tag: "room", step: 0, dropped: false, middle: false, title: false},
            {tag: "stick", step: 1, dropped: true, middle: false, title: false},
            {tag: "hub", step: 2, dropped: true, middle: true, title: false},
            {tag: "title", step: 3, dropped: true, middle: true, title: true},
        ];
    }

    function test_elisionOrder(data) {
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        const crumb = holder.crumb;
        const stickSaving = crumb.stickNatural - byName(holder, "droppedMark").naturalWidth;
        const middleSlack = crumb.middleNatural - crumb.middleFloor;
        verify(stickSaving > 20 && middleSlack > 20,
               "the fixture's names are too short to test the order");
        const shortBy = [0, 2, stickSaving + middleSlack / 2, stickSaving + middleSlack][data.step];
        holder.crumbWidth = widthForEyebrow(crumb, shortBy);
        waitForRendering(holder);
        const stick = byName(holder, "stickSegment");
        const middle = byName(holder, "middleLink");
        const title = byName(holder, "titleSegment");
        compare(!stick.visible, data.dropped, "stick dropped");
        compare(byName(holder, "droppedMark").visible, data.dropped, "\u2026 in its place");
        if (!data.dropped) {
            verify(!truncated(stick), "with room, the stick is whole");
        }
        compare(truncated(middle), data.middle, "hub");
        if (data.step === 3) {
            fuzzyCompare(middle.width, crumb.middleFloor, 1);
        }
        // The title's line is the column's width, not the eyebrow's. At
        // the last step the column is down to the hub's floor, and no
        // title of this length fits that.
        if (data.title) {
            verify(truncated(title), "title");
        }
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(holder).save(screenshotDir + "/breadcrumb-elide-" + data.tag + ".png");
        }
    }

    // The title does not give way for the eyebrow. At a width its own
    // line fits, it is whole, however much of the path above it has had
    // to go.
    function test_titleSurvivesWhileTheEyebrowGivesWay() {
        const holder = createTemporaryObject(fourComponent, testCase,
                                             {pageTitle: "Clean Up"});
        waitForRendering(holder);
        const crumb = holder.crumb;
        verify(crumb.eyebrowNatural - crumb.eyebrowPillPadding > crumb.titleNatural + 40,
               "the precondition: the path is longer than the title");
        holder.crumbWidth = byName(holder, "backButton").implicitWidth + crumb.spacing + crumb.titleNatural;
        waitForRendering(holder);
        verify(!byName(holder, "stickSegment").visible, "the eyebrow gave way");
        verify(!truncated(byName(holder, "titleSegment")), "the title did not");
    }

    // Never wider than its row, at any width and any system font size.
    //
    // Every segment used to have a floor it would not go below, so the
    // row's minimum was the floors plus the house, the separators and the
    // gaps. At 10pt that sum just fitted a 380 page; at macOS's 13pt every
    // one of those lengths is 1.3 times as long and Clean Up's title ran
    // 72px past the page (round 9, tst_CleanupPage). 16 is a KDE user
    // with large text.
    function test_neverWiderThanItsRow_data() {
        const rows = [];
        for (const pointSize of [10, 13, 16]) {
            for (const width of [700, 520, 420, 380, 340, 300, 260, 220]) {
                rows.push({tag: pointSize + "pt " + width, pointSize: pointSize, width: width});
            }
        }
        return rows;
    }

    function test_neverWiderThanItsRow(data) {
        SystemFontMetrics.generalPointSizeOverride = data.pointSize;
        // Theme never goes below the platform's smallest readable size
        // (13 on macOS, where the 10pt rows laid out at 13 and failed
        // this line in round 9). The row must then hold at that size; the
        // override is only refused where the platform would refuse it too.
        compare(Theme.baseFontPointSize, Math.max(Theme.smallestReadablePointSize, data.pointSize),
                "the precondition: the font size took");
        const holder = createTemporaryObject(fourComponent, testCase, {crumbWidth: data.width});
        waitForRendering(holder);
        const crumb = holder.crumb;
        compare(crumb.width, data.width, "the precondition: the row is as wide as asked");
        const right = crumb.mapToItem(holder, crumb.width, 0).x;
        let widest = 0;
        let widestText = "";
        function walk(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i];
                if (child.visible && child.width > 0) {
                    const edge = child.mapToItem(holder, child.width, 0).x;
                    if (edge > widest) {
                        widest = edge;
                        widestText = child.text !== undefined ? child.text : child.objectName;
                    }
                }
                walk(child);
            }
        }
        walk(crumb);
        verify(widest <= right + 0.5,
               "\"" + widestText + "\" reaches " + widest + " in a row ending at " + right);
        // Nothing dangles either: a chevron before the stick (or the "…"
        // standing in for it) and one before the hub.
        compare(visibleSeparators(holder), 2);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(holder).save(screenshotDir + "/breadcrumb-" + data.pointSize + "pt-" + data.width + ".png");
        }
    }

    // The stick goes whole and first: at exactly the eyebrow's width it
    // stays, a pixel short it goes, and the hub is untouched either way.
    function test_stickDropsBeforeTheHubElides() {
        SystemFontMetrics.generalPointSizeOverride = 13;
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        const crumb = holder.crumb;
        holder.crumbWidth = Math.ceil(widthForEyebrow(crumb, 0));
        waitForRendering(holder);
        verify(byName(holder, "stickSegment").visible, "the eyebrow fits: the stick stays");
        verify(!truncated(byName(holder, "stickSegment")));
        holder.crumbWidth = Math.floor(widthForEyebrow(crumb, 2));
        waitForRendering(holder);
        verify(!byName(holder, "stickSegment").visible, "two pixels short: the stick goes");
        verify(byName(holder, "droppedMark").visible, "and a \u2026 says it was there");
        verify(byName(holder, "middleLink").visible, "the hub stays, a link back");
        verify(!truncated(byName(holder, "middleLink")), "and whole");
    }

    // With no hub the stick is the path's last crumb: it elides rather
    // than drop, since a path ending in "…" names nothing.
    function test_stickWithoutAHubElidesInsteadOfDropping() {
        const holder = createTemporaryObject(fourComponent, testCase, {middle: ""});
        waitForRendering(holder);
        holder.crumbWidth = widthForEyebrow(holder.crumb, 40);
        waitForRendering(holder);
        const stick = byName(holder, "stickSegment");
        verify(stick.visible, "the stick stays");
        verify(truncated(stick), "and elides");
        verify(!byName(holder, "droppedMark").visible);
    }

    SignalSpy { id: homeSpy; signalName: "homeRequested" }
    SignalSpy { id: backSpy; signalName: "backRequested" }

    // The round button goes one level up: to the hub when the hub is a
    // real destination, Home when it is not. Its tooltip says which.
    function test_backButtonGoesToTheHubOrHome_data() {
        return [
            {tag: "under a hub", depth: 3, middle: "Housekeeping Extended", back: 1, home: 0,
             tip: "Back to Housekeeping Extended"},
            {tag: "one below Home", depth: 2, middle: "Housekeeping Extended", back: 0, home: 1,
             tip: "Back to Home"},
            {tag: "no hub", depth: 4, middle: "", back: 0, home: 1, tip: "Back to Home"},
        ];
    }

    function test_backButtonGoesToTheHubOrHome(data) {
        const holder = createTemporaryObject(fourComponent, testCase,
                                             {fakeStack: {depth: data.depth}, middle: data.middle});
        waitForRendering(holder);
        homeSpy.clear();
        backSpy.clear();
        homeSpy.target = holder.crumb;
        backSpy.target = holder.crumb;
        const button = byName(holder, "backButton");
        compare(button.ToolTip.text, data.tip);
        mouseClick(button);
        compare(backSpy.count, data.back, "backRequested");
        compare(homeSpy.count, data.home, "homeRequested");
    }

    // While a write runs, the button is disabled, faded, and says why.
    function test_backButtonDisabledSaysWhy() {
        const holder = createTemporaryObject(fourComponent, testCase);
        holder.crumb.backEnabled = false;
        waitForRendering(holder);
        homeSpy.clear();
        backSpy.clear();
        homeSpy.target = holder.crumb;
        backSpy.target = holder.crumb;
        const button = byName(holder, "backButton");
        verify(!button.enabled);
        compare(button.opacity, 0.5);
        compare(button.ToolTip.text, holder.crumb.backDisabledTooltip);
        compare(byName(holder, "homeCrumb").ToolTip.text, holder.crumb.backDisabledTooltip);
        mouseClick(button);
        compare(backSpy.count + homeSpy.count, 0, "a disabled Back goes nowhere");
    }

    // The button is the bar's one Tab stop; the eyebrow's crumbs are for
    // the mouse.
    function test_backButtonIsTheTabStop() {
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        compare(byName(holder, "backButton").focusPolicy, Qt.StrongFocus);
        compare(byName(holder, "homeCrumb").focusPolicy, Qt.NoFocus);
        compare(byName(holder, "middleLink").focusPolicy, Qt.NoFocus);
    }

    // The path is a caption and the title is the page's name: smaller and
    // muted above, larger and in full ink below.
    function test_eyebrowIsSmallerAndMutedTitleIsFullInk() {
        const holder = createTemporaryObject(fourComponent, testCase, {fakeStack: {depth: 2}});
        waitForRendering(holder);
        const stick = byName(holder, "stickSegment");
        const hub = byName(holder, "middleSegment");
        const title = byName(holder, "titleSegment");
        verify(stick.font.pointSize < title.font.pointSize,
               "eyebrow " + stick.font.pointSize + "pt, title " + title.font.pointSize + "pt");
        compare(stick.font.pointSize, Theme.fontSmall);
        compare(title.font.pointSize, Theme.titleCrumb);
        compare(stick.font.capitalization, Font.AllUppercase);
        compare(title.font.weight, Font.DemiBold);
        // As strings: Theme's colours are mixed in floating point, and
        // Qt.colorEqual tells a colour from its own copy through a Label
        // apart in the last bits, where no one could see it.
        compare(title.color.toString(), Theme.text.toString(), "the title is in Theme.text");
        compare(stick.color.toString(), Theme.textMuted.toString(), "the eyebrow is in Theme.textMuted");
        compare(hub.color.toString(), Theme.textMuted.toString(), "the eyebrow is in Theme.textMuted");
        verify(title.color.toString() !== stick.color.toString(), "the two inks differ");
    }

    // Two lines beside a round button: the bar is as tall as whichever
    // is taller, and the button sits on the lines' middle.
    function test_barHeightAt13pt() {
        SystemFontMetrics.generalPointSizeOverride = 13;
        const holder = createTemporaryObject(fourComponent, testCase);
        waitForRendering(holder);
        const crumb = holder.crumb;
        const eyebrow = byName(holder, "eyebrow");
        const title = byName(holder, "titleSegment");
        const button = byName(holder, "backButton");
        compare(button.width, Theme.headerBackButtonSize);
        compare(button.height, Theme.headerBackButtonSize);
        const lines = eyebrow.implicitHeight + title.implicitHeight;
        compare(crumb.implicitHeight, Math.max(Theme.headerBackButtonSize, lines));
        verify(lines > Theme.headerBackButtonSize, "the two lines are the taller part");
        // Neither more than 64 design pixels, the height the design gave
        // the bar, nor less than the button and a hair.
        verify(crumb.implicitHeight <= Theme.scaled(64),
               "the bar is " + crumb.implicitHeight + " tall at 13pt");
        const buttonMiddle = button.mapToItem(crumb, 0, button.height / 2).y;
        fuzzyCompare(buttonMiddle, crumb.height / 2, 1);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(holder).save(screenshotDir + "/breadcrumb-13pt-height.png");
        }
    }
}
