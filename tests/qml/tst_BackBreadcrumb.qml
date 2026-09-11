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
}
