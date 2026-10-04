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
}
