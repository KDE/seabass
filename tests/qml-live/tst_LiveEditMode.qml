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
            ctrl.setAllPendingDeletionIncluded(true);
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
