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
    Component { id: scanController; ScanController {} }
    Component { id: addCueComponent; AddCueController {} }
    Component { id: appSettings; AppSettingsController {} }
    Component { id: junkPage; JunkCuePage { width: 1100; height: 820 } }

    function session() {
        return EditSessionRegistry.sessionFor(testCase.libraryId, stickLabel);
    }

    // Stages one added cue and returns the track it went on. Adding a cue
    // works on any library; staging stray cues only works on a stick that
    // happens to have some, and this check may not skip.
    function stageAnAddedCue(page) {
        // The page kicks off its own scan from Component.onCompleted, on a
        // worker thread reading the very catalogs this is about to stage a
        // change in -- and test_02 then saves over export.pdb underneath
        // it. The stray-cue staging this replaced waited for that scan;
        // the replacement dropped the wait, which would show up as an
        // intermittent F5 on a big library rather than as a clean failure.
        if (page) {
            var pageScan = Live.findByType(page, "LibraryConsistencyController");
            verify(pageScan !== null, "the page has its consistency controller");
            tryVerify(function() { return pageScan.busy === false; }, 300000);
        }
        var rekordbox = createTemporaryObject(scanController, testCase);
        rekordbox.scan("rekordbox", rekordboxPath);
        tryVerify(function() { return rekordbox.busy === false; }, 300000);
        var target = null;
        for (var j = 0; j < rekordbox.tracks.trackCount() && target === null; ++j) {
            var t = rekordbox.tracks.trackAt(j);
            // Any track: this adds a memory cue at 30000 ms and asserts
            // nothing about what was there before, so demanding a cue-free
            // track only made the check fail on libraries where every
            // track has cues -- a property of the stick, not the app.
            if (t.filePath.length > 0) {
                target = t;
            }
        }
        verify(target !== null, "a rekordbox track to add a cue to");
        var adder = createTemporaryObject(addCueComponent, testCase);
        adder.addCue("rekordbox", rekordboxPath, target.sourceId, 30000, "memory", 0, "", "rig F5", false, 0,
                     target.title);
        compare(adder.errorMessage, "");
        return target;
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
        stageAnAddedCue(page);
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
        stageAnAddedCue(page);
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
