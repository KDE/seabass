// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// BackupsPage.qml (Manage Backups) headless, with a plain object standing
// in for FullBackupsController: what each backup row says, that delete
// asks first and is off for a backup open for browsing, and that Browse
// hands the archive on.
TestCase {
    id: testCase
    name: "BackupsPage"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        BackupsPage { width: 980; height: 680 }
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }

    function daysAgo(days) {
        var d = new Date();
        d.setDate(d.getDate() - days);
        return d.toISOString();
    }

    function backups() {
        return [
            {archivePath: "/home/u/Backups/MAIN.zip", fileName: "MAIN.zip", error: "", label: "MAIN", identifier: "uuid-main",
             status: "complete", createdAt: daysAgo(3), bytes: 25 * 1024 * 1024 * 1024, entries: 1500,
             trackCount: 1161, playlistCount: 24, isCurrentStick: true},
            {archivePath: "/home/u/Backups/SPARE.zip", fileName: "SPARE.zip", error: "", label: "SPARE", identifier: "uuid-spare",
             status: "partial-cancelled", createdAt: daysAgo(40), bytes: 3 * 1024 * 1024 * 1024, entries: 200,
             trackCount: -1, playlistCount: -1, isCurrentStick: false},
            {archivePath: "/home/u/Backups/BROKEN.zip", fileName: "BROKEN.zip", error: "no manifest", label: "", identifier: "",
             status: "complete", createdAt: "", bytes: 1024, entries: 0, trackCount: -1, playlistCount: -1, isCurrentStick: false},
        ];
    }

    function makeController(overrides) {
        var c = {
            backupDirectory: "/home/u/Backups",
            currentArchivePath: "",
            openArchivePaths: [],
            backups: backups(),
            totalBytes: 28 * 1024 * 1024 * 1024,
            listing: false,
            deleting: false,
            errorMessage: "",
            statusMessage: "",
            calls: [],
            refresh: function() { this.calls.push("refresh"); },
            deleteBackup: function(path) { this.calls.push("delete:" + path); },
            browsedArchiveFor: function(root) { return root === "/cache/main" ? "/home/u/Backups/MAIN.zip" : ""; },
            isOpen: function(path) { return this.openArchivePaths.indexOf(path) >= 0; },
        };
        for (var key in (overrides || {})) {
            c[key] = overrides[key];
        }
        return c;
    }

    function makePage(controller, props) {
        var all = {controller: controller};
        for (var key in (props || {})) {
            all[key] = props[key];
        }
        var page = createTemporaryObject(pageComponent, testCase, all);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    function row(page, index) {
        var list = findChild(page, "backupsList");
        list.positionViewAtIndex(index, 0);
        return findChild(list, "backupRow" + index);
    }

    function saveScreenshot(page, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        grabImage(page).save(screenshotDir + "/" + name + ".png");
    }

    function test_rowsSayWhatEachBackupIs() {
        // The page holds its own copy of a plain-object controller, so
        // what it did is read back from page.controller.
        var page = makePage(makeController(), {stickLabel: "MAIN", currentArchivePath: "/home/u/Backups/MAIN.zip"});
        compare(page.controller.currentArchivePath, "/home/u/Backups/MAIN.zip", "the page hands this stick's backup on");
        verify(page.controller.calls.indexOf("refresh") >= 0);
        compare(findChild(page, "backupsList").count, 3);

        var main = row(page, 0);
        compare(findChild(main, "backupTitle").text, "MAIN");
        compare(findChild(main, "currentStickBadge").visible, true);
        var details = findChild(main, "backupDetails").text;
        verify(details.indexOf("Backed up 3 days ago") === 0, details);
        verify(details.indexOf("25.0 GiB") > 0, details);
        // Grouping follows the locale ("1,161" or "1.161"), so only the tail is fixed.
        verify(details.indexOf("161 tracks") > 0, details);
        verify(details.indexOf("24 playlists") > 0, details);
        compare(findChild(main, "backupStatus").visible, false);

        var spare = row(page, 1);
        compare(findChild(spare, "currentStickBadge").visible, false);
        verify(findChild(spare, "backupDetails").text.indexOf("tracks") < 0, "no fingerprint: no track count, not zero");
        compare(findChild(spare, "backupStatus").visible, true);
        verify(findChild(spare, "backupStatus").text.indexOf("Incomplete") === 0);

        var broken = row(page, 2);
        compare(findChild(broken, "backupTitle").text, "BROKEN.zip");
        compare(findChild(broken, "backupError").visible, true);
        compare(findChild(broken, "browseButton").enabled, false, "an unreadable backup cannot be browsed");
        compare(findChild(broken, "deleteButton").enabled, true, "but it can be deleted");
        saveScreenshot(page, "manage-backups");
    }

    function test_deleteAsksFirst() {
        var page = makePage(makeController());
        mouseClick(findChild(row(page, 1), "deleteButton"));
        var dialog = findChild(page, "confirmDeleteDialog");
        tryCompare(dialog, "opened", true);
        verify(dialog.headline.indexOf("SPARE") > 0, dialog.headline);
        compare(page.controller.calls.filter(function(c) { return c.indexOf("delete:") === 0; }).length, 0,
                "nothing is deleted before the confirmation");
        saveScreenshot(page, "manage-backups-delete-confirm");
        dialog.accept();
        verify(page.controller.calls.indexOf("delete:/home/u/Backups/SPARE.zip") >= 0, page.controller.calls.join(","));
    }

    function test_aBackupOpenForBrowsingCannotBeDeleted() {
        var media = {sticks: [{isBrowsedBackup: true, mountPoint: "/cache/main"},
                              {isBrowsedBackup: false, mountPoint: "/media/SPARE"}]};
        var page = makePage(makeController(), {mediaController: media});
        compare(page.controller.openArchivePaths, ["/home/u/Backups/MAIN.zip"]);
        compare(findChild(row(page, 0), "deleteButton").enabled, false);
        compare(findChild(row(page, 1), "deleteButton").enabled, true);
    }

    function test_browseWaitsForTheListing() {
        var page = makePage(makeController({listing: true}));
        compare(findChild(row(page, 0), "browseButton").enabled, false,
                "leaving mid-listing would block on it, so Browse waits");
    }

    function test_browseHandsTheArchiveOn() {
        var page = makePage(makeController());
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "browseRequested"});
        mouseClick(findChild(row(page, 0), "browseButton"));
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/home/u/Backups/MAIN.zip");
    }

    function test_emptyFolder() {
        var page = makePage(makeController({backups: [], totalBytes: 0}));
        compare(findChild(page, "emptyLabel").visible, true);
        verify(findChild(page, "summaryLabel").text.indexOf("0 backups") === 0);
        saveScreenshot(page, "manage-backups-empty");
    }
}
