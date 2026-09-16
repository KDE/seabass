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
    Component { id: appSettings; AppSettingsController {} }
    Component { id: junkPage; JunkCuePage { width: 1100; height: 820 } }

    function test_saveOnAFullStickFailsCleanly() {
        if (typeof liveRigFullStick === "undefined" || !liveRigFullStick) {
            skip("SEABASS_RIG_FULL_STICK is not set: the rig fills the stick for this one");
        }
        verify(stickRoot.length > 0, "SEABASS_LIVE_STICK names the stick this runs against");
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        var page = createTemporaryObject(junkPage, testCase,
                                         {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                          enginePath: enginePath,
                                          appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = Live.findByType(page, "LibraryConsistencyController");
        verify(ctrl !== null);
        tryVerify(function() { return ctrl.busy === false; }, 300000);
        if (ctrl.junkCues.rowCount() === 0) {
            skip("no stray cues on this stick to stage");
        }

        var s = EditSessionRegistry.openSession(testCase.libraryId, stickLabel, rekordboxPath, enginePath);
        verify(s !== null);
        console.log("  free on the stick before the save: " + s.stickBytesFree + " bytes of " + s.stickBytesCapacity);
        // The whole point of this check is a stick with no room. A session
        // that measured nothing (0 of 0), or a stick with gigabytes free,
        // means the fill did not take -- and a save that then fits proves
        // nothing at all. Fail here rather than pass on the else-branch.
        verify(s.stickBytesCapacity > 0, "the session measured the stick");
        verify(s.stickBytesFree > 0 && s.stickBytesFree < 512 * 1024 * 1024,
               "the stick really is nearly full before the save (free: " + s.stickBytesFree + " bytes)");

        ctrl.removeAllJunkCues();
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
        var readBack = Live.findByType(createTemporaryObject(junkPage, testCase,
                                                            {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                             enginePath: enginePath,
                                                             appSettingsController: createTemporaryObject(appSettings, testCase)}), "LibraryConsistencyController");
        tryVerify(function() { return readBack.busy === false; }, 300000);
        compare(readBack.errorMessage, "");
        console.log("  catalogs still read after the attempt");

        // Leave nothing staged behind for the checks that follow.
        if (s.dirty === true) {
            s.discard();
            tryVerify(function() { return s.dirty === false; }, 10000);
        }
        if (s.canUndo === true && summary.error.length === 0) {
            var undone = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
            s.undoLastSave();
            tryVerify(function() { return undone.count > 0; }, 600000);
            console.log("  undone: " + Live.summaryLine(undone.signalArguments[0][0]));
        }
        EditSessionRegistry.closeSession(testCase.libraryId);
    }
}
