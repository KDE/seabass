// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Library Health's "Cue Lists" page (#55, #60), driven by a stand-in
// controller.
TestCase {
    id: testCase
    name: "PlaylistSyncPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // Only what the page and its frame (HealthCheckPage) read.
    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property bool playlistsChecked: true
            property string playlistsError: ""
            property int playlistDifferenceCount: playlistDifferences.length
            property var playlistDifferences: [{
                name: "Minimal", missingSomewhere: false, staged: "",
                sides: [
                    {format: "rekordbox", library: "rekordbox", hasPlaylist: true, members: 83, lacking: [],
                     extra: [], notInEveryLibrary: 0},
                    {format: "engine", library: "Engine", hasPlaylist: true, members: 144, lacking: ["Consciousness, Anyma"],
                     extra: [], notInEveryLibrary: 68}],
                references: [
                    {format: "rekordbox", library: "rekordbox", adds: 0, removes: 0, swaps: 7, leftOut: 0},
                    {format: "engine", library: "Engine", adds: 0, removes: 0, swaps: 0, leftOut: 7}]
            }, {
                name: "Skywalk", missingSomewhere: true, staged: "",
                sides: [{format: "rekordbox", library: "rekordbox", hasPlaylist: false, members: 0, lacking: [],
                         extra: [], notInEveryLibrary: 0},
                        {format: "engine", library: "Engine", hasPlaylist: true, members: 12, lacking: [], extra: [],
                         notInEveryLibrary: 0}],
                references: [{format: "engine", library: "Engine", adds: 0, removes: 0, swaps: 0, leftOut: 0}]
            }]
            property int danglingPlaylistEntryCount: 8
            property var danglingPlaylists: [{playlist: "Whaleshark Cooldown", entries: 8}]
            property bool danglingFixStaged: false
            property var calls: []
            function alignPlaylist(name, reference) {
                calls.push("align " + name + " " + reference);
                const list = playlistDifferences.slice();
                list[0] = Object.assign({}, list[0], {staged: reference});
                playlistDifferences = list;
            }
            function unstagePlaylist(name) {
                calls.push("unstage " + name);
                const list = playlistDifferences.slice();
                list[0] = Object.assign({}, list[0], {staged: ""});
                playlistDifferences = list;
            }
            function removeDanglingPlaylistEntries() { danglingFixStaged = true; }
            function unstageDanglingPlaylistEntries() { danglingFixStaged = false; }
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            property int scanCalls: 0
            function scan(a, b, c) { scanCalls++; }
            function cancelScan() {}
            function undoLastOperation() {}
        }
    }

    Component {
        id: pageComponent
        PlaylistSyncPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    function test_differencesAreListedAndAPlaylistFollowsALibrary() {
        const controller = createTemporaryObject(controllerComponent, testCase);
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null);
        waitForRendering(page);
        const summary = findChild(page, "playlistsSummary");
        verify(summary.text.indexOf("2 playlists do not hold the same tracks") === 0, summary.text);
        verify(summary.text.indexOf("\u2014") < 0 && summary.text.indexOf("--") < 0, "no dashes on screen");
        const dangling = findChild(page, "danglingSummary");
        verify(dangling.visible && dangling.text.indexOf("8 Engine playlist entries") === 0, dangling.text);
        verify(dangling.text.indexOf("Whaleshark Cooldown: 8") > 0, dangling.text);
        const rows = findChild(page, "playlistDifferences");
        compare(rows.count, 2);

        // Following rekordbox has work (7 swaps); following Engine has
        // none it could write, so no button for it.
        const follow = findChild(rows.itemAt(0), "match_rekordbox");
        verify(follow !== null && follow.visible, "rekordbox can be followed");
        compare(follow.text, "Follow rekordbox");
        const engineButton = findChild(rows.itemAt(0), "match_engine");
        verify(engineButton === null || !engineButton.visible, "nothing to write by following Engine");
        follow.clicked();
        compare(controller.calls[0], "align Minimal rekordbox");
        waitForRendering(page);
        const note = findChild(rows.itemAt(0), "stagedPlaylistNote");
        verify(note.visible && note.text.indexOf("following rekordbox") >= 0, note.text);
        findChild(rows.itemAt(0), "unstagePlaylistButton").clicked();
        compare(controller.calls[1], "unstage Minimal");

        findChild(page, "removeDanglingButton").clicked();
        compare(controller.danglingFixStaged, true);

        if (screenshotDir) {
            controller.unstageDanglingPlaylistEntries();
            page.width = 900;
            page.height = 700;
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/PlaylistSyncPage.png");
        }
    }
}
