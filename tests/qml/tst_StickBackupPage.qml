// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// StickBackupPage.qml headless, with a plain JS object standing in for
// StickBackupController (same technique as tst_FormatUsbPage.qml): no
// archive, no stick, no background thread -- just the page's own
// decisions about what to enable, show and call.
TestCase {
    id: testCase
    name: "StickBackupPage"
    width: 900
    height: 900
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        StickBackupPage { width: 880; height: 880 }
    }

    function makeFakeController(overrides) {
        var c = {
            stickLabel: "STICK",
            stickRoot: "/media/STICK",
            archivePath: "/home/u/Seabass Backups/STICK.zip",
            busy: false,
            backingUp: false,
            previewing: false,
            activity: "",
            phase: "",
            filesDone: 0,
            filesTotal: 0,
            bytesDone: 0,
            bytesTotal: 0,
            bytesPerSecond: 0,
            etaSeconds: -1,
            currentFile: "",
            lastBackup: {exists: true, status: "complete", createdAt: "2026-09-03T21:14:00",
                         archiveBytes: 25 * 1024 * 1024 * 1024, entries: 1161, identifierMismatch: false},
            sinceLastBackup: {added: 12, changed: 3, removed: 1, unchanged: 1145, databaseChanged: true,
                              bytesToRead: 1.3 * 1024 * 1024 * 1024, stickBytes: 25 * 1024 * 1024 * 1024,
                              entriesOnStick: 1161, freeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true,
                              uniformShiftSeconds: 0, estimatedSeconds: 240},
            deadSpace: {deadBytes: 4.2 * 1024 * 1024 * 1024, archiveBytes: 25 * 1024 * 1024 * 1024, ratio: 0.18, suggested: false},
            blockedBy: "",
            pendingCancelDecision: false,
            errorMessage: "",
            statusMessage: "",
            calls: [],
            preflight: {deadBytes: 4.2 * 1024 * 1024 * 1024, ratio: 0.18, requiredFreeBytes: 19 * 1024 * 1024 * 1024,
                        availableFreeBytes: 400 * 1024 * 1024 * 1024, enoughFreeSpace: true},
            configure: function(label, rb, engine, dir) { this.calls.push("configure:" + label + ":" + dir); },
            refresh: function() { this.calls.push("refresh"); },
            backUp: function() { this.calls.push("backUp"); },
            cancel: function() { this.calls.push("cancel"); },
            keepPartial: function() { this.calls.push("keepPartial"); },
            discardPartial: function() { this.calls.push("discardPartial"); },
            verify: function() { this.calls.push("verify"); },
            compact: function() { this.calls.push("compact"); },
            compactionPreflight: function() { this.calls.push("compactionPreflight"); return this.preflight; },
            openArchiveFolder: function() { this.calls.push("openArchiveFolder"); },
        };
        for (var key in overrides) {
            c[key] = overrides[key];
        }
        return c;
    }

    function makePage(overrides) {
        return createTemporaryObject(pageComponent, testCase, {
            stickLabel: "STICK",
            rekordboxPath: "/media/STICK/PIONEER",
            enginePath: "/media/STICK/Engine Library",
            appSettingsController: {stickBackupDirectory: "/home/u/Seabass Backups", experimentalFeaturesEnabled: true},
            controller: makeFakeController(overrides),
        });
    }

    function calls(page) {
        return page.controller.calls.join(",");
    }

    function test_configuresControllerFromItsPropertiesOnLoad() {
        var page = makePage({});
        verify(page !== null);
        verify(calls(page).indexOf("configure:STICK:/home/u/Seabass Backups") >= 0);
    }

    function test_idleWithExistingBackupOffersEverything() {
        var page = makePage({});
        var backUp = findChild(page, "backUpButton");
        verify(backUp !== null);
        compare(backUp.visible, true);
        compare(backUp.enabled, true);
        compare(findChild(page, "restoreButton").enabled, true);
        compare(findChild(page, "compactButton").visible, true);
        verify(findChild(page, "sinceLabel").text.indexOf("12 added") >= 0);
        verify(findChild(page, "sinceLabel").text.indexOf("database changed") >= 0);
        backUp.clicked();
        verify(calls(page).indexOf("backUp") >= 0);
    }

    function test_firstBackupWording() {
        var page = makePage({lastBackup: {exists: false}, deadSpace: {deadBytes: 0}});
        verify(findChild(page, "lastBackupLabel").text.indexOf("No backup") >= 0);
        verify(findChild(page, "sinceLabel").text.indexOf("reads everything") >= 0);
        compare(findChild(page, "restoreButton").enabled, false);
        compare(findChild(page, "compactButton").visible, false);
        compare(findChild(page, "backUpButton").enabled, true);
    }

    // The refusal is visible before clicking: a banner plus a disabled
    // button, from whichever source (the guard poller or the controller's
    // own refresh) knows about the running software.
    function test_runningDjSoftwareBlocksBackup() {
        var page = makePage({blockedBy: "Engine DJ"});
        compare(findChild(page, "backUpButton").enabled, false);
        var banner = findChild(page, "blockedBanner");
        verify(banner !== null);
        verify(banner.text.indexOf("Engine DJ") >= 0);

        var viaGuard = makePage({});
        viaGuard.conflictingSoftware = "rekordbox";
        compare(findChild(viaGuard, "backUpButton").enabled, false);
    }

    function test_notEnoughFreeSpaceBlocksBackup() {
        var page = makePage({sinceLastBackup: {added: 1, changed: 0, removed: 0, unchanged: 0, databaseChanged: false,
                                               bytesToRead: 10, stickBytes: 10, entriesOnStick: 1, freeBytes: 1,
                                               enoughFreeSpace: false, uniformShiftSeconds: 0, estimatedSeconds: -1}});
        compare(findChild(page, "backUpButton").enabled, false);
    }

    function test_runningBackupShowsCancelInsteadOfBackUp() {
        var page = makePage({busy: true, backingUp: true, activity: "backup", phase: "reading",
                             filesDone: 441, filesTotal: 1161, bytesDone: 500, bytesTotal: 1000});
        compare(findChild(page, "backUpButton").visible, false);
        var cancel = findChild(page, "cancelButton");
        compare(cancel.visible, true);
        cancel.clicked();
        verify(calls(page).indexOf("cancel") >= 0);
        compare(findChild(page, "restoreButton").enabled, false);
    }

    // Keep for later / discard both route to the controller; the dialog
    // cannot be dismissed any other way.
    function test_cancelDecisionDialogRoutesBothChoices() {
        var page = makePage({pendingCancelDecision: true});
        var dialog = findChild(page, "cancelDecisionDialog");
        verify(dialog !== null);
        dialog.open();
        tryVerify(function() { return dialog.visible; });
        compare(dialog.closePolicy, 0);  // Popup.NoAutoClose
        dialog.accept();
        verify(calls(page).indexOf("keepPartial") >= 0);

        var again = makePage({pendingCancelDecision: true});
        var dialog2 = findChild(again, "cancelDecisionDialog");
        dialog2.open();
        tryVerify(function() { return dialog2.visible; });
        findChild(again, "discardPartialButton").clicked();
        verify(calls(again).indexOf("discardPartial") >= 0);
        tryVerify(function() { return !dialog2.visible; });
        compare(findChild(again, "backUpButton").enabled, false);  // still pending until the controller says otherwise
    }

    function test_compactDialogShowsPreflightAndOnlyProceedsWithSpace() {
        var page = makePage({});
        findChild(page, "compactButton").clicked();
        var dialog = findChild(page, "compactDialog");
        tryVerify(function() { return dialog.visible; });
        verify(calls(page).indexOf("compactionPreflight") >= 0);
        compare(findChild(page, "compactAcceptButton").enabled, true);
        dialog.accept();
        verify(calls(page).indexOf("compact") >= 0);

        var cramped = makePage({preflight: {deadBytes: 4 * 1024 * 1024 * 1024, ratio: 0.18,
                                            requiredFreeBytes: 19 * 1024 * 1024 * 1024,
                                            availableFreeBytes: 8 * 1024 * 1024 * 1024, enoughFreeSpace: false}});
        findChild(cramped, "compactButton").clicked();
        var dialog2 = findChild(cramped, "compactDialog");
        tryVerify(function() { return dialog2.visible; });
        compare(findChild(cramped, "compactAcceptButton").enabled, false);
    }

    function test_restoreHandsOffWithStickRootAndArchive() {
        var page = makePage({});
        var got = null;
        page.restoreRequested.connect(function(label, root, archive) { got = {label: label, root: root, archive: archive}; });
        findChild(page, "restoreButton").clicked();
        verify(got !== null);
        compare(got.label, "STICK");
        compare(got.root, "/media/STICK");
        compare(got.archive, "/home/u/Seabass Backups/STICK.zip");
    }
}
