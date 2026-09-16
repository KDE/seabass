// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// Rig check F5: leaving with unsaved changes, against a REAL stick.
//
// Both ways out are taken: discard, which must leave the catalogs exactly
// as they were, and save, which must write everything it staged and only
// then leave. The saved change is undone at the end, so the stick is as
// it was found either way.
TestCase {
    id: testCase
    name: "LiveQuit"
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

    function session() {
        return EditSessionRegistry.sessionFor(testCase.libraryId, stickLabel);
    }

    function stageStrayCues(page) {
        var ctrl = Live.findByType(page, "LibraryConsistencyController");
        verify(ctrl !== null, "the page made its controller");
        tryVerify(function() { return ctrl.busy === false; }, 300000);
        if (ctrl.junkCues.rowCount() === 0) {
            return null;
        }
        // Staged the way the page stages them: every stray cue at once.
        ctrl.removeAllJunkCues();
        return ctrl;
    }

    function test_01_discardLeavesTheStickAlone() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        var page = createTemporaryObject(junkPage, testCase,
                                         {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                          enginePath: enginePath,
                                          appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = stageStrayCues(page);
        if (ctrl === null) {
            skip("no stray cues on this stick to stage");
        }
        var s = session();
        tryVerify(function() { return s !== null && s.pendingCount > 0; }, 10000);
        var staged = s.pendingCount;
        console.log("  staged " + staged + " change(s), then asked to leave");

        // Leaving with changes must ask first.
        var host = Live.findByType(page, "EditSessionHost");
        verify(host !== null, "the page has an edit session host");
        var left = 0;
        host.requestLeave(function() { left++; });
        var dialog = Live.findByObjectName(page, "unsavedDialog");
        verify(dialog !== null, "the unsaved-changes dialog exists");
        tryVerify(function() { return dialog.opened === true; }, 5000);
        compare(left, 0, "nothing left while the question is up");

        // Discard: the staged work goes, the stick keeps what it had.
        var discard = Live.findByObjectName(dialog, "discardButton");
        verify(discard !== null, "the dialog offers Discard");
        discard.clicked();
        tryVerify(function() { return s.dirty === false; }, 10000);
        tryVerify(function() { return left === 1; }, 10000);
        compare(s.pendingCount, 0);
        compare(EditSessionRegistry.anyWriting, false);
        console.log("  discarded: nothing pending, nothing written");
    }

    function test_02_saveThenLeaveWritesEverything() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        testCase.libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        var page = createTemporaryObject(junkPage, testCase,
                                         {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                          enginePath: enginePath,
                                          appSettingsController: createTemporaryObject(appSettings, testCase)});
        var ctrl = stageStrayCues(page);
        if (ctrl === null) {
            skip("no stray cues on this stick to stage");
        }
        var s = session();
        tryVerify(function() { return s !== null && s.pendingCount > 0; }, 10000);
        var staged = s.pendingCount;

        var host = Live.findByType(page, "EditSessionHost");
        var left = 0;
        host.requestLeave(function() { left++; });
        var dialog = Live.findByObjectName(page, "unsavedDialog");
        tryVerify(function() { return dialog.opened === true; }, 5000);

        var finished = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        var save = Live.findByObjectName(dialog, "saveButton");
        verify(save !== null, "the dialog offers Save");
        save.clicked();
        tryVerify(function() { return finished.count > 0; }, 600000);
        var summary = finished.signalArguments[0][0];
        console.log("  save on the way out: " + Live.summaryLine(summary));
        compare(summary.error, "");
        compare(summary.written, staged);
        compare(s.dirty, false);
        compare(EditSessionRegistry.anyWriting, false);

        // Put it back: the rig's other checks expect the stick untouched.
        var undone = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.undoLastSave();
        tryVerify(function() { return undone.count > 0; }, 600000);
        console.log("  undone: " + Live.summaryLine(undone.signalArguments[0][0]));
        compare(undone.signalArguments[0][0].error, "");
        EditSessionRegistry.closeSession(testCase.libraryId);
    }
}
