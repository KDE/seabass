// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtCore
import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse's ways into the track import, against a stand-in controller
// that records what the page asks of it. The import itself is
// track_import_controller_test's.
TestCase {
    id: testCase
    name: "TrackImport"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        ScanPage {}
    }

    Component {
        id: fakeImportComponent
        QtObject {
            property var calls: []
            property bool busy: false
            property string errorMessage: ""
            property int pendingRevision: 0
            property int stagedCount: 0
            property string stagedPlaylist: ""
            property bool stagedPlaylistIsNew: false
            property var lastResult: ({})
            signal resultReady()
            signal tracksImported()
            function importFiles(rekordboxPath, enginePath, files, playlist, create, atIndex) {
                calls.push(["import", rekordboxPath, enginePath, files.map(f => f.toString()), playlist, create, atIndex]);
                return true;
            }
            function importPossibleDuplicates() { calls.push(["duplicates"]); return true; }
            function dropStaged() {
                calls.push(["drop"]);
                stagedCount = 0;
                pendingRevision++;
            }
            function trackCountChange(playlist) { return playlist === stagedPlaylist ? stagedCount : 0; }
            function suggestedPlaylistName(files) { return files.length === 1 ? "Crate" : ""; }
            function fileDialogFilter() { return "*.mp3 *.wav"; }
        }
    }

    function makePage(experimental) {
        realAppSettings.experimentalFeaturesEnabled = experimental;
        // A library that reads, with nothing in it (see tst_ScanPage).
        browseFixture.holdCues(0);
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 880, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER", enginePath: "/nonexistent/TESTSTICK/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
        verify(page !== null);
        page.trackImportController = createTemporaryObject(fakeImportComponent, testCase);
        waitForRendering(page);
        return page;
    }

    function cleanup() {
        realAppSettings.experimentalFeaturesEnabled = false;
        browseFixture.restore();
    }

    readonly property var crate: ["file:///home/dj/Crate"]
    readonly property var twoFiles: ["file:///home/dj/a.mp3", "file:///home/dj/b.wav"]
    readonly property var stick: ["/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library"]

    // Experimental off: no menu entry, and neither list takes a file.
    function test_withoutExperimentalThereIsNoWayIn() {
        const page = makePage(false);
        page.openPlaylistMenu("Peak", page);
        const menu = findChild(page, "playlistMenu");
        tryCompare(menu, "opened", true);
        compare(findChild(page, "importTracksItem").visible, false);
        compare(findChild(page, "importFolderItem").visible, false);
        compare(findChild(page, "takeBackImportItem").visible, false);
        menu.close();
        compare(findChild(page, "playlistList").fileDropsEnabled, false);
        compare(findChild(page, "newPlaylistFileDropArea").enabled, false);
        compare(findChild(page, "trackFileDropArea").enabled, false);
    }

    // On: the playlist's menu offers files and a folder, and what the
    // file dialog gives goes into that playlist, at its end.
    function test_thePlaylistMenuImportsIntoThePlaylist() {
        const page = makePage(true);
        page.openPlaylistMenu("Peak", page);
        const menu = findChild(page, "playlistMenu");
        tryCompare(menu, "opened", true);
        compare(findChild(page, "importTracksItem").visible, true);
        compare(findChild(page, "importFolderItem").visible, true);
        compare(findChild(page, "importTracksItem").text, "Import tracks...");
        menu.close();
        compare(findChild(page, "trackFileDropArea").enabled, true);
        compare(findChild(page, "newPlaylistFileDropArea").enabled, true);
        page.importFiles(testCase.twoFiles, "Peak", false, -1);
        compare(JSON.stringify(page.trackImportController.calls),
                JSON.stringify([["import"].concat(testCase.stick, [testCase.twoFiles, "Peak", false, -1])]));
    }

    // Files dropped on a playlist row go into it; on "All tracks", into
    // no playlist.
    function test_aDropOnARowGoesIntoThatPlaylist() {
        const page = makePage(true);
        const list = findChild(page, "playlistList");
        compare(list.fileDropsEnabled, true);
        list.filesDropped("Peak", testCase.twoFiles);
        list.filesDropped("", testCase.crate);
        compare(JSON.stringify(page.trackImportController.calls),
                JSON.stringify([["import"].concat(testCase.stick, [testCase.twoFiles, "Peak", false, -1]),
                                ["import"].concat(testCase.stick, [testCase.crate, "", false, -1])]));
    }

    // Dropped on the empty part of the list, the files go into a new
    // playlist: its name is asked for, a dropped folder's proposed, and
    // nothing is asked of the controller before OK.
    function test_aDropOnTheEmptyPartAsksForANewPlaylist() {
        const page = makePage(true);
        const list = findChild(page, "playlistList");
        list.filesDroppedForNew(testCase.crate);
        const dialog = findChild(page, "importPlaylistDialog");
        tryCompare(dialog, "opened", true);
        const field = findChild(page, "importPlaylistNameField");
        tryCompare(field, "activeFocus", true);
        compare(field.text, "Crate");
        compare(page.trackImportController.calls.length, 0);
        field.text = "  ";
        compare(dialog.standardButton(Dialog.Ok).enabled, false, "spaces are no name");
        field.text = " Friday Set ";
        keyClick(Qt.Key_Return);
        tryCompare(dialog, "visible", false);
        compare(JSON.stringify(page.trackImportController.calls),
                JSON.stringify([["import"].concat(testCase.stick, [testCase.crate, "Friday Set", true, -1])]));
    }

    // A playlist staged to be created is not on the stick: a drop on it
    // is refused in words, and nothing is asked of the controller.
    function test_aDropOnAPlaylistNotYetSavedIsRefused() {
        const page = makePage(true);
        page.trackImportController.stagedPlaylist = "Fresh";
        page.trackImportController.stagedCount = 2;
        page.trackImportController.stagedPlaylistIsNew = true;
        verify(page.newPlaylistNames.indexOf("Fresh") >= 0, "the import's new playlist is listed as new");
        page.importFiles(testCase.twoFiles, "Fresh", false, -1);
        const message = findChild(page, "playlistEditMessage");
        tryCompare(message, "opened", true);
        verify(message.headline.indexOf("Save first") > 0, message.headline);
        compare(page.trackImportController.calls.length, 0);
        message.close();
    }

    // What the import came to: how many wait for Save, every file left
    // out with its reason, the possible duplicates on request, and the
    // whole batch taken back.
    function test_theResultNamesWhatWaitsAndWhatWasLeftOut() {
        const page = makePage(true);
        const fake = page.trackImportController;
        fake.lastResult = {staged: 3, megabytes: 41.2, playlist: "Peak", playlistIsNew: false, note: "",
                           notImported: [{name: "notes.txt", why: "is not a format every library on this stick lists"},
                                         {name: "old.mp3", why: "may be the track the stick has as Contents/A/old.mp3"}],
                           possibleDuplicates: 1};
        fake.stagedCount = 3;
        fake.stagedPlaylist = "Peak";
        fake.resultReady();
        const dialog = findChild(page, "importResultDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "3 track(s) ready to import into \"Peak\".");
        verify(dialog.detailText.indexOf("42 MB") > 0, dialog.detailText);
        const lines = findChild(page, "importNotImportedList");
        compare(lines.count, 2);
        compare(findChild(page, "importNotImportedHeading").visible, true);
        compare(findChild(page, "importDuplicatesButton").visible, true);
        compare(findChild(page, "importTakeBackButton").visible, true);
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/browse_import_result.png");
        }
        // The playlist's menu takes the batch back as well.
        findChild(page, "importTakeBackButton").clicked();
        tryCompare(dialog, "visible", false);
        compare(JSON.stringify(fake.calls), JSON.stringify([["drop"]]));

        fake.resultReady();
        tryCompare(dialog, "opened", true);
        findChild(page, "importDuplicatesButton").clicked();
        tryCompare(dialog, "visible", false);
        compare(JSON.stringify(fake.calls), JSON.stringify([["drop"], ["duplicates"]]));

        // Nothing staged: said, with no Take back.
        fake.lastResult = {staged: 0, megabytes: 0, playlist: "", playlistIsNew: false, note: "",
                           notImported: [{name: "notes.txt", why: "is not a format every library on this stick lists"}],
                           possibleDuplicates: 0};
        fake.resultReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "None of these files can be imported.");
        compare(findChild(page, "importTakeBackButton").visible, false);
        compare(findChild(page, "importDuplicatesButton").visible, false);
        dialog.close();
    }

    // A refusal from the controller is shown.
    function test_aRefusalIsShown() {
        const page = makePage(true);
        page.trackImportController.errorMessage = "This stick has no library to import tracks into.";
        const message = findChild(page, "playlistEditMessage");
        tryCompare(message, "opened", true);
        compare(message.headline, "This stick has no library to import tracks into.");
        message.close();
    }

    // The playlist an import waits for offers to take it back in its menu.
    function test_thePlaylistMenuTakesTheImportBack() {
        const page = makePage(true);
        page.trackImportController.stagedPlaylist = "Peak";
        page.trackImportController.stagedCount = 2;
        page.trackImportController.pendingRevision++;
        page.openPlaylistMenu("Peak", page);
        const menu = findChild(page, "playlistMenu");
        tryCompare(menu, "opened", true);
        const item = findChild(page, "takeBackImportItem");
        compare(item.visible, true);
        compare(item.text, "Don't import the 2 track(s)");
        item.triggered();
        compare(JSON.stringify(page.trackImportController.calls), JSON.stringify([["drop"]]));
        menu.close();
    }
}
