// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// The edit-mode flows against a REAL stick: real pages, their real
// controllers, real reads and writes. Run by hand (see docs/testing.md):
//
//   SEABASS_LIVE_STICK=/media/you/STICK SEABASS_SCREENSHOT_DIR=/tmp/shots \
//     QT_QPA_PLATFORM=offscreen build/seabass_qml_tests -input tests/qml-live
//
// Only ever on a scratch copy of a library. Every write here goes
// through the normal backup path and is undone again where the flow
// has an undo, but this is still a test that writes to the stick.
TestCase {
    id: testCase
    name: "LiveEditMode"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: liveStickRoot
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string enginePath: stickRoot + "/Engine Library"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)
    property string libraryId: ""

    Component { id: spyComponent; SignalSpy {} }
    Component { id: settingsPage; SettingsPage { width: 1100; height: 820 } }
    Component { id: syncPage; SyncPage { width: 1100; height: 820 } }
    Component { id: junkPage; JunkCuePage { width: 1100; height: 820 } }
    Component { id: healthPage; LibraryConsistencyPage { width: 1100; height: 820 } }
    Component { id: backupsPage; BackupsPage { width: 1100; height: 820 } }
    Component { id: pendingPage; PendingDeletionsPage { width: 1100; height: 820 } }
    Component { id: scanController; ScanController {} }
    Component { id: settingsController; SettingsController {} }
    Component { id: appSettings; AppSettingsController {} }

    readonly property var fakePlayback: ({stop: function() {}, hasTrack: false, playing: false})

    function init() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        verify(testCase.libraryId.length > 0, "the stick has a library id");
    }

    function session() {
        return EditSessionRegistry.sessionFor(testCase.libraryId, stickLabel);
    }

    function shot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) return;
        waitForRendering(item);
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function waitIdle(controller, timeoutMs) {
        tryVerify(function() { return controller.busy === false; }, timeoutMs || 120000);
    }

    // A save on the session, waited for; returns the summary map.
    // cancelAfterFirst: press Cancel once the first item has landed, so
    // the run ends "k of N" with k >= 1 (the partial-write path).
    function saveAndWait(cancelAfterFirst) {
        var s = session();
        var spy = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.save();
        if (cancelAfterFirst) {
            tryVerify(function() { return s.writeCurrent >= 1 || spy.count > 0; }, 120000);
            s.cancelWrite();
        }
        tryVerify(function() { return spy.count > 0; }, 300000);
        var summary = spy.signalArguments[0][0];
        console.log("  save: " + Live.summaryLine(summary));
        return summary;
    }

    function undoAndWait() {
        var s = session();
        var spy = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.undoLastSave();
        tryVerify(function() { return spy.count > 0; }, 300000);
        var summary = spy.signalArguments[0][0];
        console.log("  undo: " + Live.summaryLine(summary));
        compare(summary.error, "");
        return summary;
    }

    // ---- 1. A cancelled read leaves nothing behind ----
    function test_01_scanCancel() {
        var ctrl = createTemporaryObject(scanController, testCase);
        ctrl.scan("rekordbox", rekordboxPath);
        var spy = createTemporaryObject(spyComponent, testCase, {target: ctrl, signalName: "scanCancelled"});
        tryVerify(function() { return ctrl.busy; }, 5000);
        ctrl.cancelScan();
        tryVerify(function() { return spy.count > 0; }, 60000);
        tryVerify(function() { return !ctrl.busy; }, 5000);
        compare(ctrl.tracks.rowCount(), 0);
        console.log("  cancelled scan: no rows, busy=" + ctrl.busy);

        ctrl.scan("rekordbox", rekordboxPath);
        waitIdle(ctrl);
        verify(ctrl.tracks.rowCount() > 0, "a full rescan after a cancel finds the tracks");
        console.log("  full scan: " + ctrl.tracks.rowCount() + " rekordbox tracks");
    }

    // ---- 2. Device Settings: stage, save, verify on disk, undo ----
    function test_02_settingsStageSaveUndo() {
        var page = createTemporaryObject(settingsPage, testCase, {stickLabel: stickLabel, pioneerRoot: rekordboxPath});
        var ctrl = Live.findByType(page, "SettingsController");
        verify(ctrl !== null, "the page created its controller");
        waitIdle(ctrl);
        verify(ctrl.groups.length > 0, "settings files were read");

        // The first field with a real choice.
        var target = null;
        for (var g = 0; g < ctrl.groups.length && target === null; ++g) {
            var fields = ctrl.groups[g].fields;
            for (var f = 0; f < fields.length; ++f) {
                if (fields[f].options.length >= 2 && fields[f].options.indexOf(fields[f].value) >= 0) {
                    target = {fileName: ctrl.groups[g].fileName, label: fields[f].label, value: fields[f].value,
                              other: fields[f].options[(fields[f].options.indexOf(fields[f].value) + 1) % fields[f].options.length]};
                    break;
                }
            }
        }
        verify(target !== null, "a settable field exists");
        console.log("  field " + target.fileName + " / " + target.label + ": " + target.value + " -> " + target.other);

        var s = session();
        compare(s.dirty, false);
        ctrl.setField(target.fileName, target.label, target.other);
        tryCompare(s, "dirty", true, 5000);
        compare(s.lockHeld, true);
        compare(EditSessionRegistry.anyEditing, true);
        var saveButton = findChild(page, "saveButton");
        verify(saveButton !== null && saveButton.enabled, "the Save overlay is enabled once something is staged");
        shot(page, "live-settings-staged");

        var summary = saveAndWait(false);
        compare(summary.written, 1);
        compare(summary.total, 1);
        compare(summary.error, "");
        tryCompare(findChild(page, "summaryDialog"), "opened", true, 5000);
        shot(page, "live-settings-summary");
        findChild(findChild(page, "summaryDialog"), "okButton").clicked();
        compare(s.dirty, false);

        var reread = createTemporaryObject(settingsController, testCase);
        reread.load(rekordboxPath);
        waitIdle(reread);
        var onDisk = valueOf(reread, target.fileName, target.label);
        compare(onDisk, target.other);
        console.log("  on disk after save: " + onDisk);

        compare(s.canUndo, true);
        undoAndWait();
        reread.load(rekordboxPath);
        waitIdle(reread);
        compare(valueOf(reread, target.fileName, target.label), target.value);
        console.log("  on disk after undo: " + valueOf(reread, target.fileName, target.label));
    }

    function valueOf(ctrl, fileName, label) {
        for (var g = 0; g < ctrl.groups.length; ++g) {
            if (ctrl.groups[g].fileName !== fileName) continue;
            for (var f = 0; f < ctrl.groups[g].fields.length; ++f) {
                if (ctrl.groups[g].fields[f].label === label) return ctrl.groups[g].fields[f].value;
            }
        }
        return "";
    }

    // ---- 3. Sync: stage every plan, save with an immediate cancel, discard the rest, undo ----
    function test_03_syncStageSaveCancel() {
        var page = createTemporaryObject(syncPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                              enginePath: enginePath, playbackController: fakePlayback});
        var ctrl = Live.findByType(page, "SyncController");
        verify(ctrl !== null);
        waitIdle(ctrl, 300000);
        var plans = ctrl.plans.rowCount();
        console.log("  sync plans: " + plans + " (rekordbox " + ctrl.rekordboxTrackCount + ", engine " + ctrl.engineTrackCount + ")");
        if (plans === 0) {
            skip("nothing to sync on this stick");
        }
        ctrl.apply();  // stages every plan that is not an unresolved conflict
        var s = session();
        tryVerify(function() { return s.pendingCount > 0 && s.pendingCount === ctrl.stagedCount; }, 10000);
        var staged = s.pendingCount;
        console.log("  staged " + staged + " of " + plans + " (" + ctrl.unresolvedConflicts.length + " unresolved conflicts stay out)");
        verify(staged <= plans);
        shot(page, "live-sync-staged");

        var summary = saveAndWait(true);
        compare(summary.error, "");
        compare(summary.written + s.pendingCount, summary.total);
        compare(summary.total, staged);
        tryCompare(findChild(page, "summaryDialog"), "opened", true, 5000);
        shot(page, "live-sync-summary");
        findChild(findChild(page, "summaryDialog"), "okButton").clicked();
        if (s.pendingCount > 0) {
            compare(summary.cancelled, true);
            s.discard();
            tryCompare(s, "pendingCount", 0, 5000);
            console.log("  discarded the " + (staged - summary.written) + " unwritten plan(s)");
        }
        if (summary.written > 0) {
            compare(s.canUndo, true);
            undoAndWait();
        }
    }

    // ---- 4. Stray cues: stage all, save, undo ----
    function test_04_junkCuesStageSaveUndo() {
        var page = createTemporaryObject(junkPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                              enginePath: enginePath});
        var ctrl = Live.findByType(page, "LibraryConsistencyController");
        verify(ctrl !== null);
        waitIdle(ctrl, 300000);
        var count = ctrl.junkCues.rowCount();
        console.log("  stray cues: " + count);
        if (count === 0) {
            skip("no stray cues on this stick");
        }
        ctrl.removeAllJunkCues();
        var s = session();
        tryCompare(s, "pendingCount", count, 5000);
        shot(page, "live-junk-staged");
        var summary = saveAndWait(false);
        compare(summary.error, "");
        compare(summary.written, count);
        tryCompare(ctrl, "busy", false, 120000);

        // The save is only real if a fresh scan no longer sees them. A test
        // that never re-reads passes just as happily against a writer that
        // quietly left the cue in place, which is exactly the bug the
        // Engine main-cue path had.
        ctrl.scan(rekordboxPath, enginePath);
        waitIdle(ctrl, 300000);
        compare(ctrl.junkCues.rowCount(), 0);
        console.log("  rescan after save: " + ctrl.junkCues.rowCount() + " stray cues remain");

        undoAndWait();
        ctrl.scan(rekordboxPath, enginePath);
        waitIdle(ctrl, 300000);
        compare(ctrl.junkCues.rowCount(), count);
        console.log("  rescan after undo: " + ctrl.junkCues.rowCount() + " stray cues back");
    }

    // ---- 5. Library Health: stage repairs, leave the page, choose Discard ----
    function test_05_libraryHealthLeaveDiscards() {
        var page = createTemporaryObject(healthPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                enginePath: enginePath, playbackController: fakePlayback});
        var ctrl = Live.findByType(page, "LibraryConsistencyController");
        verify(ctrl !== null);
        waitIdle(ctrl, 300000);
        console.log("  issues: " + ctrl.issues.rowCount() + ", repairable: " + ctrl.repairableCount);
        if (ctrl.repairableCount === 0) {
            skip("nothing repairable on this stick");
        }
        ctrl.repairAll();
        var s = session();
        tryVerify(function() { return s.pendingCount > 0; }, 5000);
        var crumb = Live.findByType(page, "BackBreadcrumb");
        verify(crumb !== null);
        crumb.homeRequested();
        var dialog = findChild(page, "unsavedDialog");
        tryCompare(dialog, "opened", true, 5000);
        shot(page, "live-health-unsaved");
        findChild(dialog, "discardButton").clicked();
        tryCompare(s, "pendingCount", 0, 5000);
        compare(s.dirty, false);
        compare(ctrl.stagedCount, 0);
    }

    // ---- 6. Manage Backups: prune two, restore the newest ----
    function test_06_backupsCleanAndRestore() {
        var page = createTemporaryObject(backupsPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                 enginePath: enginePath});
        var ctrl = Live.findByType(page, "BackupsController");
        verify(ctrl !== null);
        waitIdle(ctrl);
        var count = ctrl.backups.rowCount();
        console.log("  backups on the stick: " + count + ", " + ctrl.totalSizeHuman);
        if (count < 3) {
            skip("fewer than three backups to prune");
        }
        var spy = createTemporaryObject(spyComponent, testCase, {target: ctrl, signalName: "writeFinished"});
        ctrl.clean(count - 2);
        tryVerify(function() { return spy.count > 0; }, 120000);
        var summary = spy.signalArguments[0][0];
        console.log("  clean: " + Live.summaryLine(summary));
        compare(summary.written, 2);
        compare(summary.total, 2);
        compare(summary.verb, "deleted");
        waitIdle(ctrl);
        compare(ctrl.backups.rowCount(), count - 2);
        tryCompare(findChild(page, "summaryDialog"), "opened", true, 5000);
        shot(page, "live-backups-pruned");
        findChild(findChild(page, "summaryDialog"), "okButton").clicked();

        var newestId = ctrl.backups.data(ctrl.backups.index(0, 0), 257);  // IdRole
        console.log("  restoring " + newestId);
        spy.clear();
        ctrl.restoreBackup(newestId);
        tryVerify(function() { return spy.count > 0; }, 120000);
        summary = spy.signalArguments[0][0];
        console.log("  restore: " + Live.summaryLine(summary));
        compare(summary.error, "");
        compare(summary.written, 1);
        compare(summary.verb, "restored");
        waitIdle(ctrl);
        compare(ctrl.backups.rowCount(), count - 1);  // the pre-restore snapshot
        compare(EditSessionRegistry.anyWriting, false);
    }

    // ---- 7. Delete Orphaned Files: cancel before the first file ----
    function test_07_pendingDeletionsCancel() {
        var page = createTemporaryObject(pendingPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                 enginePath: enginePath,
                                                                 appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = Live.findByType(page, "CleanupController");
        verify(ctrl !== null);
        waitIdle(ctrl);
        var count = ctrl.pendingDeletions.rowCount();
        console.log("  pending deletions listed: " + count);
        if (count === 0) {
            skip("no pending deletions on this stick");
        }
        ctrl.setAllPendingDeletionIncluded(true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: ctrl, signalName: "pendingDeletionsWriteFinished"});
        ctrl.deleteSelectedPendingFiles();
        compare(ctrl.writing, true);
        ctrl.cancelWrite();
        compare(ctrl.writeCancellable, false);
        tryVerify(function() { return spy.count > 0; }, 300000);
        var summary = spy.signalArguments[0][0];
        console.log("  delete: " + Live.summaryLine(summary));
        compare(summary.error, "");
        compare(summary.unit, "files");
        verify(summary.cancelled || summary.written === summary.total, "either stopped at the cancel or already through");
        waitIdle(ctrl);
        tryCompare(findChild(page, "summaryDialog"), "opened", true, 5000);
        shot(page, "live-pending-cancelled");
        compare(EditSessionRegistry.anyWriting, false);
    }
}
