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

    // The middle segment, whichever of the two forms it is currently in.
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
        header.parent.width = 380;
        waitForRendering(header);
        var middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        verify(middle.truncated,
               "the middle segment must be the one that gives way when squeezed");
        header.parent.width = 900;
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
        var middle = middleItem(header);
        verify(middle !== null, "the middle segment was not found");
        // A Label, not the AbstractButton the clickable form is.
        verify(middle.clicked === undefined,
               "the context form must not be clickable");
    }

    function test_stickNameIsAClickWhenItLeadsSomewhereElse() {
        var header = createTemporaryObject(rowComponent, testCase,
                                           {fakeStack: {depth: 3}});
        waitForRendering(header);
        verify(header.crumb.middleClickable,
               "below a hub, the middle segment is a real destination");
    }

    function test_homeIsAGlyphNotTheWord() {
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
    }
}
