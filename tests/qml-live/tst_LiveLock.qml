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
                    return {fileName: ctrl.groups[g].fileName, label: fields[f].label,
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
        var card = null;
        tryVerify(function() { card = findCardByTitle(page, "Housekeeping"); return card !== null; }, 30000);
        tryCompare(card, "readOnly", true, 10000);
        compare(findChild(card, "readOnlyBadge").visible, true);
        compare(findCardByTitle(page, "Browse Library").readOnly, false);
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

    function findCardByTitle(root, title) {
        var found = null;
        function walk(item) {
            if (found !== null || !item) return;
            if (item.cardTitle !== undefined && String(item.cardTitle) === title) { found = item; return; }
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(root);
        return found;
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
