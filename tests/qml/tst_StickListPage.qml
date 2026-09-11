import QtQuick
import QtTest
import SeabassGui

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
            isBrowsedBackup: false, libraryId: "lib-main",
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

    // ActionCards are instantiated per delegate; find the one whose title
    // starts with `title` under the delegate for `mountPoint`.
    function findCard(page, mountPoint, title) {
        var found = null;
        function walk(item) {
            if (found !== null) return;
            if (item.cardTitle !== undefined && String(item.cardTitle).indexOf(title) === 0
                && delegateMount(item) === mountPoint) {
                found = item;
                return;
            }
            for (var i = 0; i < item.children.length; ++i) {
                walk(item.children[i]);
            }
        }
        function delegateMount(item) {
            var p = item;
            while (p) {
                if (p.mountPoint !== undefined && p.hasKnownLibrary !== undefined) return p.mountPoint;
                p = p.parent;
            }
            return "";
        }
        walk(page);
        return found;
    }

    // Same idea as findCard(), for the objectName'd row controls
    // (eject/mount button) rather than an ActionCard's title.
    function findRowObject(page, mountPoint, objectName) {
        var found = null;
        function walk(item) {
            if (found !== null) return;
            if (item.objectName === objectName && delegateMount(item) === mountPoint) {
                found = item;
                return;
            }
            for (var i = 0; i < item.children.length; ++i) {
                walk(item.children[i]);
            }
        }
        function delegateMount(item) {
            var p = item;
            while (p) {
                if (p.mountPoint !== undefined && p.hasKnownLibrary !== undefined) return p.mountPoint;
                p = p.parent;
            }
            return "";
        }
        walk(page);
        return found;
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
        verify(card !== null);
        verify(card.cardSubtitle.indexOf("Newer copy on SPARE") === 0);
        verify(findCard(page, "/media/MAIN", "Update Stick") === null);
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
        var localCueCard = findChild(page, "generalLocalCueCard");
        verify(restoreCard !== null);
        verify(localCueCard !== null);
        compare(localCueCard.deprecated, true);

        var restoreSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "restoreStickBackupRequested"});
        restoreCard.clicked();
        compare(restoreSpy.count, 1);
        compare(restoreSpy.signalArguments[0][0], "");

        var localCueSpy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "localCueRequested"});
        localCueCard.clicked();
        compare(localCueSpy.count, 1);
        compare(localCueSpy.signalArguments[0][0], "");
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
    // vanish unseen (folder rows sit out the pulled-stick prompts): the
    // close is refused while the session is dirty, and a clean session's
    // lock is released before the row goes.
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

    // The entry point itself: the toolbar button opens the folder picker.
    function test_openFolderButtonIsOnTheToolbar() {
        var page = makePage([], makeAdvice({}), {});
        var button = null;
        function walk(item) {
            if (button !== null) return;
            if (item.objectName === "openFolderButton") { button = item; return; }
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(page);
        verify(button !== null, "no openFolderButton on the toolbar");
        compare(button.visible, true);

        // And its sibling, for browsing a backup archive in place.
        var backupButton = null;
        function walkBackup(item) {
            if (backupButton !== null) return;
            if (item.objectName === "openBackupButton") { backupButton = item; return; }
            for (var i = 0; i < item.children.length; ++i) walkBackup(item.children[i]);
        }
        walkBackup(page);
        verify(backupButton !== null, "no openBackupButton on the toolbar");
        compare(backupButton.visible, true);
    }

    // Finds the first descendant with `objectName`, anywhere on the page.
    function findByName(page, objectName) {
        var found = null;
        function walk(item) {
            if (found !== null) return;
            if (item.objectName === objectName) { found = item; return; }
            for (var i = 0; i < item.children.length; ++i) walk(item.children[i]);
        }
        walk(page);
        return found;
    }

    function test_theNoStickToolsStepAsideOnceAStickIsIn() {
        // Two cards about this computer's own backup stores. With no
        // stick in they are the only thing to do here; with one in they
        // sit above the thing the page is actually about, taking the top
        // of the screen for the case that is not happening.
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

    function test_theTwoStoresAreOfferedUnderTheList() {
        // Offered and off, rather than absent, when a store is empty: a
        // missing button says nothing, and a grey one says the feature
        // exists and its tooltip says how to fill it.
        var page = makePage([], {});
        var full = findByName(page, "browseFullBackupsButton");
        var meta = findByName(page, "browseMetadataBackupsButton");
        verify(full !== null, "Browse Full Backups must exist");
        verify(meta !== null, "Browse Metadata Backups must exist");
        compare(full.visible, true);
        compare(meta.visible, true);
        compare(full.text, "Browse Full Backups");
        compare(meta.text, "Browse Metadata Backups");
        // The probes run against this test run's own sandboxed
        // SEABASS_HOME, so the counts are whatever that happens to
        // hold. What is pinned is that each button's enabled state is
        // its own store's count and not the other one's, and not a
        // constant.
        var backups = findByName(page, "browseBackupsRow");
        verify(backups !== null, "the row under the list must exist");
        compare(full.enabled, page.homeBackupsFullCount > 0);
        compare(meta.enabled, page.homeBackupsMetadataCount > 0);
    }

    function test_theDonateButtonIsAHeartAndTheOnlyColouredOne() {
        // Pinned because it was got wrong once and the mistake was not
        // visible in the source: Breeze files its heart under "love",
        // the intent, rather than under a shape, so searching the theme
        // for "heart" or "favorite" turns up everything except the
        // heart -- and emblem-favorite, which looks like the obvious
        // answer, is a five-pointed star.
        var page = makePage([], {});
        var donate = findByName(page, "donateButton");
        verify(donate !== null, "the donate button must exist");
        compare(donate.icon.name, "love", "Breeze's heart is called love, not emblem-favorite");
        // And it is the one that stays coloured: the only button in the
        // header asking for something rather than offering something.
        compare(donate.icon.color, Theme.danger);
        var grey = ["openBackupButton", "openFolderButton", "aboutButton", "preferencesButton"];
        for (var i = 0; i < grey.length; ++i) {
            var button = findByName(page, grey[i]);
            verify(button !== null, grey[i] + " must exist");
            // A theme icon rather than an emoji: a bare emoji resolves
            // to the system's colour emoji font, which is how these came
            // to be five full-colour pictures in a header of flat marks.
            verify(button.icon.name.length > 0, grey[i] + " must use a theme icon, not an emoji");
            verify(button.icon.color !== donate.icon.color,
                   grey[i] + " must not be tinted like the donate button, or nothing marks that one out");
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
        var page = makePage([makeStick({label: "MAIN", mounted: false})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.text, "OK to unplug");
    }

    function test_aMountedStickSaysNothingAboutUnplugging() {
        var page = makePage([makeStick({label: "MAIN", mounted: true})], {});
        var label = findByName(page, "unmountedLabel");
        verify(label !== null, "the unmounted label must exist");
        compare(label.text, "");
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
