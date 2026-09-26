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
        // lists them in udev's enumeration order, which a replug changes.
        // Round 4 took the first Housekeeping card on the page, RV2's, and
        // found it (rightly) not locked while the lock was on A4-128GB's.
        // So the card is found under the section of the library this test
        // locked, never by title alone: the home shows one stick at a
        // time beside its rail, and Live.cardInRow selects that stick and
        // the card's group (Maintain) first, as a click on the rail would.
        tryVerify(function() { return Live.stickKeys(page).length > 0; }, 30000, "the page lists the sticks");
        let lockedKey = "";
        tryVerify(function() { lockedKey = lockedStickKey(page); return lockedKey.length > 0; }, 30000,
                  "the locked library is in the stick model (" + Live.stickKeys(page).length + " listed)");
        const keys = Live.stickKeys(page);
        console.log("  sticks listed: " + keys.join(", ") + "; locked: " + stickLabel);
        let card = null;
        tryVerify(function() { card = Live.cardInRow(page, lockedKey, "Housekeeping"); return card !== null && card.visible; },
                  30000, "the locked stick's section shows a Housekeeping card");
        tryCompare(card, "readOnly", true, 10000);
        compare(findChild(card, "readOnlyBadge").visible, true);
        compare(Live.cardInRow(page, lockedKey, "Browse Library").readOnly, false);
        // The lock is this library's alone: any other stick's cards stay
        // writable, which the first-card version could never have told.
        keys.filter(function(key) { return key !== lockedKey; }).forEach(function(key) {
            const other = Live.cardInRow(page, key, "Housekeeping");
            verify(other !== null, key + " has a Housekeeping card, shown or not");
            compare(other.readOnly, false, key + "'s Housekeeping card stays writable");
        });
        // Back to the locked stick's card, on screen: a click at the centre
        // of a card outside the window lands nowhere and opens nothing.
        card = Live.cardInRow(page, lockedKey, "Housekeeping");
        waitForRendering(page);
        const inPage = card.mapToItem(page, 0, 0);
        verify(card.visible && inPage.y >= 0 && inPage.y + card.height <= page.height,
               "the card is inside the window (y " + inPage.y + ", height " + card.height + ")");
        shot(page, "live-stick-list-read-only");

        const spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "duplicateTracksHubRequested"});
        mouseClick(card);
        compare(spy.count, 0);
        const dialog = findChild(page, "lockedDialog");
        tryCompare(dialog, "opened", true, 5000);
        verify(findChild(dialog, "holderLabel").text.indexOf(String(holder.hostname)) >= 0);
        shot(page, "live-stick-list-locked-dialog");
        findChild(dialog, "staySafeButton").clicked();
        tryCompare(dialog, "opened", false, 5000);
        EditSessionRegistry.mediaController = null;
    }

    // The key of the locked library's stick, whatever the order the page
    // lists them in: by library id, so however the stick's path was spelled
    // on the command line (a symlink, a trailing slash) it is found.
    // Mounted sticks are keyed by their mount point.
    function lockedStickKey(page) {
        const keys = Live.stickKeys(page);
        for (let i = 0; i < keys.length; ++i) {
            if (keys[i].length > 0 && EditSessionRegistry.libraryIdForPath(keys[i] + "/PIONEER") === libraryId) {
                return keys[i];
            }
        }
        return "";
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
        const dialog = findChild(page, "lockedDialog");
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
