// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// Another instance holds the stick's edit lock: run-live.sh plants a
// cookie owned by a live foreign process before this file runs. The
// first staged change must be refused with the locked-library dialog,
// the stick list must show READ ONLY, and Remove Lock must clear the
// way. Needs SEABASS_LIVE_LOCKED=1 (run-live.sh sets it) so it does not
// run in the plain live pass.
TestCase {
    id: testCase
    name: "LiveLock"
    width: 1100
    height: 900
    visible: true
    when: windowShown

    readonly property string stickRoot: liveStickRoot
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string enginePath: stickRoot + "/Engine Library"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)
    property string libraryId: ""

    Component { id: spyComponent; SignalSpy {} }
    Component { id: settingsPage; SettingsPage { width: 1100; height: 900 } }
    Component { id: stickList; StickListPage { width: 1100; height: 900 } }
    Component { id: mediaController; MediaController {} }
    Component { id: appSettings; AppSettingsController {} }
    Component { id: advisor; BackupAdvisorController {} }

    function init() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        if (!liveLockPlanted) {
            skip("no foreign lock planted (run-live.sh does that)");
        }
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
    }

    function shot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        waitForRendering(item);
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function firstSettableField(ctrl) {
        for (var g = 0; g < ctrl.groups.length; ++g) {
            var fields = ctrl.groups[g].fields;
            for (var f = 0; f < fields.length; ++f) {
                if (fields[f].options.length >= 2 && fields[f].options.indexOf(fields[f].value) >= 0) {
                    return {fileName: fields[f].fileName, label: fields[f].label,
                            other: fields[f].options[(fields[f].options.indexOf(fields[f].value) + 1) % fields[f].options.length]};
                }
            }
        }
        return null;
    }

    function test_01_stickListShowsReadOnly() {
        EditSessionRegistry.refreshLocks();
        compare(EditSessionRegistry.isLockedByOther(libraryId), true);
        verify(EditSessionRegistry.lockedByOther.indexOf(libraryId) >= 0);
        var holder = EditSessionRegistry.lockHolder(libraryId);
        console.log("  lock holder: " + holder.hostname + " pid " + holder.pid);

        var media = createTemporaryObject(mediaController, testCase);
        EditSessionRegistry.mediaController = media;
        var page = createTemporaryObject(stickList, testCase, {
            mediaController: media,
            playbackController: {stop: function() {}},
            appSettingsController: createTemporaryObject(appSettings, testCase),
            backupAdvisor: createTemporaryObject(advisor, testCase),
        });
        // Both test sticks are plugged in during a round, and the page
        // lists them in udev's enumeration order -- which a replug
        // changes. Round 4 took the first Housekeeping card on the page,
        // RV2's, and found it (rightly) not locked while the lock was on
        // A4-128GB's. So the cards are found under the row of the library
        // this test locked, never by title alone; and the row is put on
        // screen first, because a ListView only instantiates the rows
        // near its viewport, and because a click at the centre of a card
        // below a 900 px window lands outside it and opens nothing (the
        // round's first re-run).
        var list = null;
        tryVerify(function() { list = stickListView(page, media); return list !== null && list.count > 0; }, 30000,
                  "the page lists the sticks");
        // The row comes from the model, not from the rows the ListView
        // has instantiated (only those near its viewport), and the list
        // is positioned once: waitForRendering() on a list that does not
        // move waits out a whole frame interval, seconds under offscreen.
        var row = -1;
        tryVerify(function() { row = rowIndexOf(media, list, libraryId); return row >= 0; }, 30000,
                  "the locked library is in the stick model (" + list.count + " listed)");
        list.positionViewAtIndex(row, ListView.Center);
        waitForRendering(list);
        var card = null;
        tryVerify(function() { card = cardFor(page, "Housekeeping", libraryId); return card !== null; }, 30000,
                  "the locked library's row has a Housekeeping card");
        keepInsideList(list, card);
        var rows = cardsWithRows(page, "Housekeeping");
        console.log("  sticks listed: " + rows.map(function(h) { return h.row.label; }).join(", ") + "; locked: " + stickLabel);
        tryCompare(card, "readOnly", true, 10000);
        compare(findChild(card, "readOnlyBadge").visible, true);
        compare(cardFor(page, "Browse Library", libraryId).readOnly, false);
        // The lock is this library's alone: any other stick's cards stay
        // writable, which the first-card version could never have told.
        rows.filter(function(h) { return String(h.row.libraryId) !== libraryId; }).forEach(function(h) {
            compare(h.card.readOnly, false, h.row.label + "'s Housekeeping card stays writable");
        });
        shot(page, "live-stick-list-read-only");

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "duplicateTracksHubRequested"});
        mouseClick(card);
        compare(spy.count, 0);
        var dialog = findChild(page, "lockedDialog");
        tryCompare(dialog, "opened", true, 5000);
        verify(findChild(dialog, "holderLabel").text.indexOf(String(holder.hostname)) >= 0);
        shot(page, "live-stick-list-locked-dialog");
        findChild(dialog, "staySafeButton").clicked();
        tryCompare(dialog, "opened", false, 5000);
        EditSessionRegistry.mediaController = null;
    }

    // The list of sticks on the page: the ListView bound to the media
    // controller's model, whatever other lists the page holds.
    function stickListView(root, media) {
        var found = null;
        function walk(item) {
            if (found !== null || !item) return;
            if (item.positionViewAtIndex !== undefined && item.model === media.sticks) { found = item; return; }
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(root);
        return found;
    }
    // Every card with this title the page has instantiated, with the
    // stick row (StickListPage's delegateRoot, the nearest ancestor that
    // carries a libraryId) it sits in.
    function cardsWithRows(root, title) {
        var all = [];
        function rowOf(item) {
            for (var p = item; p && p !== root; p = p.parent) {
                if (p.libraryId !== undefined) return p;
            }
            return null;
        }
        function walk(item) {
            if (!item) return;
            if (item.cardTitle !== undefined && String(item.cardTitle) === title) {
                var row = rowOf(item);
                if (row !== null) all.push({card: item, row: row});
                return;
            }
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(root);
        return all;
    }
    function cardFor(root, title, libraryId) {
        var hits = cardsWithRows(root, title).filter(function(h) { return String(h.row.libraryId) === libraryId; });
        return hits.length > 0 ? hits[0].card : null;
    }
    // The model row of a library, whether or not the list has built it.
    function rowIndexOf(media, list, libraryId) {
        for (var i = 0; i < list.count; ++i) {
            if (String(media.sticks.get(i).libraryId) === libraryId) return i;
        }
        return -1;
    }
    // A click can only be trusted on a card inside the list's viewport
    // (it clips, below the page header): a card between the two is drawn
    // nowhere and a click on it goes nowhere. A row taller than the
    // viewport gets the card itself centred.
    function keepInsideList(list, card) {
        var inList = card.mapToItem(list, 0, 0);
        if (inList.y < 0 || inList.y + card.height > list.height) {
            var y = card.mapToItem(list.contentItem, 0, 0).y;
            list.contentY = Math.max(0, Math.min(y - (list.height - card.height) / 2, list.contentHeight - list.height));
            waitForRendering(list);
            inList = card.mapToItem(list, 0, 0);
        }
        verify(inList.y >= 0 && inList.y + card.height <= list.height,
               "the card is inside the list's viewport (y " + inList.y + ", height " + card.height
               + ", viewport " + list.height + ")");
    }

    function test_02_firstStageRefusedThenRemoveLock() {
        var page = createTemporaryObject(settingsPage, testCase, {stickLabel: stickLabel, pioneerRoot: rekordboxPath});
        var ctrl = Live.findByType(page, "SettingsController");
        tryCompare(ctrl, "busy", false, 120000);
        var target = firstSettableField(ctrl);
        verify(target !== null);
        var s = EditSessionRegistry.sessionFor(libraryId, stickLabel);
        var refused = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "lockRefused"});
        ctrl.setField(target.fileName, target.label, target.other);
        tryVerify(function() { return refused.count > 0; }, 5000);
        compare(s.pendingCount, 0);
        compare(s.lockHeld, false);
        var dialog = findChild(page, "lockedDialog");
        tryCompare(dialog, "opened", true, 5000);
        shot(page, "live-settings-lock-refused");

        // "Remove Lock": the cookie goes, the next attempt stages.
        findChild(dialog, "removeLockButton").clicked();
        tryCompare(dialog, "opened", false, 5000);
        compare(EditSessionRegistry.isLockedByOther(libraryId), false);
        ctrl.setField(target.fileName, target.label, target.other);
        tryCompare(s, "pendingCount", 1, 5000);
        compare(s.lockHeld, true);
        console.log("  staged after Remove Lock; discarding");
        s.discard();
        tryCompare(s, "pendingCount", 0, 5000);
    }
}
