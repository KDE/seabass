// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

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
    Component {
        id: realControllerComponent
        FullBackupsController {}
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
        compare(findChild(page, "backupsList").count, 3);

        var main = row(page, 0);
        compare(findChild(main, "backupTitle").text, "MAIN");
        // Named like its stick: nothing to add beside the title.
        compare(findChild(main, "backupStickLabel").visible, false);
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
        compare(findChild(broken, "backupTitle").text, "BROKEN");
        compare(findChild(broken, "backupError").visible, true);
        compare(findChild(broken, "browseButton").enabled, false, "an unreadable backup cannot be browsed");
        compare(findChild(broken, "deleteButton").enabled, true, "but it can be deleted");
        saveScreenshot(page, "manage-backups");
    }

    // A backup copied under a new name keeps its old stick's label inside:
    // SHAKEDOWN_8.zip is TESTRIG_2's backup, renamed for round 8. The file
    // is what is picked, so it is the title; two such rows used to read
    // "TESTRIG_2" twice, with the file name only in a tooltip.
    function test_theFileNameIsTheTitle() {
        var list = backups();
        list.push({archivePath: "/home/u/Backups/SHAKEDOWN_8.zip", fileName: "SHAKEDOWN_8.zip", error: "",
                   label: "TESTRIG_2", identifier: "uuid-rig", status: "complete", createdAt: daysAgo(3),
                   bytes: 1300917097, entries: 1200, trackCount: 156, playlistCount: 4, isCurrentStick: false});
        var page = makePage(makeController({backups: list}), {});
        var copy = row(page, 3);
        compare(findChild(copy, "backupTitle").text, "SHAKEDOWN_8");
        compare(findChild(copy, "backupStickLabel").visible, true);
        compare(findChild(copy, "backupStickLabel").text, "from TESTRIG_2");
        saveScreenshot(page, "manage-backups-renamed");
        // And deleting it names the file that goes.
        var dialog = findChild(page, "confirmDeleteDialog");
        dialog.backup = list[3];
        verify(dialog.headline.indexOf("This permanently deletes SHAKEDOWN_8 (") === 0, dialog.headline);
        verify(dialog.headline.indexOf("a full backup of TESTRIG_2") > 0, dialog.headline);
    }

    // The folder reaches the controller from the page, after this stick's
    // archive: set the other way round, the controller listed once without
    // knowing it and again with it.
    function test_handsTheFolderOnAfterTheCurrentArchive() {
        var page = makePage(makeController({backupDirectory: ""}),
                            {backupDirectory: "/home/u/Other Backups", currentArchivePath: "/home/u/Other Backups/MAIN.zip"});
        compare(page.controller.currentArchivePath, "/home/u/Other Backups/MAIN.zip");
        compare(page.controller.backupDirectory, "/home/u/Other Backups");
        page.backupDirectory = "/mnt/backups";
        compare(page.controller.backupDirectory, "/mnt/backups", "a changed folder is handed on too");
    }

    // The real controller: handing over this stick's archive before the
    // folder starts no listing (there is nothing to list yet), and the
    // folder then starts exactly one.
    function test_realControllerListsOnceTheFolderIsKnown() {
        var controller = createTemporaryObject(realControllerComponent, testCase);
        var spy = createTemporaryObject(spyComponent, testCase, {target: controller, signalName: "busyChanged"});
        controller.currentArchivePath = "/nonexistent/Backups/MAIN.zip";
        compare(controller.listing, false, "no folder yet: nothing to list");
        compare(spy.count, 0);
        controller.backupDirectory = "/nonexistent/Backups";
        // Started and finished: one busyChanged each. A missing folder lists
        // in no time, so `listing` itself may already be false here.
        tryCompare(spy, "count", 2, 5000);
        compare(controller.listing, false);
        wait(50);
        compare(spy.count, 2, "one listing, not a second one queued behind it");
        compare(controller.backups.length, 0);
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
        var media = {sticks: [{isBrowsedBackup: true, isFolder: false, mounted: true, mountPoint: "/cache/main",
                               label: "MAIN", libraryId: "uuid-main"},
                              {isBrowsedBackup: false, isFolder: false, mounted: true, mountPoint: "/media/SPARE",
                               label: "SPARE", libraryId: "uuid-spare"}]};
        var page = makePage(makeController(), {mediaController: media});
        compare(page.controller.openArchivePaths, ["/home/u/Backups/MAIN.zip"]);
        compare(findChild(row(page, 0), "deleteButton").enabled, false);
        compare(findChild(row(page, 1), "deleteButton").enabled, true);
    }

    // Restore is offered per backup, and aims at the drive that backup
    // came from when it is plugged in -- matched by identifier, not by
    // name, so two sticks called MAIN cannot be confused for each other.
    function test_restoreAimsAtTheStickTheBackupCameFrom() {
        var media = {sticks: [{isBrowsedBackup: false, isFolder: false, mounted: true,
                               mountPoint: "/media/SPARE", label: "SPARE", libraryId: "uuid-spare"},
                              {isBrowsedBackup: false, isFolder: false, mounted: true,
                               mountPoint: "/media/MAIN", label: "MAIN", libraryId: "uuid-main"}]};
        var page = makePage(makeController(), {mediaController: media, stickLabel: "MAIN"});
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreRequested"});

        mouseClick(findChild(row(page, 1), "restoreButton"));
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/home/u/Backups/SPARE.zip");
        compare(spy.signalArguments[0][1], "/media/SPARE", "SPARE's backup aims at SPARE, not at the open stick");
        compare(spy.signalArguments[0][2], "SPARE");
    }

    // A backup whose stick is not plugged in falls back to the stick this
    // page was opened for, and to nothing at all when there is none: a
    // whole-drive write is not something to aim at a guess.
    function test_restoreFallsBackAndThenGivesUp() {
        var media = {sticks: [{isBrowsedBackup: false, isFolder: false, mounted: true,
                               mountPoint: "/media/MAIN", label: "MAIN", libraryId: "uuid-main"}]};
        var page = makePage(makeController(), {mediaController: media, stickLabel: "MAIN"});
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreRequested"});

        // SPARE is not plugged in, so the open stick is the fallback.
        mouseClick(findChild(row(page, 1), "restoreButton"));
        compare(spy.signalArguments[0][1], "/media/MAIN");

        var alone = makePage(makeController(), {mediaController: {sticks: []}});
        var spyAlone = createTemporaryObject(spyComponent, testCase, {target: alone, signalName: "restoreRequested"});
        mouseClick(findChild(row(alone, 1), "restoreButton"));
        compare(spyAlone.signalArguments[0][1], "", "no drive plugged in: let the restore page ask");
    }

    // An unreadable backup has nothing to restore from, the same reason
    // it cannot be browsed.
    function test_anUnreadableBackupCannotBeRestored() {
        var page = makePage(makeController());
        compare(findChild(row(page, 2), "restoreButton").enabled, false);
        compare(findChild(row(page, 0), "restoreButton").enabled, true);
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

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from a hub on
    // top of Home). The breadcrumb reads the stack's depth to decide
    // whether its middle segment is a link, so a page on its own cannot
    // show that.
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

    function saveCrumbShot(page, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/crumb-" + name + ".png");
        }
    }

    // From a stick's Backups page it names both; from Home's menu,
    // neither, since it is then about every stick's backups.
    function test_breadcrumbFromAStickNamesTheStickAndTheHub() {
        const page = pushOnStack(2, {controller: makeController(), stickLabel: "MAIN",
                                     currentArchivePath: "/home/u/Backups/MAIN.zip"});
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "MAIN");
        compare(crumb.middle, "Backups");
        verify(crumb.middleIsLink);
        compare(crumb.title, "Manage Backups");
        saveCrumbShot(page, "manage-backups-from-hub");
    }

    function test_breadcrumbFromHomeNamesNoStick() {
        const page = pushOnStack(1, {controller: makeController()});
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "");
        compare(crumb.middle, "");
        compare(crumb.title, "Manage Backups");
    }
}
