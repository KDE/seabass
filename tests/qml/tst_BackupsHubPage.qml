// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// BackupsHubPage.qml headless with a fake advisor: the Update Stick card
// (from a peer stick -> clone page, from the disk backup -> restore page)
// and the deprecated badge on Manage Backups. Restore a Stick Backup and
// Local Cue Backup moved to a general block on Home (see
// tst_StickListPage.qml) -- neither card lives here anymore.
TestCase {
    id: testCase
    name: "BackupsHubPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        BackupsHubPage { width: 880; height: 680 }
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    function noSource() {
        return {kind: "none", label: "", mountPoint: "", backupPath: "", modifiedAt: "", enoughSpace: true, detail: "",
                rekordboxPath: "", enginePath: ""};
    }

    function makePage(advice, overrides) {
        var props = {
            stickLabel: "MAIN",
            rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library",
            mountPoint: "/media/MAIN",
            devicePath: "/dev/sdb1",
            appSettingsController: {experimentalFeaturesEnabled: true},
            backupAdvisor: {advice: advice},
        };
        for (var key in (overrides || {})) {
            props[key] = overrides[key];
        }
        var page = createTemporaryObject(pageComponent, testCase, props);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function saveScreenshot(page, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        grabImage(page).save(screenshotDir + "/" + name + ".png");
    }

    function test_readOnlyWhileAnotherInstanceEdits() {
        var registry = {
            lockedByOther: ["lib-main"], calls: [],
            refreshLocks: function() {},
            removeLock: function(id) { this.calls.push("remove:" + id); },
            lockHolder: function(id) { return {hostname: "studio-pc", pid: 4242, startedAtUtc: ""}; },
            libraryIdForPath: function(p) { return "lib-main"; },
        };
        var page = makePage({}, {editRegistry: registry});
        compare(page.lockedByOther, true);
        compare(findChild(page, "manageBackupsCard").readOnly, true);
        compare(findChild(page, "fullStickBackupCard").readOnly, true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "fullStickBackupRequested"});
        mouseClick(findChild(page, "fullStickBackupCard"));
        compare(spy.count, 0);
        tryCompare(findChild(page, "lockedDialog"), "opened", true);
        saveScreenshot(page, "backups-hub-read-only");
    }

    function test_deprecatedBadgeAndNoUpdateSource() {
        var page = makePage({});
        compare(findChild(page, "manageBackupsCard").deprecated, true);
        compare(findChild(page, "updateStickCard").visible, false);
        compare(findChild(page, "restoreCard"), null);
        compare(findChild(page, "localCueCard"), null);
        saveScreenshot(page, "backups-hub");
    }

    function test_updateFromPeerStickOpensTheClonePage() {
        var advice = {};
        advice["/media/MAIN"] = {state: "outdated", detail: "The library has changed since its last backup.",
            cloneSource: noSource(), diverged: true,
            updateSource: {kind: "stick", label: "SPARE", mountPoint: "/media/SPARE", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                           enoughSpace: true, detail: "SPARE holds a newer copy of this library.",
                           rekordboxPath: "/media/SPARE/PIONEER", enginePath: "/media/SPARE/Engine Library"}};
        var page = makePage(advice);
        var card = findChild(page, "updateStickCard");
        compare(card.visible, true);
        verify(card.cardSubtitle.indexOf("⚠") === 0);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "SPARE");
        compare(spy.signalArguments[0][1], "/media/SPARE/PIONEER");
        compare(spy.signalArguments[0][3], "/media/MAIN");
        compare(spy.signalArguments[0][4], "MAIN");
        compare(spy.signalArguments[0][5], true);
        saveScreenshot(page, "backups-hub-update");
    }

    function test_updateFromDiskBackupOpensTheRestorePage() {
        var advice = {};
        advice["/media/MAIN"] = {state: "behind-backup", detail: "The backup holds a newer copy of this library than the stick.",
            cloneSource: noSource(), diverged: false,
            updateSource: {kind: "disk-backup", label: "MAIN", mountPoint: "", backupPath: "/b/MAIN.zip", modifiedAt: "2026-09-06T10:00:00",
                           enoughSpace: true, detail: "The backup holds a newer copy of this library than this stick.",
                           rekordboxPath: "", enginePath: ""}};
        var page = makePage(advice);
        var clone = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        var restore = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        findChild(page, "updateStickCard").clicked();
        compare(clone.count, 0);
        compare(restore.count, 1);
        compare(restore.signalArguments[0][0], "/media/MAIN");
        compare(restore.signalArguments[0][2], "/b/MAIN.zip");
    }

    function test_fullStickBackupHiddenWithoutExperimentalFeatures() {
        var page = makePage({}, {appSettingsController: {experimentalFeaturesEnabled: false}});
        compare(findChild(page, "fullStickBackupCard").visible, false);
        // Not experimental at all -- always there regardless of the flag.
        compare(findChild(page, "manageBackupsCard").visible, true);
    }
}
