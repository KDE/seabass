// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The process guard's modal, driven by a fake guard object: it opens
// exactly when the guard says blocking, names the app, tells the guard
// while it is up (so polling speeds up), and closes itself again.
TestCase {
    id: testCase
    name: "DjSoftwareRunningDialog"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component {
        id: guardComponent
        QtObject {
            property string conflictingSoftware: "rekordbox"
            property bool blocking: false
            property bool dialogOpen: false
        }
    }

    Component {
        id: dialogComponent
        DjSoftwareRunningDialog {}
    }

    // A Dialog is a Popup (a QObject, no `children`): walk its contentItem.
    function findByObjectName(item, name) {
        if (!item) return null;
        if (item.objectName === name) return item;
        var kids = item.contentItem ? [item.contentItem] : [];
        var children = item.children ? item.children : [];
        for (var i = 0; i < children.length; ++i) kids.push(children[i]);
        for (var j = 0; j < kids.length; ++j) {
            var found = findByObjectName(kids[j], name);
            if (found) return found;
        }
        return null;
    }

    function test_opensWhileBlockingAndClosesItself() {
        var guard = createTemporaryObject(guardComponent, testCase);
        var dialog = createTemporaryObject(dialogComponent, testCase, {guard: guard});
        verify(dialog !== null);
        compare(dialog.opened, false);
        compare(dialog.closePolicy, Popup.NoAutoClose);

        guard.blocking = true;
        tryCompare(dialog, "opened", true);
        tryCompare(guard, "dialogOpen", true);
        verify(dialog.title.indexOf("rekordbox") >= 0);
        var label = findByObjectName(dialog, "messageLabel");
        verify(label !== null);
        verify(label.text.indexOf("rekordbox is running. Please close it until your changes have been saved.") === 0);
        verify(label.text.indexOf("(You have been warned!)") > 0);
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(testCase);
            grabImage(testCase).save(screenshotDir + "/dj-software-running-dialog.png");
        }

        guard.blocking = false;
        tryCompare(dialog, "opened", false);
        tryCompare(guard, "dialogOpen", false);
    }

    function test_namesEngineDjToo() {
        var guard = createTemporaryObject(guardComponent, testCase, {conflictingSoftware: "Engine DJ", blocking: true});
        var dialog = createTemporaryObject(dialogComponent, testCase, {guard: guard});
        tryCompare(dialog, "opened", true);
        verify(findByObjectName(dialog, "messageLabel").text.indexOf("Engine DJ is running.") === 0);
        guard.blocking = false;
        tryCompare(dialog, "opened", false);
    }
}
