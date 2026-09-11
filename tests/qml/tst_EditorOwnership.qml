// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// One library is edited by one page at a time. The C++ session refuses a
// change from a second page outright; this covers the UI half: the page
// knows it is read-only before the user tries, and the refusal explains
// itself when they do.
TestCase {
    id: testCase
    name: "EditorOwnership"
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
            // "" while nothing is staged; the owning page once something is.
            property string editorOwner: ""
            signal saveFinished(var summary)
            signal lockRefused(var holder)
            signal editorConflict(string owner, string attempted)
            function save() {}
            function discard() {}
            function cancelWrite() {}
        }
    }

    Component {
        id: registryComponent
        QtObject {
            property var session: null
            property bool quitAfterSave: false
            function openSession(id, label, rb, engine) { return session; }
            function closeSession(id) {}
            function removeLock(id) {}
        }
    }

    Component {
        id: hostComponent
        EditSessionHost { width: 900; height: 700 }
    }

    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = [];
        if (item.contentItem) kids.push(item.contentItem);
        if (item.footer) kids.push(item.footer);
        var children = item.children ? item.children : [];
        for (var i = 0; i < children.length; ++i) kids.push(children[i]);
        var resources = item.resources ? item.resources : [];
        for (var r = 0; r < resources.length; ++r) kids.push(resources[r]);
        for (var j = 0; j < kids.length; ++j) {
            var found = findByObjectName(kids[j], name);
            if (found) return found;
        }
        return null;
    }

    function makeHost(feature) {
        var session = createTemporaryObject(sessionComponent, testCase);
        var registry = createTemporaryObject(registryComponent, testCase, {session: session});
        var host = createTemporaryObject(hostComponent, testCase,
                                         {registry: registry, libraryId: "EB9F-F032", stickLabel: "A3",
                                          feature: feature});
        waitForRendering(host);
        return {session: session, host: host};
    }

    function test_nothingStagedMeansNobodyIsBlocked() {
        var t = makeHost("sync");
        compare(t.host.editorOwner, "");
        compare(t.host.blockedByOtherPage, false);
    }

    function test_thePageThatOwnsTheBatchIsNotBlocked() {
        var t = makeHost("sync");
        t.session.editorOwner = "sync";
        t.session.dirty = true;
        compare(t.host.blockedByOtherPage, false);
    }

    function test_anotherPagesBatchBlocksThisOne() {
        var t = makeHost("cleanup");
        t.session.editorOwner = "library-health";
        t.session.dirty = true;
        compare(t.host.blockedByOtherPage, true);

        // ...and stops being blocked as soon as that page is done.
        t.session.editorOwner = "";
        t.session.dirty = false;
        compare(t.host.blockedByOtherPage, false);
    }

    // A page with no feature set (Browse before a cue is staged, or a page
    // that never stages) must not be blocked by anything.
    function test_aPageWithoutAFeatureIsNeverBlocked() {
        var t = makeHost("");
        t.session.editorOwner = "sync";
        t.session.dirty = true;
        compare(t.host.blockedByOtherPage, false);
    }

    function test_aRefusedStageExplainsWhichPageHoldsIt() {
        var t = makeHost("cleanup");
        var dialog = findByObjectName(t.host, "otherPageDialog");
        verify(dialog !== null);
        compare(dialog.opened, false);

        t.session.editorConflict("library-health", "cleanup");
        tryCompare(dialog, "opened", true, 5000);
        var message = findByObjectName(dialog, "messageLabel");
        verify(message.text.indexOf("Library Health") >= 0);
        // The reassurance is its own, quieter line now, not part of the
        // consequence sentence.
        verify(findByObjectName(dialog, "messageDetailLabel").text.indexOf("Nothing was changed") >= 0);

        findByObjectName(dialog, "understoodButton").clicked();
        tryCompare(dialog, "opened", false, 5000);
    }

    // Every owner the C++ side can produce maps to a name a user knows.
    function test_everyOwnerHasAPageName() {
        var t = makeHost("sync");
        var owners = ["settings", "sync", "library-health", "cleanup", "dup", "localcue", "addcue"];
        for (var i = 0; i < owners.length; ++i) {
            var name = t.host.pageNameFor(owners[i]);
            verify(name.length > 0);
            compare(name, t.host.pageNameFor(owners[i]));
            verify(name !== "another page", "owner " + owners[i] + " has no page name");
        }
        compare(t.host.pageNameFor("something-new"), "another page");
    }
}
