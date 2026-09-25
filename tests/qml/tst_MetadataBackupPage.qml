// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

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

    AppSettingsController { id: realAppSettings }

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
            appSettingsController: realAppSettings
        }
    }

    function make(extra) {
        var page = createTemporaryObject(pageComponent, testCase, extra || {});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        return page;
    }

    // The page opens on its own stick (see the picker test below), so a
    // test about the store says which one it wants.
    function makeShowingTheStore(extra) {
        var page = make(extra);
        var picker = findChild(page, "sourcePicker");
        verify(picker, "the source picker must exist");
        picker.activated(0);
        waitForRendering(page);
        return page;
    }

    // A pick here is what the next page with a playlist picker opens on.
    function test_aPlaylistPickIsRemembered() {
        var before = realAppSettings.lastPlaylistName;
        try {
            var page = make();
            var picker = findChild(page, "playlistPicker");
            verify(picker !== null, "the playlist picker must be there");
            picker.playlistPicked(1, {name: "Peak Time", count: 3});
            compare(realAppSettings.lastPlaylistName, "Peak Time", "the next page with a picker must open on it");
        } finally {
            realAppSettings.lastPlaylistName = before;
        }
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

    // The button names the operation that is about to happen, and this
    // page stages two opposite ones. "Back Up" over a staged deletion
    // names the one thing that is not going to happen, while the summary
    // line directly above it already reads "1 to forget".
    function test_theSaveButtonSaysWhichOfTheTwoThingsItWillDo() {
        var page = make();
        compare(page.verbFor(3, 0), "Back Up", "only additions: the page's own word for them");
        compare(page.verbFor(0, 3), "Forget", "only deletions: the page's own word for them");
        compare(page.verbFor(2, 3), "Save", "both kinds: the neutral word, because no verb covers both");
        compare(page.verbFor(0, 0), "Back Up", "nothing staged: the action the page is for");
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
        // One list, two populations, and the page opens on the stick it
        // was opened from: someone who pressed "Metadata Backup" on a
        // stick is asking about that stick, and opening on the store
        // made every arrival begin by picking it again.
        var page = make();
        var picker = findChild(page, "sourcePicker");
        verify(picker, "the source picker must exist");
        verify(picker.model.length >= 2,
               "the picker offers the store and at least the stick the page was opened on");
        compare(picker.model[0].name, "Local Cue backup database", "index 0 is the store");
        verify(picker.model[0].isStore, "and it is marked as such");
        verify(picker.model[1].name.indexOf("USB Stick") === 0, "a stick says it is one: " + picker.model[1].name);
        compare(picker.currentIndex, 1, "the page opens on its own stick");

        var stored = findChild(page, "storedTrackList");
        var proposals = findChild(page, "proposalList");
        verify(proposals && proposals.visible, "the stick's list is the one showing");
        verify(stored && !stored.visible, "and the store's is not");

        // And the other way, which is the picker's whole job.
        picker.activated(0);
        waitForRendering(page);
        verify(stored.visible, "the stored list shows once the store is picked");
        verify(!proposals.visible, "and the stick's list steps aside");
    }

    function test_thePlaylistPickerBelongsToAStick() {
        // Filtering a backup by playlist only means anything when the
        // source is a stick; the store does not have this stick's
        // playlists.
        var page = make();
        var playlist = findChild(page, "playlistPicker");
        verify(playlist, "the playlist picker must exist");
        verify(playlist.visible, "a stick has playlists, and the page opens on one");
        findChild(page, "sourcePicker").activated(0);
        waitForRendering(page);
        verify(!playlist.visible, "the store does not, so it goes away with the stick");
    }

    function test_saysWhereItWritesInstead() {
        var page = makeShowingTheStore();
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
        // What this asserts: the sentence is there AND something follows
        // it. It used to require the literal word "Seabass" in the label,
        // which passed on a developer machine only because the checkout
        // happens to sit under ~/Seabass with a capital S. On CI the
        // project lives at /builds/multimedia/seabass, lowercase, and
        // indexOf is case-sensitive -- so the test was really asserting
        // where somebody keeps their source tree, and it went red the
        // first time it ever ran anywhere else.
        var marker = "only ever writes here: ";
        var namedLocation = "";
        for (var i = 0; i < labels.length; ++i) {
            var at = labels[i].indexOf(marker);
            if (at >= 0) {
                namedLocation = labels[i].substring(at + marker.length).trim();
                found = namedLocation.length > 0;
            }
        }
        verify(found, "the page must name the location it writes to; saw "
               + JSON.stringify(labels.filter(function (t) { return t.indexOf("writes here") >= 0; })));
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

    // Item nine of the polish list, and the reason it was on it: this
    // page can now hold staged work, and work you cannot see is work you
    // walk away from. The guard is the page's own, not EditSessionHost's
    // -- there is no edit session here, because nothing on a stick is
    // being changed.
    function test_leavingWithSomethingStagedAsks() {
        var page = make();
        // Wait out the opening scan. requestLeave refuses while the
        // controller is busy and says so, which is correct and is not
        // what this test is about -- but with a real stick the page is
        // still reading it when the first assertion runs, so the test
        // measured the scan rather than the guard. It only showed up
        // once the case stopped skipping.
        tryVerify(function () { return !page.busy; }, 30000,
                  "the opening scan must settle before leaving is asked about");
        var list = findChild(page, "storedTrackList");
        verify(list, "the stored track list must exist");
        if (list.count === 0) {
            skip("no stored tracks in this run's metadata store");
        }
        var dialog = findChild(page, "unsavedDialog");
        verify(dialog, "the unsaved-changes dialog must exist");
        verify(!dialog.visible, "it must not be up before anything is staged");

        // Nothing staged: leaving just leaves.
        var left = 0;
        page.requestLeave(function () { left++; });
        compare(left, 1, "with nothing staged, leaving must not be interrupted");
        verify(!dialog.visible, "and must not raise the dialog");

        // Stage one row, and the same request stops to ask.
        var row = list.itemAtIndex(0);
        verify(row, "row 0 must exist");
        row.selectionToggled(true);
        page.requestLeave(function () { left++; });
        compare(left, 1, "with something staged, leaving must be held");
        verify(dialog.visible, "and the dialog must be up");
    }

    function test_changingSourceWithSomethingStagedAsks() {
        // Staging is decided against numbers the next scan replaces, so
        // switching source throws it away -- which is a thing to be told
        // about rather than to discover afterwards.
        var page = make();
        tryVerify(function () { return !page.busy; }, 30000,
                  "the opening scan must settle before staging is asked about");
        var list = findChild(page, "storedTrackList");
        verify(list, "the stored track list must exist");
        if (list.count === 0) {
            skip("no stored tracks in this run's metadata store");
        }
        var dialog = findChild(page, "switchSourceDialog");
        verify(dialog, "the change-source dialog must exist");
        verify(!dialog.visible, "it must not be up before anything is staged");

        list.itemAtIndex(0).selectionToggled(true);
        var picker = findChild(page, "sourcePicker");
        // Index 1 is the page's own stick; index 0 is the store it is
        // already showing, which is not a change at all.
        picker.activated(1);
        verify(dialog.visible, "changing source with staging must ask first");
    }

    // The review finding that would have cost data: answering the leave
    // dialog with "Back Up" used to leave the page in the same
    // statement, which destroys the controller -- cancelling its write
    // half-done -- and, when anything was staged for deletion, left
    // before save() had even saved, because that path stops to ask
    // first. Leaving now waits for saveCompleted.
    function test_savingOnTheWayOutWaitsForTheSave() {
        var page = make();
        tryVerify(function () { return !page.busy; }, 30000,
                  "the opening scan must settle before staging is asked about");
        var list = findChild(page, "storedTrackList");
        verify(list, "the stored track list must exist");
        if (list.count === 0) {
            skip("no stored tracks in this run's metadata store");
        }
        list.itemAtIndex(0).selectionToggled(true);

        var left = 0;
        page.requestLeave(function () { left++; });
        var dialog = findChild(page, "unsavedDialog");
        verify(dialog.visible, "the dialog must be up");

        dialog.saveRequested();
        // A deletion is staged, so save() stops to ask rather than
        // saving. Leaving must not have happened.
        compare(left, 0, "leaving must wait for the save to actually finish");
        verify(page.leaveAfterSave, "and must be remembered as pending");
        verify(findChild(page, "confirmDeleteDialog").visible,
               "the delete confirmation must be what came up");

        // Backing out of the confirmation backs out of the leaving too.
        findChild(page, "confirmDeleteDialog").rejected();
        compare(left, 0, "refusing the confirmation must not leave");
        verify(!page.leaveAfterSave, "and must forget the pending leave");
    }

    function test_aCancelledScanSaysSoRatherThanLookingHung() {
        // Without this the page sat on "Reading X..." with an empty
        // list, no busy indicator and the empty-state label suppressed,
        // because everything keyed off hasScanned and a cancelled scan
        // never sets it.
        var page = makeShowingTheStore();
        var summary = findChild(page, "sourceSummary");
        verify(summary, "the source summary must exist");
        // Browsing the store: no scan has been asked for, so nothing
        // claims to be reading.
        verify(summary.text.indexOf("Reading") < 0,
               "the store population must not claim to be reading a stick");
    }

    function test_anUnreadableCatalogIsSaidBeforeTheSave() {
        // Collected by the scan and, before the review, dropped on the
        // floor: a stick whose Engine database will not open plans
        // without its tracks, and said nothing.
        var page = make();
        var label = findChild(page, "unreadableCatalogsLabel");
        verify(label, "the unreadable-catalog warning must exist on the page");
        verify(!label.visible, "and stay out of the way when every catalog read");
    }

    function test_theSaveTooltipDoesNotPromiseTheStick() {
        // The one string on this page that still said "written to the
        // stick", on a page whose whole premise is that it never writes
        // to one.
        var page = make();
        var overlay = findChild(page, "saveOverlay");
        compare(overlay.destinationPhrase, "stored on this computer",
                "the save button must say where this page's save actually lands");
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

    // ---- the waveform on an opened row -------------------------------------

    // The store holds no waveforms, so an opened stored row draws its cues
    // on the placeholder line, and hovering the line says why.
    function test_aStoredRowShowsItsCuesWithoutAWaveform() {
        const page = makeShowingTheStore();
        const list = findChild(page, "storedTrackList");
        verify(list.count > 0, "the harness seeds two stored tracks, so this list is never empty");
        const row = list.itemAtIndex(0);
        verify(findChild(row, "rowWaveform") === null, "a closed row builds no waveform");
        row.expandToggled();
        waitForRendering(page);
        const waveform = findChild(row, "rowWaveform");
        verify(waveform !== null, "an opened row shows where its cues are");
        compare(waveform.hasWaveform, false, "and nothing more: the backup has no waveform");
        compare(waveform.missingText, "Waveform not part of backup");
        compare(waveform.cueData.length, row.cueCount, "every stored cue, from the store");
        verify(waveform.trackDurationMs > 0, "placed against the stored length");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/MetadataBackupPage-stored-open.png");
        }
    }

    // A stick's row reads the stick's own analysis, for the row that is
    // open and no other. Against a stick-shaped copy of the committed
    // anonymized library, so the read is a real ANLZ file.
    function test_aStickRowReadsItsWaveformFromTheStickWhenOpened() {
        const fixture = Qt.resolvedUrl("../fixtures/anonymized_library").toString().replace(/^file:\/\//, "");
        verify(metadataRestoreFixture.prepareFromLibrary(fixture) > 1000, "the fixture copy must be made");
        const stick = metadataRestoreFixture.stickRoot();
        const reads = [];
        const player = {
            waveformFor: function (format, path, id) {
                reads.push(format);
                return realPlayer.waveformFor(format, path, id);
            }
        };
        const page = make({
            stickLabel: "FIXTURE",
            rekordboxPath: stick + "/PIONEER",
            enginePath: stick + "/Engine Library",
            playbackController: player,
        });
        const list = findChild(page, "proposalList");
        tryVerify(() => !page.busy && list.visible && list.count > 0, 60000, "the stick must be read");
        waitForRendering(page);
        compare(reads.length, 0, "a list of " + list.count + " closed rows reads no waveform");

        const row = list.itemAtIndex(0);
        row.expandToggled();
        waitForRendering(page);
        compare(reads.length, 1, "one read, for the one open row");
        const waveform = findChild(row, "rowWaveform");
        verify(waveform !== null);
        verify(waveform.hasWaveform, "the fixture's analysis has this track's waveform");
        compare(waveform.cueData.length, row.cueCount, "with the stick's cues on it");
        // And where a stick track has none, that is the stick's doing, not
        // the backup's, which is what the stored list says.
        compare(waveform.missingText, "No waveform on the stick for this track");
        if (screenshotDir) {
            grabImage(page).save(screenshotDir + "/MetadataBackupPage-fixture-open.png");
        }
    }

    PlaybackController { id: realPlayer }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make();
        wait(100);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataBackupPage.png");
    }

    // The other population, which no amount of reading the source will
    // show you: a real stick planned against the real store. Needs a
    // stick (or a stick-shaped fixture directory) in SEABASS_LIVE_STICK,
    // because planning is the one thing on this page that cannot be
    // faked -- it reads catalogs.
    function test_screenshot_theStickList() {
        if (!screenshotDir || !liveStickRoot) {
            skip("SEABASS_SCREENSHOT_DIR and SEABASS_LIVE_STICK not both set");
        }
        var page = make({
            stickLabel: "FIXTURE",
            rekordboxPath: liveStickRoot + "/PIONEER",
            enginePath: liveStickRoot + "/Engine Library",
        });
        var picker = findChild(page, "sourcePicker");
        verify(picker, "the source picker must exist");
        // Index 1 is the page's own stick: index 0 is always the store.
        compare(picker.model.length, 2, "the store and this page's stick");
        picker.activated(1);
        // The scan runs on a worker thread. Waiting on the list rather
        // than on a fixed delay, so a slow machine does not shoot an
        // empty page and call it a pass.
        var list = findChild(page, "proposalList");
        verify(list, "the stick's list must exist");
        tryVerify(function () { return list.visible; }, 15000,
                  "the stick's list must become the visible one");
        tryVerify(function () { return !findChild(page, "sourceSummary").text.startsWith("Reading"); },
                  15000, "the scan must finish");
        waitForRendering(page);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataBackupPage-stick.png");
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

    // Opened from Home only, so the stick is context and there is no hub.
    function test_breadcrumbNamesTheStick() {
        const page = pushOnStack(1, {});
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, "");
        compare(crumb.title, "Metadata Backup");
        saveCrumbShot(page, "metadata-backup");
    }
}
