// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../qml-live/LiveHelpers.js" as Live

// StickListPage.qml headless with fake controllers: which cards an empty
// stick and a library stick show once the backup advisor has spoken, and
// what the new clone / update cards request. Also the page's screenshot
// when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickListPage"
    width: 1100
    height: 1000
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        StickListPage { width: 1080; height: 980 }
    }

    function makeStick(overrides) {
        var s = {
            label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1", mounted: true,
            hasRekordbox: true, hasEngine: true, rekordboxPath: "/media/MAIN/PIONEER",
            enginePath: "/media/MAIN/Engine Library", isSdCard: false, isFolder: false,
            isBrowsedBackup: false, libraryId: "lib-main", safeToUnplug: false, hasOneLibrary: false,
        };
        for (var key in overrides) {
            s[key] = overrides[key];
        }
        return s;
    }

    function noSource() {
        return {kind: "none", label: "", mountPoint: "", backupPath: "", modifiedAt: "", enoughSpace: true, detail: "",
                rekordboxPath: "", enginePath: ""};
    }

    function makeAdvice(overrides) {
        var a = {state: "no-backups", matchedBy: "none", backupPath: "", backupLabel: "", backupCreatedAt: "",
                 trackOverlap: -1, cueOverlap: -1, detail: "No backup of this library yet.",
                 cloneSource: noSource(), updateSource: noSource(), diverged: false};
        for (var key in overrides) {
            a[key] = overrides[key];
        }
        return a;
    }

    function makePage(sticks, advice, overrides) {
        var props = {
            mediaController: {sticks: sticks, errorMessage: "", busy: false, busyDevicePath: "", calls: [],
                              mountStick: function(d) { this.calls.push("mount:" + d); },
                              unmountStick: function(d) { this.calls.push("unmount:" + d); },
                              openFolder: function(p) { this.calls.push("openFolder:" + p); return ""; },
                              closeFolder: function(p) { this.calls.push("closeFolder:" + p); },
                              openBackup: function(p) { this.calls.push("openBackup:" + p); return ""; }},
            playbackController: {stop: function() {}},
            appSettingsController: fakeAppSettings(),
            backupAdvisor: {advice: advice, calls: [],
                            assess: function(l, m, r, e) { this.calls.push("assess:" + m); },
                            reassessAll: function() { this.calls.push("reassessAll"); },
                            forget: function(m) { this.calls.push("forget:" + m); }},
        };
        for (var key in (overrides || {})) {
            props[key] = overrides[key];
        }
        var page = createTemporaryObject(pageComponent, testCase, props);
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    // Rows and cards by the objectNames StickListPage gives them, shared
    // with the live tests: one place that knows how the page is built.
    function findCard(page, mountPoint, title) {
        return Live.cardInRow(page, mountPoint, title);
    }
    function findRowObject(page, mountPoint, objectName) {
        return Live.objectInRow(page, mountPoint, objectName);
    }

    function saveScreenshot(page, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        var image = grabImage(page);
        image.save(screenshotDir + "/" + name + ".png");
    }

    // What StickListPage reads and calls on the real AppSettingsController.
    function fakeAppSettings() {
        return {
            experimentalFeaturesEnabled: true, stickBackupDirectory: "/tmp",
            toLocalFileUrl: function(p) { return "file://" + p; },
            localPathFromUrl: function(u) { return u.replace(/^file:\/\//, ""); },
        };
    }

    function fakeEditRegistry(lockedIds) {
        return {
            lockedByOther: lockedIds, calls: [],
            refreshLocks: function() { this.calls.push("refresh"); },
            removeLock: function(id) { this.calls.push("remove:" + id); },
            lockHolder: function(id) { return {hostname: "studio-pc", pid: 4242, startedAtUtc: "2026-09-06T10:00:00Z"}; },
            libraryIdForPath: function(p) { return "lib-main"; },
            // What the real registry always has; tests override as needed.
            hasSession: function(id) { return false; },
            sessionFor: function(id) { return null; },
            closeSession: function(id) { this.calls.push("closeSession:" + id); },
        };
    }

    // Another instance holds the library's edit lock: every card that
    // would change the library is read-only and explains itself when
    // clicked; Browse stays a plain card.
    function test_readOnlyCardsWhileAnotherInstanceEdits() {
        var page = makePage([makeStick({})], {}, {editRegistry: fakeEditRegistry(["lib-main"])});
        var housekeeping = findCard(page, "/media/MAIN", "Housekeeping");
        var browse = findCard(page, "/media/MAIN", "Browse Library");
        verify(housekeeping !== null && browse !== null);
        compare(housekeeping.readOnly, true);
        compare(browse.readOnly, false);
        compare(findChild(housekeeping, "readOnlyBadge").visible, true);
        compare(findChild(browse, "readOnlyBadge").visible, false);

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "duplicateTracksHubRequested"});
        mouseClick(housekeeping);
        compare(spy.count, 0);
        var dialog = findChild(page, "lockedDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.libraryId, "lib-main");
        verify(findChild(dialog, "holderLabel").text.indexOf("studio-pc") >= 0);
        saveScreenshot(page, "stick-list-read-only");

        findChild(dialog, "removeLockButton").clicked();
        tryCompare(dialog, "opened", false);
        tryCompare(dialog, "visible", false);  // the modal overlay eats clicks until the exit is over
        compare(page.editRegistry.calls.indexOf("remove:lib-main") >= 0, true);

        // No lock: the same card opens the feature.
        page.editRegistry = fakeEditRegistry([]);
        compare(housekeeping.readOnly, false);
        mouseClick(housekeeping);
        compare(spy.count, 1);
    }

    function test_everyMountedStickIsAssessed() {
        var page = makePage([makeStick({}), makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                                                       hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""})], {});
        compare(page.backupAdvisor.calls.indexOf("assess:/media/MAIN") >= 0, true);
        compare(page.backupAdvisor.calls.indexOf("assess:/media/SPARE") >= 0, true);
    }

    // USB Stick Performance needs no library: an empty mounted stick gets
    // the card, and it carries the mount point the page measures at.
    function test_emptyStickCanBeMeasured() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/SPARE"] = makeAdvice({state: "no-backups"});
        var page = makePage([spare], advice);
        var card = findCard(page, "/media/SPARE", "USB Stick Performance");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.enabled, true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "stickPerformanceRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "SPARE");
        compare(spy.signalArguments[0][3], "/media/SPARE");
    }

    // The card is experimental: with the setting off it must not show,
    // even though its own visible binding replaces ActionCard's default.
    function test_performanceCardHonoursTheExperimentalGate() {
        var settings = fakeAppSettings();
        settings.experimentalFeaturesEnabled = false;
        var page = makePage([makeStick({})], {}, {appSettingsController: settings});
        var card = findCard(page, "/media/MAIN", "USB Stick Performance");
        verify(card === null || !card.visible, "performance card shown with experimental features off");
    }

    function test_emptyStickOffersCloneFromThePeer() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "current", detail: "Backup is up to date."});
        advice["/media/SPARE"] = makeAdvice({state: "restore", backupPath: "/b/MAIN.zip", backupLabel: "MAIN",
            detail: "The newest backup can be restored onto this empty stick.",
            cloneSource: {kind: "stick", label: "MAIN", mountPoint: "/media/MAIN", backupPath: "", modifiedAt: "2026-09-06T10:00:00",
                          enoughSpace: true, detail: "Copy MAIN's library onto this stick.",
                          rekordboxPath: "/media/MAIN/PIONEER", enginePath: "/media/MAIN/Engine Library"}});
        var page = makePage([makeStick({}), spare], advice);
        var card = findCard(page, "/media/SPARE", "Create Backup USB Stick");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.enabled, true);
        compare(card.cardSubtitle, "Copy MAIN's library onto this stick.");
        verify(findCard(page, "/media/MAIN", "Create Backup USB Stick").visible === false);
        verify(findCard(page, "/media/MAIN", "Update Stick") === null);

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "cloneStickRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][1], "/media/MAIN/PIONEER");
        compare(spy.signalArguments[0][2], "/media/MAIN/Engine Library");
        compare(spy.signalArguments[0][3], "/media/SPARE");
        compare(spy.signalArguments[0][4], "SPARE");
        compare(spy.signalArguments[0][5], false);
        saveScreenshot(page, "stick-list-clone");
    }

    function test_cloneCardDisabledWithoutSpace() {
        var spare = makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/SPARE"] = makeAdvice({cloneSource: {kind: "stick", label: "MAIN", mountPoint: "/media/MAIN", backupPath: "",
            modifiedAt: "", enoughSpace: false, detail: "Not enough space on this stick for MAIN's library.",
            rekordboxPath: "/media/MAIN/PIONEER", enginePath: ""}});
        var page = makePage([makeStick({}), spare], advice);
        var card = findCard(page, "/media/SPARE", "Create Backup USB Stick");
        compare(card.visible, true);
        compare(card.enabled, false);
    }

    function test_backupsCardOpensTheHubWithMountAndDevice() {
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "outdated", detail: "The library has changed since its last backup.",
            updateSource: {kind: "stick", label: "SPARE", mountPoint: "/media/SPARE", backupPath: "", modifiedAt: "",
                           enoughSpace: true, detail: "SPARE holds a newer copy of this library.", rekordboxPath: "", enginePath: ""}});
        var page = makePage([makeStick({})], advice);
        var card = findCard(page, "/media/MAIN", "Backups");
        verify(findCard(page, "/media/MAIN", "Update Stick") === null);
        verify(card !== null);
        verify(card.cardSubtitle.indexOf("Newer copy on SPARE") === 0);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "backupsHubRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "MAIN");
        compare(spy.signalArguments[0][3], "/media/MAIN");
        compare(spy.signalArguments[0][4], "/dev/sdb1");
        saveScreenshot(page, "stick-list-update");
    }

    function test_emptyStickRestoreFallsBackToDiskBackupWhenNoPeer() {
        var advice = {};
        advice["/media/MAIN"] = makeAdvice({state: "restore", backupPath: "/b/OLD.zip", backupLabel: "OLD",
            detail: "The newest backup can be restored onto this empty stick."});
        var page = makePage([makeStick({label: "MAIN", hasRekordbox: false, hasEngine: false,
                                        rekordboxPath: "", enginePath: ""})], advice);
        var card = findCard(page, "/media/MAIN", "Create Backup USB Stick");
        verify(card !== null);
        compare(card.cardSubtitle, "Restore OLD's library onto this stick");
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "/media/MAIN");
        compare(spy.signalArguments[0][2], "/b/OLD.zip");
    }

    // A stick's own eject/mount button used to go dark while ANY other
    // stick's task was in flight (mediaController.busy is a single
    // app-wide flag) -- a click then did nothing, worst right after
    // auto-mount started running. It only reflects this row's own task
    // now; a click on it while busy elsewhere just queues.
    function test_ejectButtonStaysUsableWhileAnotherStickIsBusy() {
        var sticks = [makeStick({}), makeStick({label: "SPARE", mountPoint: "/media/SPARE", devicePath: "/dev/sdc1"})];
        var page = makePage(sticks, {}, {mediaController: {sticks: sticks, errorMessage: "", busy: true, busyDevicePath: "/dev/sdc1", calls: [],
                                    mountStick: function(d) { this.calls.push("mount:" + d); },
                                    unmountStick: function(d) { this.calls.push("unmount:" + d); }}});
        var mainButton = findRowObject(page, "/media/MAIN", "ejectButton");
        verify(mainButton !== null);
        compare(mainButton.visible, true);
        compare(mainButton.enabled, true);
        mainButton.clicked();
        compare(page.mediaController.calls.indexOf("unmount:/dev/sdb1") >= 0, true);
    }

    function test_generalBackupsBlockRequestsWithNoStick() {
        // With no stick in, which is when this block is shown at all --
        // see test_theNoStickToolsStepAsideOnceAStickIsIn.
        var page = makePage([], {});
        var restoreCard = findChild(page, "generalRestoreCard");
        verify(restoreCard !== null);
        // Local Cue Backup is gone: Metadata Backup and Restore replaced it.
        compare(findChild(page, "generalLocalCueCard"), null);

        // The block holds one card now that Local Cue Backup is gone; worth a
        // picture, because a two-column grid with one card in it is exactly
        // the kind of layout no assertion here would call wrong.
        waitForRendering(page);
        saveScreenshot(page, "stick-list-no-stick");

        var restoreSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        restoreCard.clicked();
        compare(restoreSpy.count, 1);
        compare(restoreSpy.signalArguments[0][0], "");
    }

    // The card is visible unconditionally for a blank stick (it is also
    // how you restore a backup file that never was in the default
    // directory), so its wording carries the whole burden of being
    // honest about whether one was actually found. Reported as "Seabass
    // offers to restore a backup ... but we don't have one": a blank
    // stick with an empty backup directory used to get the same "Put one
    // of your stick backups onto this empty stick" text as a stick with
    // a real match, phrased as though a backup were known to exist.
    function test_emptyStickWithNoBackupsGetsHonestRestoreCardText() {
        var empty = makeStick({label: "BLANK", mountPoint: "/media/BLANK", devicePath: "/dev/sdd1",
                               hasRekordbox: false, hasEngine: false, rekordboxPath: "", enginePath: ""});
        var advice = {};
        advice["/media/BLANK"] = makeAdvice({});  // default state: "no-backups"
        var page = makePage([empty], advice);
        var card = findCard(page, "/media/BLANK", "Create Backup USB Stick");
        verify(card !== null);
        compare(card.visible, true);
        compare(card.cardSubtitle.toLowerCase().indexOf("one of your"), -1);
        compare(card.cardSubtitle, "No known stick backups yet -- browse for a backup file to restore");

        var restore = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        card.clicked();
        compare(restore.count, 1);
        compare(restore.signalArguments[0][0], "/media/BLANK");
        // No specific match: the restore page opens to browse, not to a
        // preselected archive.
        compare(restore.signalArguments[0][2], "");
    }

    // A library opened from an ordinary folder (a restored stick backup,
    // or a copy on an internal disk) is listed like a stick and offers
    // the same library cards -- but the actions that need a real drive
    // behind them are gone, because there is not one.
    function test_folderLibraryListsWithoutDeviceOnlyActions() {
        var folder = makeStick({
            label: "restored-backup", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-abc123",
            rekordboxPath: "/home/dj/restored/PIONEER",
            enginePath: "/home/dj/restored/Engine Library",
        });
        var page = makePage([folder], makeAdvice({state: "no-backups"}),
                            {appSettingsController: fakeAppSettings()});

        // The library is reachable: the ordinary cards are all there.
        verify(findCard(page, "/home/dj/restored", "Browse Library") !== null);
        verify(findCard(page, "/home/dj/restored", "Housekeeping") !== null);
        verify(findCard(page, "/home/dj/restored", "Sync Cue Points") !== null);

        // Formatting would erase a drive this row does not have.
        var format = findCard(page, "/home/dj/restored", "Format USB Stick");
        verify(format === null || !format.visible);

        // Eject is replaced by "remove from this list", which touches
        // nothing on disk.
        var eject = findRowObject(page, "/home/dj/restored", "ejectButton");
        verify(eject === null || !eject.visible);
        var close = findRowObject(page, "/home/dj/restored", "closeFolderButton");
        verify(close !== null);
        compare(close.visible, true);
        close.clicked();
        compare(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored") >= 0, true);

        saveScreenshot(page, "stick-list-folder-library");
    }

    // A browsed stick backup is a folder row that must not be written
    // to: its analysis files are still in the archive and its directory
    // is replaced on the next open. Every card that writes is withheld;
    // the ones that only read stay. The Backups card in particular used
    // to be live here, and from this row it targets the very archive
    // being browsed.
    function test_browsedBackupWithholdsEveryWritingCard() {
        var backup = makeStick({
            label: "TOURSTICK", mountPoint: "/home/dj/Seabass/metadata/browsed-backups/folder-abc",
            devicePath: "", isFolder: true, isBrowsedBackup: true, libraryId: "folder-abc",
            rekordboxPath: "/home/dj/Seabass/metadata/browsed-backups/folder-abc/PIONEER",
            enginePath: "/home/dj/Seabass/metadata/browsed-backups/folder-abc/Engine Library",
        });
        var page = makePage([backup], makeAdvice({state: "no-backups"}),
                            {appSettingsController: fakeAppSettings()});
        var mp = backup.mountPoint;
        var reads = ["Browse Library", "Library Statistics", "Metadata Backup"];
        for (var i = 0; i < reads.length; ++i) {
            var card = findCard(page, mp, reads[i]);
            verify(card !== null, reads[i] + " missing");
            compare(card.visible, true, reads[i] + " should stay");
        }
        var writes = ["Backups", "Housekeeping", "Library Health", "Restore Metadata",
                      "Create Engine Library", "Sync Cue Points", "Device Profile", "Format USB Stick"];
        for (var j = 0; j < writes.length; ++j) {
            var w = findCard(page, mp, writes[j]);
            verify(w === null || !w.visible, writes[j] + " must be withheld on a browsed backup");
        }
        saveScreenshot(page, "stick-list-browsed-backup");
    }

    // Closing a folder row is where its unsaved edits would otherwise
    // vanish unseen: the close is refused while the session is dirty,
    // and a clean session's lock is released before the row goes.
    function test_closingAFolderRowRefusesWhileDirtyAndReleasesWhenClean() {
        var folder = makeStick({
            label: "restored", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-r1",
        });
        var registry = fakeEditRegistry([]);
        registry.session = {dirty: true};
        registry.hasSession = function(id) { this.calls.push("hasSession:" + id); return id === "folder-r1"; };
        registry.sessionFor = function(id) { return this.session; };
        registry.closeSession = function(id) { this.calls.push("closeSession:" + id); };
        var page = makePage([folder], makeAdvice({}), {editRegistry: registry});
        // Read the page's own copy, as every other fake in this file is
        // read (page.mediaController.calls): what the page holds is what
        // the handler talked to.
        var reg = page.editRegistry;
        var close = findRowObject(page, "/home/dj/restored", "closeFolderButton");
        verify(close !== null);

        close.clicked();
        verify(reg.calls.indexOf("hasSession:folder-r1") >= 0, "the registry was consulted");
        compare(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored"), -1,
                "a dirty session must not be dropped");
        compare(reg.calls.indexOf("closeSession:folder-r1"), -1, "a dirty session must not be closed");

        reg.session.dirty = false;
        close.clicked();
        verify(reg.calls.indexOf("closeSession:folder-r1") >= 0, "a clean session is closed");
        verify(page.mediaController.calls.indexOf("closeFolder:/home/dj/restored") >= 0);
    }

    // Only one folder is open at a time, so opening another folder or
    // browsing a backup replaces the current one. With staged edits on it
    // that must be refused, as closing it is: otherwise the replaced row
    // raises the stick-removed dialog, whose only live button discards.
    function test_openingAnotherFolderOrBackupRefusesWhileTheCurrentOneIsDirty() {
        var folder = makeStick({
            label: "restored", mountPoint: "/home/dj/restored", devicePath: "",
            isFolder: true, libraryId: "folder-r1",
        });
        var registry = fakeEditRegistry([]);
        registry.session = {dirty: true};
        registry.hasSession = function(id) { return id === "folder-r1"; };
        registry.sessionFor = function(id) { return this.session; };
        registry.closeSession = function(id) { this.calls.push("closeSession:" + id); };
        var page = makePage([folder], makeAdvice({}), {editRegistry: registry});
        var reg = page.editRegistry;
        var folderDialog = findChild(page, "openFolderDialog");
        var backupDialog = findChild(page, "openBackupDialog");
        var error = findChild(page, "openFolderError");
        verify(folderDialog !== null && backupDialog !== null && error !== null);

        folderDialog.accepted();
        backupDialog.accepted();
        compare(page.mediaController.calls.length, 0,
                "nothing may replace a folder with unsaved changes: " + page.mediaController.calls);
        compare(reg.calls.indexOf("closeSession:folder-r1"), -1, "a dirty session must not be closed");
        tryCompare(error, "visible", true);
        error.close();

        reg.session.dirty = false;
        folderDialog.accepted();
        verify(reg.calls.indexOf("closeSession:folder-r1") >= 0, "a clean session is closed first");
        compare(page.mediaController.calls.length, 1);
        compare(page.mediaController.calls[0].indexOf("openFolder:"), 0);
    }

    // A backup is opened to look at its library: once it is open, the page
    // asks for Browse Library on it, with the row it became. The menu's
    // entry and Manage Backups' Browse both come through here.
    function test_openingABackupGoesOnIntoBrowseLibrary() {
        var backup = makeStick({
            label: "PARTY STICK", mountPoint: "/cache/backups/b1", devicePath: "",
            isFolder: true, isBrowsedBackup: true, libraryId: "folder-b1",
            hasRekordbox: true, hasEngine: true,
            rekordboxPath: "/cache/backups/b1/PIONEER", enginePath: "/cache/backups/b1/Engine Library",
        });
        var page = makePage([backup], makeAdvice({}));
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "browseRequested"});
        findChild(page, "openBackupDialog").accepted();
        compare(page.mediaController.calls.filter(function(c) { return c.indexOf("openBackup:") === 0; }).length, 1);
        compare(spy.count, 1, "an opened backup must go on into Browse Library");
        compare(spy.signalArguments[0][0], "PARTY STICK");
        compare(spy.signalArguments[0][1], "/cache/backups/b1/PIONEER");
        compare(spy.signalArguments[0][2], "/cache/backups/b1/Engine Library");

        // One that could not be opened stays on Home, with the reason.
        page.mediaController.openBackup = function(p) { return "That backup could not be read."; };
        page.openBackupArchive("/backups/broken.zip");
        compare(spy.count, 1, "a backup that failed to open must not be browsed");
        tryCompare(findChild(page, "openFolderError"), "visible", true);
    }

    // Backups and folders on this computer sit behind one menu button in
    // the header, where the folder button used to be, rather than as two
    // header buttons and a row of buttons under the list.
    function test_theHomeMenuOffersBackupsAndFolders() {
        var page = makePage([], {});
        var button = findChild(page, "homeMenuButton");
        verify(button !== null, "the menu button must be in the header");
        compare(button.visible, true);
        compare(findChild(page, "openBackupButton"), null, "the separate backup button is gone");
        compare(findChild(page, "openFolderButton"), null, "the separate folder button is gone");
        compare(findChild(page, "browseBackupsRow"), null, "the row under the list is gone");

        var menu = findChild(page, "homeMenu");
        verify(menu !== null, "the button must carry the menu");
        mouseClick(button);
        tryCompare(menu, "opened", true);
        var full = findChild(page, "browseFullBackupItem");
        var manage = findChild(page, "manageBackupsItem");
        var meta = findChild(page, "browseMetadataBackupsItem");
        var folder = findChild(page, "openFolderItem");
        verify(full !== null && manage !== null && meta !== null && folder !== null, "all four entries must be in the menu");
        compare(folder.text, "Open a Library From a Folder…");
        // And the button closes what it opened: a press on it no longer
        // closes the menu only for the click to open it again.
        mouseClick(button);
        tryCompare(menu, "visible", false);
        // Off, not missing, while there is nothing to browse; and wired
        // to the page's request when there is.
        compare(meta.enabled, page.homeBackupsMetadataCount > 0);
        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "metadataBackupRequested"});
        meta.triggered();
        compare(spy.count, 1);
        // Manage Backups works with no stick at all: off while there is
        // no full backup, otherwise straight to the page.
        compare(manage.enabled, page.homeBackupsFullCount > 0);
        var manageSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "manageBackupsRequested"});
        manage.triggered();
        compare(manageSpy.count, 1);
    }

    // Finds the first descendant with `objectName`, anywhere on the page.
    function findByName(page, objectName) {
        return Live.findByObjectName(page, objectName);
    }

    function test_theNoStickToolsStepAsideOnceAStickIsIn() {
        // A card about this computer's own stick backups. With no stick in
        // it is the only thing to do here; with one in it would sit above
        // the thing the page is actually about, taking the top of the
        // screen for the case that is not happening.
        var empty = makePage([], {});
        var tools = findByName(empty, "noStickBackupTools");
        verify(tools !== null, "the no-stick tools must exist");
        compare(tools.visible, true, "and must be shown when no stick is in");

        var withStick = makePage([makeStick({})], {"/media/MAIN": makeAdvice({})});
        compare(findByName(withStick, "noStickBackupTools").visible, false,
                "and must step aside once a stick is in");
    }

    function test_anOpenedFolderIsNotAStick() {
        // The tools are for "no stick plugged in", and a folder someone
        // opened from disk is not one. Hiding them for a folder row
        // would take away the only route to them in exactly the session
        // where there is no stick to offer instead.
        var page = makePage([makeStick({label: "COPY", mountPoint: "/home/sebas/copy", isFolder: true,
                                        devicePath: ""})],
                            {});
        compare(findByName(page, "noStickBackupTools").visible, true,
                "an opened folder must not count as a stick being in");
    }

    function test_theHeaderIconsAreBundledAndTheHeartIsFilled() {
        var page = makePage([], {});
        var menu = findByName(page, "homeMenuButton");
        verify(menu !== null, "the menu button must exist");
        var donate = findByName(page, "donateButton");
        verify(donate !== null, "the donate button must exist");
        // Drawn, not a theme icon: Breeze's heart ("love") is an outline.
        var heart = findByName(page, "donateHeart");
        verify(heart !== null, "the donate button must draw the filled heart");
        compare(donate.contentItem, heart);
        compare(heart.color, Qt.color("#aa0000"));
        // Every icon in the header one size, the menu's included -- and the
        // heart DRAWN at it, not merely asking for it: a button stretches
        // its contentItem, and under KDE's style the heart filled the button.
        verify(page.headerIconSize > 0);
        compare(menu.icon.width, page.headerIconSize);
        compare(menu.icon.height, page.headerIconSize);
        compare(heart.drawnSize, page.headerIconSize);
        var buttons = [{name: "homeMenuButton", icon: "application-menu"}, {name: "aboutButton", icon: "help-about"},
                       {name: "preferencesButton", icon: "configure"}];
        for (var i = 0; i < buttons.length; ++i) {
            var button = findByName(page, buttons[i].name);
            verify(button !== null, buttons[i].name + " must exist");
            // A bundled Breeze icon, not a theme lookup (nothing to find on
            // Windows or macOS), flat in the text colour like the rest.
            compare(button.icon.name, "", buttons[i].name + " must not look the icon up in the theme");
            compare(button.icon.source.toString(), Theme.iconUrl(buttons[i].icon));
            compare(button.icon.color, Theme.text, buttons[i].name + " must be tinted flat");
            compare(button.icon.width, page.headerIconSize, buttons[i].name + " must be the menu icon's size");
            compare(button.icon.height, page.headerIconSize);
        }
    }

    // The stick card's actions keep a usable width: three columns where
    // the page has room, then two, then one as the window narrows.
    function test_theActionGridNarrowsWithThePage() {
        var probe = makePage([], {});
        var card = probe.minimumCardWidth;
        verify(card > 0);
        // Room for that many cards and the 12 px between them, plus the page
        // and card margins, with some to spare but never a card's worth.
        var sizes = [{width: card * 3 + 24 + 120, columns: 3}, {width: card * 2 + 12 + 120, columns: 2},
                     {width: card + 120, columns: 1}];
        for (var i = 0; i < sizes.length; ++i) {
            var page = makePage([makeStick({})], {"/media/MAIN": makeAdvice({})}, {width: sizes[i].width});
            var grid = findByName(page, "actionGrid");
            verify(grid !== null, "the action grid must exist");
            compare(grid.columns, sizes[i].columns, "at a page " + Math.round(sizes[i].width) + " px wide");
        }
    }

    function test_theHeaderCarriesTheBrandRatherThanTheWordHome() {
        // This is the one page you arrive at rather than navigate to, so
        // "Home" named the position rather than the thing.
        var page = makePage([], {});
        var name = findByName(page, "brandName");
        var slogan = findByName(page, "brandSlogan");
        verify(name !== null && slogan !== null, "the brand lockup must be in the header");
        compare(name.text, "Seabass");
        compare(slogan.text, "Your DJ Toolbox");
        // The slogan is the subtitle of the pair. At the same size they
        // read as two competing titles.
        verify(slogan.font.pointSize < name.font.pointSize,
               "the slogan must be a clear step smaller than the name");
        // A wordmark: regular weight, larger than a page title.
        compare(name.font.weight, Font.Normal, "the name is not bold");
        compare(name.font.pointSize, Theme.titleLarge);
        // Beside the name, on its baseline, rather than under it.
        verify(slogan.x >= name.x + name.width, "the slogan sits to the right of the name");
        compare(Math.round(slogan.y + slogan.baselineOffset), Math.round(name.y + name.baselineOffset),
                "the slogan shares the name's baseline");
    }

    function test_aLongMountPointElidesInsteadOfPushingTheCardWide() {
        // The label already asked to elide and could not: a Text's
        // Layout.minimumWidth defaults to its implicit width, so it had
        // a floor at full natural size and the row overflowed instead.
        //
        // A narrow page, deliberately: at this file's usual 1080px width
        // there is comfortably more than enough room for this label's
        // implicit width regardless of platform (measured 518px on
        // Windows; whatever it measures elsewhere, it is nowhere near
        // 1080 minus the icon column and the size label beside it), so
        // nothing forces the label to shrink and the assertion below
        // fails not because the elide mechanism is broken but because
        // this test never actually ran it out of room. A page this
        // narrow leaves stickPathLabel's row well under 518px on any
        // reasonable font metrics, which is what actually exercises it.
        var page = makePage([makeStick({
            label: "LONGONE",
            mountPoint: "/run/media/sebas/a-very-long-mount-point-name-that-will-not-fit-on-one-card-line"
        })], {}, {width: 480, height: 980});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        compare(label.elide, Text.ElideMiddle);
        // The observable, not the layout property that produces it: the
        // label is narrower than the text it was given, which is only
        // possible if it was allowed to shrink and did.
        verify(label.implicitWidth > 0, "the label must have measured its text");
        verify(label.width < label.implicitWidth,
               "a path too long for the card must be elided down (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    // What the card says once a stick is unmounted. "(not mounted)"
    // describes the kernel's state; the question the reader actually has
    // at that moment, having just pressed eject, is whether they may pull
    // the stick out.
    function test_anUnmountedStickSaysItIsSafeToUnplug() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, safeToUnplug: true})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.text, "OK to unplug");
    }

    // The claim is about the DEVICE, not this row's partition. A stick
    // whose other partition is still mounted (and possibly being written)
    // must not invite the user to pull it out; the model works that out
    // across every row, and the card only repeats a proven answer.
    function test_anUnmountedPartitionOfABusyDeviceDoesNotSaySoIsSafe() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, safeToUnplug: false})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.text, "(not mounted)");
    }

    // A stick that is not mounted has no mount point, so its row is named
    // by its device: two unmounted sticks must not share one name, or the
    // finders would hand back whichever row was built first.
    function test_anUnmountedStickRowIsNamedByItsDevice() {
        var page = makePage([makeStick({label: "MAIN", mounted: false, mountPoint: "", devicePath: "/dev/sdb1"}),
                             makeStick({label: "SPARE", mounted: false, mountPoint: "", devicePath: "/dev/sdc1"})], {});
        var main = Live.stickRow(page, "/dev/sdb1");
        var spare = Live.stickRow(page, "/dev/sdc1");
        verify(main !== null && spare !== null && main !== spare, "each unmounted stick has its own row");
        compare(main.label, "MAIN");
        compare(spare.label, "SPARE");
        compare(Live.stickRows(page).length, 2);
    }

    function test_aMountedStickSaysNothingAboutUnplugging() {
        var page = makePage([makeStick({label: "MAIN", mounted: true})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.visible, false, "a mounted stick shows its catalogs in that row instead");
    }

    // What is on the stick is shown as labels, one per catalog, and only
    // for the catalogs that are there.
    function test_theCardLabelsEachCatalogOnTheStick() {
        var page = makePage([makeStick({hasRekordbox: true, hasEngine: false, hasOneLibrary: true})], {});
        var device = findByName(page, "deviceLibraryBadge");
        verify(device !== null, "the DeviceLibrary label must exist");
        compare(device.visible, true);
        compare(device.label, "DeviceLibrary");
        compare(findByName(page, "oneLibraryBadge").visible, true);
        compare(findByName(page, "oneLibraryBadge").label, "OneLibrary");
        compare(findByName(page, "engineBadge").visible, false);
    }

    // A mounted stick with no catalog says so in that row, rather than
    // leaving an empty line where the labels would be.
    function test_aStickWithNoLibrarySaysSo() {
        var page = makePage([makeStick({mounted: true, hasRekordbox: false, hasEngine: false, hasOneLibrary: false})], {});
        var label = findByName(page, "noLibraryLabel");
        verify(label !== null, "the no-library label must exist");
        compare(label.visible, true);
        compare(label.text, "No library");
        compare(findByName(makePage([makeStick({})], {}), "noLibraryLabel").visible, false,
                "and a stick with a catalog shows its labels instead");
    }

    // Ejecting swaps the labels for "OK to unplug" in the same row, at the
    // same place and height, so the card does not jump under the pointer.
    function test_ejectingDoesNotMoveTheRow() {
        var mounted = makePage([makeStick({mounted: true})], {});
        var unmounted = makePage([makeStick({mounted: false, safeToUnplug: true})], {});
        var mountedRow = findByName(mounted, "stickStateRow");
        var unmountedRow = findByName(unmounted, "stickStateRow");
        verify(mountedRow !== null && unmountedRow !== null, "the state row must exist");
        compare(unmountedRow.visible, true, "the row stays when the stick is unmounted");
        compare(findByName(unmounted, "unmountedLabel").visible, true);
        compare(unmountedRow.height, mountedRow.height);
        compare(unmountedRow.mapToItem(unmounted, 0, 0).y, mountedRow.mapToItem(mounted, 0, 0).y);
    }

    // The path this card really showed abbreviated. Its natural width is
    // fractional (208.03 at the default font), the layout hands out whole
    // pixels, and a Text a fraction short of its width elides. The short
    // path below happens to measure a whole number of pixels, so it never
    // could catch this.
    function test_aRealMountPointWithAFractionalWidthIsNotElided() {
        var page = makePage([makeStick({label: "WHALESHARK2", mountPoint: "/media/sebas/WHALESHARK2",
                                        devicePath: "/dev/sdb1"})], {});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        if (Math.floor(label.implicitWidth) === label.implicitWidth) {
            skip("this font measures the path in whole pixels, so this case proves nothing here");
        }
        verify(!label.truncated,
               "a path with room to spare must not be abbreviated (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    // The other half of the one above, and the half that was wrong: a
    // path the card has room for must be shown whole.
    function test_aShortMountPointIsNotElided() {
        var page = makePage([makeStick({label: "MAIN", mountPoint: "/media/MAIN"})], {});
        var label = findByName(page, "stickPathLabel");
        verify(label !== null, "the stick path label must exist");
        verify(!label.truncated,
               "a path with room to spare must not be abbreviated (width " + label.width
               + " vs implicit " + label.implicitWidth + ")");
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }
}
