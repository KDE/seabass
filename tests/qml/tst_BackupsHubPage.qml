// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// BackupsHubPage.qml headless with a fake advisor: the Update Stick card
// (from a peer stick -> clone page, from the disk backup -> restore page)
// Restore Backup, which puts this stick's own full backup back onto it,
// and Manage Backups, which hands on this stick's own full backup.
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

    // A stick in use can have its own backup put back from here: the card
    // hands on this stick's matched backup, and the drive to restore onto.
    function test_restoreBackupPreselectsThisSticksBackup() {
        var page = makePage({"/media/MAIN": {state: "current", detail: "", backupPath: "/b/MAIN.zip",
                                             matchedBy: "fingerprint", updateSource: noSource()}});
        var card = findChild(page, "restoreBackupCard");
        compare(card.visible, true);
        compare(card.cardTitle, "Restore Backup");
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/media/MAIN");
        compare(spy.signalArguments[0][1], "/dev/sdb1");
        compare(spy.signalArguments[0][2], "/b/MAIN.zip");
        saveScreenshot(page, "backups-hub-restore");
    }

    // Matched only by name: not preselected, since two sticks called the
    // same would hand each other's backup over. The page opens to choose.
    function test_restoreBackupDoesNotGuessByName() {
        var page = makePage({"/media/MAIN": {state: "current", detail: "", backupPath: "/b/MAIN.zip",
                                             matchedBy: "label", updateSource: noSource()}});
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        findChild(page, "restoreBackupCard").clicked();
        compare(spy.signalArguments[0][2], "");
    }

    // An opened folder library has no drive behind it to restore onto.
    function test_restoreBackupNeedsADrive() {
        var page = makePage({}, {devicePath: ""});
        compare(findChild(page, "restoreBackupCard").visible, false);
    }

    function test_manageBackupsIsNoLongerDeprecatedAndNoUpdateSource() {
        var page = makePage({});
        compare(findChild(page, "manageBackupsCard").deprecated, false);
        compare(findChild(page, "manageBackupsCard").experimental, false);
        compare(findChild(page, "updateStickCard").visible, false);
        compare(findChild(page, "restoreCard"), null);
        compare(findChild(page, "localCueCard"), null);
        saveScreenshot(page, "backups-hub");
    }

    // Manage Backups lists every full backup, this stick's first -- but only
    // a backup the advisor really matched to it, not merely the newest one.
    function test_manageBackupsHandsOnThisSticksBackup() {
        var advice = {};
        advice["/media/MAIN"] = {state: "current", matchedBy: "fingerprint", backupPath: "/home/u/Backups/MAIN.zip",
            detail: "", cloneSource: noSource(), updateSource: noSource(), diverged: false};
        var page = makePage(advice);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "manageBackupsRequested"});
        findChild(page, "manageBackupsCard").clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][1], "/home/u/Backups/MAIN.zip");

        advice["/media/MAIN"].matchedBy = "newest";
        var newest = makePage(advice);
        var newestSpy = createTemporaryObject(spyComponent, testCase, {target: newest, signalName: "manageBackupsRequested"});
        findChild(newest, "manageBackupsCard").clicked();
        compare(newestSpy.signalArguments[0][1], "", "the newest backup of anything is not this stick's");

        // Nor is a backup of another stick that merely has the same label.
        advice["/media/MAIN"].matchedBy = "label";
        var byLabel = makePage(advice);
        var labelSpy = createTemporaryObject(spyComponent, testCase, {target: byLabel, signalName: "manageBackupsRequested"});
        findChild(byLabel, "manageBackupsCard").clicked();
        compare(labelSpy.signalArguments[0][1], "", "a label match is not enough to mark a backup as this stick's");
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
        compare(card.cardSubtitleIcon, "dialog-warning");
        compare(card.cardSubtitle, "SPARE holds a newer copy of this library.");
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

    // Every card on this hub graduated from experimental on 2026-09-17, so
    // none of them answers to the flag any more.
    function test_everyCardIsShownWithoutExperimentalFeatures() {
        var advice = {};
        advice["/media/MAIN"] = {state: "outdated", detail: "The library has changed since its last backup.",
            cloneSource: noSource(), diverged: false,
            updateSource: {kind: "disk", label: "MAIN", mountPoint: "", backupPath: "/b/MAIN.zip",
                           modifiedAt: "2026-09-06T10:00:00", enoughSpace: true,
                           detail: "The newest backup is newer than this stick."}};
        var page = makePage(advice, {appSettingsController: {experimentalFeaturesEnabled: false}});
        compare(findChild(page, "fullStickBackupCard").visible, true);
        compare(findChild(page, "manageBackupsCard").visible, true);
        compare(findChild(page, "updateStickCard").visible, true);
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from another
    // page on top of Home). The breadcrumb reads the stack's depth to
    // decide whether its middle segment is a link.
    Component {
        id: crumbStackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: crumbFillerComponent
        Item {}
    }
    function pushOnStack(levelsBelow, props) {
        const stack = createTemporaryObject(crumbStackComponent, testCase);
        for (let i = 0; i < levelsBelow; ++i) {
            stack.push(crumbFillerComponent, {}, StackView.Immediate);
        }
        return stack.push(pageComponent, props, StackView.Immediate);
    }

    // From Home the stick is all that stands between the house and this
    // page; opened from Library Health it names that page too, as the way back,
    // rather than putting the stick's name on a link to it.
    function test_breadcrumb_data() {
        return [
            {tag: "home", below: 1, hubLabel: "", middle: "", link: false},
            {tag: "nested", below: 2, hubLabel: "Library Health", middle: "Library Health", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const props = {
            stickLabel: "MAIN",
            rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library",
            mountPoint: "/media/MAIN",
            devicePath: "/dev/sdb1",
            appSettingsController: {experimentalFeaturesEnabled: true},
            backupAdvisor: {advice: {}},
        };
        props.hubLabel = data.hubLabel;
        const page = pushOnStack(data.below, props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "MAIN");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Backups");
    }
}
