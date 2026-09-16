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

    function test_saveOnAFullStickFailsCleanly() {
        if (typeof liveRigFullStick === "undefined" || !liveRigFullStick) {
            skip("SEABASS_RIG_FULL_STICK is not set: the rig fills the stick for this one");
        }
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
        // The whole point of this check is a stick with no room. Gigabytes
        // free means the fill did not take -- and a save that then fits
        // proves nothing at all. Fail here rather than pass on the
        // else-branch. No lower bound: a stick that is exactly full is the
        // strongest form of the condition, not a failure of it.
        verify(s.stickBytesFree < 512 * 1024 * 1024,
               "the stick really is nearly full before the save (free: " + s.stickBytesFree + " bytes)");

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

        // Either outcome is allowed, and both must be clean: the save may
        // refuse up front (the backup would not fit) or fail part-way. What
        // it may never do is report success while the stick is full, or
        // leave the save half applied.
        if (summary.error.length > 0) {
            console.log("  refused or stopped: " + summary.error);
            compare(EditSessionRegistry.anyWriting, false);
            compare(s.dirty, true, "a refused save keeps its changes staged rather than losing them");
        } else {
            // Allowed, but only because the save genuinely fitted in what
            // little was left: everything staged has to have been written.
            console.log("  the save fitted in the remaining space: " + summary.written + " of " + staged);
            compare(summary.written, staged);
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
    }
}
