// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse Library's header, against the real controllers the page builds
// itself. No stick: nothing is scanned.
TestCase {
    id: testCase
    name: "ScanPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        ScanPage {}
    }

    // Matching graduated on 2026-09-17, so its sidebar pane is the layout
    // every run gets, whatever the experimental setting says.

    function makePage() {
        var page = createTemporaryObject(pageComponent, testCase, {
            width: 880, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER", enginePath: "/nonexistent/TESTSTICK/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    // ---- Playlist editing (Experimental): a stand-in controller records
    // what the menus stage. ----

    Component {
        id: fakePlaylistEditComponent
        QtObject {
            property int pendingRevision: 0
            property string errorMessage: ""
            property var staged: ({})
            property var calls: []
            function deletePlaylist(rekordboxPath, enginePath, playlist) {
                calls.push(["delete", rekordboxPath, enginePath, playlist]);
                staged["p:" + playlist] = true;
                pendingRevision++;
                return true;
            }
            function removeFromPlaylist(rekordboxPath, enginePath, playlist, filePath, title) {
                calls.push(["remove", playlist, filePath, title]);
                staged["r:" + playlist + "|" + filePath] = true;
                pendingRevision++;
                return true;
            }
            function keepPlaylist(playlist) {
                calls.push(["keep", playlist]);
                delete staged["p:" + playlist];
                pendingRevision++;
            }
            function keepInPlaylist(playlist, filePath) {
                calls.push(["keepIn", playlist, filePath]);
                delete staged["r:" + playlist + "|" + filePath];
                pendingRevision++;
            }
            function isPlaylistStaged(playlist) { return staged["p:" + playlist] === true; }
            function isRemovalStaged(playlist, filePath) { return staged["r:" + playlist + "|" + filePath] === true; }
        }
    }

    function makeEditingPage(experimental) {
        verify(realAppSettings.experimentalBuildSupported, "an experimental build");
        realAppSettings.experimentalFeaturesEnabled = experimental;
        const page = makePage();
        page.playlistEditController = createTemporaryObject(fakePlaylistEditComponent, testCase);
        return page;
    }

    function cleanupExperimental() {
        realAppSettings.experimentalFeaturesEnabled = false;
    }

    function test_withoutExperimentalNeitherMenuOpens() {
        const page = makeEditingPage(false);
        page.shownPlaylist = "Peak";
        page.openPlaylistMenu("Peak", page);
        page.openTrackMenu("/nonexistent/a.mp3", "A", page);
        compare(findChild(page, "playlistMenu").opened, false);
        compare(findChild(page, "trackMenu").opened, false);
        compare(page.playlistEditController.calls.length, 0);
        cleanupExperimental();
    }

    // Delete asks first, in the page; only OK stages, and the menu then
    // offers Keep, which takes it back.
    function test_deletingAPlaylistAsksThenStagesAndKeepTakesItBack() {
        const page = makeEditingPage(true);
        const menu = findChild(page, "playlistMenu");
        const dialog = findChild(page, "deletePlaylistDialog");
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        const item = findChild(page, "deletePlaylistItem");
        compare(item.text, "Delete playlist...");
        item.triggered();
        tryCompare(dialog, "opened", true);
        compare(page.playlistEditController.calls.length, 0, "nothing staged before OK");
        dialog.accept();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["delete", "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library", "Peak"]));
        menu.close();
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        compare(item.text, "Keep this playlist");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[1]), JSON.stringify(["keep", "Peak"]));
        cleanupExperimental();
    }

    // Remove from the playlist shown, and Keep; with "All tracks" shown
    // there is no playlist to take a track out of, so no menu.
    function test_removingATrackFromThePlaylistShownStagesAndKeeps() {
        const page = makeEditingPage(true);
        const menu = findChild(page, "trackMenu");
        page.shownPlaylist = "";
        page.openTrackMenu("/nonexistent/a.mp3", "A Track", page);
        compare(menu.opened, false, "no playlist shown, no menu");
        page.shownPlaylist = "Peak";
        page.openTrackMenu("/nonexistent/a.mp3", "A Track", page);
        tryCompare(menu, "opened", true);
        const item = findChild(page, "removeFromPlaylistItem");
        compare(item.text, "Remove from \"Peak\"");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["remove", "Peak", "/nonexistent/a.mp3", "A Track"]));
        menu.close();
        page.openTrackMenu("/nonexistent/a.mp3", "A Track", page);
        tryCompare(menu, "opened", true);
        compare(item.text, "Keep in \"Peak\"");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[1]), JSON.stringify(["keepIn", "Peak", "/nonexistent/a.mp3"]));
        cleanupExperimental();
    }

    function test_cancellingTheDeleteDialogStagesNothing() {
        const page = makeEditingPage(true);
        page.openPlaylistMenu("Peak", page);
        findChild(page, "deletePlaylistItem").triggered();
        const dialog = findChild(page, "deletePlaylistDialog");
        tryCompare(dialog, "opened", true);
        dialog.reject();
        compare(page.playlistEditController.calls.length, 0);
        cleanupExperimental();
    }

    // The sort combo stands in the same page as the library picker, one
    // row down, and is as tall as it.
    function test_theSortComboIsAsTallAsTheLibraryPicker() {
        const page = makePage();
        const combo = findChild(page, "sortCombo");
        const toggle = findChild(page, "librarySourceToggle");
        verify(combo !== null && toggle !== null);
        verify(toggle.height > 0);
        compare(toggle.height, Theme.compactControlHeight);
        compare(combo.height, toggle.height);
    }

    // One left line: the search field and the list below it -- the
    // playlist column, which is the list's left edge -- start at the same
    // x. The list used to run to the window edge, 16 px left of the search.
    // One left line for the page. Since Matching graduated (2026-09-17) the
    // header row starts with the sidebar's own pill button rather than the
    // search field, and the playlists sidebar -- open by default, where the
    // classic playlist column used to be -- is the list's left edge.
    function test_theHeaderAndTheListShareOneLeftLine() {
        var page = makePage();
        var pill = findChild(page, "playlistSidebarButton");
        var search = findChild(page, "searchField");
        var sidebar = findChild(page, "playlistSidebar");
        verify(pill !== null && search !== null && sidebar !== null);
        compare(sidebar.visible, true, "the playlists sidebar is the list's left edge");
        compare(Math.round(pill.mapToItem(page, 0, 0).x), Math.round(sidebar.mapToItem(page, 0, 0).x));
        compare(Math.round(sidebar.mapToItem(page, 0, 0).x), Theme.pageMargin);
        verify(search.mapToItem(page, 0, 0).x > pill.mapToItem(page, 0, 0).x,
               "the search field follows the pill in the same row");
    }

    // The pane is where the playing track is shown. A track played from
    // the list with the pane shut, or turned to another track, used to
    // play with nothing to show for it.
    function test_playingARowTurnsTheDetailsPaneToIt() {
        var page = makePage();
        var pane = findChild(page, "trackDetailPanel");
        compare(page.trackPanelOpen, false);
        var row = {sourceId: "17", title: "Played From The List", artist: "Someone", filePath: "/nonexistent/a.mp3",
                   artworkPath: "", cues: [], durationSeconds: 300, playlistNames: [], streamingSource: "",
                   rating: -1, bpm: 124, key: "8A", bitrate: 0, playCount: 0, comment: "", album: ""};
        page.playRow(row);
        compare(page.trackPanelOpen, true, "the pane opens");
        compare(pane.trackSourceId, "17", "on the track that was played");
        compare(realPlayback.currentSourceId, "17");
        compare(pane.isLoadedTrack, true, "so the pane knows it is showing the loaded track");
        realPlayback.stop();
    }

    // ---- Browse in two phases, against a held catalog cache
    // (browseFixture): the rekordbox list is up at once and its cue pass
    // waits until the test lets it go. ----

    Component {
        id: controllerComponent
        ScanController {}
    }

    function cleanup() {
        browseFixture.restore();
    }

    function makeHeldPage(trackCount) {
        browseFixture.holdCues(trackCount);
        const page = makePage();
        // The list's full width, so every column is there to look at.
        page.playlistSidebarOpen = false;
        const controller = findChild(page, "scanController");
        verify(controller !== null);
        const list = findChild(page, "trackListView");
        tryCompare(list, "count", trackCount);
        tryCompare(controller, "busy", false);
        tryVerify(() => browseFixture.cuePassWaiting(), 5000, "the cue pass is running, held");
        waitForRendering(page);
        return {page: page, controller: controller, list: list};
    }

    function saveScreenshot(item, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(item).save(screenshotDir + "/" + name + ".png");
        }
    }

    // The list before its cues, and the same rows after: the delegates,
    // the scroll position and the current row all survive the cues
    // landing, because they land as an update, not a new list.
    function test_theListShowsBeforeItsCuesAndUpdatesInPlace() {
        const held = makeHeldPage(60);
        const list = held.list;
        compare(held.controller.cuesPending, true);
        compare(findChild(held.page, "cuesPendingNote").visible, true, "the header says the cues are on their way");
        compare(held.controller.busy, false, "no busy overlay over the list while the cues are read");

        list.positionViewAtIndex(20, ListView.Beginning);
        list.currentIndex = 22;
        waitForRendering(held.page);
        const contentY = list.contentY;
        verify(contentY > 0, "scrolled");
        const row = list.itemAtIndex(22);
        verify(row !== null);
        compare(row.title, "Held Track 1022");
        const cueLabel = findChild(row, "cueCountLabel");
        compare(cueLabel.visible, true);
        compare(cueLabel.text, "", "an unknown cue count is empty, not 0");
        saveScreenshot(held.page, "browse-cues-pending");

        const resets = createTemporaryObject(signalSpyComponent, testCase,
                                             {target: held.controller.tracks, signalName: "modelReset"});
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: held.controller, signalName: "tracksPublished"});
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        compare(published.count, 1);
        compare(published.signalArguments[0][0], true, "published as the cues landing");

        compare(resets.count, 0, "no list reset");
        compare(list.itemAtIndex(22), row, "the same row object");
        compare(list.contentY, contentY, "the same scroll position");
        compare(list.currentIndex, 22, "the current row kept");
        compare(cueLabel.text, "2", "the row's cue count, where it stands");
        compare(findChild(held.page, "cuesPendingNote").visible, false);
        compare(browseFixture.cuePasses(), 1, "one cue pass");
        waitForRendering(held.page);
        saveScreenshot(held.page, "browse-cues-settled");
    }

    // ---- Sorting by the column headers ----

    function header(page, key) {
        const item = findChild(page, "sortHeader_" + key);
        verify(item !== null, "a header for " + key);
        return item;
    }

    function titles(controller) {
        const result = [];
        for (let i = 0; i < controller.tracks.trackCount(); ++i) {
            result.push(controller.tracks.trackAt(i).title);
        }
        return result;
    }

    // The one header that shows an arrow, and which way it points; every
    // other sortable header shows none.
    function compareIndicators(page, activeKey, ascending) {
        // The active arrow is waited for: under a parallel ctest load the
        // click's effect reached the scene a frame late once (the Races
        // session, 2026-09-28) and a synchronous read called it absent.
        // A click that never landed still fails here, after the wait.
        if (activeKey.length > 0) {
            tryCompare(findChild(header(page, activeKey), "sortIndicator"), "visible", true, 5000);
        }
        for (const key of ["title", "key", "bpm", "duration", "cues", "plays"]) {
            const arrow = findChild(header(page, key), "sortIndicator");
            verify(arrow !== null);
            compare(arrow.visible, key === activeKey, "the arrow on " + key);
            if (key === activeKey) {
                compare(arrow.iconName, ascending ? "arrow-up" : "arrow-down");
            }
        }
    }

    // A click on a header sorts the list by its column, ascending; a
    // second click on the same header reverses it.
    function test_aHeaderClickSortsByItsColumn() {
        const held = makeHeldPage(12);
        const scanOrder = titles(held.controller);
        const reversed = scanOrder.slice().reverse();
        const title = header(held.page, "title");
        compare(title.visible, true);
        // Scan order is title order in the fixture; sort the other way
        // first so the ascending click has something to do.
        held.page.sortByColumn("title");
        held.page.sortByColumn("title");
        compare(titles(held.controller), reversed, "descending");

        mouseClick(header(held.page, "duration"));
        compare(held.page.sortField, "duration");
        compare(held.page.sortAscending, true, "a new column sorts ascending");
        compare(titles(held.controller), scanOrder, "longer tracks later");

        mouseClick(header(held.page, "duration"));
        compare(held.page.sortAscending, false, "the sorted column flips");
        compare(titles(held.controller), reversed, "longest first");
        const durations = [];
        for (let i = 0; i < held.controller.tracks.trackCount(); ++i) {
            durations.push(held.controller.tracks.trackAt(i).durationSeconds);
        }
        for (let i = 1; i < durations.length; ++i) {
            verify(durations[i] <= durations[i - 1], "sorted by length, descending, at row " + i);
        }
        compare(findChild(held.page, "sortCombo").currentText, "Duration", "the combo shows the field");
    }

    // The arrow: on the sorted header only, up for ascending and down for
    // descending; the header says so in its tooltip, and takes Space and
    // Enter as a click. Replaces the separate direction button the header
    // row used to carry.
    function test_theSortedHeaderShowsItsDirection() {
        const held = makeHeldPage(8);
        verify(findChild(held.page, "sortDirectionButton") === null, "no direction button any more");
        compareIndicators(held.page, "", true);
        for (const key of ["title", "key", "bpm", "duration", "cues", "plays"]) {
            compare(findChild(header(held.page, key), "sortHeaderLabel").truncated, false,
                    "the " + key + " label whole beside the room for its arrow");
        }

        const cues = header(held.page, "cues");
        compare(cues.toolTipText, "Sort by Cues");
        const widthBefore = cues.implicitWidth;
        waitForRendering(cues);
        mouseClick(cues);
        compareIndicators(held.page, "cues", true);
        compare(cues.toolTipText, "Sorted by Cues, ascending. Click to sort descending");
        compare(cues.implicitWidth, widthBefore, "the header keeps its width when the arrow shows");
        const arrow = findChild(cues, "sortIndicator");
        compare(arrow.color, Theme.text);
        verify(arrow.x + arrow.width <= cues.width + 1, "the arrow inside its column");

        cues.forceActiveFocus();
        keyClick(Qt.Key_Space);
        compareIndicators(held.page, "cues", false);
        compare(cues.toolTipText, "Sorted by Cues, descending. Click to sort ascending");
        keyClick(Qt.Key_Return);
        compareIndicators(held.page, "cues", true);

        header(held.page, "key").forceActiveFocus();
        keyClick(Qt.Key_Enter);
        compareIndicators(held.page, "key", true);
    }

    // The combo and the headers show one sort. A header click moves the
    // combo; a column field picked in the combo lights its header and
    // keeps the direction; Playlist Order and Artist, which have no
    // column, light none and sort ascending.
    function test_theComboAndTheHeadersShowOneSort() {
        const held = makeHeldPage(10);
        const combo = findChild(held.page, "sortCombo");
        compare(combo.currentText, "Playlist Order");

        mouseClick(header(held.page, "bpm"));
        mouseClick(header(held.page, "bpm"));
        compare(combo.currentText, "BPM");
        compareIndicators(held.page, "bpm", false);

        combo.activated(held.page.sortOptions.findIndex(option => option.value === "cues"));
        compare(combo.currentText, "Cues");
        compareIndicators(held.page, "cues", false);

        combo.activated(held.page.sortOptions.findIndex(option => option.value === "artist"));
        compare(combo.currentText, "Artist");
        compareIndicators(held.page, "", true);

        // Title order is scan order in the fixture: back to a column
        // through the combo, the kept direction applies again.
        combo.activated(held.page.sortOptions.findIndex(option => option.value === "title"));
        compareIndicators(held.page, "title", false);
        verify(titles(held.controller)[0] > titles(held.controller)[1], "title, descending");

        combo.activated(held.page.sortOptions.findIndex(option => option.value === "playlist"));
        compareIndicators(held.page, "", true);
        verify(titles(held.controller)[0] < titles(held.controller)[1],
               "playlist order ascending, whatever the headers last showed");
    }

    // The page at its usual width, sorted by Cues, most first.
    function test_screenshot_sortedByCuesDescending() {
        const held = makeHeldPage(40);
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        mouseClick(header(held.page, "cues"));
        mouseClick(header(held.page, "cues"));
        compareIndicators(held.page, "cues", false);
        compare(held.controller.tracks.trackAt(0).cueCount, 4, "most cues first");
        waitForRendering(held.page);
        saveScreenshot(held.page, "browse-sorted");
    }

    // A narrow page drops the numeric columns; the header row that stays
    // keeps inside the page.
    function test_aNarrowPageKeepsTheHeaderRowInside() {
        const held = makeHeldPage(6);
        held.page.width = 420;
        waitForRendering(held.page);
        compare(held.page.browseTier, 1);
        const title = header(held.page, "title");
        compare(title.visible, true);
        for (const key of ["key", "bpm", "duration", "cues", "plays"]) {
            compare(header(held.page, key).visible, false, key + " is dropped");
        }
        const row = title.parent;
        const right = row.mapToItem(held.page, row.width, 0).x;
        verify(right <= held.page.width - Theme.pageMargin, "the header row ends inside the page: " + right);
        mouseClick(title);
        compareIndicators(held.page, "title", true);
        const arrow = findChild(title, "sortIndicator");
        verify(arrow.mapToItem(held.page, arrow.width, 0).x <= held.page.width - Theme.pageMargin,
               "the arrow inside the page");
        saveScreenshot(held.page, "browse-sorted-narrow");
    }

    // Sorting by cues while they are read sorts what is known (nothing
    // yet, so the scan order stays); when they land, the rows move to
    // where the sort puts them rather than the list being rebuilt.
    function test_aSortByCuesMovesTheRowsWhenTheCuesLand() {
        const held = makeHeldPage(30);
        held.controller.setSort("cues", true);
        compare(held.controller.tracks.trackAt(0).sourceId, "1");
        compare(held.controller.tracks.trackAt(1).sourceId, "2", "unknown cues sort as scan order");
        const firstRow = held.list.itemAtIndex(0);

        const resets = createTemporaryObject(signalSpyComponent, testCase,
                                             {target: held.controller.tracks, signalName: "modelReset"});
        const moves = createTemporaryObject(signalSpyComponent, testCase,
                                            {target: held.controller.tracks, signalName: "rowsMoved"});
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        compare(resets.count, 0, "no list reset");
        verify(moves.count > 0, "the rows moved");
        let previous = -1;
        for (let i = 0; i < held.controller.tracks.trackCount(); ++i) {
            const cueCount = held.controller.tracks.trackAt(i).cueCount;
            verify(cueCount >= previous, "sorted by cues at row " + i);
            previous = cueCount;
        }
        compare(held.controller.tracks.trackAt(1).sourceId, "6", "the next track without cues");
        compare(held.list.itemAtIndex(0), firstRow, "the first row, which stayed first, kept its delegate");
    }

    // The details pane holds a copy of its track; the cues reach it too.
    function test_theDetailsPaneGetsTheCuesWhenTheyLand() {
        const held = makeHeldPage(20);
        const pane = findChild(held.page, "trackDetailPanel");
        pane.showFor(held.controller.tracks.trackAt(7));
        held.page.trackPanelOpen = true;
        compare(pane.trackSourceId, "8");
        compare(pane.trackCues.length, 0);
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        compare(pane.trackCues.length, 2, "track 8 has 7 % 5 cues");
    }

    // A track played before its cues were read was loaded with none; when
    // they land the player takes them, without a new track (no reload,
    // playback not interrupted).
    function test_thePlayerTakesTheCuesWhenTheyLand() {
        const held = makeHeldPage(20);
        held.page.playRow(held.controller.tracks.trackAt(7));
        compare(realPlayback.currentSourceId, "8");
        compare(realPlayback.cues.length, 0, "loaded before the cues were read");
        const reloads = createTemporaryObject(signalSpyComponent, testCase,
                                              {target: realPlayback, signalName: "trackChanged"});
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        const rowCues = held.controller.tracks.trackAt(held.controller.tracks.indexOfSourceId("8")).cues;
        compare(rowCues.length, 2, "track 8 has 7 % 5 cues");
        tryVerify(() => realPlayback.cues.length === 2, 5000, "the player took the row's cues");
        for (let i = 0; i < rowCues.length; ++i) {
            compare(realPlayback.cues[i].positionMs, rowCues[i].positionMs);
            compare(realPlayback.cues[i].hotCueNumber, rowCues[i].hotCueNumber);
        }
        compare(reloads.count, 0, "the same track, not loaded again");
        compare(realPlayback.currentSourceId, "8");
        realPlayback.stop();
    }

    // Only the loaded track's own row hands its cues over.
    function test_anotherRowsCuesDoNotReachThePlayer() {
        const held = makeHeldPage(20);
        held.page.playRow(held.controller.tracks.trackAt(5));
        compare(realPlayback.currentSourceId, "6");
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);
        verify(held.controller.tracks.trackAt(held.controller.tracks.indexOfSourceId("8")).cues.length > 0,
               "other rows have cues now");
        wait(0);
        compare(realPlayback.cues.length, 0, "track 6 has 5 % 5 cues, and keeps none");
        realPlayback.stop();
    }

    // takeCues() itself: the format, the library and the id must all be
    // the loaded track's.
    function test_takeCuesOnlyTakesTheLoadedTracksCues() {
        const cues = [{kind: "hot", hotCueNumber: 1, positionMs: 5000, isLoop: false, loopEndMs: 0,
                       color: "#ffcc00", comment: ""}];
        realPlayback.load("rekordbox", "/nonexistent/A/PIONEER", "3", "/nonexistent/a.mp3", "T", "A", "", []);
        verify(!realPlayback.takeCues("rekordbox", "/nonexistent/A/PIONEER", "4", cues), "another id");
        verify(!realPlayback.takeCues("engine", "/nonexistent/A/PIONEER", "3", cues), "another format");
        verify(!realPlayback.takeCues("rekordbox", "/nonexistent/B/PIONEER", "3", cues), "another library");
        compare(realPlayback.cues.length, 0);
        const changed = createTemporaryObject(signalSpyComponent, testCase,
                                              {target: realPlayback, signalName: "cuesChanged"});
        verify(realPlayback.takeCues("rekordbox", "/nonexistent/A/PIONEER", "3", cues));
        compare(realPlayback.cues.length, 1);
        compare(realPlayback.cues[0].positionMs, 5000);
        compare(changed.count, 1);
        realPlayback.stop();
        verify(!realPlayback.takeCues("rekordbox", "/nonexistent/A/PIONEER", "3", cues), "nothing loaded");
    }

    // An Engine row names art the stick does not have; the rekordbox copy
    // of the same song has art, and the row shows that instead.
    function test_anEngineRowFallsBackToTheRekordboxArt() {
        const held = makeHeldPage(5);
        held.page.format = "engine";
        tryCompare(held.controller, "busy", false);
        compare(held.controller.cuesPending, false, "Engine's cues are in its catalog");
        tryVerify(() => !browseFixture.cuePassWaiting(), 5000, "the rekordbox cue pass was let go");
        const row = held.list.itemAtIndex(0);
        verify(row !== null);
        verify(row.artworkPath.indexOf("not-there.jpg") >= 0, "the row names its own art");
        verify(row.fallbackArtworkPath.indexOf("cover.png") >= 0, "and the rekordbox art as its fallback");
        const art = findChild(row, "rowArtwork");
        tryCompare(art, "showing", "fallback");
        compare(findChild(art, "artworkImage").visible, true);
        compare(row.cues.length, 1, "with its own cues from the start");

        // Played, the player carries the fallback too, from the queue.
        held.page.playRow(held.controller.tracks.trackAt(0));
        // Settled at load: the player never carries a cover that is not on
        // disk (MPRIS publishes its artworkPath as a fact), so the missing
        // one gives way to the fallback, which is then the only cover.
        compare(realPlayback.artworkPath, row.fallbackArtworkPath);
        compare(realPlayback.fallbackArtworkPath, "");
        realPlayback.stop();
    }

    // The scan itself, without the page: a rekordbox scan publishes the
    // list, then the cues; one cancelled between the two publishes once,
    // and the cues, when the read lets go, never arrive.
    function test_aScanPublishesTwice() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        controller.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryCompare(published, "count", 1);
        compare(published.signalArguments[0][0], false, "the list first");
        compare(controller.cuesPending, true);
        compare(controller.tracks.trackCount(), 12);
        browseFixture.releaseCues();
        tryCompare(published, "count", 2);
        compare(published.signalArguments[1][0], true, "then its cues");
        compare(controller.cuesPending, false);
        compare(controller.tracks.trackAt(4).cueCount, 4);
    }

    // OneLibrary reads its cues from the analysis files too (#59), so its
    // scan publishes the list with the cues still on their way, then the
    // cues, like rekordbox's. It once published the list as final.
    function test_aOneLibraryScanPublishesItsCuesToo() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        controller.scan("onelibrary", "/nonexistent/HELD/PIONEER");
        tryCompare(published, "count", 1);
        compare(published.signalArguments[0][0], false, "the list first");
        compare(controller.cuesPending, true, "the cues are on their way");
        browseFixture.releaseCues();
        tryCompare(published, "count", 2);
        compare(published.signalArguments[1][0], true, "then its cues");
        compare(controller.cuesPending, false);
        compare(controller.tracks.trackAt(4).cueCount, 4);
    }

    // And a third publish, with no bar and no note: the Full stage's
    // lengths and sizes land in the rows where they stand, after the
    // cues, and it is not a third cue publish.
    function test_theFullStageLandsAfterTheCues() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const details = createTemporaryObject(signalSpyComponent, testCase,
                                              {target: controller, signalName: "detailsPublished"});
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        controller.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryCompare(published, "count", 1);
        compare(details.count, 0, "nothing beyond the list while the cues are held");
        browseFixture.releaseCues();
        tryCompare(published, "count", 2);
        tryCompare(details, "count", 1);
        compare(published.count, 2, "the Full stage is not a cue publish");
        compare(controller.cuesPending, false);
        compare(controller.tracks.trackCount(), 12);
    }

    // An Engine catalog holds its cues, so its scan publishes the list
    // and then the Full stage, with no cue phase between.
    function test_anEngineScanPublishesTheListThenItsDetails() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const details = createTemporaryObject(signalSpyComponent, testCase,
                                              {target: controller, signalName: "detailsPublished"});
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        controller.scan("engine", "/nonexistent/HELD/Engine Library");
        tryCompare(published, "count", 1);
        tryCompare(details, "count", 1);
        compare(published.count, 1, "no cue phase for a catalog that holds its cues");
        compare(controller.cuesPending, false);
        compare(controller.tracks.trackCount(), 12);
    }

    function test_aScanCancelledBetweenThePhasesPublishesOnce() {
        browseFixture.holdCues(12);
        const controller = createTemporaryObject(controllerComponent, testCase);
        const published = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "tracksPublished"});
        const cancelled = createTemporaryObject(signalSpyComponent, testCase,
                                                {target: controller, signalName: "scanCancelled"});
        controller.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryCompare(published, "count", 1);
        tryVerify(() => browseFixture.cuePassWaiting(), 5000, "the cue pass is running, held");
        controller.cancelScan();
        compare(cancelled.count, 1, "cancelled at once, without waiting for the read");
        compare(controller.cuesPending, false);
        // The read notices the cancel and unwinds; everything it reported
        // has been delivered once the pool is idle and events have run.
        tryVerify(() => !browseFixture.cuePassWaiting(), 5000, "the held pass let go");
        browseFixture.waitForScans();
        wait(0);
        compare(published.count, 1, "the cues never land");
        compare(cancelled.count, 1);
        compare(controller.tracks.trackAt(4).cueCount, 0);
        compare(controller.tracks.trackCount(), 12, "the list stays");
    }

    // Leaving the page mid cue pass: the controller goes, and the pass it
    // was running stops rather than reading on for nobody.
    function test_leavingThePageStopsTheCuePass() {
        browseFixture.holdCues(12);
        const controller = controllerComponent.createObject(testCase);
        controller.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryCompare(controller, "cuesPending", true);
        tryVerify(() => browseFixture.cuePassWaiting(), 5000, "the cue pass is running, held");
        controller.destroy();
        tryVerify(() => !browseFixture.cuePassWaiting(), 5000, "the pass stopped without a release");
        browseFixture.waitForScans();
        compare(browseFixture.cuePasses(), 1);
    }

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }
}
