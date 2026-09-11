// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// CloneStickPage.qml headless with a fake controller: what it configures,
// the mode default, when the start button is enabled, the confirm
// dialog's type-to-confirm gating, the stage strip and the result frame.
TestCase {
    id: testCase
    name: "CloneStickPage"
    width: 900
    height: 900
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        CloneStickPage { width: 880; height: 880 }
    }

    function makePreview(overrides) {
        var p = {
            error: "", ready: true, archiveExists: false, archiveCurrent: false,
            added: 0, changed: 0, removed: 0, databaseChanged: false,
            bytesToRead: 25 * 1024 * 1024 * 1024, sourceBytes: 25 * 1024 * 1024 * 1024,
            backupFreeBytes: 400 * 1024 * 1024 * 1024, enoughBackupSpace: true,
            bytesToTarget: 25 * 1024 * 1024 * 1024, targetFreeBytes: 60 * 1024 * 1024 * 1024, enoughTargetSpace: true,
            targetHasEngineLibrary: false, restoreKnown: false, restoreFilesToWrite: -1, restoreExtras: -1,
        };
        for (var key in overrides) {
            p[key] = overrides[key];
        }
        return p;
    }

    function makeFakeController(overrides) {
        var c = {
            sourceLabel: "MAIN",
            sourceRoot: "/media/MAIN",
            targetLabel: "SPARE",
            targetRoot: "/media/SPARE",
            archivePath: "/home/u/Seabass Backups/MAIN.zip",
            preview: makePreview({}),
            blockedBy: "",
            busy: false,
            previewing: false,
            cloning: false,
            stage: "",
            phase: "",
            filesDone: 0,
            filesTotal: 0,
            bytesDone: 0,
            bytesTotal: 0,
            bytesPerSecond: 0,
            etaSeconds: -1,
            currentFile: "",
            result: {},
            errorMessage: "",
            statusMessage: "",
            calls: [],
            configure: function(sl, rb, en, tm, tl, dir) { this.calls.push("configure:" + sl + ":" + rb + ":" + en + ":" + tm + ":" + tl + ":" + dir); },
            refresh: function() { this.calls.push("refresh"); },
            start: function(exact) { this.calls.push("start:" + exact); },
            cancel: function() { this.calls.push("cancel"); },
            clearResult: function() { this.calls.push("clearResult"); },
        };
        for (var key in overrides) {
            c[key] = overrides[key];
        }
        return c;
    }

    function makePage(controllerOverrides, pageOverrides) {
        var props = {
            controller: makeFakeController(controllerOverrides || {}),
            appSettingsController: {stickBackupDirectory: "/home/u/Seabass Backups"},
            sourceLabel: "MAIN",
            sourceRekordboxPath: "/media/MAIN/PIONEER",
            sourceEnginePath: "/media/MAIN/Engine Library",
            targetMountPoint: "/media/SPARE",
            targetLabel: "SPARE",
            targetHasLibrary: false,
        };
        for (var key in (pageOverrides || {})) {
            props[key] = pageOverrides[key];
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

    function test_screenshots() {
        saveScreenshot(makePage(), "clone-page-fresh");
        saveScreenshot(makePage({preview: makePreview({targetHasEngineLibrary: true, archiveExists: true, added: 3, changed: 1,
                                                       databaseChanged: true, bytesToRead: 300 * 1024 * 1024, restoreKnown: true,
                                                       restoreFilesToWrite: 5, restoreExtras: 2, bytesToTarget: 310 * 1024 * 1024})},
                                {targetHasLibrary: true}), "clone-page-update");
        saveScreenshot(makePage({busy: true, cloning: true, stage: "restore", phase: "writing", filesDone: 3, filesTotal: 14,
                                 bytesDone: 1024 * 1024 * 1024, bytesTotal: 4 * 1024 * 1024 * 1024, bytesPerSecond: 30 * 1024 * 1024,
                                 etaSeconds: 95, currentFile: "Contents/Artist - Title.mp3"}), "clone-page-progress");
        saveScreenshot(makePage({result: {status: "cloned", message: "", backupStatus: "complete", backupSkipped: false,
                                          backupBytesRead: 300 * 1024 * 1024, restoreStarted: true, filesWritten: 5, filesUnchanged: 1156,
                                          directoriesCreated: 0, extrasRemoved: 2, bytesWritten: 310 * 1024 * 1024, rejected: [],
                                          writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
                                 statusMessage: "SPARE now holds MAIN's library: 5 files written, 1156 already up to date."}), "clone-page-result");
    }

    function test_configuresTheControllerOnOpen() {
        var page = makePage();
        compare(page.controller.calls[0],
            "configure:MAIN:/media/MAIN/PIONEER:/media/MAIN/Engine Library:/media/SPARE:SPARE:/home/u/Seabass Backups");
    }

    function test_freshStickDefaultsToOverlayAndNeedsNoTyping() {
        var page = makePage();
        compare(findChild(page, "exactCheckBox").checked, false);
        compare(findChild(page, "openConfirmButton").enabled, true);
        findChild(page, "openConfirmButton").clicked();
        var dialog = findChild(page, "confirmDialog");
        tryCompare(dialog, "visible", true);
        compare(findChild(page, "confirmField").visible, false);
        compare(findChild(page, "cloneAcceptButton").enabled, true);
        findChild(page, "cloneAcceptButton").clicked();
        compare(page.controller.calls.indexOf("start:false") >= 0, true);
    }

    function test_updateDefaultsToExactAndRequiresTypedLabel() {
        var page = makePage({preview: makePreview({targetHasEngineLibrary: true, archiveExists: true, restoreKnown: true,
                                                   restoreFilesToWrite: 14, restoreExtras: 2})},
                            {targetHasLibrary: true});
        compare(findChild(page, "exactCheckBox").checked, true);
        verify(findChild(page, "openConfirmButton").text.indexOf("Update SPARE") === 0);
        findChild(page, "openConfirmButton").clicked();
        var dialog = findChild(page, "confirmDialog");
        tryCompare(dialog, "visible", true);
        var accept = findChild(page, "cloneAcceptButton");
        var field = findChild(page, "confirmField");
        compare(field.visible, true);
        compare(accept.enabled, false);
        field.text = "SPARX";
        compare(accept.enabled, false);
        field.text = "SPARE";
        compare(accept.enabled, true);
        accept.clicked();
        compare(page.controller.calls.indexOf("start:true") >= 0, true);
    }

    function test_uncheckingExactOnAnUpdateStillRequiresTyping() {
        var page = makePage({preview: makePreview({targetHasEngineLibrary: true})}, {targetHasLibrary: true});
        findChild(page, "exactCheckBox").checked = false;
        compare(page.needsTypedConfirmation, true);
    }

    function test_startDisabledWithoutSpaceOrPreviewOrWhileBusyOrBlocked() {
        compare(findChild(makePage({preview: makePreview({enoughTargetSpace: false})}), "openConfirmButton").enabled, false);
        compare(findChild(makePage({preview: makePreview({enoughBackupSpace: false})}), "openConfirmButton").enabled, false);
        compare(findChild(makePage({preview: makePreview({ready: false, error: "The target is not a mounted drive."})}), "openConfirmButton").enabled, false);
        compare(findChild(makePage({busy: true, previewing: true}), "openConfirmButton").enabled, false);
        var blocked = makePage({blockedBy: "Engine DJ"});
        compare(findChild(blocked, "openConfirmButton").enabled, false);
        compare(findChild(blocked, "blockedBanner").visible, true);
        var guard = makePage({}, {conflictingSoftware: "rekordbox"});
        compare(findChild(guard, "openConfirmButton").enabled, false);
    }

    function test_planTextsFollowThePreview() {
        var first = makePage();
        verify(findChild(first, "backupStepLabel").text.indexOf("First backup of MAIN") === 0);
        var current = makePage({preview: makePreview({archiveExists: true, archiveCurrent: true, restoreKnown: true, restoreFilesToWrite: 0})});
        verify(findChild(current, "backupStepLabel").text.indexOf("up to date") > 0);
        verify(findChild(current, "copyStepLabel").text.indexOf("0 file(s)") === 0);
        var incremental = makePage({preview: makePreview({archiveExists: true, added: 3, changed: 1, removed: 0, databaseChanged: true})});
        verify(findChild(incremental, "backupStepLabel").text.indexOf("3 added, 1 changed") > 0);
    }

    function test_stageStripAndProgressWhileCloning() {
        var page = makePage({busy: true, cloning: true, stage: "restore", phase: "writing",
                             filesDone: 3, filesTotal: 14, bytesDone: 1024 * 1024, bytesTotal: 4 * 1024 * 1024});
        compare(findChild(page, "openConfirmButton").enabled, false);
        compare(findChild(page, "cancelCloneButton").visible, true);
        compare(findChild(page, "stageChip-restore").current, true);
        compare(findChild(page, "stageChip-backup").done, true);
        findChild(page, "cancelCloneButton").clicked();
        compare(page.controller.calls.indexOf("cancel") >= 0, true);
    }

    function test_resultFrameShowsAfterARunAndStartOverClears() {
        var page = makePage({result: {status: "cloned", message: "", backupStatus: "complete", backupSkipped: false,
                                      backupBytesRead: 1024 * 1024, restoreStarted: true, filesWritten: 14, filesUnchanged: 0,
                                      directoriesCreated: 3, extrasRemoved: 0, bytesWritten: 1024 * 1024, rejected: [],
                                      writeErrors: [], warnings: [], missingTracks: [], databaseChecked: true},
                             statusMessage: "SPARE now holds MAIN's library."});
        compare(findChild(page, "startOverButton").visible, true);
        verify(findChild(page, "backupOutcomeLabel").text.indexOf("complete") > 0);
        findChild(page, "startOverButton").clicked();
        compare(page.controller.calls.indexOf("clearResult") >= 0, true);
        compare(page.controller.calls.indexOf("refresh") >= 0, true);
    }

    function test_backupIncompleteShowsTheMessageWithoutCounts() {
        var page = makePage({result: {status: "backup-incomplete", message: "The backup step did not capture the library: Engine DJ appeared. The target was not touched.",
                                      backupStatus: "conflict-aborted", backupSkipped: false, backupBytesRead: 0, restoreStarted: false},
                             errorMessage: "The backup step did not capture the library: Engine DJ appeared. The target was not touched."});
        compare(findChild(page, "startOverButton").visible, false);
        compare(findChild(page, "backupOutcomeLabel").visible, true);
    }
}
