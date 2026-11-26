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
            property string progressStep: ""
            property int progressDone: 0
            property int progressTotal: 0
            property string progressFile: ""
            property var progressFiles: []
            function cancel() { calls.push(["cancel"]); }
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
            // Analyse again.
            property string reanalysingPlaylist: ""
            property var lastReanalysis: ({})
            property var reanalysed: ({})
            signal reanalysisReady()
            function analyseAgain(rekordboxPath, enginePath, playlist) {
                calls.push(["analyseAgain", rekordboxPath, enginePath, playlist]);
                return true;
            }
            function dropReanalysis(playlist) {
                calls.push(["dropReanalysis", playlist]);
                delete reanalysed[playlist];
                pendingRevision++;
            }
            function reanalysisCount(playlist) { return reanalysed[playlist] ?? 0; }
            property var cueReview: []
            property var cueReviewTooDifferent: []
            signal moveCuesChanged(string changeId, bool move)
            // The choices made since the list was, by id, and which of
            // them the person made (as the controller: the threshold
            // leaves those alone).
            property var choices: ({})
            property var byHand: ({})
            property bool saveWriting: false
            function movesCues(id) {
                if (id in choices) {
                    return choices[id];
                }
                const track = cueReview.find(t => t.id === id);
                return track ? track.moveCues : false;
            }
            function setMoveCues(id, move) {
                calls.push(["moveCues", id, move]);
                choices[id] = move;
                byHand[id] = true;
                moveCuesChanged(id, move);
                return true;
            }
            function setMoveCuesForAll(move) {
                calls.push(["moveCuesForAll", move]);
                for (const track of cueReview) {
                    choices[track.id] = move;
                    byHand[track.id] = true;
                    moveCuesChanged(track.id, move);
                }
                return true;
            }
            property int cueFollowThresholdMs: 10
            function applyCueThreshold(ms) {
                calls.push(["applyCueThreshold", ms]);
                cueFollowThresholdMs = ms;
                let kept = 0;
                for (const track of cueReview) {
                    const follows = track.largestMoveMs < ms;
                    if (byHand[track.id]) {
                        kept += movesCues(track.id) !== follows ? 1 : 0;
                        continue;
                    }
                    choices[track.id] = follows;
                    moveCuesChanged(track.id, follows);
                }
                return kept;
            }
        }
    }

    // Just enough of the playlist edits for the menu to tell a playlist
    // staged to be made from one on the stick.
    Component {
        id: fakePlaylistEditComponent
        QtObject {
            property int pendingRevision: 0
            property string errorMessage: ""
            property var newPlaylists: []
            property var pendingMoves: ({})
            property var pendingAdditions: ({})
            property string lastAddPlaylist: ""
            signal pendingChanged()
            function isNewPlaylistStaged(name) { return newPlaylists.indexOf(name) >= 0; }
            function isPlaylistStaged(name) { return false; }
            function isRemovalStaged() { return false; }
            function isAdditionStaged() { return false; }
            function isRenameStaged() { return false; }
            function isPlaylistEdited() { return false; }
            function trackCountChange() { return 0; }
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

    // Files held at the lower edge of a list longer than its room scroll
    // it down to "Create new playlist"; at the end the edge is no scroll
    // zone any more, so the row under it takes the drop.
    function test_filesHeldAtAnEdgeScrollTheList() {
        const page = makePage(true);
        const list = findChild(page, "playlistList");
        const down = findChild(page, "edgeScrollDown");
        const up = findChild(page, "edgeScrollUp");
        // Short: nothing to scroll to, no zone in the rows' way.
        compare(down.enabled, false);
        compare(up.enabled, false);
        const names = [];
        for (let i = 0; i < 60; ++i) {
            names.push("New " + i);
        }
        list.newPlaylists = names;
        tryVerify(() => list.contentHeight > list.height * 2);
        compare(down.enabled, true);
        compare(up.enabled, false);
        const start = list.contentY;
        list.edgeScrollStep(1);
        verify(list.contentY > start, "a step scrolls down");
        compare(up.enabled, true);
        for (let i = 0; i < 1000; ++i) {
            list.edgeScrollStep(1);
        }
        fuzzyCompare(list.contentY, list.edgeScrollBottom, 0.5);
        compare(down.enabled, false);
        const create = findChild(list, "createPlaylistRow");
        const bottom = create.mapToItem(list, 0, create.height).y;
        verify(bottom <= list.height + 1, "the row to create a playlist is in view: " + bottom + " of " + list.height);
        for (let i = 0; i < 1000; ++i) {
            list.edgeScrollStep(-1);
        }
        fuzzyCompare(list.contentY, list.edgeScrollTop, 0.5);
        compare(up.enabled, false);
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
        // "All tracks", the first row, takes files as well, for no
        // playlist: asked of the row itself, as its drop area asks.
        let allTracks = null;
        (function walk(item) {
            if (allTracks === null && item.acceptsFileDrop !== undefined && item.index === 0) {
                allTracks = item;
            }
            for (let i = 0; allTracks === null && i < item.children.length; ++i) {
                walk(item.children[i]);
            }
        })(list.contentItem);
        verify(allTracks !== null, "the list has its first row");
        compare(allTracks.name, "All tracks");
        compare(allTracks.acceptsFileDrop, true);
        allTracks.filesDropped(testCase.crate);
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

    // More files for the playlist an import that waits will make: asked
    // of the controller as for that new playlist, which joins the two.
    function test_aDropOnTheImportsOwnNewPlaylistJoinsIt() {
        const page = makePage(true);
        page.trackImportController.stagedPlaylist = "Fresh";
        page.trackImportController.stagedCount = 2;
        page.trackImportController.stagedPlaylistIsNew = true;
        verify(page.newPlaylistNames.indexOf("Fresh") >= 0, "the import's new playlist is listed as new");
        page.importFiles(testCase.twoFiles, "Fresh", false, 3);
        compare(findChild(page, "playlistEditMessage").opened, false);
        compare(JSON.stringify(page.trackImportController.calls),
                JSON.stringify([["import"].concat(testCase.stick, [testCase.twoFiles, "Fresh", true, -1])]));
    }

    // While an offer is prepared a strip under the playlists says how
    // far it is; it opens to the steps and the file in hand, and Cancel
    // there stops the preparing. Idle, it takes no room.
    function test_theProgressStripShowsThePreparing() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const strip = findChild(page, "importProgress");
        const list = findChild(page, "playlistList");
        compare(strip.visible, false);
        const fullHeight = list.height;
        fake.progressStep = "files";
        fake.progressDone = 11;
        fake.progressTotal = 40;
        fake.progressFile = "b two.wav";
        fake.progressFiles = ["b two.wav"];
        fake.busy = true;
        tryCompare(strip, "visible", true);
        compare(findChild(strip, "passiveProgressSummary").text, "Import: reading file 12 of 40");
        const bar = findChild(strip, "passiveProgressBar");
        compare(bar.indeterminate, false);
        fuzzyCompare(bar.value, 11 / 40, 0.0001);
        tryVerify(() => list.height < fullHeight, 1000, "the list makes room for the strip");
        compare(findChild(strip, "passiveProgressCancel").visible, false);
        compare(findChild(strip, "passiveProgressDetail").visible, false);
        const closedHeight = strip.height;
        mouseClick(strip, 20, 8);
        compare(strip.expanded, true);
        // Open, it names the file in hand and counts the step.
        compare(strip.activity.join("|"), "Reading b two.wav");
        compare(findChild(strip, "passiveProgressDetail").visible, false);
        compare(strip.steps.map(s => s.count).join("|"), "|11 of 40 done||");
        compare(findChild(strip, "passiveProgressNote").text, "Nothing is written until you save.");
        tryVerify(() => strip.height > closedHeight * 4, 1000, "the open panel takes room to read");
        const sidebar = findChild(page, "playlistSidebar");
        verify(strip.width > sidebar.width + 100, "open, it is wider than the sidebar: " + strip.width);
        compare(strip.steps.map(s => s.state).join(" "), "done now later later");
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/browse_import_progress.png");
        }
        const cancel = findChild(strip, "passiveProgressCancel");
        compare(cancel.visible, true);
        mouseClick(cancel);
        compare(JSON.stringify(fake.calls), JSON.stringify([["cancel"]]));
        fake.progressStep = "check";
        fake.progressTotal = 0;
        compare(bar.indeterminate, true);
        compare(findChild(strip, "passiveProgressSummary").text, "Import: checking against the stick");
        // The analysis, the last step, counts its tracks.
        fake.progressStep = "analysis";
        fake.progressDone = 2;
        fake.progressTotal = 5;
        fake.progressFile = "c three.wav";
        fake.progressFiles = ["d four.wav", "e five.wav"];
        compare(findChild(strip, "passiveProgressSummary").text, "Import: analysing track 3 of 5");
        // Two are analysed at once: both named, and the one last done.
        compare(strip.activity.join("|"), "Analysing d four.wav|Analysing e five.wav");
        compare(findChild(strip, "passiveProgressDetail").text, "Last done: c three.wav");
        compare(findChild(strip, "passiveProgressDetail").visible, true);
        compare(strip.steps.map(s => s.state).join(" "), "done done done now");
        compare(bar.indeterminate, false);
        fake.busy = false;
        tryCompare(strip, "visible", false);
        compare(strip.expanded, false);
        compare(strip.width, findChild(page, "playlistSidebar").width);
        tryCompare(list, "height", fullHeight);
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
        verify(dialog.detailText.indexOf("no beat grid or waveform yet") > 0, dialog.detailText);
        // The files left out are counted; which ones, on request.
        const lines = findChild(page, "importNotImportedList");
        const scroll = findChild(page, "importNotImportedScroll");
        compare(findChild(page, "importNotImportedHeading").visible, true);
        compare(findChild(page, "importNotImportedHeading").text, "2 file(s) left out.");
        compare(scroll.visible, false);
        // The dialog comes in with a movement: the click waits for it.
        const toggle = findChild(page, "importNotImportedToggle");
        tryVerify(() => dialog.opacity === 1 && dialog.scale === 1);
        waitForRendering(page);
        mouseClick(toggle);
        tryCompare(scroll, "visible", true);
        compare(lines.count, 2);
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

        // Tracks the stick already has, on their way into the playlist:
        // said beside the new ones, and alone when there are no new ones.
        fake.lastResult = {staged: 1, addedToPlaylist: 2, analysed: 1, withGrid: 1, followedGrids: ["live.flac"],
                           megabytes: 3, playlist: "Peak",
                           playlistIsNew: false, note: "",
                           notImported: [], possibleDuplicates: 0};
        fake.resultReady();
        tryCompare(dialog, "opened", true);
        verify(dialog.detailText.indexOf("2 more are on the stick already") > 0, dialog.detailText);
        verify(dialog.detailText.indexOf("each has a beat grid and waveforms") > 0, dialog.detailText);
        // Counted, not named: a playlist of hundreds has dozens of them.
        verify(dialog.detailText.indexOf("The tempo of 1 of them moves") > 0, dialog.detailText);
        verify(dialog.detailText.indexOf("live.flac") < 0, dialog.detailText);
        dialog.close();
        tryCompare(dialog, "visible", false);
        fake.lastResult = {staged: 0, addedToPlaylist: 2, megabytes: 0, playlist: "Peak", playlistIsNew: false, note: "",
                           notImported: [], possibleDuplicates: 0};
        fake.resultReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "2 track(s) of the stick ready to go into \"Peak\".");
        compare(findChild(page, "importTakeBackButton").visible, true);
        dialog.close();
        tryCompare(dialog, "visible", false);

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

    // Analyse again: offered beside the import for a playlist on the stick,
    // not for one the save is still to make, and not while something runs.
    function test_thePlaylistMenuAnalysesAgain() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const menu = findChild(page, "playlistMenu");
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        const item = findChild(page, "playlistAnalyseAgainItem");
        compare(item.visible, true);
        compare(item.enabled, true);
        compare(item.text, "Analyse again");
        compare(findChild(page, "takeBackReanalysisItem").visible, false);
        item.triggered();
        compare(JSON.stringify(fake.calls), JSON.stringify([["analyseAgain"].concat(testCase.stick, ["Peak"])]));
        menu.close();
        tryCompare(menu, "visible", false);
        // While an import or an analysis runs it waits.
        fake.busy = true;
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        compare(item.visible, true);
        compare(item.enabled, false);
        menu.close();
        tryCompare(menu, "visible", false);
        fake.busy = false;
        // A playlist the save is still to make has no tracks on the stick.
        const edits = createTemporaryObject(fakePlaylistEditComponent, testCase);
        edits.newPlaylists = ["Fresh"];
        page.playlistEditController = edits;
        page.openPlaylistMenu("Fresh", page);
        tryCompare(menu, "opened", true);
        compare(item.visible, false);
        compare(findChild(page, "importTracksItem").visible, false);
        menu.close();
        tryCompare(menu, "visible", false);
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        compare(item.visible, true);
        menu.close();
    }

    // Experimental off: no Analyse again either.
    function test_withoutExperimentalNoAnalyseAgain() {
        const page = makePage(false);
        page.openPlaylistMenu("Peak", page);
        const menu = findChild(page, "playlistMenu");
        tryCompare(menu, "opened", true);
        compare(findChild(page, "playlistAnalyseAgainItem").visible, false);
        menu.close();
    }

    // While it runs the import's panel shows it, with its own two steps.
    function test_theProgressStripShowsAnalyseAgain() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const strip = findChild(page, "importProgress");
        fake.reanalysingPlaylist = "Peak";
        fake.progressStep = "stick";
        fake.busy = true;
        tryCompare(strip, "visible", true);
        compare(findChild(strip, "passiveProgressSummary").text, "Analysing the tracks of \"Peak\" again");
        compare(strip.steps.map(s => s.label).join("|"), "Read the stick|Analyse the tracks");
        compare(strip.steps.map(s => s.state).join(" "), "now later");
        fake.progressStep = "analysis";
        fake.progressDone = 1;
        fake.progressTotal = 4;
        fake.progressFiles = ["b two.wav"];
        compare(findChild(strip, "passiveProgressSummary").text, "Analysing the tracks of \"Peak\" again: track 2 of 4");
        compare(strip.activity.join("|"), "Analysing b two.wav");
        compare(strip.steps.map(s => s.state).join(" "), "done now");
        compare(strip.steps.map(s => s.count).join("|"), "|1 of 4 done");
        fake.busy = false;
        fake.reanalysingPlaylist = "";
        tryCompare(strip, "visible", false);
    }

    // The result: counts only, the note about Save, and Take back; the
    // playlist's menu takes them back too.
    function test_theAnalyseAgainResult() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const dialog = findChild(page, "reanalysisResultDialog");
        fake.lastReanalysis = {playlist: "Peak", analysed: 5, leftAlone: 2};
        fake.reanalysed = {"Peak": 5};
        fake.pendingRevision++;
        fake.reanalysisReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "5 track(s) analysed again.");
        compare(dialog.detailText, "2 track(s) left as they are. Nothing is written until you save.");
        compare(findChild(page, "reanalysisTakeBackButton").visible, true);
        findChild(page, "reanalysisTakeBackButton").clicked();
        tryCompare(dialog, "visible", false);
        compare(JSON.stringify(fake.calls), JSON.stringify([["dropReanalysis", "Peak"]]));

        // None left alone: not said.
        fake.lastReanalysis = {playlist: "Peak", analysed: 3, leftAlone: 0};
        fake.reanalysisReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.detailText, "Nothing is written until you save.");
        dialog.close();
        tryCompare(dialog, "visible", false);

        // Nothing analysed: said, no Take back.
        fake.lastReanalysis = {playlist: "Peak", analysed: 0, leftAlone: 4};
        fake.reanalysisReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "No track of \"Peak\" could be analysed again.");
        compare(dialog.detailText, "4 track(s) left as they are.");
        compare(findChild(page, "reanalysisTakeBackButton").visible, false);
        dialog.close();
        tryCompare(dialog, "visible", false);

        fake.reanalysed = {"Peak": 3};
        fake.pendingRevision++;
        page.openPlaylistMenu("Peak", page);
        const menu = findChild(page, "playlistMenu");
        tryCompare(menu, "opened", true);
        const item = findChild(page, "takeBackReanalysisItem");
        compare(item.visible, true);
        compare(item.text, "Don't analyse the 3 track(s) again");
        item.triggered();
        compare(JSON.stringify(fake.calls.slice(-1)), JSON.stringify([["dropReanalysis", "Peak"]]));
        menu.close();
    }

    // The cues on the old grid: the result counts them and offers the
    // review; the review lists the track, its cue with both times, a strip
    // with both grids and both places, and the switches reach the
    // controller.
    function test_theCueReview() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const strip = {spanMs: 2000, columns: [{low: 0.2, mid: 0.1, high: 0.05}, {low: 0.9, mid: 0.3, high: 0.1}],
                       oldBeats: [200, 700, 1200, 1700], newBeats: [240, 740, 1240, 1740],
                       oldCueMs: 700, newCueMs: 740};
        fake.cueReview = [{id: "addcue:reanalyse-track:/contents/a.wav", title: "Opener, Somebody", gridShiftMs: 40,
                           oldBpm: 120, newBpm: 120.5, moveCues: false, largestMoveMs: 40.4, offBeat: 2,
                           rows: [{label: "Hot cue 1", name: "drop", color: "#00FF00", isLoop: false,
                                   oldText: "0:04.247", newText: "0:04.287", diffMs: 40, strip: strip}]}];
        fake.cueReviewTooDifferent = ["Closer, Somebody Else"];
        fake.lastReanalysis = {playlist: "Peak", analysed: 2, leftAlone: 0, cueTracks: 1, cuesToMove: 1,
                               autoCueTracks: 0, autoCues: 0, askedCueTracks: 1, askedCues: 1,
                               cueFollowThresholdMs: 10, gridChangedTooMuch: 1};
        fake.reanalysisReady();
        const result = findChild(page, "reanalysisResultDialog");
        tryCompare(result, "opened", true);
        verify(result.detailText.indexOf("On 1 track(s) cue points or loops would move by 10 ms or more. They stay "
                                         + "where they are unless you choose to move them (cue points: 1).") >= 0,
               result.detailText);
        verify(result.detailText.indexOf("On 1 track(s) the grid changed too much to carry cue points") >= 0,
               result.detailText);
        const reviewButton = findChild(page, "reanalysisReviewCuesButton");
        compare(reviewButton.visible, true);
        reviewButton.clicked();
        const review = findChild(page, "cueReviewDialog");
        tryCompare(review, "opened", true);
        tryVerify(() => review.opacity === 1 && review.scale === 1);
        // The track's section, made by the review's Repeater.
        const tracks = findChild(page, "cueReviewTracks");
        compare(tracks.count, 1);
        const section = tracks.itemAt(0);
        compare(findChild(section, "cueReviewTrackTitle").text, "Opener, Somebody");
        compare(findChild(section, "cueReviewTrackShift").text,
                "The cue points move by up to 40 ms. Tempo 120.00 BPM before, 120.50 BPM now.");
        // No track under the threshold: no such section; the asked ones
        // are headed by the size of their moves.
        compare(findChild(page, "cueReviewOwnHeading").visible, false);
        compare(findChild(page, "cueReviewAskedHeading").visible, true);
        compare(findChild(page, "cueReviewAskedHeading").text, "Moves of 10 ms or more: 1 track(s)");
        compare(findChild(section, "cueReviewRowLabel").text, "Hot cue 1, drop");
        compare(findChild(section, "cueReviewRowTimes").text, "0:04.247 now, 0:04.287 on the new grid (+40 ms)");
        compare(findChild(section, "cueReviewOffBeat").text, "2 cue point(s) not on a beat stay where they are.");
        compare(findChild(page, "cueReviewTooDifferentList").text, "Closer, Somebody Else");
        const cueStrip = findChild(section, "cueReviewStrip");
        verify(cueStrip.width > 100, "the strip has room: " + cueStrip.width);
        // Both grids and both places, in order and inside the strip.
        for (const beats of [cueStrip.oldBeats, cueStrip.newBeats]) {
            compare(beats.length, 4);
            for (let i = 0; i < beats.length; ++i) {
                verify(beats[i] >= 0 && beats[i] <= cueStrip.spanMs);
                verify(i === 0 || beats[i] > beats[i - 1]);
            }
        }
        compare(cueStrip.oldCueMs, 700);
        compare(cueStrip.newCueMs, 740);
        verify(cueStrip.xOf(cueStrip.oldCueMs) < cueStrip.xOf(cueStrip.newCueMs));
        verify(cueStrip.xOf(cueStrip.newCueMs) <= cueStrip.width);
        // Old and new are said in words beside their markers, old first.
        const oldLabel = findChild(cueStrip, "cueStripOldLabel");
        const newLabel = findChild(cueStrip, "cueStripNewLabel");
        compare(oldLabel.text, "old");
        compare(newLabel.text, "new");
        verify(oldLabel.x < newLabel.x);
        compare(findChild(cueStrip, "cueStripWaveform").waveformData.length, 2);
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/browse_cue_review.png");
        }
        // Off by default; the switch and the two buttons reach the
        // controller.
        const moveSwitch = findChild(section, "moveCuesSwitch");
        compare(moveSwitch.checked, false);
        mouseClick(moveSwitch);
        findChild(page, "cueReviewAllOn").clicked();
        findChild(page, "cueReviewAllOff").clicked();
        compare(JSON.stringify(fake.calls), JSON.stringify([["moveCues", "addcue:reanalyse-track:/contents/a.wav", true],
                                                            ["moveCuesForAll", true], ["moveCuesForAll", false]]));
        review.close();
        tryCompare(review, "visible", false);

        // Nothing on the old grid: no review offered, nothing said.
        fake.lastReanalysis = {playlist: "Peak", analysed: 2, leftAlone: 0, cueTracks: 0, cuesToMove: 0,
                               gridChangedTooMuch: 0};
        fake.reanalysisReady();
        tryCompare(result, "opened", true);
        compare(reviewButton.visible, false);
        compare(result.detailText, "Nothing is written until you save.");
        result.close();
    }

    // A switch turned, or all of them: only the switches follow; the list,
    // its pictures and where it is scrolled to stay as they are.
    function test_aCueChoiceKeepsTheReviewWhereItIs() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const strip = {spanMs: 2000, columns: [{low: 0.5, mid: 0.2, high: 0.1}], oldBeats: [200, 700],
                       newBeats: [240, 740], oldCueMs: 700, newCueMs: 740};
        const tracks = [];
        for (let t = 0; t < 6; ++t) {
            tracks.push({id: "track" + t, title: "Track " + t, gridShiftMs: 40, oldBpm: 120, newBpm: 120,
                         moveCues: false, largestMoveMs: 40, offBeat: 0,
                         rows: [{label: "Hot cue 1", name: "", color: "", isLoop: false, oldText: "0:01.000",
                                 newText: "0:01.040", diffMs: 40, strip: strip}]});
        }
        fake.cueReview = tracks;
        const review = findChild(page, "cueReviewDialog");
        review.open();
        tryCompare(review, "opened", true);
        tryVerify(() => review.opacity === 1 && review.scale === 1);
        const scroll = findChild(page, "cueReviewScroll");
        const flick = scroll.contentItem;
        tryVerify(() => flick.contentHeight > flick.height + 100, 2000, "the review scrolls");
        flick.contentY = 120;
        const repeater = findChild(page, "cueReviewTracks");
        const sections = [];
        for (let t = 0; t < 6; ++t) {
            sections.push(repeater.itemAt(t));
        }
        const firstStrip = findChild(sections[0], "cueReviewStrip");
        const switchOf = t => findChild(sections[t], "moveCuesSwitch");
        // Turned by hand (the keyboard: it may sit outside the scrolled
        // view), as onToggled sends it on.
        switchOf(2).forceActiveFocus();
        keyClick(Qt.Key_Space);
        compare(JSON.stringify(fake.calls), JSON.stringify([["moveCues", "track2", true]]));
        compare(switchOf(2).checked, true);
        compare(switchOf(1).checked, false);
        fake.setMoveCuesForAll(true);
        for (let t = 0; t < 6; ++t) {
            compare(switchOf(t).checked, true);
        }
        fake.setMoveCuesForAll(false);
        compare(switchOf(4).checked, false);
        compare(flick.contentY, 120);
        for (let t = 0; t < 6; ++t) {
            verify(repeater.itemAt(t) === sections[t], "section " + t + " not made again");
        }
        verify(findChild(sections[0], "cueReviewStrip") === firstStrip, "the picture not made again");
        review.close();
    }

    // The threshold: the result says apart what follows on its own and
    // what waits for a choice; the review asks first, then holds the
    // tracks under the threshold in a section closed until opened, their
    // switches on; "Apply to all" sends the spin box's value, and the
    // spin box alone sends nothing.
    function test_theCueThreshold() {
        const page = makePage(true);
        const fake = page.trackImportController;
        const result = findChild(page, "reanalysisResultDialog");
        const reviewButton = findChild(page, "reanalysisReviewCuesButton");
        const ownSentence = "3 cue point(s) or loop(s) will follow the new grid on their own when you save, as they "
            + "move by less than 10 ms (tracks: 2).";
        const askedSentence = "On 1 track(s) cue points or loops would move by 10 ms or more. They stay where they "
            + "are unless you choose to move them (cue points: 2).";
        const showResult = outcome => {
            fake.lastReanalysis = outcome;
            fake.reanalysisReady();
            tryCompare(result, "opened", true);
        };
        const closeResult = () => {
            result.close();
            tryCompare(result, "visible", false);
        };
        // Only tracks under the threshold: said, and the review offered all
        // the same, to look and switch off.
        showResult({playlist: "Peak", analysed: 2, leftAlone: 0, cueTracks: 2, cuesToMove: 3, autoCueTracks: 2,
                    autoCues: 3, askedCueTracks: 0, askedCues: 0, cueFollowThresholdMs: 10, gridChangedTooMuch: 0});
        compare(result.detailText, "Nothing is written until you save.\n\n" + ownSentence);
        compare(reviewButton.visible, true);
        closeResult();
        // Only tracks over it.
        showResult({playlist: "Peak", analysed: 1, leftAlone: 0, cueTracks: 1, cuesToMove: 2, autoCueTracks: 0,
                    autoCues: 0, askedCueTracks: 1, askedCues: 2, cueFollowThresholdMs: 10, gridChangedTooMuch: 0});
        compare(result.detailText, "Nothing is written until you save.\n\n" + askedSentence);
        compare(reviewButton.visible, true);
        closeResult();
        // Both, each its own sentence, and the grid that changed too much.
        showResult({playlist: "Peak", analysed: 4, leftAlone: 0, cueTracks: 3, cuesToMove: 5, autoCueTracks: 2,
                    autoCues: 3, askedCueTracks: 1, askedCues: 2, cueFollowThresholdMs: 10, gridChangedTooMuch: 1});
        compare(result.detailText, "Nothing is written until you save.\n\n" + ownSentence + "\n\n" + askedSentence
                + "\n\nOn 1 track(s) the grid changed too much to carry cue points; they stay where they are.");
        closeResult();
        // A threshold of 0 asks about every track, and says no threshold.
        showResult({playlist: "Peak", analysed: 1, leftAlone: 0, cueTracks: 1, cuesToMove: 2, autoCueTracks: 0,
                    autoCues: 0, askedCueTracks: 1, askedCues: 2, cueFollowThresholdMs: 0, gridChangedTooMuch: 0});
        compare(result.detailText, "Nothing is written until you save.\n\nOn 1 track(s) cue points or loops sat on a "
                + "beat of the old grid, which the new one has elsewhere. They stay where they are unless you choose "
                + "to move them (cue points: 2).");
        closeResult();

        const strip = {spanMs: 2000, columns: [{low: 0.5, mid: 0.2, high: 0.1}, {low: 0.9, mid: 0.3, high: 0.1}],
                       oldBeats: [200, 700, 1200, 1700], newBeats: [205, 705, 1205, 1705], oldCueMs: 700, newCueMs: 705};
        const track = (id, title, largest, on) => ({
            id: id, title: title, gridShiftMs: Math.round(largest), oldBpm: 120, newBpm: 120, moveCues: on,
            largestMoveMs: largest, offBeat: 0,
            rows: [{label: "Hot cue 1", name: "", color: "#00FF00", isLoop: false, oldText: "0:01.000",
                    newText: "0:01.005", diffMs: Math.round(largest), strip: strip}]});
        fake.cueReview = [track("far", "Far, Somebody", 40.2, false), track("near", "Near, Somebody", 4.6, true),
                          track("edge", "Edge, Somebody", 9.9, true)];
        fake.calls = [];
        const review = findChild(page, "cueReviewDialog");
        review.open();
        tryCompare(review, "opened", true);
        tryVerify(() => review.opacity === 1 && review.scale === 1);
        // The dialog gives Done the focus once open; a key before that
        // would land there.
        tryVerify(() => findChild(page, "cueReviewDoneButton").activeFocus);
        // and once more when the footer is first laid out.
        waitForRendering(page);
        // Asked first: the track over the threshold.
        const asked = findChild(page, "cueReviewTracks");
        compare(asked.count, 1);
        compare(findChild(asked.itemAt(0), "cueReviewTrackTitle").text, "Far, Somebody");
        compare(findChild(asked.itemAt(0), "moveCuesSwitch").checked, false);
        // Then the tracks under it, closed, counted in the heading.
        const heading = findChild(page, "cueReviewOwnHeading");
        const own = findChild(page, "cueReviewOwnTracks");
        compare(heading.visible, true);
        compare(heading.text, "Moves of less than 10 ms: 2 track(s)");
        compare(findChild(page, "cueReviewAskedHeading").text, "Moves of 10 ms or more: 1 track(s)");
        const keptLine = findChild(page, "cueReviewKeptByHand");
        compare(keptLine.visible, false);
        compare(own.count, 0);
        // A choice made while it is closed is what its switch shows once
        // opened, not the list as it was made.
        fake.setMoveCues("edge", false);
        fake.calls = [];
        // By the keyboard: below the first track it may sit outside the
        // scrolled view.
        heading.forceActiveFocus();
        wait(50);
        verify(heading.activeFocus, "the heading keeps the focus");
        keyClick(Qt.Key_Space);
        tryCompare(own, "count", 2);
        compare(findChild(own.itemAt(0), "cueReviewTrackTitle").text, "Near, Somebody");
        compare(findChild(own.itemAt(0), "moveCuesSwitch").checked, true);
        compare(findChild(own.itemAt(1), "moveCuesSwitch").checked, false);
        verify(findChild(own.itemAt(1), "cueReviewStrip") !== null, "its cue has its picture");
        // The largest move, rounded down: 9.9 is under 10 and says so.
        compare(findChild(own.itemAt(1), "cueReviewTrackShift").text, "The cue points move by up to 9 ms.");
        waitForRendering(page);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/browse_cue_review_threshold.png");
        }
        // The spin box alone changes nothing; Apply to all sends its value.
        const spin = findChild(page, "cueReviewThresholdSpin");
        compare(spin.value, 10);
        compare(spin.displayText, "10 ms");
        compare(spin.from, 0);
        compare(spin.to, 100);
        spin.forceActiveFocus();
        for (let i = 0; i < 5; ++i) {
            keyClick(Qt.Key_Up);
        }
        compare(spin.value, 15);
        compare(JSON.stringify(fake.calls), JSON.stringify([]));
        spin.value = 50;
        findChild(page, "cueReviewApplyThreshold").clicked();
        compare(JSON.stringify(fake.calls), JSON.stringify([["applyCueThreshold", 50]]));
        // Every track is under 50 now: all in the section, under a heading
        // that says the size of their moves. Those the threshold decides
        // are on; the one switched off by hand stays off, and a line
        // under the buttons says one track kept its choice.
        tryCompare(asked, "count", 0);
        compare(findChild(page, "cueReviewAskedHeading").visible, false);
        compare(heading.text, "Moves of less than 50 ms: 3 track(s)");
        tryCompare(own, "count", 3);
        const ownSwitch = title => {
            for (let t = 0; t < own.count; ++t) {
                if (findChild(own.itemAt(t), "cueReviewTrackTitle").text === title) {
                    return findChild(own.itemAt(t), "moveCuesSwitch");
                }
            }
            return null;
        };
        compare(ownSwitch("Far, Somebody").checked, true);
        compare(ownSwitch("Near, Somebody").checked, true);
        compare(ownSwitch("Edge, Somebody").checked, false);
        compare(keptLine.visible, true);
        compare(keptLine.text, "1 track(s) kept the choice you made by hand.");
        verify(waitForPolish(page.Window.window));
        verify(keptLine.width > 0 && keptLine.height > 0, "the line has room: " + keptLine.width + "x" + keptLine.height);
        const applyButton = findChild(page, "cueReviewApplyThreshold");
        verify(keptLine.mapToItem(null, 0, 0).y >= applyButton.mapToItem(null, 0, applyButton.height).y,
               "the line sits under the buttons");
        // A choice made in the section stays the section's: the switch
        // turns, the list is not made again.
        const first = own.itemAt(0);
        findChild(first, "moveCuesSwitch").forceActiveFocus();
        keyClick(Qt.Key_Space);
        compare(JSON.stringify(fake.calls.slice(-1)), JSON.stringify([["moveCues", "far", false]]));
        verify(own.itemAt(0) === first, "the section not made again");
        compare(findChild(first, "moveCuesSwitch").checked, false);
        // Move all is the person's choice for every track: the line goes.
        findChild(page, "cueReviewAllOn").clicked();
        compare(keptLine.visible, false);
        compare(ownSwitch("Edge, Somebody").checked, true);
        // While a save writes, no switch or button can be used.
        fake.saveWriting = true;
        for (const name of ["cueReviewApplyThreshold", "cueReviewAllOn", "cueReviewAllOff"]) {
            compare(findChild(page, name).enabled, false, name);
        }
        for (let t = 0; t < own.count; ++t) {
            compare(findChild(own.itemAt(t), "moveCuesSwitch").enabled, false);
        }
        fake.saveWriting = false;
        compare(findChild(first, "moveCuesSwitch").enabled, true);
        compare(applyButton.enabled, true);
        review.close();
        tryCompare(review, "visible", false);
    }
}
