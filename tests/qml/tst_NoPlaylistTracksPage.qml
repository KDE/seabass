// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's "Tracks not in any playlist" page, driven by a
// stand-in controller.
TestCase {
    id: testCase
    name: "NoPlaylistTracksPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property bool playlistsChecked: true
            property string playlistsError: ""
            property bool stickHasPlaylists: true
            property var noPlaylistTracks: [
                {filePath: "/S/Contents/a.mp3", title: "Alpha", artist: "Someone", libraries: "rekordbox, OneLibrary, Engine", staged: false},
                {filePath: "/S/Contents/b.mp3", title: "Bravo", artist: "", libraries: "Engine", staged: false},
                {filePath: "/S/Contents/c.mp3", title: "Charlie", artist: "Else", libraries: "rekordbox", staged: false}]
            property int noPlaylistTrackCount: noPlaylistTracks.length
            property int noPlaylistStagedCount: 0
            property var calls: []
            function deleteTracksInNoPlaylist(paths) {
                calls.push("delete " + paths.slice().sort().join(","));
                noPlaylistStagedCount = paths.length;
                noPlaylistTracks = noPlaylistTracks.map(t => Object.assign({}, t, {staged: paths.indexOf(t.filePath) >= 0}));
            }
            function unstageTracksInNoPlaylist() {
                calls.push("unstage");
                noPlaylistStagedCount = 0;
                noPlaylistTracks = noPlaylistTracks.map(t => Object.assign({}, t, {staged: false}));
            }
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            function scan(a, b, c) {}
            function cancelScan() {}
            function undoLastOperation() {}
        }
    }

    Component {
        id: pageComponent
        NoPlaylistTracksPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    // Built from character codes, so this file holds no dash literal.
    function noDashes(text) {
        return text.indexOf(String.fromCharCode(0x2014)) < 0 && text.indexOf("-".repeat(2)) < 0;
    }

    // Two hundred tracks in no playlist do not fit the page: the list
    // scrolls, with room under the last row for the floating Save button,
    // and the last row can be brought on screen.
    function test_aLongListOfTracksScrolls() {
        const many = [];
        for (let i = 0; i < 200; ++i) {
            many.push({filePath: "/S/Contents/t" + i + ".mp3", title: "Track " + i, artist: "Someone",
                       libraries: "rekordbox", staged: false});
        }
        const controller = createTemporaryObject(controllerComponent, testCase, {noPlaylistTracks: many});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null);
        waitForRendering(page);
        const view = findChild(page, "noPlaylistTracksView");
        verify(view !== null, "the list is a view");
        tryVerify(() => view.contentHeight > view.height, 5000, "more than fits");
        verify(view.interactive, "and so it scrolls");
        verify(page.saveClearance > 0);
        verify(view.bottomMargin >= page.saveClearance - 0.5, "room for the Save button under the last row");
        view.contentY = view.contentHeight + view.bottomMargin - view.height;
        const rows = findChild(page, "noPlaylistTracks");
        const last = rows.itemAt(199);
        verify(last !== null);
        const bottom = last.mapToItem(view, 0, last.height).y;
        verify(bottom <= view.height + 0.5 && bottom > 0, "the last row is on screen at the end: " + bottom);
        verify(bottom <= view.height - page.saveClearance + 0.5, "and clear of the Save button: " + bottom);
    }

    function makePage(hasPlaylists) {
        const controller = createTemporaryObject(controllerComponent, testCase, {stickHasPlaylists: hasPlaylists});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null);
        waitForRendering(page);
        return {page: page, controller: controller};
    }

    function test_twoTicksStageThoseTwoAndUnstageTakesThemBack() {
        const t = makePage(true);
        const summary = findChild(t.page, "noPlaylistSummary");
        compare(summary.text, "3 tracks are in no playlist in any library on this stick.");
        verify(findChild(t.page, "noPlaylistExplanation").visible);
        verify(!findChild(t.page, "noPlaylistsOnStick").visible);
        const button = findChild(t.page, "deleteNoPlaylistButton");
        compare(button.enabled, false, "nothing ticked, nothing to delete");
        for (const path of ["/S/Contents/a.mp3", "/S/Contents/c.mp3"]) {
            const box = findChild(t.page, "pick_" + path);
            verify(box !== null && box.visible);
            box.toggle();
            box.toggled();
        }
        compare(button.text, "Delete 2 Tracks");
        button.clicked();
        compare(t.controller.calls[0], "delete /S/Contents/a.mp3,/S/Contents/c.mp3");
        compare(button.text, "Unstage");
        compare(findChild(t.page, "pick_/S/Contents/b.mp3").enabled, false, "the selection is locked while staged");
        button.clicked();
        compare(t.controller.calls[1], "unstage");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(t.page).save(screenshotDir + "/no-playlist-tracks-page.png");
        }
        for (const label of [summary, findChild(t.page, "noPlaylistExplanation")]) {
            verify(noDashes(label.text), "no dashes on screen");
        }
    }

    function test_selectAllAndNone() {
        const t = makePage(true);
        findChild(t.page, "pickAllButton").clicked();
        compare(findChild(t.page, "deleteNoPlaylistButton").text, "Delete 3 Tracks");
        findChild(t.page, "pickNoneButton").clicked();
        compare(findChild(t.page, "deleteNoPlaylistButton").enabled, false);
    }

    // Every track listed because the stick has no playlists: shown, but
    // no box to tick and no button.
    function test_aStickWithoutPlaylistsOffersNoDeletion() {
        const t = makePage(false);
        const note = findChild(t.page, "noPlaylistsOnStick");
        verify(note.visible);
        verify(noDashes(note.text), "no dashes on screen");
        compare(findChild(t.page, "deleteNoPlaylistButton").visible, false);
        compare(findChild(t.page, "pick_/S/Contents/a.mp3").visible, false);
        compare(t.controller.calls.length, 0);
    }
    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_JunkCuePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: realControllerComponent
        LibraryConsistencyController {}
    }

    // Whatever a test staged in a real session is not handed to the next.
    function cleanup() {
        EditSessionRegistry.discardAll();
    }

    // A copy of the fixture, checked by the real controller.
    function checkedStick() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const controller = createTemporaryObject(realControllerComponent, testCase);
        controller.scan(stick + "/PIONEER", stick + "/Engine Library", "");
        tryVerify(() => !controller.busy, 300000, "the check finishes");
        verify(controller.playlistsChecked && controller.stickHasPlaylists, "precondition: the playlists were checked");
        return {stick: stick, controller: controller};
    }

    // The real controller's entries, through a real save: a file only
    // Engine lists beside one only rekordbox and OneLibrary list (the
    // fixture's Engine library shares no file with the other two). Each
    // goes from the libraries that list it, and no library without it is
    // asked about it.
    function test_theRealControllerDeletesFilesOnlySomeLibrariesList() {
        const checked = checkedStick();
        const controller = checked.controller;
        const engineOnly = controller.noPlaylistTracks.find((t) => t.libraries === "Engine");
        const pioneerOnly = controller.noPlaylistTracks.find((t) => t.libraries.indexOf("Engine") < 0);
        verify(engineOnly !== undefined && pioneerOnly !== undefined, "precondition: a file of each kind in no playlist");
        const paths = [String(engineOnly.filePath), String(pioneerOnly.filePath)];
        const before = controller.noPlaylistTrackCount;
        controller.deleteTracksInNoPlaylist(paths);
        compare(controller.errorMessage, "");
        compare(controller.noPlaylistStagedCount, 2);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(checked.stick + "/PIONEER"),
                                                       "TESTSTICK");
        verify(session !== null);
        compare(session.pendingCount, 1);
        let summary = null;
        session.saveFinished.connect((result) => { summary = result; });
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        compare(summary.error, "", "the save wrote it: " + JSON.stringify(summary));
        tryVerify(() => !controller.busy && controller.playlistsChecked && controller.noPlaylistTrackCount < before, 300000,
                  "the check after the save finishes");
        compare(controller.noPlaylistTrackCount, before - 2, "the two are gone, every other stays");
        for (const path of paths) {
            verify(!controller.noPlaylistTracks.some((t) => t.filePath === path), path + " is gone");
        }
    }

    // A file whose rows another repair staged on this page changes is not
    // deleted in the same save, either way round, and the page says why:
    // by the delete's turn its rows would be gone or point elsewhere, and
    // the whole save would fail. A relink points the rows elsewhere (a
    // move back does not touch them); an orphan's delete takes OneLibrary's
    // row out.
    function test_aFileARepairChangesIsNotDeletedInTheSameSave() {
        const checked = checkedStick();
        const controller = checked.controller;
        const listed = controller.noPlaylistTracks.filter((t) => t.libraries === "rekordbox, OneLibrary");
        verify(listed.length >= 3, "precondition: files in rekordbox and OneLibrary in no playlist");
        const moved = String(listed[0].filePath);
        const other = String(listed[1].filePath);
        controllerFixture.plantMovedFile(controller, checked.stick, moved, String(listed[0].title),
                                         checked.stick + "/Contents/Elsewhere/moved.mp3");
        controller.relinkMovedFile(moved);
        compare(controller.errorMessage, "");
        compare(controller.movedFileStagedCount, 1);
        controller.deleteTracksInNoPlaylist([moved, other]);
        compare(controller.noPlaylistStagedCount, 1, "only the other file is staged");
        verify(controller.noPlaylistTracks.find((t) => t.filePath === other).staged);
        verify(!controller.noPlaylistTracks.find((t) => t.filePath === moved).staged);
        verify(controller.statusMessage.indexOf("is not deleted: a repair staged on this page changes its rows. Save that "
                                                + "first, then delete it.") >= 0, controller.statusMessage);
        verify(noDashes(controller.statusMessage));

        controller.unstageTracksInNoPlaylist();
        controller.unstageMovedFile(moved);
        controller.deleteTracksInNoPlaylist([moved]);
        compare(controller.noPlaylistStagedCount, 1);
        controller.relinkMovedFile(moved);
        compare(controller.movedFileStagedCount, 0, "the relink is refused while the delete holds the file");
        verify(controller.errorMessage.indexOf("is staged for deletion as a track in no playlist, so the library cannot be "
                                               + "pointed at") >= 0, controller.errorMessage);
        verify(noDashes(controller.errorMessage));
        controller.moveFileBack(moved);
        compare(controller.errorMessage, "", "moving the file back changes no row: offered");
        compare(controller.movedFileStagedCount, 1);
        controller.unstageMovedFile(moved);
        controller.unstageTracksInNoPlaylist();

        // OneLibrary's row of a file with no file on the stick is an orphan
        // Library Health offers to delete: one in no playlist.
        const model = controller.issues;
        const kindRole = Qt.UserRole + 1;
        const formatRole = Qt.UserRole + 2;
        const brokenRole = Qt.UserRole + 4;
        let orphan = -1;
        let orphanPath = "";
        for (let i = 0; i < model.count && orphan < 0; ++i) {
            const index = model.index(i, 0);
            if (model.data(index, kindRole) !== "missing" || model.data(index, formatRole) !== "onelibrary") {
                continue;
            }
            for (const broken of model.data(index, brokenRole)) {
                const path = String(broken.filePath);
                if (listed.some((t) => t.filePath === path) && path !== moved) {
                    orphan = i;
                    orphanPath = path;
                    break;
                }
            }
        }
        verify(orphan >= 0, "precondition: an orphan OneLibrary row of a file in no playlist");
        controller.deleteOrphan(orphan);
        compare(controller.errorMessage, "");
        compare(controller.stagedIssueCount, 1);
        controller.deleteTracksInNoPlaylist([orphanPath]);
        compare(controller.noPlaylistStagedCount, 0, "the delete is not staged");
        verify(controller.errorMessage.indexOf("is not deleted: a repair staged on this page changes its rows") >= 0,
               controller.errorMessage);
        controller.unstageIssue(orphan);
        controller.deleteTracksInNoPlaylist([orphanPath]);
        compare(controller.noPlaylistStagedCount, 1);
        controller.deleteOrphan(orphan);
        compare(controller.stagedIssueCount, 0, "the orphan's delete is refused while the delete holds the file");
        verify(controller.errorMessage.indexOf("is staged for deletion as a track in no playlist, so it cannot be repaired "
                                               + "in the same save") >= 0, controller.errorMessage);
        verify(noDashes(controller.errorMessage));
    }

    // A repair also changes its survivor's rows: the broken copy's
    // playlist entries and cues move onto it. So a survivor in no playlist
    // is not deleted beside that repair, either way round: by the repair's
    // turn at Save its rows would be gone, and the whole save would fail.
    // The fixture's stick copy holds no audio and no Engine duplicates, so
    // two Engine tracks in no playlist get a file each, and two broken
    // Engine rows without cues are made copies of them: two repairable
    // issues, each with a survivor in no playlist.
    function test_aRepairsSurvivorIsNotDeletedInTheSameSave() {
        const checked = checkedStick();
        const controller = checked.controller;
        const model = controller.issues;
        const kindRole = Qt.UserRole + 1;
        const formatRole = Qt.UserRole + 2;
        const survivorRole = Qt.UserRole + 3;
        const brokenRole = Qt.UserRole + 4;
        const stagedRole = Qt.UserRole + 7;
        const inNoPlaylist = (path) => controller.noPlaylistTracks.some((t) => t.filePath === path);
        // Engine rows the check lists as missing, one per issue.
        const engineRows = [];
        for (let i = 0; i < model.count; ++i) {
            const index = model.index(i, 0);
            const broken = model.data(index, brokenRole);
            if (model.data(index, kindRole) === "missing" && model.data(index, formatRole) === "engine"
                && broken.length === 1) {
                engineRows.push(broken[0]);
            }
        }
        const survivors = engineRows.filter((r) => controller.noPlaylistTracks.some(
            (t) => t.filePath === String(r.filePath) && t.libraries === "Engine")).slice(0, 2);
        const doomed = engineRows.filter((r) => r.cues.length === 0 && !inNoPlaylist(String(r.filePath))).slice(0, 2);
        verify(survivors.length === 2 && doomed.length === 2, "precondition: Engine rows of both kinds");
        const library = checked.stick + "/Engine Library";
        const paths = [];
        for (let k = 0; k < 2; ++k) {
            const path = String(survivors[k].filePath);
            verify(stickFixture.makeDirectory(path.substring(0, path.lastIndexOf("/"))));
            verify(stickFixture.touchNew(path), path);
            verify(artworkFixture.makeEngineDuplicate(library, Number(survivors[k].sourceId), Number(doomed[k].sourceId)));
            paths.push(path);
        }
        controller.scan(checked.stick + "/PIONEER", library, "");
        tryVerify(() => !controller.busy, 300000, "the check after planting finishes");
        const repairOf = (path) => {
            for (let i = 0; i < model.count; ++i) {
                const index = model.index(i, 0);
                if (model.data(index, kindRole) === "repairable" && String(model.data(index, survivorRole).filePath) === path) {
                    return i;
                }
            }
            return -1;
        };
        const repair = repairOf(paths[0]);
        verify(repair >= 0 && repairOf(paths[1]) >= 0, "precondition: a repair onto each planted file");
        verify(inNoPlaylist(paths[0]) && inNoPlaylist(paths[1]), "precondition: both survivors are in no playlist");
        const staged = (path) => model.data(model.index(repairOf(path), 0), stagedRole);

        // Delete first: the repair onto it is refused, one by one and all
        // at once, and Repair all says how many it left out.
        controller.deleteTracksInNoPlaylist(paths);
        compare(controller.errorMessage, "");
        compare(controller.noPlaylistStagedCount, 2);
        controller.repairOne(repair);
        compare(controller.stagedIssueCount, 0, "the repair is refused while the delete holds its survivor");
        verify(!staged(paths[0]));
        verify(controller.errorMessage.indexOf("is staged for deletion as a track in no playlist, and this repair would "
                                               + "move another copy's playlist entries and cues onto it.") >= 0,
               controller.errorMessage);
        verify(noDashes(controller.errorMessage));
        controller.repairAll();
        verify(!staged(paths[0]) && !staged(paths[1]), "Repair all leaves both out");
        verify(controller.errorMessage.indexOf("2 repairs were not staged. The first of them: \"") === 0,
               controller.errorMessage);
        verify(controller.errorMessage.indexOf("and this repair would move another copy's playlist entries") >= 0,
               controller.errorMessage);
        verify(noDashes(controller.errorMessage));
        controller.unstageTracksInNoPlaylist();
        for (let i = 0; i < model.count; ++i) {
            if (model.data(model.index(i, 0), stagedRole)) {
                controller.unstageIssue(i);
            }
        }
        compare(controller.stagedIssueCount, 0);

        // Repair first: the delete leaves its survivor out and says why.
        controller.repairOne(repair);
        compare(controller.errorMessage, "");
        verify(staged(paths[0]));
        controller.deleteTracksInNoPlaylist(paths);
        compare(controller.noPlaylistStagedCount, 1, "only the other survivor is staged");
        verify(!controller.noPlaylistTracks.find((t) => t.filePath === paths[0]).staged);
        verify(controller.noPlaylistTracks.find((t) => t.filePath === paths[1]).staged);
        verify(controller.statusMessage.indexOf("is not deleted: a repair staged on this page changes its rows. Save that "
                                                + "first, then delete it.") >= 0, controller.statusMessage);
        verify(noDashes(controller.statusMessage));
    }
}
