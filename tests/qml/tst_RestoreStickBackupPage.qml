// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

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
    // request (and capped), and "Start Over" clears the report.
    function test_manyProblemsAreCountedNotListedAndStartOverClears() {
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
        findChild(page, "startOverButton").clicked();
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
}
