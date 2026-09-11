// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// The stick disappears while a library is being edited. run-live.sh
// unmounts the stick about 10 s into this test and mounts it again
// later (the app's own auto-mount usually beats it). Needs
// SEABASS_LIVE_STICK_PULL=1.
TestCase {
    id: testCase
    name: "LiveStickPull"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: liveStickRoot
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)

    Component { id: settingsPage; SettingsPage { width: 1100; height: 820 } }
    Component { id: mediaComponent; MediaController {} }
    Component { id: removedDialog; StickRemovedDialog {} }

    function init() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        if (!liveStickPullRun) {
            skip("not a stick-pull run (run-live.sh sets SEABASS_LIVE_STICK_PULL)");
        }
    }

    function shot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        waitForRendering(item);
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function test_stickPulledWhileEditing() {
        var media = createTemporaryObject(mediaComponent, testCase);
        EditSessionRegistry.mediaController = media;
        var libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        verify(libraryId.length > 0);
        var page = createTemporaryObject(settingsPage, testCase, {stickLabel: stickLabel, pioneerRoot: rekordboxPath});
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
        var s = EditSessionRegistry.sessionFor(libraryId, stickLabel);
        ctrl.setField(target.fileName, target.label, target.other);
        tryCompare(s, "lockHeld", true, 5000);
        compare(s.stickPresent, true);
        var dialog = createTemporaryObject(removedDialog, page, {session: s});

        console.log("  editing; waiting for the stick to go away");
        // The udev monitor refreshes on its own; a poll on top keeps the
        // test independent of how a plain unmount shows up there.
        var poll = Qt.createQmlObject('import QtQuick; Timer { interval: 1500; repeat: true; running: true }', testCase);
        poll.triggered.connect(function() { media.detect(); });
        tryVerify(function() { return EditSessionRegistry.stickRemovedSession === s; }, 60000);
        compare(s.stickPresent, false);
        tryCompare(dialog, "opened", true, 5000);
        compare(findChild(dialog, "understoodButton").enabled, false);
        shot(page, "live-stick-removed");

        console.log("  gone; waiting for the same stick to come back");
        tryCompare(s, "stickPresent", true, 90000);
        console.log("  back, matched by " + s.stickIdentityStrength);
        compare(s.stickIdentityStrength, "hardware");
        tryCompare(findChild(dialog, "understoodButton"), "enabled", true, 5000);
        shot(page, "live-stick-returned");
        findChild(dialog, "understoodButton").clicked();
        tryCompare(dialog, "opened", false, 5000);
        poll.running = false;
        s.discard();
        tryCompare(s, "pendingCount", 0, 5000);
        EditSessionRegistry.mediaController = null;
    }
}
