// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The edit-mode dialogs and the Save overlay, each driven by a fake
// session object: what they say, which button is the safe default, and
// what they ask the session to do.
TestCase {
    id: testCase
    name: "EditModeDialogs"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: sessionComponent
        QtObject {
            property string libraryId: "EB9F-F032"
            property string stickLabel: "A3"
            property string state: "idle"
            property bool dirty: false
            property bool writing: false
            property int pendingCount: 0
            property var pendingDescriptions: []
            property string writeLabel: ""
            property int writeCurrent: 0
            property int writeTotal: 0
            property bool cancelRequested: false
            property bool stickPresent: true
            property string stickIdentityStrength: "hardware"
            property var calls: []
            function save() { calls.push("save"); }
            function discard() { calls.push("discard"); }
            function cancelWrite() { calls.push("cancelWrite"); cancelRequested = true; }
        }
    }

    Component { id: saveOverlayComponent; SaveOverlayButton {} }
    Component { id: unsavedComponent; UnsavedChangesDialog {} }
    Component { id: writeProgressComponent; WriteProgressDialog {} }
    Component { id: summaryComponent; OperationSummaryDialog {} }
    Component { id: lockedComponent; LockedLibraryDialog {} }
    Component { id: stickRemovedComponent; StickRemovedDialog {} }
    Component { id: spyComponent; SignalSpy {} }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = [];
        if (item.contentItem) kids.push(item.contentItem);
        if (item.footer) kids.push(item.footer);
        var children = item.children ? item.children : [];
        for (var i = 0; i < children.length; ++i) kids.push(children[i]);
        for (var j = 0; j < kids.length; ++j) {
            var found = findByObjectName(kids[j], name);
            if (found) return found;
        }
        return null;
    }

    function shot(item, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(testCase);
            grabImage(item).save(screenshotDir + "/" + name + ".png");
        }
    }

    function test_saveOverlayEnablesWithChangesAndSaves() {
        var session = createTemporaryObject(sessionComponent, testCase);
        var overlay = createTemporaryObject(saveOverlayComponent, testCase, {session: session});
        waitForRendering(overlay);
        var button = findByObjectName(overlay, "saveButton");
        verify(button !== null);
        compare(button.enabled, false);
        compare(findByObjectName(overlay, "pendingBadge").visible, false);

        session.dirty = true;
        session.state = "editing";
        session.pendingCount = 3;
        session.pendingDescriptions = ["one", "two", "three"];
        tryCompare(button, "enabled", true);
        compare(findByObjectName(overlay, "pendingBadge").visible, true);
        button.clicked();
        compare(session.calls[session.calls.length - 1], "save");
        shot(overlay, "save-overlay-enabled");

        session.state = "writing";
        session.writing = true;
        tryCompare(button, "enabled", false);
    }

    function test_unsavedChangesDialogRoutesBothButtons() {
        var dialog = createTemporaryObject(unsavedComponent, testCase, {pendingCount: 2});
        var saveSpy = createTemporaryObject(spyComponent, testCase, {target: dialog, signalName: "saveRequested"});
        var discardSpy = createTemporaryObject(spyComponent, testCase, {target: dialog, signalName: "discardRequested"});
        dialog.open();
        tryCompare(dialog, "opened", true);
        compare(dialog.closePolicy, Popup.NoAutoClose);
        verify(findByObjectName(dialog, "messageLabel").text.indexOf("Please save these changes") > 0);
        shot(testCase, "unsaved-changes-dialog");
        findByObjectName(dialog, "saveButton").clicked();
        tryCompare(saveSpy, "count", 1);
        tryCompare(dialog, "opened", false);
        dialog.open();
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog, "discardButton").clicked();
        tryCompare(discardSpy, "count", 1);
        tryCompare(dialog, "opened", false);
    }

    function test_writeProgressDialogFollowsTheSession() {
        var session = createTemporaryObject(sessionComponent, testCase);
        var dialog = createTemporaryObject(writeProgressComponent, testCase, {session: session});
        compare(dialog.opened, false);
        session.writing = true;
        session.state = "writing";
        session.writeLabel = "Writing cues for Track 7";
        session.writeCurrent = 5;
        session.writeTotal = 31;
        tryCompare(dialog, "opened", true);
        compare(findByObjectName(dialog, "writeLabel").text, "Writing cues for Track 7");
        compare(findByObjectName(dialog, "countLabel").text, "5 / 31");
        verify(findByObjectName(dialog, "stickWarning").visible);
        shot(testCase, "write-progress-dialog");
        var cancel = findByObjectName(dialog, "cancelButton");
        compare(cancel.enabled, true);
        cancel.clicked();
        compare(session.calls[session.calls.length - 1], "cancelWrite");
        tryCompare(cancel, "enabled", false);
        session.writing = false;
        tryCompare(dialog, "opened", false);
    }

    function test_summaryDialogCountsAndReasons() {
        var dialog = createTemporaryObject(summaryComponent, testCase);
        dialog.show({written: 5, total: 31, unit: "tracks", cancelled: true, error: ""});
        tryCompare(dialog, "opened", true);
        compare(findByObjectName(dialog, "countLabel").text, "5 of 31 tracks written.");
        verify(findByObjectName(dialog, "detailLabel").text.indexOf("Stopped at your request") === 0);
        compare(dialog.title, "Cancelled");
        shot(testCase, "operation-summary-dialog");
        findByObjectName(dialog, "okButton").clicked();
        tryCompare(dialog, "opened", false);

        dialog.show({written: 2, total: 9, unit: "backups", verb: "deleted", cancelled: false, error: "disk gone"});
        compare(findByObjectName(dialog, "countLabel").text, "2 of 9 backups deleted.");
        compare(findByObjectName(dialog, "detailLabel").text, "Then: disk gone");
        compare(dialog.title, "Stopped with an error");
        dialog.close();
    }

    function test_lockedLibraryDialogDefaultsToStayingSafe() {
        var dialog = createTemporaryObject(lockedComponent, testCase);
        var spy = createTemporaryObject(spyComponent, testCase, {target: dialog, signalName: "removeLockRequested"});
        dialog.openFor("EB9F-F032", {hostname: "studio", pid: 4242, startedAtUtc: "2026-09-06T10:00:00Z"});
        tryCompare(dialog, "opened", true);
        var message = findByObjectName(dialog, "messageLabel").text;
        verify(message.indexOf("This library is currently locked by another instance of Seabass.") === 0);
        verify(message.indexOf("(Don't come complaining.)") > 0);
        verify(findByObjectName(dialog, "holderLabel").text.indexOf("studio (process 4242)") > 0);
        // The safe choice is the accept (default) role; removing the lock is
        // the destructive one.
        compare(findByObjectName(dialog, "staySafeButton").DialogButtonBox.buttonRole, DialogButtonBox.AcceptRole);
        compare(findByObjectName(dialog, "removeLockButton").DialogButtonBox.buttonRole, DialogButtonBox.DestructiveRole);
        shot(testCase, "locked-library-dialog");
        findByObjectName(dialog, "staySafeButton").clicked();
        tryCompare(dialog, "opened", false);
        compare(spy.count, 0);
        dialog.openFor("EB9F-F032", {});
        tryCompare(dialog, "opened", true);
        compare(findByObjectName(dialog, "holderLabel").visible, false);
        findByObjectName(dialog, "removeLockButton").clicked();
        tryCompare(spy, "count", 1);
        tryCompare(dialog, "opened", false);
    }

    function test_stickRemovedDialogWaitsForTheSameStick() {
        var session = createTemporaryObject(sessionComponent, testCase, {stickPresent: false, stickIdentityStrength: ""});
        var dialog = createTemporaryObject(stickRemovedComponent, testCase, {session: session});
        var discardSpy = createTemporaryObject(spyComponent, testCase, {target: dialog, signalName: "discardRequested"});
        var understoodSpy = createTemporaryObject(spyComponent, testCase, {target: dialog, signalName: "understood"});
        tryCompare(dialog, "opened", true);
        verify(findByObjectName(dialog, "messageLabel").text.indexOf("USB Stick A3 has been removed while editing.") === 0);
        var understood = findByObjectName(dialog, "understoodButton");
        compare(understood.enabled, false);
        shot(testCase, "stick-removed-dialog");

        session.stickPresent = true;
        session.stickIdentityStrength = "hardware";
        tryCompare(understood, "enabled", true);
        verify(findByObjectName(dialog, "presenceLabel").text.indexOf("serial number") > 0);
        understood.clicked();
        tryCompare(understoodSpy, "count", 1);
        tryCompare(dialog, "opened", false);
        compare(discardSpy.count, 0);

        // Gone again, and discarded this time. `opened` drops as soon as
        // the close starts; wait for the exit transition to finish before
        // reopening, or the reopen is lost to it.
        dialog.session = null;
        tryCompare(dialog, "opened", false);
        tryCompare(dialog, "visible", false);
        session.stickPresent = false;
        dialog.session = session;
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog, "discardButton").clicked();
        tryCompare(discardSpy, "count", 1);
    }

    // How the summary says what happened. The shapes matter more than
    // the words: a complete run and a partial one have to look
    // different at a glance, and counting one of something must not
    // read like counting several.
    function test_countSentenceShapes_data() {
        return [
            // Complete: no "of", because comparing two identical numbers
            // to learn that nothing was left is work the reader should
            // not have to do.
            {tag: "complete", written: 27, total: 27, unit: "cues", verb: "removed",
             expected: "27 cues removed."},
            // Incomplete: the shortfall is the whole point, so it is named.
            {tag: "partial", written: 5, total: 31, unit: "tracks", verb: "synchronised",
             expected: "5 of 31 tracks synchronised."},
            // One of something is singular, including the -ies case.
            {tag: "one", written: 1, total: 1, unit: "cues", verb: "added",
             expected: "1 cue added."},
            {tag: "one entry", written: 1, total: 1, unit: "entries", verb: "removed",
             expected: "1 entry removed."},
            {tag: "one setting", written: 1, total: 1, unit: "settings", verb: "saved",
             expected: "1 setting saved."},
            // One of several stays plural on the total, singular on the count.
            {tag: "one of many", written: 1, total: 9, unit: "groups", verb: "cleaned up",
             expected: "1 of 9 groups cleaned up."},
            // Nothing went through: still plural, still says so.
            {tag: "none", written: 0, total: 4, unit: "cues", verb: "removed",
             expected: "0 of 4 cues removed."},
        ];
    }

    function test_countSentenceShapes(row) {
        var dialog = createTemporaryObject(summaryComponent, testCase);
        verify(dialog !== null);
        dialog.show({written: row.written, total: row.total, unit: row.unit, verb: row.verb,
                     cancelled: false, error: ""});
        compare(findByObjectName(dialog, "countLabel").text, row.expected);
        dialog.close();
    }
}
