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
    // Wide enough for Browse at the 1000 x 700 the column test renders.
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
            property var newPlaylists: []
            function createPlaylist(rekordboxPath, enginePath, name) {
                calls.push(["create", rekordboxPath, enginePath, name]);
                staged["n:" + name] = true;
                newPlaylists = newPlaylists.concat([name]);
                pendingRevision++;
                return true;
            }
            function dropNewPlaylist(name) {
                calls.push(["dropNew", name]);
                delete staged["n:" + name];
                newPlaylists = newPlaylists.filter(n => n !== name);
                pendingRevision++;
            }
            function isNewPlaylistStaged(name) { return staged["n:" + name] === true; }
            property string lastAddPlaylist: ""
            function addToPlaylist(rekordboxPath, enginePath, playlist, filePath, title) {
                calls.push(["add", rekordboxPath, enginePath, playlist, filePath, title]);
                staged["a:" + playlist + "|" + filePath] = true;
                lastAddPlaylist = playlist;
                pendingRevision++;
                return true;
            }
            function keepOutOfPlaylist(playlist, filePath) {
                calls.push(["keepOut", playlist, filePath]);
                delete staged["a:" + playlist + "|" + filePath];
                pendingRevision++;
            }
            function isAdditionStaged(playlist, filePath) { return staged["a:" + playlist + "|" + filePath] === true; }
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
            function moveInPlaylist(rekordboxPath, enginePath, playlist, filePath, title, toIndex) {
                calls.push(["move", rekordboxPath, enginePath, playlist, filePath, title, toIndex]);
                staged["m:" + playlist + "|" + filePath] = true;
                pendingRevision++;
                return true;
            }
            function keepPositionInPlaylist(playlist, filePath) {
                calls.push(["keepPosition", playlist, filePath]);
                delete staged["m:" + playlist + "|" + filePath];
                pendingRevision++;
            }
            function isMoveStaged(playlist, filePath) { return staged["m:" + playlist + "|" + filePath] === true; }
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

    Component {
        id: fakeRowComponent
        QtObject {
            property string filePath: "/nonexistent/a.mp3"
            property string title: "A Track"
            property string streamingSource: ""
            property bool pendingAddition: false
            property bool pendingMove: false
        }
    }

    Component {
        id: playlistRowComponent
        PlaylistRowDelegate { index: 1; width: 240 }
    }

    // A playlist with tracks staged into or out of it: bold italics, and
    // the count after Save in parentheses; a new one counts its tracks.
    function test_anEditedPlaylistShowsItsNewCount() {
        const row = createTemporaryObject(playlistRowComponent, testCase,
                                          {name: "Peak", count: 12, edited: true, countChange: 1});
        const labels = row.children[1].children;
        compare(labels[0].font.bold, true);
        compare(labels[0].font.italic, true);
        compare(labels[1].text, "12 (13)");
        row.countChange = -2;
        compare(labels[1].text, "12 (10)");
        row.edited = false;
        compare(labels[1].text, "12");
        compare(labels[0].font.bold, false);
        row.pendingNew = true;
        row.countChange = 2;
        compare(labels[1].text, "new (2)");
        row.countChange = 0;
        compare(labels[1].text, "new");
    }

    // A Matching result's menu button opens Browse's row menu for it,
    // Add to included; no "Remove from" for a track not in the playlist
    // shown.
    function test_aMatchingResultHasTheRowMenu() {
        const page = makeEditingPage(true);
        page.playlistEditController.newPlaylists = ["Chill"];
        page.shownPlaylist = "Peak";
        const menu = findChild(page, "trackMenu");
        const matching = findChild(page, "matchingPage");
        verify(matching !== null);
        const candidate = {sourceId: "7", title: "Match", artist: "Someone", key: "8A", bpm: 124,
                           filePath: "/nonexistent/m.mp3", streamingSource: "", artworkPath: "",
                           fallbackArtworkPath: "", playlistNames: ["Other"]};
        matching.menuRequested(candidate, page);
        tryCompare(menu, "opened", true);
        compare(menu.row, candidate);
        compare(findChild(page, "removeFromPlaylistItem").visible, false, "not in Peak");
        const addTo = findChild(page, "addToMenu");
        compare(menu.itemAt(menu.count - 1).subMenu, addTo);
        addTo.itemAt(0).triggered();
        compare(JSON.stringify(page.playlistEditController.calls[0]), JSON.stringify(["add",
                "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library", "Chill",
                "/nonexistent/m.mp3", "Match"]));
        menu.close();
        tryCompare(menu, "visible", false);
        candidate.playlistNames = ["Peak"];
        matching.menuRequested(candidate, page);
        tryCompare(menu, "opened", true);
        compare(findChild(page, "removeFromPlaylistItem").visible, true, "in Peak");
        menu.close();
        cleanupExperimental();
    }

    // A row shown in the playlist only because it is staged to be added:
    // its menu takes the add back instead of offering a removal.
    function test_aTrackStagedIntoThePlaylistShownCanBeTakenBack() {
        const page = makeEditingPage(true);
        const menu = findChild(page, "trackMenu");
        const item = findChild(page, "removeFromPlaylistItem");
        const row = createTemporaryObject(fakeRowComponent, testCase, {pendingAddition: true});
        page.shownPlaylist = "Peak";
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(item.visible, true);
        compare(item.text, "Don't add to \"Peak\"");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["keepOut", "Peak", "/nonexistent/a.mp3"]));
        menu.close();
        cleanupExperimental();
    }

    // A row shown at a place a staged move gives it: its menu offers to
    // keep it at its place, which takes the move back; a row not moved
    // has no such entry.
    function test_aTrackMovedWithinThePlaylistShownCanBeKeptAtItsPlace() {
        const page = makeEditingPage(true);
        const menu = findChild(page, "trackMenu");
        const item = findChild(page, "keepPositionItem");
        page.shownPlaylist = "Peak";
        page.openTrackMenu(createTemporaryObject(fakeRowComponent, testCase), page);
        tryCompare(menu, "opened", true);
        compare(item.visible, false, "not moved");
        menu.close();
        tryCompare(menu, "visible", false);
        const row = createTemporaryObject(fakeRowComponent, testCase, {pendingMove: true});
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(item.visible, true);
        compare(item.text, "Keep at its place in \"Peak\"");
        compare(findChild(page, "removeFromPlaylistItem").text, "Remove from \"Peak\"", "a moved track can still go");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["keepPosition", "Peak", "/nonexistent/a.mp3"]));
        menu.close();
        cleanupExperimental();
    }

    // ---- Drop to reorder: a stand-in drag item with the playlist-dnd
    // contract's keys and properties. ----

    Component {
        id: fakeDragComponent
        Item {
            width: 10
            height: 10
            Drag.keys: ["seabass/track"]
            Drag.hotSpot.x: 5
            Drag.hotSpot.y: 5
            property string sourceId: "3"
            property string filePath: "/nonexistent/d.mp3"
            property string title: "Dragged"
            property string artist: "Someone"
            property double durationSeconds: 300
            property double bpm: 124
            property string key: "8A"
            property string artworkPath: ""
            property string fallbackArtworkPath: ""
            property string streamingSource: ""
            property var playlistNames: ["Peak"]
        }
    }

    // The component alone, fed by a DropArea as ScanPage feeds it: five
    // rows of 50, the dragged track the fourth (index 3).
    Component {
        id: dropHarnessComponent
        Item {
            width: 600
            height: 400
            property alias gap: harnessGap
            property var moves: []
            DropArea {
                id: harnessArea
                anchors.fill: parent
                keys: ["seabass/track"]
                onDropped: (drop) => {
                    if (harnessGap.release()) {
                        drop.accept(Qt.MoveAction);
                    }
                }
            }
            PlaylistDropGap {
                id: harnessGap
                anchors.fill: parent
                draggedTrack: harnessArea.containsDrag ? harnessArea.drag.source : null
                draggedIndex: draggedTrack ? Number(draggedTrack.sourceId) : -1
                playlist: "Peak"
                orderShown: true
                hovering: harnessArea.containsDrag
                pointerY: harnessArea.drag.y
                rowHeight: 50
                rowCount: 5
                hoverDelay: 400
                onMoveRequested: (filePath, title, toIndex) => moves.push([filePath, title, toIndex])
            }
        }
    }

    function dragOver(parent, props, x, y) {
        const drag = createTemporaryObject(fakeDragComponent, parent, props || {});
        drag.x = x - 5;
        drag.y = y - 5;
        drag.Drag.active = true;
        return drag;
    }

    // Held over a gap for the delay, the gap opens: the rows below move
    // down a row and the faded copy says where it goes; it then follows
    // the pointer at once, and a release asks for the move, as the place
    // the track takes afterwards.
    function test_aTrackHeldOverAGapOpensItAndDropsThere() {
        const h = createTemporaryObject(dropHarnessComponent, testCase);
        const ghost = findChild(h.gap, "dropGapGhost");
        const drag = dragOver(h, {}, 100, 52);
        tryCompare(h.gap, "hovering", true);
        compare(h.gap.gapIndex, -1, "not at once");
        wait(150);
        compare(h.gap.gapIndex, -1, "not before the delay");
        compare(ghost.visible, false);
        tryCompare(h.gap, "gapIndex", 1, 2000);
        verify(ghost.visible);
        compare(ghost.y, 50);
        compare(findChild(h.gap, "dropGapLabel").text, "Move track here");
        compare(findChild(h.gap, "dropGapTitle").text, "Dragged");
        compare(h.gap.shiftFor(0), 0);
        compare(h.gap.shiftFor(1), 50, "the rows from the gap on make room");
        compare(h.gap.shiftFor(4), 50);
        waitForRendering(h);
        saveScreenshot(h, "browse_drop_gap");
        // Over the copy it stays; below it, the rows' places count as they
        // were before they moved down.
        drag.y = 80 - 5;
        tryCompare(h.gap, "pointerY", 80);
        compare(h.gap.gapIndex, 1, "over the copy");
        drag.y = 130 - 5;
        tryCompare(h.gap, "pointerY", 130);
        compare(h.gap.gapIndex, 2, "below the copy, a row lower: follows at once once open");
        drag.y = 4 - 5;
        tryCompare(h.gap, "pointerY", 4);
        compare(h.gap.gapIndex, 0, "follows at once once open");
        drag.Drag.drop();
        compare(JSON.stringify(h.moves), JSON.stringify([["/nonexistent/d.mp3", "Dragged", 0]]));
        compare(h.gap.gapIndex, -1, "closed after the drop");
        verify(!ghost.visible);
    }

    // Below its own row the place counts without it: gap 5 (after the
    // last of five) is place 4; the gaps on either side of its own row
    // leave it where it is and ask for nothing.
    function test_aDropBelowItsOwnRowCountsWithoutIt() {
        const h = createTemporaryObject(dropHarnessComponent, testCase);
        let drag = dragOver(h, {}, 100, 248);
        tryCompare(h.gap, "gapIndex", 5, 2000);
        drag.Drag.drop();
        compare(JSON.stringify(h.moves), JSON.stringify([["/nonexistent/d.mp3", "Dragged", 4]]));
        drag = dragOver(h, {}, 100, 198);
        tryCompare(h.gap, "gapIndex", 4, 2000);
        drag.Drag.drop();
        drag = dragOver(h, {}, 100, 152);
        tryCompare(h.gap, "gapIndex", 3, 2000);
        drag.Drag.drop();
        compare(h.moves.length, 1, "its own place asks for nothing");
    }

    // Leaving the list closes the gap; a track from another playlist, or a
    // list not in the playlist's order, never opens one.
    function test_theGapClosesOnLeavingAndOpensOnlyForThePlaylistsTracks() {
        const h = createTemporaryObject(dropHarnessComponent, testCase);
        let drag = dragOver(h, {}, 100, 100);
        tryCompare(h.gap, "gapIndex", 2, 2000);
        drag.x = 2000;
        tryCompare(h.gap, "hovering", false);
        compare(h.gap.gapIndex, -1, "closed on leaving");
        drag.Drag.active = false;

        drag = dragOver(h, {playlistNames: ["Other"]}, 100, 100);
        tryCompare(h.gap, "hovering", true);
        wait(h.gap.hoverDelay + 300);
        compare(h.gap.gapIndex, -1, "not a track of the playlist shown");
        drag.Drag.active = false;

        h.gap.orderShown = false;
        drag = dragOver(h, {}, 100, 100);
        tryCompare(h.gap, "hovering", true);
        wait(h.gap.hoverDelay + 300);
        compare(h.gap.gapIndex, -1, "a sorted list has no places");
        drag.Drag.drop();
        compare(h.moves.length, 0);
    }

    // On the page: the drop area over the track list feeds the gap, and a
    // drop at an open gap stages the move with the controller. Nothing is
    // scanned here, so the list's rows are given to the gap by hand.
    function test_theTrackListStagesAMoveDroppedOnIt() {
        const page = makeEditingPage(true);
        realAppSettings.hideStreamingTracks = false;
        page.shownPlaylist = "Peak";
        const list = findChild(page, "trackListView");
        const gap = findChild(page, "playlistDropGap");
        verify(gap !== null);
        compare(gap.parent, list, "over the list, not its scrolling content");
        compare(gap.hoverDelay, 1500);
        verify(gap.orderShown, "the playlist's own order");
        gap.hoverDelay = 300;
        gap.draggedIndex = 3;
        gap.rowCount = 5;
        gap.rowHeight = 50;
        const at = list.mapToItem(page, 100, 52);
        const drag = dragOver(page, {}, at.x, at.y);
        tryCompare(gap, "hovering", true);
        compare(gap.draggedTrack, drag);
        compare(gap.pointerY, 52);
        tryCompare(gap, "gapIndex", 1, 3000);
        drag.Drag.drop();
        compare(JSON.stringify(page.playlistEditController.calls[0]), JSON.stringify(["move",
                "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library", "Peak",
                "/nonexistent/d.mp3", "Dragged", 1]));
        page.sortAscending = false;
        verify(!gap.orderShown, "sorted the other way: no places");
        cleanupExperimental();
    }

    // Without Experimental: no playlist menu, and the row menu (Merge,
    // Find) has no "Remove from".
    function test_withoutExperimentalThereIsNoPlaylistEditing() {
        const page = makeEditingPage(false);
        page.shownPlaylist = "Peak";
        page.openPlaylistMenu("Peak", page);
        compare(findChild(page, "playlistMenu").opened, false);
        const menu = findChild(page, "trackMenu");
        page.openTrackMenu(createTemporaryObject(fakeRowComponent, testCase), page);
        tryCompare(menu, "opened", true);
        verify(findChild(page, "mergeTrackItem").visible && findChild(page, "findMatchingItem").visible);
        compare(findChild(page, "removeFromPlaylistItem").visible, false);
        compare(page.playlistEditController.calls.length, 0);
        menu.close();
        cleanupExperimental();
    }

    // A streaming track has no file to merge: the item says so, disabled.
    function test_aStreamingRowCannotBeMerged() {
        const page = makeEditingPage(false);
        const menu = findChild(page, "trackMenu");
        page.openTrackMenu(createTemporaryObject(fakeRowComponent, testCase, {streamingSource: "TIDAL"}), page);
        tryCompare(menu, "opened", true);
        const merge = findChild(page, "mergeTrackItem");
        compare(merge.enabled, false);
        verify(merge.text.indexOf("streaming track") > 0, merge.text);
        menu.close();
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
        waitForRendering(page);
        saveScreenshot(page, "browse_playlist_menu");
        item.triggered();
        tryCompare(dialog, "opened", true);
        waitForRendering(page);
        saveScreenshot(page, "browse_delete_playlist_dialog");
        compare(page.playlistEditController.calls.length, 0, "nothing staged before OK");
        dialog.accept();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["delete", "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library", "Peak"]));
        menu.close();
        tryCompare(menu, "visible", false);
        page.openPlaylistMenu("Peak", page);
        tryCompare(menu, "opened", true);
        compare(item.text, "Keep this playlist");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[1]), JSON.stringify(["keep", "Peak"]));
        cleanupExperimental();
    }

    // Remove from the playlist shown, and Keep, in the row menu; with "All
    // tracks" shown there is no playlist to take a track out of.
    function test_removingATrackFromThePlaylistShownStagesAndKeeps() {
        const page = makeEditingPage(true);
        const menu = findChild(page, "trackMenu");
        const item = findChild(page, "removeFromPlaylistItem");
        const row = createTemporaryObject(fakeRowComponent, testCase);
        page.shownPlaylist = "";
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(item.visible, false, "no playlist shown, no Remove");
        menu.close();
        tryCompare(menu, "visible", false);
        page.shownPlaylist = "Peak";
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(item.visible, true);
        compare(item.text, "Remove from \"Peak\"");
        waitForRendering(page);
        saveScreenshot(page, "browse_track_menu");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["remove", "Peak", "/nonexistent/a.mp3", "A Track"]));
        menu.close();
        tryCompare(menu, "visible", false);
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(item.text, "Keep in \"Peak\"");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[1]), JSON.stringify(["keepIn", "Peak", "/nonexistent/a.mp3"]));
        menu.close();
        cleanupExperimental();
    }

    // The playlist shown is the one picked, by name, not by its row: with
    // the playlist search in use the row number is the filtered list's.
    function test_theShownPlaylistIsThePickedName() {
        const page = makeEditingPage(true);
        page.selectPlaylist(2, "Tech House", false);
        compare(page.shownPlaylist, "Tech House");
        page.selectPlaylist(0, "", false);
        compare(page.shownPlaylist, "");
        cleanupExperimental();
    }

    // The list's last row, with Experimental on: it asks for a name, OK
    // only once there is one, and stages the create; the new playlist is
    // listed until Save, shows its staged tracks when clicked, and its
    // menu takes it back.
    function test_theLastRowCreatesAPlaylist() {
        const page = makeEditingPage(true);
        const list = findChild(page, "playlistList");
        const row = findChild(page, "createPlaylistRow");
        verify(row !== null);
        tryCompare(row, "visible", true);
        const rowTop = row.mapToItem(list.contentItem, 0, 0).y;
        verify(rowTop >= list.contentItem.childrenRect.y + 32 - 1, "after the playlists: " + rowTop);
        compare(row.contentItem.children[1].text, "Create new playlist");
        mouseClick(row);
        const dialog = findChild(page, "createPlaylistDialog");
        tryCompare(dialog, "opened", true);
        const field = findChild(page, "newPlaylistNameField");
        tryCompare(field, "activeFocus", true);
        compare(dialog.standardButton(Dialog.Ok).enabled, false, "no name, no OK");
        field.text = "   ";
        compare(dialog.standardButton(Dialog.Ok).enabled, false, "spaces are no name");
        field.text = "Warm & Melodic 2";
        compare(dialog.standardButton(Dialog.Ok).enabled, true);
        waitForRendering(page);
        saveScreenshot(page, "browse_create_playlist_dialog");
        compare(page.playlistEditController.calls.length, 0, "nothing staged before OK");
        keyClick(Qt.Key_Return);
        tryCompare(dialog, "visible", false);
        compare(JSON.stringify(page.playlistEditController.calls[0]),
                JSON.stringify(["create", "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library",
                                "Warm & Melodic 2"]));
        tryVerify(() => findChild(list, "createPlaylistRow").mapToItem(list.contentItem, 0, 0).y >= rowTop + 32 - 1,
                  5000, "the new one is listed above the create row");
        waitForRendering(page);
        saveScreenshot(page, "browse_new_playlist_pending");
        // Clicked, it is the playlist shown (the tracks staged into it).
        const pending = findChild(list, "createPlaylistRow").parent.children[0];
        compare(pending.name, "Warm & Melodic 2");
        mouseClick(pending);
        compare(page.shownPlaylist, "Warm & Melodic 2");
        tryCompare(pending, "isCurrent", true);

        const menu = findChild(page, "playlistMenu");
        page.openPlaylistMenu("Warm & Melodic 2", page);
        tryCompare(menu, "opened", true);
        const item = findChild(page, "deletePlaylistItem");
        compare(item.text, "Don't create this playlist");
        item.triggered();
        compare(JSON.stringify(page.playlistEditController.calls[1]), JSON.stringify(["dropNew", "Warm & Melodic 2"]));
        compare(page.playlistEditController.newPlaylists.length, 0);
        menu.close();
        tryCompare(menu, "visible", false);

        // Opened again, the field starts empty.
        mouseClick(row);
        tryCompare(dialog, "opened", true);
        compare(field.text, "");
        dialog.reject();
        compare(page.playlistEditController.calls.length, 2, "cancel stages nothing");
        cleanupExperimental();
    }

    // The row menu ends in "Add to..." with every playlist but the one
    // shown, a tick on the ones the track is staged into; once a track was
    // added somewhere, "Add to <that one>" stands above it.
    function test_aTrackIsAddedToAPlaylistFromItsMenu() {
        const page = makeEditingPage(true);
        const fake = page.playlistEditController;
        fake.newPlaylists = ["Peak", "Chill"];  // the targets, with no stick scanned
        const menu = findChild(page, "trackMenu");
        const addTo = findChild(page, "addToMenu");
        const last = findChild(page, "addToLastPlaylistItem");
        const row = createTemporaryObject(fakeRowComponent, testCase);
        page.shownPlaylist = "Peak";
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(menu.itemAt(menu.count - 1).subMenu, addTo, "the last entry");
        compare(addTo.title, "Add to...");
        compare(addTo.count, 1, "not the playlist shown");
        compare(addTo.itemAt(0).text, "Chill");
        compare(last.visible, false, "nothing added yet");
        menu.itemAt(menu.count - 1).triggered();
        tryCompare(addTo, "opened", true);
        waitForRendering(page);
        saveScreenshot(page, "browse_add_to_menu");
        addTo.itemAt(0).triggered();
        compare(JSON.stringify(fake.calls[0]), JSON.stringify(["add", "/nonexistent/TESTSTICK/PIONEER",
                "/nonexistent/TESTSTICK/Engine Library", "Chill", "/nonexistent/a.mp3", "A Track"]));
        menu.close();
        tryCompare(menu, "visible", false);

        page.shownPlaylist = "";
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(addTo.count, 2);
        verify(addTo.itemAt(1).checked, "Chill: staged, ticked");
        verify(!addTo.itemAt(0).checked);
        compare(last.visible, true);
        compare(last.text, "Don't add to \"Chill\"");
        waitForRendering(page);
        saveScreenshot(page, "browse_add_to_last");
        last.triggered();
        compare(JSON.stringify(fake.calls[1]), JSON.stringify(["keepOut", "Chill", "/nonexistent/a.mp3"]));
        menu.close();
        tryCompare(menu, "visible", false);
        page.openTrackMenu(row, page);
        tryCompare(menu, "opened", true);
        compare(last.text, "Add to \"Chill\"");
        verify(!addTo.itemAt(1).checked, "taken back, no tick");
        last.triggered();
        compare(fake.calls[2][0], "add");
        compare(fake.calls[2][3], "Chill");
        menu.close();
        cleanupExperimental();
    }

    function test_addToListsTheNewPlaylistsFirst() {
        const page = makePage();
        compare(JSON.stringify(page.addTargetsFor(["Chill", "Peak", "Warm"], ["Fresh"], "Peak")),
                JSON.stringify(["Fresh", "Chill", "Warm"]));
        compare(JSON.stringify(page.addTargetsFor(["Chill"], ["Fresh", "Chill"], "")),
                JSON.stringify(["Fresh", "Chill"]), "a name on both lists once");
        compare(JSON.stringify(page.addTargetsFor(["Chill"], [], "Chill")), "[]");
    }

    function test_withoutExperimentalTracksHaveNoAddTo() {
        const page = makeEditingPage(false);
        page.playlistEditController.newPlaylists = ["Chill"];
        const menu = findChild(page, "trackMenu");
        page.openTrackMenu(createTemporaryObject(fakeRowComponent, testCase), page);
        tryCompare(menu, "opened", true);
        for (let i = 0; i < menu.count; ++i) {
            verify(!menu.itemAt(i).subMenu, "no submenu");
        }
        compare(findChild(page, "addToLastPlaylistItem").visible, false);
        menu.close();
        cleanupExperimental();
    }

    function test_withoutExperimentalTheListHasNoCreateRow() {
        const page = makeEditingPage(false);
        const row = findChild(page, "createPlaylistRow");
        verify(row === null || !row.visible);
        page.openCreatePlaylistDialog();
        compare(findChild(page, "createPlaylistDialog").opened, false);
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

    // Browse remembers the playlist picked (lastPlaylistName), and every
    // page with a playlist picker opens on it, Sync included: a name left
    // behind here scoped tst_SyncPage, which runs after this file in the
    // same process, to a playlist none of its rows are in.
    property string lastPlaylistBefore: ""
    function init() {
        lastPlaylistBefore = realAppSettings.lastPlaylistName;
    }

    function cleanup() {
        realAppSettings.lastPlaylistName = lastPlaylistBefore;
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

    // Nothing under the list: the "N tracks" line repeated what the
    // playlist sidebar says, and the rows need the room.
    function test_theListReachesTheBottomOfThePage() {
        const held = makeHeldPage(3);
        const bottom = held.list.mapToItem(held.page, 0, held.list.height).y;
        verify(held.page.height - bottom <= 1, "list ends at " + bottom + " of " + held.page.height);
        browseFixture.releaseCues();
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

    // ---- Dragging a track to a playlist (Experimental) ----

    // Browse on six held tracks with files, in "Held Playlist" and "Held
    // Reversed", with the stand-in playlist controller.
    function makeDragPage(experimental, withFiles) {
        browseFixture.holdCues(6, true, withFiles !== false);
        verify(realAppSettings.experimentalBuildSupported, "an experimental build");
        realAppSettings.experimentalFeaturesEnabled = experimental;
        const page = makePage();
        page.playlistEditController = createTemporaryObject(fakePlaylistEditComponent, testCase);
        const controller = findChild(page, "scanController");
        const list = findChild(page, "trackListView");
        tryCompare(list, "count", 6);
        tryCompare(controller, "busy", false);
        browseFixture.releaseCues();
        tryCompare(controller, "cuesPending", false);
        waitForRendering(page);
        return {page: page, controller: controller, list: list, fake: page.playlistEditController,
                sidebar: findChild(page, "playlistList"), card: findChild(page, "trackDragItem")};
    }

    // The sidebar's row for a playlist, a new one in the footer included.
    function playlistRow(t, name) {
        let found = null;
        function walk(item) {
            if (found !== null) {
                return;
            }
            if (item.acceptsTrackDrop !== undefined && item.name === name) {
                found = item;
                return;
            }
            for (let i = 0; i < item.children.length; ++i) {
                walk(item.children[i]);
            }
        }
        walk(t.sidebar.contentItem);
        verify(found !== null, "a sidebar row for " + name);
        return found;
    }

    // Where a press on a track row lands: on the title column, clear of
    // the cover (which plays) and the key badge (which opens the wheel).
    function grabPoint(row) {
        return Qt.point(Theme.iconSizeNormal + 3 * Theme.rowSpacing, row.height / 2);
    }

    // Presses on `row`, moves past the drag threshold and then onto the
    // middle of `target`, in steps, and leaves the button down. A DropArea
    // hears of a drag only from a move after it started, so every step is
    // its own move.
    function dragOnto(row, target) {
        const start = grabPoint(row);
        mousePress(row, start.x, start.y, Qt.LeftButton);
        for (let i = 1; i <= 4; ++i) {
            mouseMove(row, start.x - 6 * i, start.y + 2 * i, 0, Qt.LeftButton);
        }
        moveOnto(row, target);
    }
    function moveOnto(row, target) {
        const p = target.mapToItem(row, target.width / 2, target.height / 2);
        for (let i = 1; i <= 4; ++i) {
            mouseMove(row, p.x + (4 - i) * 12, p.y + (4 - i) * 6, 0, Qt.LeftButton);
        }
        mouseMove(row, p.x + 1, p.y, 0, Qt.LeftButton);
        return p;
    }
    function dropOnto(row, target) {
        const p = target.mapToItem(row, target.width / 2, target.height / 2);
        mouseRelease(row, p.x + 1, p.y, Qt.LeftButton);
    }

    function isAccentOutlined(item) {
        return item.border.width === 2 && Qt.colorEqual(item.border.color, Theme.accent);
    }

    // A row dragged: the card plops up under the pointer with the track on
    // it, the page says what is dragged, "All tracks" takes no drop and a
    // playlist row lights up as the target. Let go there, nothing is staged:
    // the card stays and asks, the target still marked, and only "Add to"
    // stages, as the row menu's Add to does. With no playlist shown there
    // is nothing to move the track out of, so no Move.
    function test_aTrackDroppedOnAPlaylistAsksBeforeAnythingIsStaged() {
        const t = makeDragPage(true);
        const row = t.list.itemAtIndex(0);
        compare(row.title, "Held Track 1000");
        const all = playlistRow(t, "All tracks");
        const target = playlistRow(t, "Held Playlist");
        compare(t.card.visible, false);
        compare(t.page.dragging, false);
        compare(t.page.draggedTrack, null);

        dragOnto(row, all);
        compare(t.page.dragging, true, "a drag");
        compare(t.card.visible, true);
        compare(t.card.Drag.active, true);
        compare(JSON.stringify(t.card.Drag.keys), JSON.stringify(["seabass/track"]));
        compare(t.page.draggedTrack.title, "Held Track 1000");
        compare(t.page.draggedTrack.filePath, "/nonexistent/HELD/Contents/held-1000.mp3");
        compare(t.page.draggedTrack.sourceId, "1");
        compare(t.page.draggedTrack.durationSeconds, 200);
        compare(JSON.stringify(Object.keys(t.page.draggedTrack).sort()), JSON.stringify(["artist", "artworkPath",
                "bpm", "durationSeconds", "fallbackArtworkPath", "filePath", "key", "playlistNames", "sourceId",
                "streamingSource", "title"]), "the contract's fields");
        compare(findChild(t.card, "dragTitle").text, "Held Track 1000");
        compare(findChild(t.card, "dragArtist").text, "Held Artist");
        compare(findChild(t.card, "dragDuration").text, "3:20");
        compare(all.isCurrent, true, "All tracks is shown, and outlined for that");
        verify(!all.dropTarget, "but is no target");
        verify(!target.dropTarget);

        const p = moveOnto(row, target);
        tryCompare(target, "dropTarget", true);
        verify(isAccentOutlined(target), "the target wears the accent outline");
        tryCompare(t.card, "opacity", 1);
        waitForRendering(t.page);
        saveScreenshot(t.page, "browse_drag_over_playlist");

        dropOnto(row, target);
        compare(t.page.dragging, false);
        compare(t.page.draggedTrack, null);
        compare(t.fake.calls.length, 0, "nothing staged on the drop");
        compare(t.card.confirming, true, "the card stays and asks");
        compare(t.card.visible, true);
        verify(isAccentOutlined(target), "the target stays marked while the card asks");
        const hot = t.card.mapToItem(row, t.card.Drag.hotSpot.x, t.card.Drag.hotSpot.y);
        verify(Math.abs(hot.x - p.x) <= 2 && Math.abs(hot.y - p.y) <= 2, "where it was dropped: " + hot.x + "," + hot.y
               + " for " + p.x + "," + p.y);
        const add = findChild(t.card, "dropAddButton");
        const move = findChild(t.card, "dropMoveButton");
        compare(add.visible, true);
        compare(add.text, "Add to \"Held Playlist\"");
        compare(move.visible, false, "no playlist shown, nothing to move out of");
        compare(t.page.trackPanelOpen, false, "a drag is no click");
        compare(findChild(t.page, "trackMenu").opened, false, "nor a press and hold");
        waitForRendering(t.page);
        saveScreenshot(t.page, "browse_drop_confirm_add");

        mouseClick(add);
        compare(JSON.stringify(t.fake.calls), JSON.stringify([["add", "/nonexistent/TESTSTICK/PIONEER",
                "/nonexistent/TESTSTICK/Engine Library", "Held Playlist", "/nonexistent/HELD/Contents/held-1000.mp3",
                "Held Track 1000"]]));
        tryCompare(t.card, "visible", false);
        verify(!isAccentOutlined(target));
        cleanupExperimental();
    }

    // With a playlist shown that the track is in, the card also offers
    // Move: the add to the target and the removal from the shown one, in
    // that order. The shown playlist itself is no target.
    function test_aTrackInThePlaylistShownCanBeMoved() {
        const t = makeDragPage(true);
        const reversed = t.controller.playlistNames.indexOf("Held Reversed");
        t.page.selectPlaylist(reversed + 1, "Held Reversed", false);
        waitForRendering(t.page);
        const row = t.list.itemAtIndex(0);
        compare(row.title, "Held Track 1005", "the reversed playlist's first");
        const shown = playlistRow(t, "Held Reversed");
        const target = playlistRow(t, "Held Playlist");

        dragOnto(row, shown);
        compare(t.page.dragging, true);
        verify(!shown.dropTarget, "the playlist shown is no target");
        dropOnto(row, shown);
        compare(t.card.confirming, false, "let go over no target: nothing asked");
        tryCompare(t.card, "visible", false);

        dragOnto(row, target);
        tryCompare(target, "dropTarget", true);
        dropOnto(row, target);
        compare(t.card.confirming, true);
        const move = findChild(t.card, "dropMoveButton");
        compare(move.visible, true);
        compare(move.text, "Move to \"Held Playlist\"");
        compare(findChild(t.card, "dropAddButton").text, "Add to \"Held Playlist\"");
        tryCompare(t.card, "opacity", 1);
        waitForRendering(t.page);
        saveScreenshot(t.page, "browse_drop_confirm_move");
        compare(t.fake.calls.length, 0);
        mouseClick(move);
        compare(JSON.stringify(t.fake.calls), JSON.stringify([
            ["add", "/nonexistent/TESTSTICK/PIONEER", "/nonexistent/TESTSTICK/Engine Library", "Held Playlist",
             "/nonexistent/HELD/Contents/held-1005.mp3", "Held Track 1005"],
            ["remove", "Held Reversed", "/nonexistent/HELD/Contents/held-1005.mp3", "Held Track 1005"]]));
        tryCompare(t.card, "visible", false);
        cleanupExperimental();
    }

    // The card asking goes away, staging nothing, on Escape, on a click
    // anywhere else (which does nothing else), and on a press on the row
    // it came from.
    function test_aDropIsCancelledByEscapeOrAClickElsewhere() {
        const t = makeDragPage(true);
        const row = t.list.itemAtIndex(1);
        const target = playlistRow(t, "Held Playlist");
        const confirmDrop = () => {
            dragOnto(row, target);
            dropOnto(row, target);
            compare(t.card.confirming, true);
        };

        confirmDrop();
        tryCompare(t.card, "activeFocus", true);
        keyClick(Qt.Key_Escape);
        compare(t.card.confirming, false);
        tryCompare(t.card, "visible", false);

        confirmDrop();
        mouseClick(t.list.itemAtIndex(3), grabPoint(row).x, row.height / 2);
        tryCompare(t.card, "visible", false);
        compare(t.page.trackPanelOpen, false, "the click only let the card go");

        confirmDrop();
        mousePress(row, grabPoint(row).x, row.height / 2);
        compare(t.card.confirming, false, "the row pressed again");
        mouseRelease(row, grabPoint(row).x, row.height / 2);
        tryCompare(t.card, "visible", false);

        // Let go over the list, not a playlist: the card goes at once.
        dragOnto(row, t.list.itemAtIndex(4));
        mouseRelease(row, grabPoint(row).x, row.height / 2 + row.height * 3);
        compare(t.card.confirming, false);
        tryCompare(t.card, "visible", false);
        compare(t.fake.calls.length, 0, "nothing staged at all");
        cleanupExperimental();
    }

    // A playlist staged for deletion takes no drop; one staged to be
    // created does, from the end of the list.
    function test_aDeletedPlaylistTakesNoDropANewOneDoes() {
        const t = makeDragPage(true);
        t.fake.staged["p:Held Playlist"] = true;
        t.fake.pendingRevision++;
        t.fake.newPlaylists = ["Fresh"];
        waitForRendering(t.page);
        const row = t.list.itemAtIndex(0);
        const deleted = playlistRow(t, "Held Playlist");
        dragOnto(row, deleted);
        compare(t.page.dragging, true);
        verify(!deleted.dropTarget, "staged for deletion");
        dropOnto(row, deleted);
        compare(t.card.confirming, false);
        tryCompare(t.card, "visible", false);

        const fresh = playlistRow(t, "Fresh");
        compare(fresh.pendingNew, true);
        dragOnto(row, fresh);
        tryCompare(fresh, "dropTarget", true);
        dropOnto(row, fresh);
        compare(t.card.confirming, true);
        compare(findChild(t.card, "dropAddButton").text, "Add to \"Fresh\"");
        keyClick(Qt.Key_Escape);
        tryCompare(t.card, "visible", false);
        cleanupExperimental();
    }

    // With drags on, a row still opens the details pane on a click and its
    // menu on a right click or a press and hold, and its cover still plays.
    function test_aDraggableRowStillClicks() {
        const t = makeDragPage(true);
        const row = t.list.itemAtIndex(2);
        const at = grabPoint(row);
        mouseClick(row, at.x, at.y);
        compare(t.page.trackPanelOpen, true, "a click opens the details pane");
        compare(findChild(t.page, "trackDetailPanel").trackSourceId, "3");
        compare(t.card.visible, false, "and drags nothing");
        const menu = findChild(t.page, "trackMenu");
        mouseClick(row, at.x, at.y, Qt.RightButton);
        tryCompare(menu, "opened", true);
        menu.close();
        tryCompare(menu, "visible", false);
        mousePress(row, at.x, at.y);
        tryCompare(menu, "opened", true, 3000, "a press and hold opens the menu");
        mouseRelease(row, at.x, at.y);
        menu.close();
        tryCompare(menu, "visible", false);
        const cover = findChild(row, "rowCell_artwork");
        mouseClick(cover);
        compare(realPlayback.currentSourceId, "3", "the cover plays");
        realPlayback.stop();
        compare(t.card.visible, false);
        cleanupExperimental();
    }

    // Without Experimental nothing drags, and a track without a file (here
    // the held rows without one, or a streaming one) cannot be dragged.
    function test_nothingDragsWithoutExperimentalOrAFile() {
        let t = makeDragPage(false);
        let row = t.list.itemAtIndex(0);
        dragOnto(row, playlistRow(t, "Held Playlist"));
        compare(t.page.dragging, false, "Experimental off");
        compare(t.card.visible, false);
        dropOnto(row, playlistRow(t, "Held Playlist"));
        compare(t.card.confirming, false);

        t = makeDragPage(true, false);
        row = t.list.itemAtIndex(0);
        compare(row.filePath, "");
        dragOnto(row, playlistRow(t, "Held Playlist"));
        compare(t.page.dragging, false, "no file");
        dropOnto(row, playlistRow(t, "Held Playlist"));
        compare(t.card.confirming, false);
        verify(!t.card.canDrag({filePath: "/nonexistent/s.mp3", streamingSource: "TIDAL"}), "a streaming track");
        verify(t.card.canDrag({filePath: "/nonexistent/s.mp3", streamingSource: ""}));
        compare(t.fake.calls.length, 0);
        cleanupExperimental();
    }

    // The real drag over the playlist shown: held over the gap before the
    // fourth row, the card says to hold, then the gap opens, the rows from
    // there move down a row (the Translate on the real rows), and the card
    // stands back. Let go there, the move is staged with
    // the place the track takes, and the card fades as on any release
    // that is not on a playlist: no question asked. Over a playlist row the
    // gap never opens, however long the pointer waits.
    function test_theRealDragReordersThePlaylistShown() {
        const t = makeDragPage(true);
        realAppSettings.hideStreamingTracks = false;
        t.page.selectPlaylist(t.controller.playlistNames.indexOf("Held Playlist") + 1, "Held Playlist", false);
        waitForRendering(t.page);
        const gap = findChild(t.page, "playlistDropGap");
        verify(gap.orderShown);
        gap.hoverDelay = 400;
        const row = t.list.itemAtIndex(0);
        compare(row.title, "Held Track 1000");
        const rowsY = [];
        for (let i = 0; i < 6; ++i) {
            rowsY.push(t.list.itemAtIndex(i).mapToItem(t.list, 0, 0).y);
        }

        // Over a playlist row first, for longer than the delay.
        const target = playlistRow(t, "Held Reversed");
        dragOnto(row, target);
        compare(t.page.dragging, true);
        tryCompare(target, "dropTarget", true);
        wait(gap.hoverDelay * 3);
        compare(gap.hovering, false, "the list hears nothing of a drag over the sidebar");
        compare(gap.gapIndex, -1, "no gap over a playlist row");
        compare(t.list.itemAtIndex(3).mapToItem(t.list, 0, 0).y, rowsY[3], "no row moved");
        compare(findChild(t.card, "dragHint").visible, false);

        // Then over the line between the third and fourth rows.
        const line = t.list.itemAtIndex(3);
        const at = line.mapToItem(row, grabPoint(line).x, 0);
        for (let i = 1; i <= 3; ++i) {
            mouseMove(row, at.x - 40 * (3 - i), at.y, 0, Qt.LeftButton);
        }
        mouseMove(row, at.x, at.y + 1, 0, Qt.LeftButton);
        // A drag's moves reach the drop areas from the event loop.
        tryVerify(() => !target.dropTarget, 2000, "the sidebar lets go of it over the list");
        tryCompare(gap, "hovering", true);
        compare(findChild(t.card, "dragHint").text, "Hold to move it here");
        tryCompare(gap, "gapIndex", 3, 3000);
        compare(findChild(t.card, "dragHint").visible, false, "the gap's own row says it now");
        compare(t.card.dimmed, true);
        for (let i = 0; i < 6; ++i) {
            const shift = i >= 3 ? gap.rowHeight : 0;
            tryCompare(t.list.itemAtIndex(i).transform[0], "y", shift, 2000, "row " + i + " shifted by " + shift);
            compare(t.list.itemAtIndex(i).mapToItem(t.list, 0, 0).y, rowsY[i] + shift, "row " + i + " drawn there");
        }
        compare(findChild(t.page, "dropGapGhost").visible, true);
        waitForRendering(t.page);
        saveScreenshot(t.page, "browse_reorder_real_drag");

        mouseRelease(row, at.x, at.y + 1, Qt.LeftButton);
        compare(JSON.stringify(t.fake.calls), JSON.stringify([["move", "/nonexistent/TESTSTICK/PIONEER",
                "/nonexistent/TESTSTICK/Engine Library", "Held Playlist", "/nonexistent/HELD/Contents/held-1000.mp3",
                "Held Track 1000", 2]]), "third place, counted without its own row");
        compare(t.card.confirming, false, "the list's accept is no playlist drop");
        tryCompare(t.card, "visible", false);
        compare(gap.gapIndex, -1);
        for (let i = 0; i < 6; ++i) {
            tryCompare(t.list.itemAtIndex(i).transform[0], "y", 0, 2000);
        }
        cleanupExperimental();
    }

    // A Matching result drags to a playlist as a Browse row does; a
    // streaming one does not.
    function test_aMatchingResultDragsToAPlaylist() {
        const t = makeDragPage(true);
        t.page.matchingPanelOpen = true;
        const matching = findChild(t.page, "matchingPage");
        waitForRendering(t.page);
        // Results of our own, once a refresh due will not replace them.
        tryCompare(findChild(matching, "refreshDebounce"), "running", false);
        matching.candidates = [
            {sourceId: "7", title: "Match", artist: "Someone", key: "8A", keyRelation: "", camelotLabel: "8A", bpm: 124,
             rating: -1, durationSeconds: 245, filePath: "/nonexistent/m.mp3", streamingSource: "", artworkPath: "",
             fallbackArtworkPath: "", playlistNames: [], cues: []},
            {sourceId: "8", title: "Streamed", artist: "Someone", key: "8A", keyRelation: "", camelotLabel: "8A", bpm: 124,
             rating: -1, durationSeconds: 245, filePath: "/nonexistent/s.mp3", streamingSource: "TIDAL", artworkPath: "",
             fallbackArtworkPath: "", playlistNames: [], cues: []}];
        waitForRendering(t.page);
        // A result's drag area, found afresh each time: staging can
        // rebuild the list.
        function dragArea(title) {
            let found = null;
            function walk(item) {
                if (item.objectName === "candidateDragArea" && item.track && item.track.title === title) {
                    found = item;
                }
                for (let i = 0; i < item.children.length; ++i) {
                    walk(item.children[i]);
                }
            }
            walk(matching);
            verify(found !== null, "a drag area for " + title);
            return found;
        }
        const target = playlistRow(t, "Held Playlist");
        let area = dragArea("Streamed");
        dragOnto(area, target);
        compare(t.page.dragging, false, "a streaming result");
        dropOnto(area, target);
        compare(t.card.confirming, false);

        area = dragArea("Match");
        dragOnto(area, target);
        compare(t.page.dragging, true);
        compare(t.page.draggedTrack.title, "Match");
        compare(findChild(t.card, "dragBpm").text, "124");
        compare(findChild(t.card, "dragDuration").text, "4:05");
        tryCompare(target, "dropTarget", true);
        waitForRendering(t.page);
        saveScreenshot(t.page, "matching_drag_over_playlist");
        dropOnto(area, target);
        compare(t.card.confirming, true);
        compare(t.fake.calls.length, 0);
        mouseClick(findChild(t.card, "dropAddButton"));
        compare(JSON.stringify(t.fake.calls[0]), JSON.stringify(["add", "/nonexistent/TESTSTICK/PIONEER",
                "/nonexistent/TESTSTICK/Engine Library", "Held Playlist", "/nonexistent/m.mp3", "Match"]));
        tryCompare(t.card, "visible", false);

        cleanupExperimental();
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
        compare(cues.toolTipText, "Sorted by Cues, descending. Click for the library's own order");
        keyClick(Qt.Key_Return);
        compareIndicators(held.page, "", true);
        compare(held.page.sortField, "playlist", "a third press goes back to the list's own order");
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

    // The playlist sidebar shows no scroll bar while its rows fit.
    function test_thePlaylistSidebarHasNoScrollBarWhenItFits() {
        const page = makePage();
        page.playlistSidebarOpen = true;
        waitForRendering(page);
        const list = findChild(page, "playlistList");
        verify(list !== null);
        tryVerify(() => list.height > 0);
        const bar = list.ScrollBar.vertical;
        verify(list.contentHeight <= list.height, "the rows fit: " + list.contentHeight + " in " + list.height);
        compare(bar.visible, false, "so no scroll bar");
    }

    // BigScrollBar itself, which every scrolling page uses: there while
    // there is something to scroll, gone while there is not.
    Component {
        id: scrollingComponent
        Flickable {
            width: 200
            height: 100
            contentWidth: 200
            ScrollBar.vertical: BigScrollBar {}
        }
    }
    function test_theBigScrollBarIsThereOnlyWhenThereIsSomethingToScroll() {
        const f = createTemporaryObject(scrollingComponent, testCase, {contentHeight: 1000});
        tryCompare(f.ScrollBar.vertical, "visible", true);
        f.contentHeight = 60;
        tryCompare(f.ScrollBar.vertical, "visible", false);
    }

    // On a long list the thumb stays big enough to see and grab: without a
    // minimum it is the visible fraction of the list, 2 px of this 100.
    function test_theBigScrollBarThumbStaysVisibleOnALongList() {
        const f = createTemporaryObject(scrollingComponent, testCase, {contentHeight: 5000});
        const bar = f.ScrollBar.vertical;
        tryCompare(bar, "visible", true);
        waitForRendering(f);
        verify(bar.contentItem.height >= 32, "the thumb is " + bar.contentItem.height + " px");
    }

    // Browse's track list scrolls with the same bar as every other list;
    // it had none at all.
    function test_theTrackListHasAScrollBar() {
        const held = makeHeldPage(40);
        const bar = held.list.ScrollBar.vertical;
        verify(bar !== null, "a scroll bar is attached");
        tryCompare(bar, "visible", true);
        verify(bar.width > 0 && bar.contentItem.height >= 32, "and can be seen: " + bar.width + " x " + bar.contentItem.height);
    }

    // ---- One column geometry for the header row and the rows ----

    // Each column's header cell and the cell under it in a row, by
    // objectName: [column, header, row].
    readonly property var columnCells: [
        ["artwork", "headerCell_artwork", "rowCell_artwork"],
        ["title", "sortHeader_title", "rowCell_title"],
        ["key", "sortHeader_key", "rowCell_key"],
        ["bpm", "sortHeader_bpm", "rowCell_bpm"],
        ["duration", "sortHeader_duration", "rowCell_duration"],
        ["cues", "sortHeader_cues", "cueCountLabel"],
        ["plays", "sortHeader_plays", "rowCell_plays"],
        ["actions", "headerCell_actions", "trackRowMenuButton"],
    ]

    // Every column the header shows starts and ends where the same column
    // does in the rows, to the pixel; a column the header drops, the rows
    // drop too. Returns how many columns were compared.
    function compareColumnGeometry(page, list, where) {
        const row = list.itemAtIndex(0);
        verify(row !== null, where + ": a first row");
        let compared = 0;
        for (const cell of testCase.columnCells) {
            const head = findChild(page, cell[1]);
            const body = findChild(row, cell[2]);
            verify(head !== null && body !== null, where + ": both cells of " + cell[0]);
            compare(body.visible, head.visible, where + ": " + cell[0] + " shown in both or neither");
            if (!head.visible) {
                continue;
            }
            const headX = Math.round(head.mapToItem(page, 0, 0).x);
            const bodyX = Math.round(body.mapToItem(page, 0, 0).x);
            compare(headX, bodyX, where + ": " + cell[0] + " starts on its header's line");
            compare(Math.round(head.width), Math.round(body.width), where + ": " + cell[0] + " as wide as its header");
            ++compared;
        }
        return compared;
    }

    function test_theHeadersStandOverTheirColumns() {
        const held = makeHeldPage(12);
        browseFixture.releaseCues();
        tryCompare(held.controller, "cuesPending", false);

        // The usual window, with the playlists sidebar beside the list.
        held.page.width = 1000;
        held.page.height = 700;
        held.page.playlistSidebarOpen = true;
        waitForRendering(held.page);
        saveScreenshot(held.page, "browse-columns-1000x700");
        compare(held.page.browseTier, 2);
        compare(compareColumnGeometry(held.page, held.list, "1000 wide"), 8, "every column at full width");

        // Narrow: the numeric columns go, in the header and the rows alike.
        held.page.playlistSidebarOpen = false;
        held.page.width = 520;
        waitForRendering(held.page);
        saveScreenshot(held.page, "browse-columns-narrow");
        compare(held.page.browseTier, 1);
        compare(compareColumnGeometry(held.page, held.list, "520 wide"), 3, "artwork, title and actions");

        // Narrower still: the title goes too.
        held.page.width = 340;
        waitForRendering(held.page);
        compare(held.page.browseTier, 0);
        compare(compareColumnGeometry(held.page, held.list, "340 wide"), 2, "artwork and actions");
    }

    // ---- Back to the playlist's own order ----

    // A third click on the sorted header returns the list to the order the
    // playlist has on the player, and the page says so: the combo reads
    // Playlist Order and no header shows an arrow.
    function test_aThirdClickReturnsToThePlaylistsOrder() {
        browseFixture.holdCues(12, true);
        const page = makePage();
        page.playlistSidebarOpen = false;
        const controller = findChild(page, "scanController");
        const list = findChild(page, "trackListView");
        tryCompare(list, "count", 12);
        tryCompare(controller, "busy", false);
        browseFixture.releaseCues();
        tryCompare(controller, "cuesPending", false);
        const index = controller.playlistNames.indexOf("Held Reversed");
        verify(index >= 0, "the reversed playlist is listed");
        // Not remembered: the last playlist is a setting every later page
        // reads, and Sync Cue Points would open on this one and find nothing.
        page.selectPlaylist(index + 1, "Held Reversed", false);
        waitForRendering(page);

        const playlistOrder = titles(controller);
        compare(playlistOrder[0], "Held Track 1011", "the playlist's first track is the scan's last");
        compare(playlistOrder[11], "Held Track 1000");
        const combo = findChild(page, "sortCombo");
        compare(combo.currentText, "Playlist Order");

        const cues = header(page, "cues");
        mouseClick(cues);
        compareIndicators(page, "cues", true);
        verify(String(titles(controller)) !== String(playlistOrder), "sorted by cues, ascending");
        mouseClick(cues);
        compareIndicators(page, "cues", false);
        verify(String(titles(controller)) !== String(playlistOrder), "sorted by cues, descending");
        compare(cues.toolTipText, "Sorted by Cues, descending. Click for the playlist's own order");

        mouseClick(cues);
        compare(page.sortField, "playlist");
        compare(titles(controller), playlistOrder, "back in the playlist's own order");
        compare(combo.currentText, "Playlist Order", "the combo says so");
        compareIndicators(page, "", true);
        compare(cues.toolTipText, "Sort by Cues");

        // And from there a click sorts by the column again, ascending.
        mouseClick(cues);
        compareIndicators(page, "cues", true);
        waitForRendering(page);
        saveScreenshot(page, "browse-sorted-by-cues");
        mouseClick(cues);
        mouseClick(cues);
        waitForRendering(page);
        saveScreenshot(page, "browse-playlist-order");
    }
}
