// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// A save killed mid-write, then read and undone, on a REAL stick: rig
// check F6, issue #48. Driven by tools/rig-interrupted-save.sh, one test
// function per process, never on its own:
//
// test_saveKilledMidWrite stages every ready track on the Sync page,
// starts the save, and once the first track has been written writes the
// marker named by SEABASS_LIVE_KILL_MARKER and carries on saving. The
// script is polling for that file and SIGKILLs this process when it
// appears, which is what a stick pulled mid-save does to the files on it:
// the writer stops between one write and the next. This test never
// finishes on its own; if it is not killed it fails.
//
// test_recoversAndUndoes runs in a fresh process afterwards, the session a
// person opens once the stick is back: every catalog must read, the page
// must say the last save did not finish, and its Undo Last Save must put
// the stick back and take the note with it.
//
// Writes to the stick through the normal backup path, and the script
// restores the stick from its reference afterwards whatever happened.
TestCase {
    id: testCase
    name: "LiveInterruptedSave"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: liveStickRoot
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string enginePath: stickRoot + "/Engine Library"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)
    readonly property string saveNote: stickRoot + "/Seabass/backups/.save-in-progress"
    property string libraryId: ""

    Component { id: spyComponent; SignalSpy {} }
    Component { id: syncPage; SyncPage { width: 1100; height: 820 } }
    Component { id: appSettings; AppSettingsController {} }

    readonly property var fakePlayback: ({stop: function() {}, hasTrack: false, playing: false})

    function init() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        if (liveKillMarker.length === 0) {
            skip("SEABASS_LIVE_KILL_MARKER is not set: run through tools/rig-interrupted-save.sh");
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

    function openSyncPage() {
        const page = createTemporaryObject(syncPage, testCase, {stickLabel: stickLabel, rekordboxPath: rekordboxPath,
                                                                enginePath: enginePath, playbackController: fakePlayback,
                                                                appSettingsController: createTemporaryObject(appSettings, testCase)});
        const ctrl = Live.findByType(page, "SyncController");
        verify(ctrl !== null, "the page created its controller");
        tryVerify(function() { return Live.settled(ctrl); }, 300000);
        return {page: page, ctrl: ctrl};
    }

    // Every catalog read: no error, and both libraries' tracks counted.
    function requireEveryCatalogRead(ctrl, expected, when) {
        console.log("  " + when + ": rekordbox " + ctrl.rekordboxTrackCount + ", engine " + ctrl.engineTrackCount
                    + ", plans " + ctrl.planCount + (ctrl.errorMessage ? ", error: " + ctrl.errorMessage : ""));
        compare(ctrl.errorMessage, "", when + ": the scan read every catalog");
        verify(ctrl.rekordboxTrackCount > 0, when + ": rekordbox tracks were read");
        verify(ctrl.engineTrackCount > 0, when + ": Engine tracks were read");
        if (expected) {
            compare(ctrl.rekordboxTrackCount, expected.rekordbox, when + ": as many rekordbox tracks as before the save");
            compare(ctrl.engineTrackCount, expected.engine, when + ": as many Engine tracks as before the save");
        }
    }

    // ---- 1. The save, killed by the script mid-write ----
    function test_saveKilledMidWrite() {
        const opened = openSyncPage();
        const ctrl = opened.ctrl;
        requireEveryCatalogRead(ctrl, null, "before the save");
        const s = session();
        // A note already there would make the script's "the save was cut
        // short" meaningless: it looks for the note this save leaves.
        compare(s.interruptedSave, false, "no earlier interrupted save on the stick");
        verify(!stickFixture.fileExists(saveNote), "no save note on the stick before the save");
        verify(ctrl.planCount > 0, "there is something to sync on this stick, so the save has work to be cut short in");

        ctrl.stageSelected(false);  // every ready track starts ticked; decisions are never staged
        tryVerify(function() { return s.pendingCount > 0 && s.pendingCount === ctrl.stagedCount; }, 10000);
        const staged = s.pendingCount;
        console.log("  staged " + staged + " of " + ctrl.planCount);

        // On the first track written, not before: by then the save has
        // made its backups and noted them, and has started changing the
        // catalogs -- the state a pull leaves behind.
        let marked = false;
        const markWhenWriting = function() {
            if (marked || s.writeCurrent < 1) return;
            marked = stickFixture.writeText(liveKillMarker, "rekordbox=" + ctrl.rekordboxTrackCount + "\nengine="
                                            + ctrl.engineTrackCount + "\nstaged=" + staged + "\n");
            console.log("  marker written at " + s.writeCurrent + " of " + s.writeTotal + ": " + marked);
        };
        s.writeProgressChanged.connect(markWhenWriting);
        const spy = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        s.save();
        tryVerify(function() { return marked || spy.count > 0; }, 300000, "the save reached its first track");
        verify(marked, "the marker was written while the save was running");

        // From here the script kills this process. A save that finishes
        // first is said, and then this waits to be killed all the same:
        // the script decides from the stick, not from this process.
        tryVerify(function() { return spy.count > 0; }, 300000);
        console.log("  the save finished before the kill: " + Live.summaryLine(spy.signalArguments[0][0]));
        wait(300000);
        fail("this process was never killed");
    }

    // ---- 2. A fresh session reads the stick and undoes the save ----
    function test_recoversAndUndoes() {
        const facts = stickFixture.readText(liveKillMarker);
        verify(facts.length > 0, "the first test's counts are in " + liveKillMarker);
        const expected = {};
        facts.split("\n").forEach(function(line) {
            const eq = line.indexOf("=");
            if (eq > 0) expected[line.substring(0, eq)] = parseInt(line.substring(eq + 1), 10);
        });
        verify(expected.rekordbox > 0 && expected.engine > 0, "the counts before the save were recorded: " + facts);

        verify(stickFixture.fileExists(saveNote), "the interrupted save's note is on the stick");
        const opened = openSyncPage();
        const page = opened.page;
        requireEveryCatalogRead(opened.ctrl, expected, "after the interrupted save");

        const s = session();
        tryCompare(s, "interruptedSave", true, 10000, "the session knows the last save did not finish");
        compare(s.canUndo, true, "and offers the undo");
        const banner = findChild(page, "interruptedSaveBanner");
        verify(banner !== null, "the page has the interrupted-save notice");
        tryCompare(banner, "visible", true, 5000, "the notice is shown");
        shot(page, "live-interrupted-save-banner");

        const undo = findChild(banner, "undoInterruptedSaveButton");
        verify(undo !== null && undo.visible && undo.enabled, "the notice's Undo Last Save can be pressed");
        const spy = createTemporaryObject(spyComponent, testCase, {target: s, signalName: "saveFinished"});
        mouseClick(undo);
        tryVerify(function() { return spy.count > 0; }, 300000, "the undo finished");
        const summary = spy.signalArguments[0][0];
        console.log("  undo: " + Live.summaryLine(summary));
        compare(summary.error, "", "the undo had no error");
        compare(summary.warning === undefined ? "" : summary.warning, "", "the undo left nothing unfinished");
        verify(summary.written > 0, "the undo wrote something back");

        compare(s.interruptedSave, false, "the session no longer calls the last save unfinished");
        tryCompare(banner, "visible", false, 5000, "the notice is gone");
        verify(!stickFixture.fileExists(saveNote), "the note is gone from the stick");
        // The page as a person sees it once the undo is over: its summary
        // read and closed, the progress gone, the page's own rescan done.
        const dialog = findChild(page, "summaryDialog");
        tryCompare(dialog, "opened", true, 5000, "the undo says what it did");
        findChild(dialog, "okButton").clicked();
        tryVerify(function() { return !dialog.visible; }, 5000);
        tryVerify(function() { return !findChild(page, "writeProgressDialog").visible; }, 10000);
        tryVerify(function() { return Live.settled(opened.ctrl); }, 300000);
        shot(page, "live-interrupted-save-undone");

        const again = openSyncPage();
        requireEveryCatalogRead(again.ctrl, expected, "rescan after the undo");
    }
}
