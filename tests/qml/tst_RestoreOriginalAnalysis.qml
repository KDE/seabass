// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse's Restore original analysis: the playlist menu's item and the
// dialog that follows, with a QtObject standing in for
// OriginalAnalysisController (same properties, signal and calls). What is
// restored and refused is original_analysis_restore_test's and
// kept_analysis_rules_test's to prove; this is what the menu and the
// dialog show and ask.
TestCase {
    id: testCase
    name: "RestoreOriginalAnalysis"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: fakeControllerComponent
        QtObject {
            id: fake
            property var calls: []
            property bool busy: false
            property string keptCountPlaylist: ""
            property int keptCount: -1
            property var lastRestore: ({})
            property var cueReview: []
            // What cachedKeptCount() answers: -1 for nothing known.
            property int cachedCount: -1
            signal restoreReady()

            function cachedKeptCount(rekordbox, engine, playlist) {
                return playlist === "Warm Up" ? fake.cachedCount : -1;
            }
            function refreshKeptCount(rekordbox, engine, playlist) { fake.calls.push("count:" + playlist); }
            function restoreOriginalAnalysis(rekordbox, engine, playlist) {
                fake.calls.push("restore:" + playlist);
                return true;
            }
            function setMoveCuesBack(changeId, move) {
                fake.calls.push("move:" + changeId + ":" + move);
                return true;
            }
            function setMoveCuesBackForAll(move) {
                fake.calls.push("all:" + move);
                return true;
            }
            function dropStaged() { fake.calls.push("drop"); }
        }
    }

    Component {
        id: menuComponent
        Menu {
            id: menu
            property alias item: restoreItem
            required property var controller
            // The playlist menu's other items, always there.
            MenuItem { text: "Rename" }
            RestoreOriginalAnalysisMenuItem {
                id: restoreItem
                controller: menu.controller
                rekordboxPath: "/stick/PIONEER"
                enginePath: "/stick/Engine Library"
                playlist: "Warm Up"
            }
        }
    }

    Component {
        id: dialogComponent
        Item {
            id: host
            width: 900
            height: 700
            property alias dialog: restoreDialog
            required property var controller
            RestoreOriginalAnalysisDialog {
                id: restoreDialog
                controller: host.controller
            }
        }
    }

    function calledWith(fake, call) {
        return fake.calls.indexOf(call) >= 0;
    }

    function countCalls(fake) {
        return fake.calls.filter(call => call.startsWith("count:")).length;
    }

    function test_menuItem() {
        const fake = createTemporaryObject(fakeControllerComponent, testCase);
        const menu = createTemporaryObject(menuComponent, testCase, {controller: fake});
        verify(menu !== null);
        const item = menu.item;
        compare(countCalls(fake), 0, "nothing asked before the menu opens");
        compare(item.visible, false, "absent before the menu opens");
        compare(item.enabled, false, "and not reachable by the keyboard");

        // Opened with no answer known: asked once, absent for this opening,
        // and an answer that lands while it is open changes nothing.
        menu.open();
        tryCompare(menu, "opened", true);
        compare(countCalls(fake), 1, "asked once for this opening");
        verify(calledWith(fake, "count:Warm Up"));
        compare(item.visible, false, "absent while no answer is known");
        compare(item.enabled, false, "not reachable while absent");
        const heightBefore = menu.height;
        fake.cachedCount = 2;
        fake.keptCountPlaylist = "Warm Up";
        fake.keptCount = 2;
        wait(50);
        compare(item.visible, false, "an answer landing while open does not show it");
        compare(menu.height, heightBefore, "the open menu keeps its height");
        menu.close();
        tryCompare(menu, "visible", false);

        // The next opening goes by the answer at once.
        menu.open();
        tryCompare(menu, "opened", true);
        compare(countCalls(fake), 2, "asked once again for the second opening");
        compare(item.visible, true, "shown the moment the menu opens, the answer cached");
        compare(item.enabled, true);
        compare(item.text, "Restore original analysis (2 track(s))", "the count in the item");
        fake.busy = true;
        compare(item.enabled, false, "disabled while the controller is busy");
        fake.busy = false;
        item.triggered();
        verify(calledWith(fake, "restore:Warm Up"), "triggered, the restore of the playlist starts");
        menu.close();
        tryCompare(menu, "visible", false);

        // Nothing kept any more, or another playlist: absent.
        fake.cachedCount = 0;
        menu.open();
        tryCompare(menu, "opened", true);
        compare(item.visible, false, "absent with nothing kept");
        menu.close();
        tryCompare(menu, "visible", false);
        fake.cachedCount = 3;
        item.playlist = "Peak Time";
        compare(countCalls(fake), 3, "a playlist changed is not asked about until the menu opens");
        menu.open();
        tryCompare(menu, "opened", true);
        verify(calledWith(fake, "count:Peak Time"));
        compare(item.visible, false, "another playlist's answer does not show it");
        menu.close();
    }

    function test_dialogStaged() {
        const fake = createTemporaryObject(fakeControllerComponent, testCase);
        const host = createTemporaryObject(dialogComponent, testCase, {controller: fake});
        const dialog = host.dialog;
        fake.lastRestore = {playlist: "Warm Up", staged: 2, tracks: 3,
                            leftAlone: [{name: "gone.wav", why: "its audio file is not on the stick"}],
                            cueTracks: 1, cuesToMove: 2, autoCueTracks: 0, askedCueTracks: 1,
                            cueFollowThresholdMs: 20, blockedCueTracks: 1, unknownOriginTracks: 1};
        fake.cueReview = [{changeId: "restore:one", subject: "One, Artist", moves: 2, largestMoveMs: 31,
                           moveCues: false, chosen: false}];
        fake.restoreReady();
        tryCompare(dialog, "opened", true);
        verify(dialog.headline.indexOf("2 track(s) will get back the analysis") >= 0, dialog.headline);
        verify(dialog.detailText.indexOf("On 1 track(s) cue points sit on a beat") >= 0, dialog.detailText);
        verify(dialog.detailText.indexOf("On 1 track(s) a cue point stays where it is: another cue point is already "
                                         + "at the place it would go back to.") >= 0, dialog.detailText);
        verify(dialog.detailText.indexOf("On 1 track(s) what goes back is the analysis that was on the stick before "
                                         + "Seabass analysed them again") >= 0, dialog.detailText);
        const leftLabel = findChild(dialog, "restoreLeftAloneLabel");
        verify(leftLabel.visible && leftLabel.text.indexOf("gone.wav: its audio file is not on the stick") >= 0,
               leftLabel.text);

        // The cue switches: one track's, and all of them.
        const list = findChild(dialog, "restoreCueTracks");
        verify(list.visible, "the tracks whose cue points moved are listed");
        tryVerify(() => list.itemAtIndex(0) !== null, 2000, "a row per track whose cue points moved");
        compare(list.count, 1);
        const cueSwitch = findChild(list.itemAtIndex(0), "restoreCueSwitch");
        verify(cueSwitch !== null, "a switch per track whose cue points moved");
        tryVerify(() => cueSwitch.width > 0 && cueSwitch.height > 0, 2000, "the switch is shown");
        compare(cueSwitch.checked, false, "off as the review set it");
        mouseClick(cueSwitch);
        verify(calledWith(fake, "move:restore:one:true"), fake.calls.join(","));
        mouseClick(findChild(dialog, "restoreCuesAllOn"));
        verify(calledWith(fake, "all:true"));
        mouseClick(findChild(dialog, "restoreCuesAllOff"));
        verify(calledWith(fake, "all:false"));

        // Take back drops what was staged and closes.
        const takeBack = findChild(dialog, "restoreTakeBackButton");
        verify(takeBack.visible, "Take back is offered while something is staged");
        mouseClick(takeBack);
        verify(calledWith(fake, "drop"));
        tryCompare(dialog, "opened", false);
    }

    function test_dialogNothingStaged() {
        const fake = createTemporaryObject(fakeControllerComponent, testCase);
        const host = createTemporaryObject(dialogComponent, testCase, {controller: fake});
        const dialog = host.dialog;
        fake.lastRestore = {playlist: "Warm Up", staged: 0, tracks: 1,
                            leftAlone: [{name: "one.wav", why: "Analyse again of it waits to be saved"}],
                            cueTracks: 0, cuesToMove: 0, autoCueTracks: 0, askedCueTracks: 0,
                            cueFollowThresholdMs: 20};
        fake.cueReview = [];
        fake.restoreReady();
        tryCompare(dialog, "opened", true);
        compare(dialog.headline, "No track of \"Warm Up\" can get its original analysis back.");
        verify(dialog.detailText.indexOf("cue point stays") < 0 && dialog.detailText.indexOf("on the stick before") < 0,
               "no line for what is not there: " + dialog.detailText);
        compare(findChild(dialog, "restoreTakeBackButton").visible, false, "nothing to take back");
        compare(findChild(dialog, "restoreCueTracks").visible, false, "no cue switches");
        verify(findChild(dialog, "restoreLeftAloneLabel").text.indexOf("one.wav") >= 0);
        mouseClick(findChild(dialog, "restoreOkButton"));
        tryCompare(dialog, "opened", false);
        verify(!calledWith(fake, "drop"), "OK keeps what is staged");
    }
}
