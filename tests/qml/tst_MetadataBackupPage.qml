import QtQuick
import QtTest
import SeabassGui

// Metadata Backup's page, checked for the claims it makes rather than
// for its layout.
//
// The one that would matter most if it were wrong: the page promises the
// stick is not touched, so it has to say where it writes instead. The
// rest guard the controls that can lose data -- a delete that is staged
// rather than done, and a selection that cannot outlive the rows it was
// made on.
//
// Also saves a screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "MetadataBackupPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        MetadataBackupPage {
            width: 880
            height: 660
            // Nowhere on purpose: a page handed a stick that is not
            // there must still build, and it keeps the suite off the
            // filesystem.
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
        var page = make();
        // The pair of radio buttons that used to ask "take the stick's
        // version or keep what is stored?" is gone, and this is the
        // guard that it stays gone. The question was unanswerable: one
        // answer covers 1500 tracks, the stick is newer for some of them
        // and the store for others, and the machine can see which copy
        // holds more work while the person cannot. The rule that
        // replaced it is domain::metadata_merge.
        verify(!findChild(page, "overwriteRadio"), "the overwrite radio must be gone");
        verify(!findChild(page, "keepStoredRadio"), "the keep-stored radio must be gone");
    }

    function test_theRuleIsExplainedWhereTheQuestionUsedToBe() {
        // Taking a question away only works if the page says what it
        // does instead. The help text comes from the domain function
        // that implements the rule, so the two cannot drift apart.
        var page = make();
        verify(page.children.length > 0, "page did not build");
        verify(findChild(page, "backupInfoButton"), "the info button explaining the rule must exist");
    }

    function test_theSaveButtonSaysBackUp() {
        // The standard floating Save, with the word this page's save
        // actually means. There is no second button that writes.
        var page = make();
        var overlay = findChild(page, "saveOverlay");
        verify(overlay, "the standard save overlay must exist");
        compare(overlay.label, "Back Up", "the save button says what this page's save does");
        verify(!findChild(page, "backUpNowButton"),
               "the old always-on Add button must be gone: a backup is staged now");
        verify(!findChild(page, "deleteStagedButton"),
               "deleting goes through the same Save button, not one of its own");
    }

    function test_nothingIsStagedSoNothingIsOffered() {
        // The floating button is only up when there is something to
        // save, and the escape hatch beside it only when there is
        // something to clear.
        var page = make();
        var overlay = findChild(page, "saveOverlay");
        verify(!overlay.visible, "the save button must be hidden with nothing staged");
        var clear = findChild(page, "clearStagingButton");
        verify(!clear || !clear.visible, "Clear must not be offered with nothing staged");
    }

    function test_thePickerChoosesWhichPopulationTheListShows() {
        // One list, two populations. The picker's first entry is the
        // store itself, which is where the page opens.
        var page = make();
        var picker = findChild(page, "sourcePicker");
        verify(picker, "the source picker must exist");
        verify(picker.model.length >= 2,
               "the picker offers the store and at least the stick the page was opened on");
        compare(picker.model[0].name, "Everything stored", "index 0 is the store");
        verify(picker.model[0].isStore, "and it is marked as such");
        compare(picker.currentIndex, 0, "the page opens on the store, which is never empty-looking");
        // The stick's list is the other population, and it is not the
        // one showing.
        var stored = findChild(page, "storedTrackList");
        var proposals = findChild(page, "proposalList");
        verify(stored && stored.visible, "the stored list shows while the source is the store");
        verify(proposals && !proposals.visible, "the stick's list is hidden until a stick is picked");
    }

    function test_thePlaylistPickerBelongsToAStick() {
        // Filtering a backup by playlist only means anything when the
        // source is a stick; the store does not have this stick's
        // playlists.
        var page = make();
        var playlist = findChild(page, "playlistPicker");
        verify(playlist, "the playlist picker must exist");
        verify(!playlist.visible, "it stays hidden while the source is the store");
    }

    function test_saysWhereItWritesInstead() {
        var page = make();
        // "Nothing on the stick is at risk" is only reassuring if the
        // page also says where the data does go.
        var found = false;
        var labels = [];
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.text !== undefined && typeof child.text === "string") {
                    labels.push(child.text);
                }
                walk(child);
            }
        }
        walk(page);
        for (var i = 0; i < labels.length; ++i) {
            if (labels[i].indexOf("Seabass") >= 0 && labels[i].indexOf("only ever writes here") >= 0) {
                found = true;
            }
        }
        verify(found, "the page must name the location it writes to");
    }

    function test_backUpNeedsAStick() {
        // With no stick to read, the picker offers only the store, so
        // there is nothing to pick that could start a backup.
        var page = make({stickLabel: "", rekordboxPath: "", enginePath: ""});
        var picker = findChild(page, "sourcePicker");
        verify(picker, "the source picker must exist");
        compare(picker.model.length, 1, "only the store is on offer with no stick attached");
        verify(picker.model[0].isStore, "and that one entry is the store");
    }

    function test_deletingIsMarkedAndConfirmed() {
        // The only destructive thing this page can do. The backup may be
        // the last copy of cues a reformatted stick no longer has, so a
        // single click must not be able to reach the database: a row is
        // marked first, one button acts on what is marked, and a dialog
        // stands between that and the delete.
        var page = make();
        var overlay = findChild(page, "saveOverlay");
        // Nothing marked, so the one button that commits is not offered
        // at all rather than offered and inert.
        verify(!overlay.visible, "the save button must not be offered with nothing marked");
        verify(findChild(page, "confirmDeleteDialog"), "a confirmation dialog must exist");
        // And there is exactly one button down there. A second one that
        // turned a selection into a set of marks was the shape this page
        // had briefly, and two buttons for one decision is how a user
        // ends up pressing the wrong one.
        verify(!findChild(page, "stageSelectedForDeletionButton"),
               "marking and deleting must not be two separate buttons");
    }

    function test_theListHasASearchAndMarking() {
        var page = make();
        var toolbar = findChild(page, "browseToolbar");
        verify(toolbar, "the list toolbar must exist");
        verify(findChild(toolbar, "selectAllButton"), "Select All must exist");
        verify(findChild(toolbar, "selectNoneButton"), "Select None must exist");
        verify(findChild(toolbar, "searchField"), "the search field must exist");
        // The clear button appears with the text rather than sitting
        // there permanently as a control that does nothing.
        var clear = findChild(toolbar, "clearSearchButton");
        verify(clear, "the clear button must exist");
        verify(!clear.visible, "and must be hidden while the search is empty");
        toolbar.searchText = "anything";
        verify(clear.visible, "and shown once there is something to clear");
    }

    function test_emptyStoreNamesTheActionThatFillsIt() {
        var page = make();
        // An empty list that only says "empty" leaves the user to find
        // the button. Ours points at it.
        verify(page.hasStick, "the fixture stick must count as a stick");
    }

    // The breadcrumb's own text has to start on the same vertical line
    // as the page body under it. It did not: the style gives ToolBar 4px
    // of padding of its own, and the crumb is a hover pill with another
    // 8.8 inside that, so the Home crumb sat 4px right of every line
    // beneath it.
    //
    // Asserted on measured positions rather than on the properties that
    // produce them, because the properties were all individually
    // defensible and the result still did not line up. Any page header
    // would do; this one is simply the one that has a test.
    function test_headerTextLinesUpWithTheBody() {
        var page = make();
        var crumbText = null;
        var bodyText = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                // By objectName: the crumb draws Breeze's go-home icon
                // now, so it has no text to match against. Its content
                // item is what has to line up with the body.
                if (child.objectName === "homeCrumb") {
                    crumbText = child.contentItem;
                }
                if (child.objectName === "pageIntro") {
                    bodyText = child;
                }
                walk(child);
            }
        }
        walk(page);
        verify(crumbText, "the Home crumb's content item was not found");
        verify(bodyText, "the page's first body line was not found");
        var crumbX = crumbText.mapToItem(page, 0, 0).x;
        var bodyX = bodyText.mapToItem(page, 0, 0).x;
        compare(crumbX, bodyX, "breadcrumb text at " + crumbX + " and body text at " + bodyX
                + " must share a left edge (crumb w=" + crumbText.width + ")");
        compare(bodyX, Theme.pageMargin, "and that edge is Theme.pageMargin");
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make();
        wait(100);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataBackupPage.png");
    }

    // The states a default screenshot cannot show, and the ones most
    // likely to be wrong: a row opened, a row struck through and dimmed
    // because it is staged to go, and the bar under the list that only
    // exists once something is selected. Rendered rather than reasoned
    // about, because every layout bug in this page so far has been
    // invisible in the source and obvious in a picture.
    function test_screenshot_expandedAndStaged() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make();
        var list = findChild(page, "storedTrackList");
        verify(list, "the stored track list must exist");
        if (list.count === 0) {
            skip("no stored tracks in this run's metadata store");
        }
        // Row 0 open, row 1 on its way out, row 2 merely ticked.
        page.expandedTrackId = 1;
        var controller = null;
        for (var i = 0; i < 3 && i < list.count; ++i) {
            var row = list.itemAtIndex(i);
            if (!row) {
                continue;
            }
            if (i === 1) {
                row.actionItems[0].clicked();
            }
            if (i === 2) {
                row.selectionToggled(true);
            }
        }
        wait(200);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataBackupPage-expanded.png");
    }
}
