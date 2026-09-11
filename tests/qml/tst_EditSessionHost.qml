// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// EditSessionHost with a fake registry and session: the leave guard
// (leave at once when clean, ask when dirty, save-then-leave, discard-
// then-leave), the summary after a save, and the lock refusal.
TestCase {
    id: testCase
    name: "EditSessionHost"
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
            signal saveFinished(var summary)
            signal lockRefused(var holder)
            function save() { calls.push("save"); }
            function discard() { calls.push("discard"); dirty = false; pendingCount = 0; state = "idle"; }
            function cancelWrite() { calls.push("cancelWrite"); }
        }
    }

    Component {
        id: registryComponent
        QtObject {
            property var session: null
            property var calls: []
            property bool quitAfterSave: false
            function openSession(id, label, rb, engine) { calls.push("open:" + id); return session; }
            function closeSession(id) { calls.push("close:" + id); }
            function removeLock(id) { calls.push("removeLock:" + id); }
        }
    }

    Component {
        id: hostComponent
        EditSessionHost { width: 800; height: 600 }
    }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = [];
        if (item.contentItem) kids.push(item.contentItem);
        if (item.footer) kids.push(item.footer);
        var children = item.children ? item.children : [];
        for (var i = 0; i < children.length; ++i) kids.push(children[i]);
        // Popups declared inside an Item are non-visual children: resources.
        var resources = item.resources ? item.resources : [];
        for (var r = 0; r < resources.length; ++r) kids.push(resources[r]);
        for (var j = 0; j < kids.length; ++j) {
            var found = findByObjectName(kids[j], name);
            if (found) return found;
        }
        return null;
    }

    function makeHost() {
        var session = createTemporaryObject(sessionComponent, testCase);
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var host = createTemporaryObject(hostComponent, testCase,
                                         {registry: registry, libraryId: "EB9F-F032", stickLabel: "A3"});
        waitForRendering(host);
        return {session: session, registry: registry, host: host};
    }

    function test_opensTheSessionAndLeavesAtOnceWhenClean() {
        var t = makeHost();
        compare(t.registry.calls[0], "open:EB9F-F032");
        verify(t.host.session === t.session);
        var left = 0;
        t.host.requestLeave(function() { left++; });
        compare(left, 1);
        compare(findByObjectName(t.host, "unsavedDialog").opened, false);
    }

    function test_dirtyAsksThenDiscardsAndLeaves() {
        var t = makeHost();
        t.session.dirty = true;
        t.session.state = "editing";
        t.session.pendingCount = 2;
        var left = 0;
        t.host.requestLeave(function() { left++; });
        compare(left, 0);
        var dialog = findByObjectName(t.host, "unsavedDialog");
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog, "discardButton").clicked();
        tryCompare(dialog, "opened", false);
        compare(t.session.calls[t.session.calls.length - 1], "discard");
        compare(left, 1);
    }

    function test_dirtySavesThenLeavesAfterTheSummary() {
        var t = makeHost();
        t.session.dirty = true;
        t.session.state = "editing";
        t.session.pendingCount = 2;
        var left = 0;
        t.host.requestLeave(function() { left++; });
        var dialog = findByObjectName(t.host, "unsavedDialog");
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog, "saveButton").clicked();
        tryCompare(dialog, "opened", false);
        compare(t.session.calls[t.session.calls.length - 1], "save");
        compare(left, 0);

        // The save completed: the session is clean and reports it.
        t.session.dirty = false;
        t.session.pendingCount = 0;
        t.session.state = "idle";
        t.session.saveFinished({written: 2, total: 2, unit: "tracks", cancelled: false, error: ""});
        var summary = findByObjectName(t.host, "summaryDialog");
        tryCompare(summary, "opened", true);
        // Everything staged went through, so no "of": see
        // OperationSummaryDialog.countSentence.
        compare(findByObjectName(summary, "countLabel").text, "2 tracks written.");
        findByObjectName(summary, "okButton").clicked();
        tryCompare(summary, "opened", false);
        compare(left, 1);
    }

    function test_cancelledSaveStaysOnThePage() {
        var t = makeHost();
        t.session.dirty = true;
        t.session.state = "editing";
        t.session.pendingCount = 5;
        var left = 0;
        t.host.requestLeave(function() { left++; });
        var dialog = findByObjectName(t.host, "unsavedDialog");
        tryCompare(dialog, "opened", true);
        findByObjectName(dialog, "saveButton").clicked();
        // Cancelled after 2 of 5: still dirty.
        t.session.pendingCount = 3;
        t.session.saveFinished({written: 2, total: 5, unit: "tracks", cancelled: true, error: ""});
        var summary = findByObjectName(t.host, "summaryDialog");
        tryCompare(summary, "opened", true);
        findByObjectName(summary, "okButton").clicked();
        tryCompare(summary, "opened", false);
        compare(left, 0);
        // A later leave asks again.
        t.host.requestLeave(function() { left++; });
        tryCompare(dialog, "opened", true);
        dialog.close();
    }

    function test_plainSaveShowsTheSummaryAndStays() {
        var t = makeHost();
        t.session.dirty = true;
        t.session.state = "editing";
        t.session.pendingCount = 1;
        findByObjectName(t.host, "saveButton").clicked();
        compare(t.session.calls[t.session.calls.length - 1], "save");
        t.session.dirty = false;
        t.session.saveFinished({written: 1, total: 1, unit: "settings", cancelled: false, error: ""});
        var summary = findByObjectName(t.host, "summaryDialog");
        tryCompare(summary, "opened", true);
        findByObjectName(summary, "okButton").clicked();
        tryCompare(summary, "opened", false);
    }

    function test_lockRefusalOpensTheLockedDialog() {
        var t = makeHost();
        t.session.lockRefused({hostname: "studio", pid: 7, startedAtUtc: ""});
        var locked = findByObjectName(t.host, "lockedDialog");
        tryCompare(locked, "opened", true);
        findByObjectName(locked, "removeLockButton").clicked();
        tryCompare(locked, "opened", false);
        compare(t.registry.calls[t.registry.calls.length - 1], "removeLock:EB9F-F032");
    }

    function test_writingIgnoresLeaveRequests() {
        var t = makeHost();
        t.session.writing = true;
        t.session.state = "writing";
        var left = 0;
        t.host.requestLeave(function() { left++; });
        compare(left, 0);
        compare(findByObjectName(t.host, "unsavedDialog").opened, false);
    }

    function test_closesTheSessionOnDestruction() {
        var session = createTemporaryObject(sessionComponent, testCase);
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var host = hostComponent.createObject(testCase, {registry: registry, libraryId: "EB9F-F032"});
        host.destroy();
        wait(50);
        compare(registry.calls[registry.calls.length - 1], "close:EB9F-F032");
    }
}
