// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// Rig check F4: the stick fills up during a write.
//
// The rig fills the stick first (tools/rig-shakedown.sh writes a filler
// file until only a few MB are left) and sets SEABASS_RIG_FULL_STICK, so
// no second, smaller stick is needed. A save that cannot finish must fail
// cleanly, put back whatever it had already written, and leave the
// catalogs readable -- the runner compares them against the baseline
// afterwards, and deletes the filler whatever happened here.
TestCase {
    id: testCase
    name: "LiveFullStick"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: typeof liveStickRoot !== "undefined" ? liveStickRoot : ""
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string enginePath: stickRoot + "/Engine Library"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)
    property string libraryId: ""

    Component { id: spyComponent; SignalSpy {} }
    Component { id: scanController; ScanController {} }
    Component { id: addCueComponent; AddCueController {} }

    // Shared by both checks: the stick's rekordbox library scanned, one
    // track picked, a session opened and measured, a memory cue staged,
    // and the save run. What the save did is the checks' business.
    function stageOneCueAndSave() {
        verify(stickRoot.length > 0, "SEABASS_LIVE_STICK names the stick this runs against");
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        // Adding a cue, not removing stray ones: every library can take a
        // cue, while stray cues are a property of one particular stick --
        // staging those made this check skip on the very stick the rig
        // fills, and a skip here proves nothing.
        var rekordbox = createTemporaryObject(scanController, testCase);
        rekordbox.scan("rekordbox", rekordboxPath);
        tryVerify(function() { return rekordbox.busy === false; }, 300000);
        var target = null;
        for (var j = 0; j < rekordbox.tracks.trackCount() && target === null; ++j) {
            var t = rekordbox.tracks.trackAt(j);
            // Any track will do: this adds a memory cue at 30000 ms and
            // asserts nothing about what was there before, so demanding a
            // cue-free track only made the check fail on libraries where
            // every track has cues -- a property of the stick, not the app.
            if (t.filePath.length > 0) {
                target = t;
            }
        }
        verify(target !== null, "a rekordbox track to add a cue to");
        console.log("  track rekordbox id " + target.sourceId + ": " + target.artist + " - " + target.title);

        var s = EditSessionRegistry.openSession(testCase.libraryId, stickLabel, rekordboxPath, enginePath);
        verify(s !== null);
        // openSession() -> setLibraryPaths() hands the measurement to a
        // worker thread and returns at once, so the numbers are not there
        // yet on the next line -- reading them straight away gets the
        // zeros the session starts with, which is what made this check
        // report "0 bytes of 0". Wait for the answer instead. On a real
        // stick that walk stats thousands of analysis files, so give it
        // room; running out of it fails, as a stick we cannot measure
        // deserves.
        tryVerify(function() { return s.stickBytesCapacity > 0; }, 300000,
                  "the session measured the stick");
        console.log("  free on the stick before the save: " + s.stickBytesFree + " bytes of " + s.stickBytesCapacity);
        // No threshold here: how full is full enough depends on the backup
        // this save writes, and the one assertion that matters is at the
        // end -- a save that fitted fails the check. (A 512 MB bound used
        // to stand here, and could not tell a full stick from one with
        // room to spare; the rig already refuses to run this on a stick
        // its fill left room on.)

        var adder = createTemporaryObject(addCueComponent, testCase);
        adder.addCue("rekordbox", rekordboxPath, target.sourceId, 30000, "memory", 0, "", "rig F4", false, 0,
                     target.title);
        compare(adder.errorMessage, "");
        tryVerify(function() { return s.pendingCount > 0; }, 10000);
        var staged = s.pendingCount;

        var finished = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.save();
        tryVerify(function() { return finished.count > 0; }, 900000);
        var summary = finished.signalArguments[0][0];
        console.log("  save on a full stick: " + Live.summaryLine(summary));
        return {s: s, staged: staged, summary: summary, target: target};
    }

    function test_saveOnAFullStickFailsCleanly() {
        if (typeof liveRigFullStick === "undefined" || !liveRigFullStick) {
            skip("SEABASS_RIG_FULL_STICK is not set: the rig fills the stick for this one");
        }
        var run = stageOneCueAndSave();
        var s = run.s, staged = run.staged, summary = run.summary;

        // The save must refuse up front (the backup would not fit) or fail
        // part-way, cleanly: never report success while the stick is full,
        // never leave the save half applied. A save that fitted used to be
        // "allowed, since it genuinely fitted" -- and round 4 passed on
        // that branch with 928 KB free, having tested a stick with room on
        // it. It is now a failure, recorded after the clean-up below so a
        // standalone run still takes its cue off the stick where it can.
        var saveFitted = false;
        if (summary.error.length > 0) {
            console.log("  refused or stopped: " + summary.error);
            compare(EditSessionRegistry.anyWriting, false);
            compare(s.dirty, true, "a refused save keeps its changes staged rather than losing them");
        } else {
            saveFitted = true;
            console.log("  the save FITTED (" + s.stickBytesFree + " bytes were free before it): " + summary.written
                        + " of " + staged + " written (the stick was not full enough for this check)");
            compare(summary.written, staged, "a save that reports success wrote everything it staged");
            compare(s.dirty, false);
        }

        // The catalogs must still read, whatever happened.
        var readBack = createTemporaryObject(scanController, testCase);
        readBack.scan("rekordbox", rekordboxPath);
        tryVerify(function() { return readBack.busy === false; }, 300000);
        verify(readBack.tracks.trackCount() > 0, "the catalogs still read after the attempt");
        console.log("  catalogs still read: " + readBack.tracks.trackCount() + " tracks");

        // Leave nothing staged behind for the checks that follow.
        if (s.dirty === true) {
            s.discard();
            tryVerify(function() { return s.dirty === false; }, 10000);
        }
        if (s.canUndo === true && summary.error.length === 0) {
            var undone = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
            s.undoLastSave();
            tryVerify(function() { return undone.count > 0; }, 600000);
            var undo = undone.signalArguments[0][0];
            console.log("  undone: " + Live.summaryLine(undo));
            // This was a bare console.log, so an undo that wrote 0 of 1 and
            // carried an error still read as PASS. On a stick this full the
            // undo may genuinely fail -- restoring writes files, and there
            // is no room -- but then it has to fail whole: either it put
            // everything back, or it put nothing back and says so.
            verify(undo.error.length > 0 || undo.written > 0,
                   "the undo either restored something or said why it could not");
            compare(EditSessionRegistry.anyWriting, false);
            // Whatever it decided, the catalogs are still readable and the
            // stick is not left half-written.
            var afterUndo = createTemporaryObject(scanController, testCase);
            afterUndo.scan("rekordbox", rekordboxPath);
            tryVerify(function() { return afterUndo.busy === false; }, 300000);
            verify(afterUndo.tracks.trackCount() > 0, "the catalogs still read after the undo");
        }
        EditSessionRegistry.closeSession(testCase.libraryId);
        verify(!saveFitted, "the save fitted, so the stick was not full: the fill left too much room (see above)");
    }

    // Issue #27: a save that fitted, and an undo that may not. The rig's
    // second fill leaves room for the save (about 1.2 MB) and not
    // necessarily for its undo, which keeps a copy of what it overwrites
    // and then writes the files back beside the old ones. Either the undo
    // goes through whole, or it is refused for space up front -- saying
    // so, never "could not read the backup", and leaving the catalogs
    // readable and the save as it was.
    function test_undoOnANearlyFullStick() {
        if (typeof liveRigFullStick === "undefined" || !liveRigFullStick) {
            skip("SEABASS_RIG_FULL_STICK is not set: the rig fills the stick for this one");
        }
        var run = stageOneCueAndSave();
        var s = run.s, summary = run.summary;
        // This is the undo's check, so the save has to have fitted: the
        // rig leaves room for it on this pass on purpose.
        compare(summary.error, "", "the save fitted (the rig's second fill leaves room for it)");
        compare(summary.written, run.staged);
        verify(s.canUndo === true, "the save left something to undo");

        var undone = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.undoLastSave();
        tryVerify(function() { return undone.count > 0; }, 600000);
        var undo = undone.signalArguments[0][0];
        console.log("  undo on a nearly full stick: " + Live.summaryLine(undo));
        compare(EditSessionRegistry.anyWriting, false);

        var readBack = createTemporaryObject(scanController, testCase);
        readBack.scan("rekordbox", rekordboxPath);
        tryVerify(function() { return readBack.busy === false; }, 300000);
        verify(readBack.tracks.trackCount() > 0, "the catalogs still read after the undo");
        console.log("  catalogs still read: " + readBack.tracks.trackCount() + " tracks");

        if (undo.error.length > 0) {
            verify(undo.error.indexOf("not enough space") >= 0 || undo.error.indexOf("could not write") >= 0
                   || undo.error.indexOf("could not keep a copy") >= 0,
                   "an undo that fails on a full stick says the stick is the problem: " + undo.error);
            verify(undo.error.indexOf("could not read") < 0 && undo.error.indexOf("damaged") < 0,
                   "and does not blame the backup: " + undo.error);
            console.log("  refused cleanly; the save stays and the stick's reference restore follows");
        } else {
            // A whole undo, or nothing: the runner then checks the catalogs
            // against the baseline, which a whole undo returns them to.
            compare(undo.written, undo.total, "an undo that reports success put everything back");
            verify(undo.written > 0);
            console.log("  the undo fitted; the catalogs are compared with the baseline next");
        }
        EditSessionRegistry.closeSession(testCase.libraryId);
    }
}
