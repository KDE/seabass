import QtQuick
import QtTest
import SeabassGui

// Restore Metadata's page. This is the half of the feature that writes
// to a stick, so what is guarded here is that nothing reaches one by
// accident: staging is a separate act from saving, and the button that
// does the writing says so.
//
// Also saves a screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "MetadataRestorePage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        MetadataRestorePage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            libraryId: ""
        }
    }

    function make(extra) {
        var page = createTemporaryObject(pageComponent, testCase, extra || {});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        return page;
    }

    function test_thereIsNoConflictQuestionToGetWrong() {
        // The pair of radio buttons that used to ask whether to keep the
        // stick's cues or replace them from the store is gone, in favour
        // of one rule applied per field: a blank is filled, more cues
        // wins, otherwise the later edit wins. This is the guard that
        // the question does not come back.
        var page = make();
        verify(!findChild(page, "keepStickRadio"), "the keep radio must be gone");
        verify(!findChild(page, "replaceFromStoreRadio"), "the replace radio must be gone");
    }

    function test_theSaveButtonSaysRestore() {
        // The page's one write. Everywhere else in Seabass this button
        // says "Save", which is right on a page you have been editing;
        // here the whole page is a single verb and the button is the
        // moment it happens.
        var page = make();
        var host = findChild(page, "saveOverlay");
        verify(host, "the floating save button must exist");
        compare(host.label, "Restore", "and must say what it does");
    }

    function test_thereIsOnlyTheStandardSaveButton() {
        // One button on this page, and it is the one every other editing
        // page has. There was briefly a second, under the list, that
        // turned a selection into staged changes -- two ways to say the
        // same thing, and a tooltip that had to explain which of them it
        // did. Ticking a row stages it; the standard button writes.
        var page = make();
        verify(!findChild(page, "stageSelectedButton"),
               "staging must not have a button of its own beside the save button");
        verify(findChild(page, "saveOverlay"), "the standard save button must be the one that writes");
    }

    function test_theListHasASearchAndMarking() {
        var page = make();
        var toolbar = findChild(page, "proposalToolbar");
        verify(toolbar, "the list toolbar must exist");
        verify(findChild(toolbar, "selectAllButton"), "Select All must exist");
        verify(findChild(toolbar, "selectNoneButton"), "Select None must exist");
        verify(findChild(toolbar, "searchField"), "the search field must exist");
        var clear = findChild(toolbar, "clearSearchButton");
        verify(clear, "the clear button must exist");
        verify(!clear.visible, "and must be hidden while the search is empty");
        toolbar.searchText = "anything";
        verify(clear.visible, "and shown once there is something to clear");
    }

    function test_headerTextLinesUpWithTheBody() {
        var page = make();
        var crumbText = null;
        var bodyText = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                // "⌂", not "Home": the crumb draws a house glyph now.
                if (child.text === "⌂" && child.width < 120) {
                    crumbText = child;
                }
                if (child.objectName === "pageIntro") {
                    bodyText = child;
                }
                walk(child);
            }
        }
        walk(page);
        verify(crumbText, "the Home crumb's label was not found");
        verify(bodyText, "the page's first body line was not found");
        compare(crumbText.mapToItem(page, 0, 0).x, bodyText.mapToItem(page, 0, 0).x,
                "breadcrumb text and body text must share a left edge");
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make();
        wait(100);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataRestorePage.png");
    }
}
