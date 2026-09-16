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
    Component { id: pendingPage; PendingDeletionsPage { width: 1100; height: 820 } }
    Component { id: scanController; ScanController {} }
    Component { id: settingsController; SettingsController {} }
    Component { id: appSettings; AppSettingsController {} }
    Component { id: cleanupComponent; CleanupController {} }
    Component { id: addCueComponent; AddCueController {} }
    Component { id: metadataBackupComponent; MetadataBackupController {} }
    // One row per file listed in Delete Orphaned Files: its path, and a way
    // to tick just that row (test_09 must not select files other saves
    // listed, which the delete would really remove).
    Component {
        id: pendingRowsComponent
        Instantiator {
            delegate: QtObject {
                readonly property string path: model.filePath
                function include(on) { model.included = on; }
            }
        }
    }
    Component { id: metadataRestoreComponent; MetadataRestoreController {} }

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
                    target = {fileName: fields[f].fileName, label: fields[f].label, value: fields[f].value,
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

    // Groups are categories now, not files, so the file name is on each
    // field and one label can only be matched together with its file.
    function valueOf(ctrl, fileName, label) {
        for (var g = 0; g < ctrl.groups.length; ++g) {
            for (var f = 0; f < ctrl.groups[g].fields.length; ++f) {
                var field = ctrl.groups[g].fields[f];
                if (field.fileName === fileName && field.label === label) return field.value;
            }
        }
        return "";
    }

    // ---- 3. Sync: stage every plan, save with an immediate cancel, discard the rest, undo ----
    function test_03_syncStageSaveCancel() {
        var page = createTemporaryObject(syncPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                              enginePath: enginePath, playbackController: fakePlayback,
                                                              appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = Live.findByType(page, "SyncController");
        verify(ctrl !== null);
        waitIdle(ctrl, 300000);
        var plans = ctrl.planCount;
        console.log("  sync plans: " + plans + " (rekordbox " + ctrl.rekordboxTrackCount + ", engine " + ctrl.engineTrackCount + ")");
        if (plans === 0) {
            skip("nothing to sync on this stick");
        }
        ctrl.stageSelected(false);  // every ready track starts ticked; decisions are never staged
        var s = session();
        tryVerify(function() { return s.pendingCount > 0 && s.pendingCount === ctrl.stagedCount; }, 10000);
        var staged = s.pendingCount;
        console.log("  staged " + staged + " of " + plans + " (" + ctrl.conflictCount + " decisions stay out)");
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
                                                              enginePath: enginePath,
                                                              appSettingsController: createTemporaryObject(appSettings, testCase)});
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
            if (typeof liveRigRequireRepairable !== "undefined" && liveRigRequireRepairable) {
                fail("an issue was planted, and Library Health finds nothing repairable");
            }
            skip("nothing repairable on this stick (tools/rig_plant_repairable plants one)");
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

    // ---- 8. Add a cue: rekordbox and its OneLibrary mirror, save, undo ----
    // The one place Seabass writes a cue nobody had before. A memory cue on
    // a rekordbox track without cues that OneLibrary lists too (same file):
    // after Save a fresh read of both catalogs has it, after Undo neither.
    readonly property double addedCueMs: 12345

    function findTrack(ctrl, key, value) {
        for (var i = 0; i < ctrl.tracks.trackCount(); ++i) {
            var t = ctrl.tracks.trackAt(i);
            if (t[key] === value) return t;
        }
        return null;
    }

    function addedCue(track) {
        if (track === null) return null;
        for (var c = 0; c < track.cues.length; ++c) {
            var cue = track.cues[c];
            if (cue.kind === "memory" && Math.abs(cue.positionMs - addedCueMs) < 50) return cue;
        }
        return null;
    }

    function test_08_addCueSaveUndo() {
        var rekordbox = createTemporaryObject(scanController, testCase);
        rekordbox.scan("rekordbox", rekordboxPath);
        waitIdle(rekordbox, 300000);
        if (!rekordbox.hasOneLibrary(rekordboxPath)) {
            skip("no OneLibrary on this stick");
        }
        var oneLibrary = createTemporaryObject(scanController, testCase);
        oneLibrary.scan("onelibrary", rekordboxPath);
        waitIdle(oneLibrary, 300000);

        var listed = {};
        for (var i = 0; i < oneLibrary.tracks.trackCount(); ++i) {
            listed[oneLibrary.tracks.trackAt(i).filePath] = true;
        }
        var target = null;
        for (var j = 0; j < rekordbox.tracks.trackCount() && target === null; ++j) {
            var t = rekordbox.tracks.trackAt(j);
            if (t.cues.length === 0 && t.filePath.length > 0 && listed[t.filePath] === true) target = t;
        }
        verify(target !== null, "a rekordbox track without cues that OneLibrary lists too");
        console.log("  track rekordbox id " + target.sourceId + ": " + target.artist + " - " + target.title);

        var ctrl = createTemporaryObject(addCueComponent, testCase);
        ctrl.addCue("rekordbox", rekordboxPath, target.sourceId, addedCueMs, "memory", 0, "", "rig W2", false, 0,
                    target.title);
        compare(ctrl.errorMessage, "");
        compare(ctrl.pendingCuesFor(target.sourceId).length, 1);
        var s = session();
        tryCompare(s, "pendingCount", 1, 5000);
        var summary = saveAndWait(false);
        compare(summary.error, "");
        compare(summary.written, 1);

        rekordbox.scan("rekordbox", rekordboxPath);
        waitIdle(rekordbox, 300000);
        oneLibrary.scan("onelibrary", rekordboxPath);
        waitIdle(oneLibrary, 300000);
        var inRekordbox = addedCue(findTrack(rekordbox, "sourceId", target.sourceId));
        var inOneLibrary = addedCue(findTrack(oneLibrary, "filePath", target.filePath));
        console.log("  after save: rekordbox " + (inRekordbox ? inRekordbox.positionMs + " ms" : "no cue")
                    + ", OneLibrary " + (inOneLibrary ? inOneLibrary.positionMs + " ms" : "no cue"));
        verify(inRekordbox !== null, "rekordbox has the added cue");
        verify(inOneLibrary !== null, "its OneLibrary mirror has it too");

        undoAndWait();
        rekordbox.scan("rekordbox", rekordboxPath);
        waitIdle(rekordbox, 300000);
        oneLibrary.scan("onelibrary", rekordboxPath);
        waitIdle(oneLibrary, 300000);
        var afterUndoRekordbox = findTrack(rekordbox, "sourceId", target.sourceId);
        var afterUndoOneLibrary = findTrack(oneLibrary, "filePath", target.filePath);
        console.log("  after undo: rekordbox cues " + afterUndoRekordbox.cues.length + ", OneLibrary cues "
                    + afterUndoOneLibrary.cues.length);
        compare(addedCue(afterUndoRekordbox), null);
        compare(addedCue(afterUndoOneLibrary), null);
        compare(afterUndoRekordbox.cues.length, 0);
    }

    // ---- 9. Clean Up: one duplicate group's extra copies, save, rescan, undo ----
    // Counts, not rows: the group is no longer offered, its extra copies'
    // rows are gone and their files wait in Delete Orphaned Files; undo
    // brings every row back. The files themselves are only deleted there.
    function test_09_cleanupOneGroupSaveUndo() {
        var ctrl = createTemporaryObject(cleanupComponent, testCase);
        ctrl.scan("rekordbox", rekordboxPath);
        waitIdle(ctrl, 300000);
        var groups = ctrl.plans.rowCount();
        console.log("  duplicate groups offered: " + groups);
        if (groups === 0) {
            skip("no duplicate groups on this stick");
        }
        var tracks = createTemporaryObject(scanController, testCase);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        var rowsBefore = tracks.totalTrackCount;
        var pendingBefore = ctrl.pendingDeletions.rowCount();
        // Files other saves already listed, on this stick's own list.
        var listedBefore = {};
        var rowsBeforeSave = createTemporaryObject(pendingRowsComponent, testCase, {model: ctrl.pendingDeletions});
        for (var b = 0; b < rowsBeforeSave.count; ++b) {
            listedBefore[rowsBeforeSave.objectAt(b).path] = true;
        }

        ctrl.setAllIncluded(false);
        ctrl.setIncluded(0, true);
        compare(ctrl.includedCount, 1);
        ctrl.apply();
        var s = session();
        tryVerify(function() { return s.pendingCount === 1; }, 5000);
        var summary = saveAndWait(false);
        compare(summary.error, "");
        compare(summary.written, 1);
        waitIdle(ctrl, 300000);

        ctrl.scan("rekordbox", rekordboxPath);
        waitIdle(ctrl, 300000);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        console.log("  after save: groups " + groups + " -> " + ctrl.plans.rowCount() + ", rekordbox rows " + rowsBefore
                    + " -> " + tracks.totalTrackCount + ", files waiting for deletion " + pendingBefore + " -> "
                    + ctrl.pendingDeletions.rowCount());
        compare(ctrl.plans.rowCount(), groups - 1);
        verify(tracks.totalTrackCount < rowsBefore, "the extra copies' rows are gone");
        verify(ctrl.pendingDeletions.rowCount() > pendingBefore, "their files wait in Delete Orphaned Files");

        undoAndWait();
        ctrl.scan("rekordbox", rekordboxPath);
        waitIdle(ctrl, 300000);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        console.log("  after undo: groups " + ctrl.plans.rowCount() + ", rekordbox rows " + tracks.totalTrackCount
                    + ", files waiting for deletion " + ctrl.pendingDeletions.rowCount());
        compare(ctrl.plans.rowCount(), groups);
        compare(tracks.totalTrackCount, rowsBefore);

        // Undo brings the rows back but leaves the files listed in Delete
        // Orphaned Files. Deleting them now must refuse every file a
        // catalog references again -- or undo would have made the track
        // playable only to lose its audio on the next delete.
        var listed = ctrl.pendingDeletions.rowCount();
        if (listed > pendingBefore) {
            // Only this save's files. Ticking every row also selected files
            // other saves listed, which are still orphaned: the delete
            // removed them for real and the check below then failed.
            ctrl.setAllPendingDeletionIncluded(false);
            var rowsAfterUndo = createTemporaryObject(pendingRowsComponent, testCase, {model: ctrl.pendingDeletions});
            var ticked = 0;
            for (var r = 0; r < rowsAfterUndo.count; ++r) {
                var row = rowsAfterUndo.objectAt(r);
                if (!listedBefore[row.path]) {
                    row.include(true);
                    ticked++;
                }
            }
            compare(ticked, listed - pendingBefore, "every file this save listed, and no other");
            var spy = createTemporaryObject(spyComponent, testCase, {target: ctrl, signalName: "pendingDeletionsWriteFinished"});
            ctrl.deleteSelectedPendingFiles();
            tryVerify(function() { return spy.count > 0; }, 300000);
            var deletion = spy.signalArguments[0][0];
            console.log("  delete after undo: " + Live.summaryLine(deletion));
            waitIdle(ctrl, 300000);
            compare(deletion.written, 0, "no file a catalog references again is deleted");
            tracks.scan("rekordbox", rekordboxPath);
            waitIdle(tracks, 300000);
            compare(tracks.totalTrackCount, rowsBefore);
        }
    }

    // ---- 13. Delete Orphaned Files: cancel part-way, then finish ----
    // Rig check W8. test_07 only ever cancels, and skips whenever the
    // stick lists nothing -- which, since undo learnt to take a Clean
    // Up's files back off the list, is always. So this plants the state
    // itself: one Clean Up group saved and NOT undone, which is what puts
    // real files on the list. The runner restores the stick from its
    // reference afterwards, because this check deletes audio for real.
    function test_13_pendingDeletionsCancelThenComplete() {
        if (typeof liveRigDeleteOrphans === "undefined" || !liveRigDeleteOrphans) {
            skip("SEABASS_RIG_DELETE_ORPHANS is not set: this one deletes files for good");
        }
        var cleanup = createTemporaryObject(cleanupComponent, testCase);
        cleanup.scan("rekordbox", rekordboxPath);
        waitIdle(cleanup, 300000);
        if (cleanup.plans.rowCount() === 0) {
            skip("no duplicate groups on this stick to plant a deletion with");
        }
        // What is already listed, by path: an earlier run of this check
        // can have left entries behind, and counting alone cannot tell
        // those from the ones this run is about to make.
        var listedBefore = {};
        var before = createTemporaryObject(pendingRowsComponent, testCase, {model: cleanup.pendingDeletions});
        for (var b = 0; b < before.count; ++b) {
            listedBefore[before.objectAt(b).path] = true;
        }
        var pendingBefore = before.count;

        // Plant: two groups, so the cancel has more than one file to leave
        // behind. Groups are staged by default only when nothing about
        // them is ambiguous, which is exactly the kind that orphans a file.
        cleanup.setAllIncluded(false);
        var staged = 0;
        for (var g = 0; g < cleanup.plans.rowCount() && staged < 2; ++g) {
            cleanup.setIncluded(g, true);
            if (cleanup.includedCount > staged) {
                staged = cleanup.includedCount;
            }
        }
        verify(staged >= 1, "at least one group staged");
        cleanup.apply();
        var s = session();
        // stagedCount, not includedCount: apply() skips a plan whose
        // survivor another plan already staged, so pendingCount can land
        // below the number of ticks.
        tryVerify(function() { return s.pendingCount === cleanup.stagedCount && s.pendingCount > 0; }, 10000);
        var saved = saveAndWait(false);
        compare(saved.error, "");
        waitIdle(cleanup, 300000);
        cleanup.refreshPendingDeletions();

        // The list has to have gained a file this save orphaned; a group
        // whose copies were already removed adds nothing, and a check that
        // accepted that would be testing the previous run's leftovers.
        var afterSave = createTemporaryObject(pendingRowsComponent, testCase, {model: cleanup.pendingDeletions});
        var planted = 0;
        for (var a = 0; a < afterSave.count; ++a) {
            if (!listedBefore[afterSave.objectAt(a).path]) {
                planted++;
            }
        }
        var listed = cleanup.pendingDeletions.rowCount();
        console.log("  cleaned up " + staged + " group(s); files waiting for deletion: " + pendingBefore + " -> "
                    + listed + " (" + planted + " new)");
        if (planted === 0) {
            // Nothing was orphaned, so there is nothing to cancel or
            // finish. The usual cause is this check having run once
            // already: it deletes the extra copies for good, and the
            // groups then have nothing left to remove.
            //
            // A failure, not a skip: the rig arms this check only on a
            // stick it has just restored, so there is no honest way for it
            // to find nothing -- and the runner counts a skipped test as a
            // pass, which is how a check quietly stops checking.
            fail("no extra copies left to orphan on this stick: restore it before running W8 "
                 + "(this check deletes the copies it plants)");
        }

        // Cancel: tick only what this save orphaned, start, and stop it.
        // The worker can be through before the cancel lands -- with one
        // file it usually is -- so a finished run is accepted below rather
        // than demanded to be still writing here.
        cleanup.setAllPendingDeletionIncluded(false);
        var toDelete = createTemporaryObject(pendingRowsComponent, testCase, {model: cleanup.pendingDeletions});
        var doomed = [];
        for (var t = 0; t < toDelete.count; ++t) {
            var row = toDelete.objectAt(t);
            if (!listedBefore[row.path]) {
                row.include(true);
                doomed.push(row.path);
            }
        }
        compare(doomed.length, planted);
        var cancelSpy = createTemporaryObject(spyComponent, testCase,
                                              {target: cleanup, signalName: "pendingDeletionsWriteFinished"});
        cleanup.deleteSelectedPendingFiles();
        // Proof it started: deleteSelectedPendingFiles() also returns in
        // silence when the edit lock is refused, and cancelWrite() is then
        // a no-op -- a clear failure here beats a five-minute timeout.
        verify(cleanup.writing === true || cancelSpy.count > 0, "the delete started");
        cleanup.cancelWrite();
        tryVerify(function() { return cancelSpy.count > 0; }, 300000);
        var cancelled = cancelSpy.signalArguments[0][0];
        console.log("  cancelled delete: " + Live.summaryLine(cancelled));
        // What the app guarantees when a delete is stopped: it says so --
        // through the `cancelled` flag, or through the message, depending
        // on whether the run had started deleting -- and it deletes fewer
        // files than were listed. Not an error either way.
        // Either it stopped in time, or it was already through -- with one
        // or two files the worker often is. Both are fine; silently
        // deleting more than was ticked is not.
        verify(cancelled.cancelled === true || cancelled.error.length > 0 || cancelled.written === doomed.length,
               "a stopped delete says it stopped, or had already finished");
        verify(cancelled.written <= doomed.length, "it never deletes more than was ticked");
        waitIdle(cleanup, 300000);
        cleanup.refreshPendingDeletions();
        var leftListed = cleanup.pendingDeletions.rowCount();
        console.log("  still listed after the cancel: " + leftListed + " of " + listed
                    + " (the cancel deleted " + cancelled.written + ")");
        verify(leftListed === listed - cancelled.written,
               "exactly what the cancel did not delete is still listed");

        // Finish: the rest of this save's files go, and only those. Ticking
        // everything would sweep up entries other saves left listed, which
        // are still orphaned and are not this check's to delete.
        cleanup.setAllPendingDeletionIncluded(false);
        var remaining = createTemporaryObject(pendingRowsComponent, testCase, {model: cleanup.pendingDeletions});
        var stillDoomed = [];
        for (var i = 0; i < remaining.count; ++i) {
            var left = remaining.objectAt(i);
            if (!listedBefore[left.path]) {
                left.include(true);
                stillDoomed.push(left.path);
            }
        }
        if (stillDoomed.length === 0) {
            // The cancel was too late and everything already went. Nothing
            // left to finish -- and asking the app to delete an empty
            // selection returns without a signal, which would have been a
            // ten-minute wait for nothing.
            for (var g = 0; g < doomed.length; ++g) {
                verify(!Live.fileExists(doomed[g]), "deleted for real: " + doomed[g]);
            }
            compare(cleanup.pendingDeletions.rowCount(), pendingBefore);
            EditSessionRegistry.closeSession(testCase.libraryId);
            // Half of what this check is named for -- resume, then finish
            // exactly the rest -- did not run. Returning quietly wrote PASS
            // for it, and a cancelWrite() that regressed to a no-op would
            // land here on every run and never be seen. skip() is the one
            // marker both runners already count as a failure.
            skip("the cancel came too late: all " + doomed.length + " files had gone, so the "
                 + "resume-and-finish half never ran");
        }
        var finishSpy = createTemporaryObject(spyComponent, testCase,
                                              {target: cleanup, signalName: "pendingDeletionsWriteFinished"});
        cleanup.deleteSelectedPendingFiles();
        tryVerify(function() { return finishSpy.count > 0; }, 600000);
        var finished = finishSpy.signalArguments[0][0];
        console.log("  completed delete: " + Live.summaryLine(finished));
        compare(finished.error, "");
        compare(finished.written, stillDoomed.length);
        waitIdle(cleanup, 300000);
        cleanup.refreshPendingDeletions();
        // Back to what was listed before this check ran: its own files are
        // gone, everything another save listed is untouched.
        compare(cleanup.pendingDeletions.rowCount(), pendingBefore);
        for (var d = 0; d < doomed.length; ++d) {
            verify(!Live.fileExists(doomed[d]), "deleted for real: " + doomed[d]);
        }
        EditSessionRegistry.closeSession(testCase.libraryId);
    }

    // ---- 10. Library Health: repair, save, rescan, undo ----
    // Needs something to repair. tools/rig-edits.sh plants a Repairable
    // issue first (tools/rig_plant_repairable moves one copy of a cue-free
    // duplicate aside) and puts the file back afterwards.
    function test_10_libraryHealthRepairSaveUndo() {
        var page = createTemporaryObject(healthPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                enginePath: enginePath, playbackController: fakePlayback});
        var ctrl = Live.findByType(page, "LibraryConsistencyController");
        verify(ctrl !== null);
        waitIdle(ctrl, 300000);
        var issuesBefore = ctrl.issues.rowCount();
        var repairable = ctrl.repairableCount;
        console.log("  issues: " + issuesBefore + ", repairable: " + repairable);
        if (repairable === 0) {
            if (typeof liveRigRequireRepairable !== "undefined" && liveRigRequireRepairable) {
                fail("an issue was planted, and Library Health finds nothing repairable");
            }
            skip("nothing repairable on this stick (tools/rig_plant_repairable plants one)");
        }
        var tracks = createTemporaryObject(scanController, testCase);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        var rowsBefore = tracks.totalTrackCount;

        ctrl.repairAll();
        var s = session();
        tryVerify(function() { return s.pendingCount > 0; }, 5000);
        shot(page, "live-health-repair-staged");
        var summary = saveAndWait(false);
        compare(summary.error, "");
        verify(summary.written > 0, "the repair wrote something");
        waitIdle(ctrl, 300000);

        // Only a fresh read proves the repair: the issue is gone and the
        // broken copy's row with it.
        ctrl.scan(rekordboxPath, enginePath);
        waitIdle(ctrl, 300000);
        console.log("  rescan after repair: issues " + ctrl.issues.rowCount() + ", repairable " + ctrl.repairableCount);
        compare(ctrl.repairableCount, 0);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        console.log("  rekordbox rows: " + rowsBefore + " -> " + tracks.totalTrackCount);
        verify(tracks.totalTrackCount < rowsBefore, "the broken copy's row is gone");

        undoAndWait();
        ctrl.scan(rekordboxPath, enginePath);
        waitIdle(ctrl, 300000);
        console.log("  rescan after undo: issues " + ctrl.issues.rowCount() + ", repairable " + ctrl.repairableCount);
        compare(ctrl.repairableCount, repairable);
        compare(ctrl.issues.rowCount(), issuesBefore);
        tracks.scan("rekordbox", rekordboxPath);
        waitIdle(tracks, 300000);
        compare(tracks.totalTrackCount, rowsBefore);
    }

    // ---- 11. For the two-stick checks: add a memory cue and keep it ----
    // Runs only when SEABASS_RIG_KEEP_CUE_MS names a position (the runner
    // exposes it as liveRigKeepCueMs): tools/rig-clones.sh changes one
    // stick's library this way so the other stick has something to catch
    // up on. Nothing here undoes it; the runner restores the stick.
    function test_11_rigKeepCue() {
        if (typeof liveRigKeepCueMs === "undefined" || liveRigKeepCueMs <= 0) {
            skip("SEABASS_RIG_KEEP_CUE_MS is not set");
        }
        var rekordbox = createTemporaryObject(scanController, testCase);
        rekordbox.scan("rekordbox", rekordboxPath);
        waitIdle(rekordbox, 300000);
        // A track OneLibrary lists too: this test exists to change the
        // library, not to find out what Add Cue does with a track only
        // rekordbox knows (test_08 and the rig notes cover that).
        var listed = {};
        if (rekordbox.hasOneLibrary(rekordboxPath)) {
            var oneLibrary = createTemporaryObject(scanController, testCase);
            oneLibrary.scan("onelibrary", rekordboxPath);
            waitIdle(oneLibrary, 300000);
            for (var o = 0; o < oneLibrary.tracks.trackCount(); ++o) {
                listed[oneLibrary.tracks.trackAt(o).filePath] = true;
            }
        }
        var target = null;
        for (var i = 0; i < rekordbox.tracks.trackCount() && target === null; ++i) {
            var t = rekordbox.tracks.trackAt(i);
            if (t.filePath.length === 0) continue;
            if (rekordbox.hasOneLibrary(rekordboxPath) && listed[t.filePath] !== true) continue;
            var taken = false;
            for (var c = 0; c < t.cues.length; ++c) {
                if (Math.abs(t.cues[c].positionMs - liveRigKeepCueMs) < 50) taken = true;
            }
            if (!taken) target = t;
        }
        verify(target !== null, "a track without a cue at that position");
        console.log("  keeping a memory cue at " + liveRigKeepCueMs + " ms on rekordbox id " + target.sourceId + ": "
                    + target.artist + " - " + target.title);
        var ctrl = createTemporaryObject(addCueComponent, testCase);
        ctrl.addCue("rekordbox", rekordboxPath, target.sourceId, liveRigKeepCueMs, "memory", 0, "", "rig kept", false, 0,
                    target.title);
        compare(ctrl.errorMessage, "");
        var summary = saveAndWait(false);
        compare(summary.error, "");
        compare(summary.written, 1);
        rekordbox.scan("rekordbox", rekordboxPath);
        waitIdle(rekordbox, 300000);
        var reread = findTrack(rekordbox, "sourceId", target.sourceId);
        var found = false;
        for (var k = 0; k < reread.cues.length; ++k) {
            if (reread.cues[k].kind === "memory" && Math.abs(reread.cues[k].positionMs - liveRigKeepCueMs) < 50) found = true;
        }
        verify(found, "the kept cue reads back");
    }

    // ---- 12. Metadata backup from another stick, restored onto this one, undo ----
    // Needs SEABASS_LIVE_SECOND_STICK (liveSecondStickRoot): its metadata
    // goes into the local store, and the restore onto this stick offers it
    // for the tracks both libraries have. A proposal only exists for a
    // matched track, so what the save writes is matched tracks only; a
    // rescan afterwards has less left to offer, and undo brings the offer
    // back.
    function test_12_metadataFromSecondStickSaveUndo() {
        if (typeof liveSecondStickRoot === "undefined" || liveSecondStickRoot.length === 0) {
            skip("SEABASS_LIVE_SECOND_STICK is not set");
        }
        var sourcePioneer = liveSecondStickRoot + "/PIONEER";
        var sourceLabel = liveSecondStickRoot.substring(liveSecondStickRoot.lastIndexOf("/") + 1);
        var sourceId = EditSessionRegistry.libraryIdForPath(sourcePioneer);
        verify(sourceId.length > 0, "the second stick has a library id");

        var backup = createTemporaryObject(metadataBackupComponent, testCase);
        verify(backup.selectStick(sourcePioneer, sourceId, sourceLabel), "the second stick can be selected");
        tryVerify(function() { return backup.hasScanned && !backup.busy; }, 600000);
        compare(backup.errorMessage, "");
        console.log("  backup from " + sourceLabel + ": " + backup.tracksSeen + " tracks seen, " + backup.proposalCount
                    + " to add, " + backup.alreadyCurrent + " already in the store");
        if (backup.proposalCount > 0) {
            backup.stageAllForAdd();
            compare(backup.stagedAddCount, backup.proposalCount);
            var stored = createTemporaryObject(spyComponent, testCase, {target: backup, signalName: "saveCompleted"});
            backup.save();
            tryVerify(function() { return stored.count > 0; }, 600000);
            compare(backup.errorMessage, "");
            console.log("  store now holds " + backup.storedTrackCount + " tracks");
        }

        var restore = createTemporaryObject(metadataRestoreComponent, testCase);
        restore.scan(rekordboxPath);
        tryVerify(function() { return restore.hasScanned && !restore.busy; }, 600000);
        compare(restore.errorMessage, "");
        var offered = restore.proposalCount;
        console.log("  restore onto " + stickLabel + ": " + restore.stickTrackCount + " tracks, " + offered
                    + " proposals, " + restore.conflictCount + " conflicts (" + restore.conflictsLeftAlone + " left alone)");
        if (offered === 0) {
            skip("no matched track has anything to restore from the second stick");
        }
        // The Restore Metadata page opens this stick's session through its
        // EditSessionHost, which is what gives the session the library
        // paths a save needs; without a page, the test opens it the same way.
        var s = EditSessionRegistry.openSession(testCase.libraryId, stickLabel, rekordboxPath, "");
        verify(s !== null);
        restore.stageAll();
        tryVerify(function() { return s.pendingCount === restore.stagedCount && s.pendingCount > 0; }, 10000);
        var staged = s.pendingCount;
        var summary = saveAndWait(false);
        compare(summary.error, "");
        compare(summary.written, staged);

        restore.scan(rekordboxPath);
        tryVerify(function() { return !restore.busy; }, 600000);
        console.log("  after save: " + restore.proposalCount + " proposals left");
        verify(restore.proposalCount < offered, "what was restored is no longer offered");

        undoAndWait();
        restore.scan(rekordboxPath);
        tryVerify(function() { return !restore.busy; }, 600000);
        console.log("  after undo: " + restore.proposalCount + " proposals");
        // Undo puts the bytes back (the runner compares the catalogs), but
        // the files keep the undo's modification time, so for a track whose
        // cues conflict the stick can now count as the newer side. Every
        // proposal without a conflict must be offered again.
        verify(restore.proposalCount >= offered - restore.conflictCount,
               "undo brought back every proposal that had no conflict");
        EditSessionRegistry.closeSession(testCase.libraryId);
    }

    // ---- 7. Delete Orphaned Files: cancel before the first file ----
    function test_07_pendingDeletionsCancel() {
        // Whether this run had to make its own precondition. A planted
        // Clean Up save removes duplicate rows and merges their cues onto
        // the survivor, so the stick no longer matches the reference the
        // rig compares it against after the bundle (rig-shakedown.sh's
        // unchanged_catalogs). It is undone at the end.
        var planted = false;
        // What the stick listed before this check touched anything, and
        // what the plant added on top: the cancel below must only ever tick
        // a file this check created.
        var listedAtStart = {};
        var plantedPaths = {};
        var page = createTemporaryObject(pendingPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                 enginePath: enginePath,
                                                                 appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = Live.findByType(page, "CleanupController");
        verify(ctrl !== null);
        waitIdle(ctrl);
        var count = ctrl.pendingDeletions.rowCount();
        console.log("  pending deletions listed: " + count);
        var atStart = createTemporaryObject(pendingRowsComponent, testCase, {model: ctrl.pendingDeletions});
        for (var b = 0; b < atStart.count; ++b) {
            listedAtStart[atStart.objectAt(b).path] = true;
        }
        if (count === 0) {
            // Plant one rather than skip: a tidy stick is not a reason to
            // prove nothing. A Clean Up save orphans a file and lists it,
            // which is exactly the state this check cancels a delete in.
            // It destroys a real duplicate copy -- the rig restores the
            // stick from its reference afterwards, which is what makes
            // planting affordable here.
            console.log("  nothing listed: planting a deletion with a Clean Up save");
            var planter = createTemporaryObject(cleanupComponent, testCase);
            planter.scan("rekordbox", rekordboxPath);
            waitIdle(planter, 300000);
            verify(planter.plans.rowCount() > 0,
                   "a duplicate group to plant a deletion with (nothing to plant from)");
            // Try groups until one actually lists a file, rather than
            // predicting which will. Two models of "plantable" were wrong
            // before this: any-group (W8 had already deleted the second
            // copy, so the save orphaned nothing) and unreferencedCount >
            // unreferencedHeldBackCount -- which is 0 for all 224 plans on
            // a reference stick, because that role counts copies that are
            // ALREADY unreferenced, not ones a clean-up would orphan. The
            // save itself is the only honest test, and an attempt that
            // lists nothing is undone before the next one.
            var plantedGroups = 0;
            var attempts = Math.min(6, planter.plans.rowCount());
            for (var attempt = 0; attempt < attempts && count === 0; ++attempt) {
                planter.setAllIncluded(false);
                planter.setIncluded(attempt, true);
                if (planter.includedCount === 0) {
                    continue;  // ambiguous group, nothing staged
                }
                planter.apply();
                var ps = session();
                tryVerify(function() { return ps.pendingCount === planter.stagedCount && ps.pendingCount > 0; }, 10000);
                var plantSave = saveAndWait(false);
                compare(plantSave.error, "");
                waitIdle(planter, 300000);
                ctrl.refreshPendingDeletions();
                waitIdle(ctrl, 300000);
                count = ctrl.pendingDeletions.rowCount();
                if (count > 0) {
                    plantedGroups = 1;
                    console.log("  planted with group " + attempt + ", pending deletions listed: " + count);
                    var afterPlant = createTemporaryObject(pendingRowsComponent, testCase, {model: ctrl.pendingDeletions});
                    for (var a = 0; a < afterPlant.count; ++a) {
                        var plantedPath = afterPlant.objectAt(a).path;
                        if (!listedAtStart[plantedPath]) {
                            plantedPaths[plantedPath] = true;
                        }
                    }
                    break;
                }
                // Listed nothing: put it back before trying the next group,
                // so a failed attempt never leaves the stick off reference.
                console.log("  group " + attempt + " cleaned up but orphaned nothing; undoing");
                var undoAttempt = session();
                if (undoAttempt === null || undoAttempt.canUndo !== true) {
                    // Six applied clean-ups could otherwise stay on the stick
                    // and the round would end dirty with no clue which check
                    // did it.
                    fail("an attempt that orphaned nothing cannot be undone; the stick is off its reference");
                }
                var undone = createTemporaryObject(spyComponent, testCase,
                                                  {target: undoAttempt, signalName: "saveFinished"});
                undoAttempt.undoLastSave();
                tryVerify(function() { return undone.count > 0; }, 600000);
                compare(undone.signalArguments[0][0].error, "", "the undo of a fruitless attempt worked");
                planter.scan("rekordbox", rekordboxPath);
                waitIdle(planter, 300000);
            }
            if (plantedGroups === 0) {
                skip("no duplicate group on this stick whose clean-up lists a file for deletion");
            }
            planted = true;
        }
        // A row this check planted, never one that was already listed: the
        // cancel below is a race against the worker, and a lost race
        // deletes the audio for good. undoLastSave() puts catalog rows
        // back, not media -- and unchanged_catalogs() only checksums the
        // catalogs, so the rig would call the stick clean while a file it
        // lists is gone. Ticking row 0 risked exactly that on a stick that
        // already carried orphans.
        ctrl.setAllPendingDeletionIncluded(false);
        var rows = createTemporaryObject(pendingRowsComponent, testCase, {model: ctrl.pendingDeletions});
        var ticked = 0;
        for (var r = 0; r < rows.count && ticked < 1; ++r) {
            var candidate = rows.objectAt(r);
            if (plantedPaths[candidate.path] === true) {
                candidate.include(true);
                ticked = 1;
            }
        }
        if (ticked === 0) {
            // Everything listed predates this check: cancelling a delete of
            // one of those risks real audio for nothing.
            skip("no pending deletion this check planted, so none it may safely cancel");
        }
        var spy = createTemporaryObject(spyComponent, testCase, {target: ctrl, signalName: "pendingDeletionsWriteFinished"});
        ctrl.deleteSelectedPendingFiles();
        compare(ctrl.writing, true);
        ctrl.cancelWrite();
        compare(ctrl.writeCancellable, false);
        tryVerify(function() { return spy.count > 0; }, 300000);
        var summary = spy.signalArguments[0][0];
        console.log("  delete: " + Live.summaryLine(summary));
        compare(summary.unit, "files");
        // Not compare(error, ""): the worker raises OperationCancelled, so a
        // cancel that lands before the first file is written comes back as
        // 0 of 0 with "operation cancelled" on it. That was invisible while
        // this check leaned on whatever the stick happened to carry -- with
        // enough entries the cancel always arrived mid-run. Planting exactly
        // one made it deterministic. Same contract W8 states: a stopped
        // delete says it stopped, or it had already finished.
        // "operation cancelled" specifically, not any error: a real I/O
        // failure ("could not delete X: permission denied") would satisfy
        // a bare error.length check and hide exactly what this is here to
        // catch.
        verify(summary.cancelled === true
                   || summary.error.indexOf("cancel") >= 0
                   || summary.written === summary.total,
               "a stopped delete says it stopped, or had already finished (got: " + summary.error + ")");
        verify(summary.written <= summary.total, "it never deletes more than was ticked");
        waitIdle(ctrl);
        tryCompare(findChild(page, "summaryDialog"), "opened", true, 5000);
        shot(page, "live-pending-cancelled");
        compare(EditSessionRegistry.anyWriting, false);

        // Put the library back if this run planted its own deletion: every
        // other check in the round compares this stick against its
        // reference, and a Clean Up save left standing fails all of them.
        if (planted) {
            var s = session();
            if (s !== null && s.canUndo === true) {
                var undone = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
                s.undoLastSave();
                tryVerify(function() { return undone.count > 0; }, 600000);
                console.log("  planted save undone: " + Live.summaryLine(undone.signalArguments[0][0]));
                compare(undone.signalArguments[0][0].error, "");
            } else {
                fail("the planted Clean Up save cannot be undone, so the stick is left off its reference");
            }
        }
    }
}
