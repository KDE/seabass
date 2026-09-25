// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// RestoreStickBackupPage.qml headless with a fake controller: drive
// selection rules, the analyze/restore calls it makes, and the confirm
// dialog's type-to-confirm gating.
TestCase {
    id: testCase
    name: "RestoreStickBackupPage"
    width: 900
    height: 900
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        RestoreStickBackupPage { width: 880; height: 880 }
    }

    // A stand-in whose properties notify, for the tests where the drive
    // list itself changes under the page (a plain JS object's do not).
    Component {
        id: liveDisksControllerComponent
        QtObject {
            property var disks: []
            property var nextDisks: []
            property string archivePath: "/home/u/Seabass Backups/STICK.zip"
            property string defaultBackupDirectory: "/home/u/Seabass Backups"
            property var archiveInfo: ({error: "", label: "STICK", identifier: "uuid", status: "complete",
                                        createdAt: "2026-09-03T21:14:00", entries: 1161, bytes: 25 * 1024 * 1024 * 1024,
                                        rejectedCount: 0})
            property var preview: ({filesToWrite: 14, filesUnchanged: 1147, bytesToWrite: 500 * 1024 * 1024, extras: 0,
                                    targetHasEngineLibrary: false, freeBytes: 60 * 1024 * 1024 * 1024, enoughFreeSpace: true})
            property var result: ({})
            property bool busy: false
            property bool restoring: false
            property bool analyzing: false
            property string phase: ""
            property real filesDone: 0
            property real filesTotal: 0
            property real bytesDone: 0
            property real bytesTotal: 0
            property real bytesPerSecond: 0
            property real etaSeconds: -1
            property string currentFile: ""
            property string errorMessage: ""
            property string statusMessage: ""
            property var knownBackups: []
            property bool listingBackups: false
            property var analyzeCalls: []
            property var lastRestore: null
            property int refreshCalls: 0
            property int clearCalls: 0
            function refresh() { refreshCalls += 1; disks = nextDisks; }
            function refreshKnownBackups() {}
            function analyze(mountPoint) { analyzeCalls = analyzeCalls.concat([mountPoint]); }
            function restore(mountPoint, exact) { lastRestore = {mountPoint: mountPoint, exact: exact}; }
            function restoreAnyway(mountPoint, exact) {}
            function retryLockedAction() {}
            function cancel() {}
            function clearResult() { result = ({}); errorMessage = ""; statusMessage = ""; clearCalls += 1; }
            function mount(devicePath) {}
            function archivePathForLabel(label) { return defaultBackupDirectory + "/" + label + ".zip"; }
        }
    }

    Component {
        id: realControllerComponent
        RestoreStickBackupController {}
    }
    // Holds the controller by a QObject property, which Qt nulls once the
    // object is really gone (destroy() only schedules it).
    QtObject {
        id: holder
        property QtObject controller: null
    }

    function makeDisk(overrides) {
        var disk = {
            label: "STICK",
            mountPoint: "/media/STICK",
            devicePath: "/dev/sdb1",
            wholeDiskPath: "/dev/sdb",
            capacityBytes: 64 * 1024 * 1024 * 1024,
            mounted: true,
            hasNoFilesystem: false,
            hasDjLibrary: false,
            usable: true,
            rootEntries: [],
        };
        for (var key in overrides) {
            disk[key] = overrides[key];
        }
        return disk;
    }

    function makeFakeController(disks, overrides) {
        var c = {
            disks: disks,
            archivePath: "/home/u/Seabass Backups/STICK.zip",
            defaultBackupDirectory: "/home/u/Seabass Backups",
            archiveInfo: {error: "", label: "STICK", identifier: "uuid", status: "complete",
                          createdAt: "2026-09-03T21:14:00", entries: 1161, bytes: 25 * 1024 * 1024 * 1024, rejectedCount: 0},
            preview: {filesToWrite: 14, filesUnchanged: 1147, bytesToWrite: 500 * 1024 * 1024, extras: 0,
                      targetHasEngineLibrary: false, freeBytes: 60 * 1024 * 1024 * 1024, enoughFreeSpace: true},
            result: {},
            busy: false,
            restoring: false,
            analyzing: false,
            phase: "",
            filesDone: 0,
            filesTotal: 0,
            bytesDone: 0,
            bytesTotal: 0,
            bytesPerSecond: 0,
            etaSeconds: -1,
            currentFile: "",
            errorMessage: "",
            statusMessage: "",
            knownBackups: [],
            listingBackups: false,
            analyzeCalls: [],
            lastRestore: null,
            mountCalls: [],
            refresh: function() {},
            refreshKnownBackups: function() {},
            analyze: function(mountPoint) { this.analyzeCalls.push(mountPoint); },
            restore: function(mountPoint, exact) { this.lastRestore = {mountPoint: mountPoint, exact: exact}; },
            lastRestoreAnyway: null,
            restoreAnyway: function(mountPoint, exact) { this.lastRestoreAnyway = {mountPoint: mountPoint, exact: exact}; },
            cancel: function() {},
            clearCalls: 0,
            clearResult: function() { this.result = {}; this.clearCalls += 1; },
            mount: function(devicePath) { this.mountCalls.push(devicePath); },
            archivePathForLabel: function(label) { return this.defaultBackupDirectory + "/" + label + ".zip"; },
        };
        for (var key in overrides) {
            c[key] = overrides[key];
        }
        return c;
    }

    // What the page reads on the real AppSettingsController. Without it
    // the currentFolder binding throws in every test in this file --
    // invisible, because the QML runner has no global failOnWarning.
    function fakeAppSettings() {
        return {
            toLocalFileUrl: function(p) { return "file://" + p; },
            localPathFromUrl: function(u) { return u.replace(/^file:\/\//, ""); },
        };
    }

    function makePage(disks, controllerOverrides, pageProps) {
        var props = {controller: makeFakeController(disks, controllerOverrides || {}),
                     appSettingsController: fakeAppSettings()};
        for (var key in (pageProps || {})) {
            props[key] = pageProps[key];
        }
        return createTemporaryObject(pageComponent, testCase, props);
    }

    // The first *usable* drive is preselected, not merely the first one:
    // an unmounted or unformatted drive cannot receive a restore.
    function test_screenshots() {
        if (!screenshotDir || screenshotDir.length === 0) return;
        grabImage(makePage([makeDisk({})], {})).save(screenshotDir + "/restore-page.png");
        grabImage(makePage([makeDisk({})], {busy: true, restoring: true, phase: "writing", filesDone: 3, filesTotal: 14,
                                            bytesDone: 1024 * 1024 * 1024, bytesTotal: 4 * 1024 * 1024 * 1024,
                                            bytesPerSecond: 30 * 1024 * 1024, etaSeconds: 95,
                                            currentFile: "Contents/Artist - Title.mp3"})).save(screenshotDir + "/restore-page-progress.png");
        // Run again onto a drive the first run got most of the way through:
        // the bar picks up where that run stopped, against the whole backup.
        grabImage(makePage([makeDisk({})], {busy: true, restoring: true, phase: "writing", filesDone: 1150, filesTotal: 1161,
                                            bytesDone: 24 * 1024 * 1024 * 1024, bytesTotal: 25 * 1024 * 1024 * 1024,
                                            bytesPerSecond: 30 * 1024 * 1024, etaSeconds: 34,
                                            currentFile: "Contents/Artist - Title.mp3"})).save(screenshotDir + "/restore-page-progress-resumed.png");
        grabImage(makePage([makeDisk({})], {
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 3, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
            statusMessage: "Restored STICK from its backup."})).save(screenshotDir + "/restore-page-result.png");
        // The report with rows pointing at files that are gone, as round
        // 8's TESTRIG_2 restored onto A1 showed it: twelve of them, long
        // paths, one with a control byte in a folder name. What the
        // picture has to show is the note and the repair button, which
        // the unbounded path labels once pushed off the overlay's edge.
        grabImage(makePage([makeDisk({})], {
            result: {filesWritten: 1, filesUnchanged: 760, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], databaseChecked: true,
                     missingTracks: [
                         "/media/sebas/A1/Contents/Ann Clue/FCKNG SERIOUS - NINE YEARS/Ann Clue - Fräulein Schmidt (Original Mix).mp3",
                         "/media/sebas/A1/Contents/2088/You Can Feel It/2088 - You Can Feel It.m4a.missing",
                         "/media/sebas/A1/Engine Library/Contents",
                         "/media/sebas/A1/Contents/UnknownArtist/UnknownAlbum/A11-flac48.flac.missing",
                         "/media/sebas/A1/Contents/AKKI (DE)/Olympus/Tiësto, Böhmer - Røyksopp Café.mp3",
                         "/media/sebas/A1/Contents/UnknownArtist/UnknownAlbum/A15-untagged.mp3",
                         "/media/sebas/A1/Contents/A\uFFFD/Olympus/A16-control-byte.mp3",
                         "/media/sebas/A1/Engine Library/..\\Contents\\Adrianna, Tao Andra\\Shake the Underground (Tao Andra Remix).mp3",
                         "/media/sebas/A1/Engine Library/..\\Contents\\AKKI (DE)\\Lost In Time\\AKKI (DE) - Lost In Time.mp3",
                         "/media/sebas/A1/Contents/2 Unlimited/Unlimited Hits & Remixes/2 Unlimited - No Limit.mp3.missing",
                         "/media/sebas/A1/Contents/2 Unlimited/Unlimited Hits & Remixes/2 Unlimited - No Limit-1.mp3 ",
                         "/media/sebas/A1/Engine Library/..\\Contents\\AREA ØNE\\Back To The Oldschool\\AREA ØNE - Back To The Oldschool.mp3"]},
            statusMessage: "Restored 1 files, but with problems."})).save(screenshotDir + "/restore-page-result-missing-tracks.png");
    }

    // Restoring graduated from experimental on 2026-09-17; the header
    // still carried the EXPERIMENTAL pill afterwards.
    function test_carriesNoExperimentalBadge() {
        const page = makePage([makeDisk({})], {});
        verify(page !== null);
        compare(findChild(page, "experimentalBadge"), null);
    }

    // While the backup folder is read the page shows the same overlay as
    // Match Duplicate Cues, and takes it away once the list has landed.
    function test_scanningOverlayWhileListingBackups() {
        const page = makePage([makeDisk({})], {listingBackups: true});
        const overlay = findChild(page, "scanOverlay");
        verify(overlay !== null);
        compare(overlay.visible, true);
        compare(overlay.label, "Scanning existing backups...");
        if (screenshotDir && screenshotDir.length > 0) {
            wait(500);  // the sweeping bar starts off to the left of its track
            grabImage(page).save(screenshotDir + "/restore-page-scanning.png");
        }
        page.controller = makeFakeController([makeDisk({})], {listingBackups: false});
        compare(overlay.visible, false);
    }

    // Step 1 must not say the folder is empty before it has been read:
    // while the listing runs it says it is looking, and "No backups found"
    // only once a listing came back with none.
    function test_noBackupsFoundOnlyAfterTheListing() {
        const page = makePage([makeDisk({})], {listingBackups: true, knownBackups: []});
        const label = findChild(page, "knownBackupsLabel");
        verify(label.text.indexOf("No backups found") < 0, label.text);
        page.controller = makeFakeController([makeDisk({})], {listingBackups: false, knownBackups: []});
        compare(label.text.indexOf("No backups found"), 0, label.text);
    }

    // Leaving the page while its backup folder is still being listed
    // destroys the controller mid-listing. That must not hold the window
    // until the listing is done (it did: the destructor waited for it, 0.5
    // s on a warm folder of four 1 GB backups, 5 s and more cold), and the
    // result that lands later must reach nothing.
    function test_leavingMidListingDoesNotWaitForIt() {
        const folder = controllerFixture.slowBackupFolder(20000);
        verify(folder.length > 0, "could not make the slow folder");
        // How long the listing takes here, run to the end.
        const whole = createTemporaryObject(realControllerComponent, testCase);
        const started = Date.now();
        whole.defaultBackupDirectory = folder;
        verify(whole.listingBackups);
        tryVerify(function() { return !whole.listingBackups && whole.knownBackups.length === 20000; }, 30000);
        const listingMs = Date.now() - started;
        verify(listingMs >= 200, "the folder lists in " + listingMs + " ms, too fast to tell a wait from none");

        holder.controller = realControllerComponent.createObject(null);
        holder.controller.defaultBackupDirectory = folder;
        verify(holder.controller.listingBackups);
        const left = Date.now();
        holder.controller.destroy();
        tryVerify(function() { return holder.controller === null; }, 30000);
        const stalledMs = Date.now() - left;
        verify(stalledMs < listingMs / 2, "destroying the controller took " + stalledMs
               + " ms against a " + listingMs + " ms listing: it waited for the listing");
        // Let the orphaned listing run out before its folder goes.
        wait(listingMs);
        controllerFixture.removeSlowBackupFolder();
    }

    function test_preselectsFirstUsableDriveAndAnalyzesIt() {
        var unmounted = makeDisk({label: "OLD", mountPoint: "", mounted: false, usable: false});
        var blank = makeDisk({label: "NEW", mountPoint: "", hasNoFilesystem: true, usable: false});
        var good = makeDisk({label: "STICK"});
        var page = makePage([unmounted, blank, good]);
        verify(page !== null);
        compare(page.selectedIndex, 2);
        compare(page.controller.analyzeCalls.length, 1);
        compare(page.controller.analyzeCalls[0], "/media/STICK");
    }

    function test_preselectedMountPointWinsAndArchiveIsTakenOver() {
        var a = makeDisk({label: "A", mountPoint: "/media/A"});
        var b = makeDisk({label: "B", mountPoint: "/media/B"});
        var page = makePage([a, b], {}, {preselectedMountPoint: "/media/B", preselectedArchivePath: "/x/B.zip"});
        compare(page.selectedIndex, 1);
        compare(page.controller.archivePath, "/x/B.zip");
        compare(page.controller.analyzeCalls[0], "/media/B");
    }

    function test_labelOnlyPreselectionDerivesTheArchivePath() {
        var page = makePage([makeDisk({})], {}, {preselectedLabel: "WHALESHARK2"});
        compare(page.controller.archivePath, "/home/u/Seabass Backups/WHALESHARK2.zip");
    }

    function test_restoreDisabledUntilAnalyzed() {
        var page = makePage([makeDisk({})], {preview: {}});
        compare(findChild(page, "openConfirmButton").enabled, false);
        var analyzed = makePage([makeDisk({})]);
        compare(findChild(analyzed, "openConfirmButton").enabled, true);
    }

    function test_restoreDisabledWithoutFreeSpace() {
        var page = makePage([makeDisk({})], {preview: {filesToWrite: 1, filesUnchanged: 0, bytesToWrite: 10, extras: 0,
                                                        targetHasEngineLibrary: false, freeBytes: 1, enoughFreeSpace: false}});
        compare(findChild(page, "openConfirmButton").enabled, false);
    }

    // An unfinished update is restorable: the restore rolls it back. The page
    // says so, and does not hold the Restore button back for it.
    function test_unfinishedUpdateIsNotedAndStillRestorable() {
        var plain = makePage([makeDisk({})]);
        compare(findChild(plain, "rollbackNote").visible, false);
        var page = makePage([makeDisk({})], {preview: {filesToWrite: 14, filesUnchanged: 1147, bytesToWrite: 500 * 1024 * 1024,
                                                        extras: 0, targetHasEngineLibrary: false, freeBytes: 8 * 1024 * 1024 * 1024,
                                                        enoughFreeSpace: true, rollsBackUnfinishedUpdate: true}});
        compare(findChild(page, "rollbackNote").visible, true);
        compare(findChild(page, "openConfirmButton").enabled, true);
    }

    // A blank target with nothing at stake: no typing required.
    function test_confirmOnBlankTargetNeedsNoTypingAndCallsRestore() {
        var page = makePage([makeDisk({})]);
        var dialog = findChild(page, "confirmDialog");
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(findChild(page, "confirmField").visible, false);
        compare(findChild(page, "restoreAcceptButton").enabled, true);
        dialog.accept();
        verify(page.controller.lastRestore !== null);
        compare(page.controller.lastRestore.mountPoint, "/media/STICK");
        compare(page.controller.lastRestore.exact, false);
    }

    // The drive at the restore target changed between the preview and the
    // confirm. Nothing has been written, and the dialog's default is Cancel:
    // the highlighted button, and what Return does. Only "Restore Anyway"
    // goes on, and it goes through restoreAnyway(), never a plain restore()
    // that would check the drive again and ask again.
    function test_changedDriveDialogDefaultsToCancel() {
        var page = makePage([makeDisk({})]);
        var dialog = findChild(page, "targetChangedDialog");
        verify(dialog !== null, "a changed drive must raise a dialog");
        dialog.targetRoot = "/media/STICK";
        dialog.exact = true;
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        // Waited for: MessageDialog assigns the default button when it
        // opens, after DialogButtonBox has written its own highlight.
        tryCompare(findChild(page, "cancelChangedTargetButton"), "highlighted", true);
        tryCompare(findChild(page, "restoreAnywayButton"), "highlighted", false);
        dialog.reject();
        tryVerify(function() { return !dialog.visible; });
        compare(page.controller.lastRestoreAnyway, null, "cancelling must not restore");
        compare(page.controller.lastRestore, null);

        dialog.open();
        tryVerify(function() { return dialog.visible; });
        findChild(page, "restoreAnywayButton").clicked();
        verify(page.controller.lastRestoreAnyway !== null, "Restore Anyway must go on");
        compare(page.controller.lastRestoreAnyway.mountPoint, "/media/STICK");
        compare(page.controller.lastRestoreAnyway.exact, true);
        compare(page.controller.lastRestore, null, "and must not loop back into a checking restore()");
    }

    // A target that already holds a DJ library: the exact label must be
    // typed, like Format USB Stick.
    function test_confirmOnLibraryTargetRequiresTypedLabel() {
        var page = makePage([makeDisk({hasDjLibrary: true})]);
        var dialog = findChild(page, "confirmDialog");
        var accept = findChild(page, "restoreAcceptButton");
        var field = findChild(page, "confirmField");
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(field.visible, true);
        compare(accept.enabled, false);
        field.text = "stick";
        compare(accept.enabled, false);
        field.text = "STICK";
        compare(accept.enabled, true);
    }

    function test_exactRestorePassesTheFlagAndRequiresTyping() {
        var page = makePage([makeDisk({})]);
        var exact = findChild(page, "exactCheckBox");
        exact.toggle();
        compare(page.exact, true);
        var dialog = findChild(page, "confirmDialog");
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(findChild(page, "restoreAcceptButton").enabled, false);
        findChild(page, "confirmField").text = "STICK";
        dialog.accept();
        compare(page.controller.lastRestore.exact, true);
    }

    function test_busyControllerDisablesRestore() {
        var page = makePage([makeDisk({})], {busy: true, restoring: true, phase: "writing", filesDone: 3, filesTotal: 14,
                                             bytesDone: 100, bytesTotal: 1000});
        compare(findChild(page, "openConfirmButton").enabled, false);
        compare(findChild(page, "cancelRestoreButton").visible, true);
    }

    // The progress and the report used to sit at the foot of the form,
    // below the fold: Restore was pressed and nothing on screen changed.
    // They are on an overlay now, over the page, from the first moment.
    function test_aRunningRestoreIsShownOverThePage() {
        var page = makePage([makeDisk({})], {busy: true, restoring: true, phase: "writing", filesDone: 3, filesTotal: 14,
                                             bytesDone: 100, bytesTotal: 1000});
        var overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true);
        verify(overlay.title.indexOf("Restoring onto STICK") === 0);
        // Inside the overlay, not somewhere down the scrolled page.
        verify(findChild(overlay, "cancelRestoreButton") !== null);
        compare(findChild(overlay, "cancelRestoreButton").visible, true);
        // No way out of the overlay while it writes, other than Cancel.
        compare(findChild(overlay, "closeReportButton").visible, false);
        // The card sits inside the window, whatever the page scrolled to.
        var card = findChild(overlay, "transferOverlayCard");
        var topLeft = card.mapToItem(page, 0, 0);
        verify(topLeft.y >= 0 && topLeft.y + card.height <= page.height);
    }

    function test_noOverlayBeforeARestore() {
        var page = makePage([makeDisk({})], {});
        compare(findChild(page, "restoreOverlay").visible, false);
    }

    function test_theResultStaysOnTheOverlayUntilClosed() {
        var page = makePage([makeDisk({})], {
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
            statusMessage: "Restored 14 files.",
        });
        var overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true);
        compare(overlay.title, "Restore finished");
        verify(findChild(overlay, "restoreResultFrame") !== null);
        compare(findChild(overlay, "closeReportButton").visible, true);
        findChild(overlay, "closeReportButton").clicked();
        compare(overlay.visible, false);
    }

    // Refused or failed before any report: the reason is on the overlay,
    // since the form's own error line is scrolled out of sight by then.
    function test_aRestoreThatStopsWithoutAReportSaysWhyOnTheOverlay() {
        var page = makePage([makeDisk({})], {busy: true, restoring: true, phase: "analyzing"});
        compare(findChild(page, "restoreOverlay").visible, true);
        page.controller = makeFakeController([makeDisk({})], {errorMessage: "The drive was removed."});
        var overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true);
        compare(overlay.title, "Restore stopped");
        compare(findChild(overlay, "restoreFailedLabel").text, "The drive was removed.");
        findChild(overlay, "closeReportButton").clicked();
        compare(overlay.visible, false);
    }

    // A restore refused before it starts (the write hold, a lock) never
    // sets the controller's restoring flag, so it used to miss the overlay
    // and its reason sat on the form's own error line, out of view. The
    // overlay counts from the press instead.
    function test_aRestoreRefusedBeforeItStartsSaysWhyOnTheOverlay() {
        var page = makePage([makeDisk({})], {});
        compare(findChild(page, "restoreOverlay").visible, false);
        findChild(page, "confirmDialog").accepted();
        compare(page.controller.lastRestore.mountPoint, "/media/STICK", "the restore was asked for");
        // The controller answers with a refusal and never starts.
        page.controller = makeFakeController([makeDisk({})], {errorMessage: "Another Seabass instance is writing to this stick."});
        var overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true);
        compare(overlay.title, "Restore stopped");
        compare(findChild(overlay, "restoreFailedLabel").text, "Another Seabass instance is writing to this stick.");
        // Closed, then refused again on a second press: shown again.
        findChild(overlay, "closeReportButton").clicked();
        compare(overlay.visible, false);
        findChild(page, "confirmDialog").accepted();
        page.controller = makeFakeController([makeDisk({})], {errorMessage: "Still refused."});
        compare(findChild(page, "restoreOverlay").visible, true);
    }

    function makeBackup(overrides) {
        var backup = {
            archivePath: "/home/u/Seabass Backups/WHALESHARK2.zip",
            fileName: "WHALESHARK2.zip",
            error: "",
            label: "WHALESHARK2",
            identifier: "uuid-2",
            status: "complete",
            createdAt: "2026-09-05T20:00:00",
            entries: 1161,
            bytes: 25 * 1024 * 1024 * 1024,
        };
        for (var key in overrides) {
            backup[key] = overrides[key];
        }
        return backup;
    }

    function findChildren(item, objectName, found) {
        found = found || [];
        for (var i = 0; i < item.children.length; ++i) {
            var child = item.children[i];
            if (child.objectName === objectName) {
                found.push(child);
            }
            findChildren(child, objectName, found);
        }
        return found;
    }

    // With nothing preselected, the newest readable backup is chosen by
    // itself and analyzed against the default drive; picking another one
    // takes over as the archive and re-analyzes.
    // The restore page lists backups by file too: that is what is picked.
    function test_backupsAreListedByFileName() {
        var page = makePage([makeDisk({})], {knownBackups: [
            makeBackup({archivePath: "/b/SHAKEDOWN_8.zip", fileName: "SHAKEDOWN_8.zip", label: "TESTRIG_2"}),
            makeBackup({archivePath: "/b/TESTRIG_2.zip", fileName: "TESTRIG_2.zip", label: "TESTRIG_2"})]});
        var titles = findChildren(page, "backupRadioTitle").map(function(l) { return l.text; }).sort();
        compare(titles.join(","), "SHAKEDOWN_8,TESTRIG_2");
    }

    function test_newestKnownBackupIsPickedAndAnotherCanBeChosen() {
        var older = makeBackup({archivePath: "/home/u/Seabass Backups/OLD.zip", fileName: "OLD.zip", label: "OLD",
                                createdAt: "2026-08-01T10:00:00"});
        var broken = makeBackup({archivePath: "/home/u/Seabass Backups/BAD.zip", fileName: "BAD.zip", label: "",
                                 error: "the backup is unreadable"});
        var page = makePage([makeDisk({})], {archivePath: "", knownBackups: [broken, makeBackup({}), older]});
        compare(page.controller.archivePath, "/home/u/Seabass Backups/WHALESHARK2.zip");
        compare(page.archiveIsCustom, false);
        // The drive settles before the backup list does, so the last
        // analyze (the one with the archive known) is what matters.
        var calls = page.controller.analyzeCalls.length;
        verify(calls >= 1);
        compare(page.controller.analyzeCalls[calls - 1], "/media/STICK");
        var radios = findChildren(page, "backupRadio");
        compare(radios.length, 3);
        compare(radios[0].enabled, false);
        // toggled() only fires for user interaction, so a real click, not toggle().
        mouseClick(radios[2]);
        compare(page.controller.archivePath, "/home/u/Seabass Backups/OLD.zip");
        compare(page.controller.analyzeCalls.length, calls + 1);
        compare(page.controller.analyzeCalls[calls], "/media/STICK");
    }

    // A file from outside the backup folder (chosen by hand, or handed
    // over by the per-stick page) is shown as its own selected row.
    function test_archiveOutsideTheFolderShowsAsCustomRow() {
        var page = makePage([makeDisk({})], {archivePath: "/elsewhere/OLD.zip", knownBackups: [makeBackup({})]});
        compare(page.archiveIsCustom, true);
        compare(findChild(page, "customBackupRadio").visible, true);
        var inFolder = makePage([makeDisk({})], {knownBackups: [makeBackup({archivePath: "/home/u/Seabass Backups/STICK.zip", fileName: "STICK.zip", label: "STICK"})]});
        compare(inFolder.archiveIsCustom, false);
        compare(findChild(inFolder, "customBackupRadio").visible, false);
    }

    // The list titles a backup by its file already; the places that say
    // what a restore is made from name the file in full, .zip and all,
    // since the list can be scrolled away by then: the What will happen
    // block, and the overlay the restore runs on.
    function test_backupFileNameIsShownForAKnownBackup() {
        const backups = [makeBackup({}),
                         makeBackup({archivePath: "/home/u/Seabass Backups/WHALESHARK2 (before Berlin).zip",
                                     fileName: "WHALESHARK2 (before Berlin).zip", createdAt: "2026-08-01T10:00:00"})];
        const page = makePage([makeDisk({})], {archivePath: "/home/u/Seabass Backups/WHALESHARK2 (before Berlin).zip",
                                               knownBackups: backups});
        compare(page.archiveIsCustom, false);
        const summary = findChild(page, "previewArchiveLabel");
        verify(summary !== null, "the What will happen block must name the file");
        verify(summary.visible);
        verify(summary.text.indexOf("WHALESHARK2 (before Berlin).zip") === 0, summary.text);
        compare(summary.font.family, Theme.dataFamily);

        const running = makePage([makeDisk({})], {archivePath: "/home/u/Seabass Backups/WHALESHARK2 (before Berlin).zip",
                                                  knownBackups: backups, busy: true, restoring: true, phase: "writing"});
        const onOverlay = findChild(findChild(running, "restoreOverlay"), "overlayArchiveLabel");
        verify(onOverlay !== null && onOverlay.visible);
        compare(onOverlay.text, "From WHALESHARK2 (before Berlin).zip");
    }

    function test_backupFileNameIsShownForACustomFile() {
        const page = makePage([makeDisk({})], {archivePath: "/elsewhere/Old Stick 2025.zip", knownBackups: [makeBackup({})]});
        compare(page.archiveIsCustom, true);
        const summary = findChild(page, "previewArchiveLabel");
        verify(summary !== null && summary.visible);
        verify(summary.text.indexOf("Old Stick 2025.zip") === 0, summary.text);
        compare(summary.font.family, Theme.dataFamily);
    }

    // A finished restore has one way off its overlay, not two that read as
    // synonyms ("Close" beside "Done", or beside the report's own "Start
    // Over"). Counted over every visible button on the overlay, the
    // report's included; a clean report has nothing to show details of
    // and no missing tracks to repair, so any other button is an exit.
    // Close then does what Start Over did: clears the report and looks at
    // the drives again, so the form under it is not the stale preview.
    function test_aFinishedRestoreOffersOneWayOut() {
        const page = makePage([makeDisk({})], {
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
            statusMessage: "Restored 14 files.",
        });
        const overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true);
        verify(findChild(overlay, "restoreResultFrame").visible);
        const buttons = [];
        function collect(item) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i];
                if (child instanceof Button && child.visible) {
                    buttons.push(child);
                }
                collect(child);
            }
        }
        collect(overlay);
        compare(buttons.length, 1, buttons.map(function(b) { return b.text; }).join(", "));
        compare(buttons[0].objectName, "closeReportButton");
        const analyzed = page.controller.analyzeCalls.length;
        buttons[0].clicked();
        compare(overlay.visible, false);
        compare(page.controller.clearCalls, 1, "Close must clear the report");
        compare(page.controller.analyzeCalls.length, analyzed + 1, "and look at the drive again");
        compare(page.controller.analyzeCalls[analyzed], "/media/STICK");
    }

    // Close refreshes the drive list and must keep the drive the user
    // chose, found again by what it is (mount point and device), not by
    // its row. It used to fall back to the first usable drive: pick B,
    // restore, Close, and the form had quietly moved to A and analysed
    // it, and a blank A asks for no typed confirmation, so the next
    // Restore wrote onto a drive nobody picked. Checked after each way a
    // restore ends, with B moved to another row by the refresh.
    function closeAfter(outcome) {
        const a = makeDisk({label: "A", mountPoint: "/media/A", devicePath: "/dev/sdb1"});
        const b = makeDisk({label: "B", mountPoint: "/media/B", devicePath: "/dev/sdc1"});
        const c = makeDisk({label: "C", mountPoint: "/media/C", devicePath: "/dev/sdd1"});
        const controller = createTemporaryObject(liveDisksControllerComponent, testCase, {disks: [a, b]});
        const page = createTemporaryObject(pageComponent, testCase,
                                           {controller: controller, appSettingsController: fakeAppSettings()});
        compare(page.selectedDisk.label, "A", "A is the default");
        const radioB = findChild(page, "driveRadio_1");
        radioB.checked = true;
        radioB.toggled();
        compare(page.selectedDisk.label, "B", "the user picks B");
        findChild(page, "confirmDialog").accepted();
        compare(controller.lastRestore.mountPoint, "/media/B");
        controller.restoring = true;
        controller.busy = true;
        controller.restoring = false;
        controller.busy = false;
        if (outcome === "finished") {
            controller.result = {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                                 writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true};
            controller.statusMessage = "Restored 14 files (1147 already up to date).";
        } else if (outcome === "cancelled") {
            controller.result = {filesWritten: 3, filesUnchanged: 0, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                                 writeErrors: [], warnings: [], missingTracks: [], databaseChecked: false};
            controller.statusMessage = "Restore cancelled after 3 files.";
        } else {
            controller.errorMessage = "The drive was removed.";
        }
        const overlay = findChild(page, "restoreOverlay");
        compare(overlay.visible, true, outcome + ": the overlay shows how it ended");
        // The refresh finds the drives in another order: B is row 2 now.
        controller.nextDisks = outcome === "gone" ? [c, a] : [c, a, b];
        const analyzed = controller.analyzeCalls.length;
        findChild(overlay, "closeReportButton").clicked();
        compare(overlay.visible, false);
        compare(controller.refreshCalls, 1, "Close looks at the drives again");
        return {page: page, controller: controller, analyzed: analyzed};
    }

    function test_closeKeepsTheChosenDrive_data() {
        return [{tag: "finished"}, {tag: "failed"}, {tag: "cancelled"}];
    }

    function test_closeKeepsTheChosenDrive(data) {
        const run = closeAfter(data.tag);
        compare(run.page.selectedDisk !== null, true, "a drive is still selected");
        compare(run.page.selectedDisk.label, "B", "still the drive the user chose");
        compare(run.page.selectedIndex, 2, "found at its new row");
        compare(run.controller.analyzeCalls.length, run.analyzed + 1, "and analysed afresh");
        compare(run.controller.analyzeCalls[run.analyzed], "/media/B");
        compare(findChild(run.page, "driveRadio_2").checked, true);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(run.page).save(screenshotDir + "/restore-page-closed-after-" + data.tag + ".png");
        }
    }

    // The chosen drive is gone: nothing is selected and nothing can be
    // restored until the user picks, never another drive in its place.
    function test_closeWithTheChosenDriveGoneSelectsNothing() {
        const run = closeAfter("gone");
        compare(run.page.selectedIndex, -1);
        compare(run.page.selectedDisk, null);
        compare(run.controller.analyzeCalls.length, run.analyzed + 1);
        compare(run.controller.analyzeCalls[run.analyzed], "", "the stale preview is dropped");
        compare(findChild(run.page, "openConfirmButton").enabled, false);
        compare(findChild(run.page, "driveRadio_0").checked, false);
        compare(findChild(run.page, "driveRadio_1").checked, false);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(run.page).save(screenshotDir + "/restore-page-closed-drive-gone.png");
        }
    }

    // The stick list hands over a device path for a stick it could not
    // preselect by mount point: mounted on open, or selected if it turns
    // out to be mounted already.
    function test_preselectedDevicePathMountsOrSelects() {
        var unmounted = makeDisk({label: "NEW", mountPoint: "", devicePath: "/dev/sdc1", mounted: false, usable: false});
        var page = makePage([makeDisk({}), unmounted], {}, {preselectedDevicePath: "/dev/sdc1"});
        compare(page.controller.mountCalls.length, 1);
        compare(page.controller.mountCalls[0], "/dev/sdc1");
        var mounted = makeDisk({label: "NEW", mountPoint: "/media/NEW", devicePath: "/dev/sdc1"});
        var selected = makePage([makeDisk({}), mounted], {}, {preselectedDevicePath: "/dev/sdc1"});
        compare(selected.controller.mountCalls.length, 0);
        compare(selected.selectedIndex, 1);
    }

    // A formatted stick that is not mounted (fresh from Format USB Stick)
    // offers to be mounted right here; a blank one offers formatting.
    function test_unmountedFormattedDriveOffersMount() {
        var unmounted = makeDisk({label: "NEW", mountPoint: "", devicePath: "/dev/sdc1", mounted: false, usable: false});
        var page = makePage([unmounted, makeDisk({})]);
        var button = findChild(page, "mountDriveButton");
        compare(button.visible, true);
        button.clicked();
        compare(page.controller.mountCalls.length, 1);
        compare(page.controller.mountCalls[0], "/dev/sdc1");
        var blank = makePage([makeDisk({label: "BLANK", mountPoint: "", devicePath: "", mounted: false, hasNoFilesystem: true, usable: false})]);
        compare(findChild(blank, "mountDriveButton").visible, false);
    }

    // A stick yanked mid-restore: the problems are counted, listed only on
    // request (and capped), and Close clears the report.
    function test_manyProblemsAreCountedNotListedAndCloseClears() {
        var errors = [];
        for (var i = 0; i < 500; ++i) {
            errors.push("track" + i + ".mp3: write failed");
        }
        var page = makePage([makeDisk({})], {
            result: {filesWritten: 12, filesUnchanged: 0, directoriesCreated: 1, extrasRemoved: 0, rejected: [],
                     writeErrors: errors, warnings: [], missingTracks: [], databaseChecked: false},
            errorMessage: "the drive disappeared after 12 files were restored",
        });
        compare(findChild(page, "problemCountLabel").text, "500 problems");
        compare(findChild(page, "problemList").count, 0);
        findChild(page, "toggleProblemsButton").clicked();
        compare(findChild(page, "problemList").count, 200);
        findChild(page, "closeReportButton").clicked();
        compare(page.controller.clearCalls, 1);
    }

    // Missing referenced tracks can predate the backup entirely (a
    // re-numbered or re-imported track leaving its database row
    // dangling) rather than meaning this restore did something wrong --
    // the result report hands that question to Library Health for the
    // disk actually just restored onto.
    function test_missingTracksOffersLibraryHealthForTheRestoredDisk() {
        var disk = makeDisk({label: "STICK", mountPoint: "/media/STICK",
                             rekordboxPath: "/media/STICK/PIONEER", enginePath: "/media/STICK/Engine Library"});
        var page = makePage([disk], {
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: ["Contents/Artist/track.mp3"], databaseChecked: true},
        });
        var button = findChild(page, "repairLibraryButton");
        compare(button.visible, true);

        var spy = createTemporaryObject(spyComponent, testCase, {target: page, signalName: "libraryHealthRequested"});
        button.clicked();
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "STICK");
        compare(spy.signalArguments[0][1], "/media/STICK/PIONEER");
        compare(spy.signalArguments[0][2], "/media/STICK/Engine Library");
    }

    // Nothing missing: no reason to offer a repair for a database that
    // opened clean.
    function test_noMissingTracksHidesTheRepairButton() {
        var page = makePage([makeDisk({})], {
            result: {filesWritten: 14, filesUnchanged: 1147, directoriesCreated: 0, extrasRemoved: 0, rejected: [],
                     writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
        });
        compare(findChild(page, "repairLibraryButton").visible, false);
    }

    Component {
        id: spyComponent
        SignalSpy {}
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

    // Reached four ways, and the crumb must be right each time.
    function test_breadcrumb_data() {
        return [
            // Home's general card: no stick, nothing between.
            {tag: "home-card", below: 1, stickLabel: "", hubLabel: "",
             stick: "", middle: "", link: false},
            // A stick's own row on Home: the stick, as context.
            {tag: "home-row", below: 1, stickLabel: "STICK", hubLabel: "",
             stick: "STICK", middle: "", link: false},
            // The stick's Backups page.
            {tag: "backups-hub", below: 2, stickLabel: "STICK", hubLabel: "Backups",
             stick: "STICK", middle: "Backups", link: true},
            // Full Stick Backup, itself under the Backups page.
            {tag: "full-backup", below: 3, stickLabel: "STICK", hubLabel: "Full Stick Backup",
             stick: "STICK", middle: "Full Stick Backup", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const page = pushOnStack(data.below, {
            controller: makeFakeController([makeDisk({})], {}),
            appSettingsController: fakeAppSettings(),
            stickLabel: data.stickLabel,
            hubLabel: data.hubLabel,
        });
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, data.stick);
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Restore a Stick Backup");
        saveCrumbShot(page, "restore-" + data.tag);
    }
}
