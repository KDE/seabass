// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// The process guard and the CLI's lock probe, against the real stick.
// run-live.sh starts a fake `rekordbox` process before this file, kills
// it about 25 s in, and runs seabass-cli against the stick while this
// test still holds the edit lock. Needs SEABASS_LIVE_GUARD=1.
TestCase {
    id: testCase
    name: "LiveGuard"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: liveStickRoot
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)

    Component { id: settingsPage; SettingsPage { width: 1100; height: 820 } }
    Component { id: guardComponent; DjSoftwareGuardController {} }
    Component { id: dialogComponent; DjSoftwareRunningDialog {} }

    function init() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        if (!liveGuardRun) {
            skip("not a guard run (run-live.sh sets SEABASS_LIVE_GUARD)");
        }
    }

    function shot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        waitForRendering(item);
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function test_guardBlocksWhileEditingAndCliIsRefused() {
        var guard = createTemporaryObject(guardComponent, testCase);
        compare(guard.blocking, false);
        var page = createTemporaryObject(settingsPage, testCase, {stickLabel: stickLabel, pioneerRoot: rekordboxPath});
        var dialog = createTemporaryObject(dialogComponent, page, {guard: guard});
        var ctrl = Live.findByType(page, "SettingsController");
        tryCompare(ctrl, "busy", false, 120000);
        var target = null;
        for (var g = 0; g < ctrl.groups.length && target === null; ++g) {
            var fields = ctrl.groups[g].fields;
            for (var f = 0; f < fields.length; ++f) {
                if (fields[f].options.length >= 2 && fields[f].options.indexOf(fields[f].value) >= 0) {
                    target = {fileName: ctrl.groups[g].fileName, label: fields[f].label,
                              other: fields[f].options[(fields[f].options.indexOf(fields[f].value) + 1) % fields[f].options.length]};
                    break;
                }
            }
        }
        verify(target !== null);
        var s = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(rekordboxPath), stickLabel);
        ctrl.setField(target.fileName, target.label, target.other);
        tryCompare(s, "lockHeld", true, 5000);
        console.log("  editing; waiting for the guard to see the fake rekordbox (5 s poll)");
        tryCompare(guard, "blocking", true, 15000);
        compare(guard.conflictingSoftware, "rekordbox");
        tryCompare(dialog, "opened", true, 5000);
        compare(guard.dialogOpen, true);
        shot(page, "live-guard-blocking");

        console.log("  blocking; waiting for the fake rekordbox to be killed (1 s poll while the dialog is up)");
        tryCompare(guard, "blocking", false, 60000);
        tryCompare(dialog, "opened", false, 5000);
        console.log("  clear again; holding the lock 25 s for the CLI probe");
        wait(25000);
        compare(s.lockHeld, true);
        s.discard();
        tryCompare(s, "pendingCount", 0, 5000);
    }
}
