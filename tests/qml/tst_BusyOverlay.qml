// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// BusyOverlay's Cancel button: present only for a cancellable (read-only)
// operation, and it asks the page to cancel rather than doing anything
// itself.
TestCase {
    id: testCase
    name: "BusyOverlay"
    width: 600
    height: 400
    visible: true
    when: windowShown

    Component {
        id: overlayComponent
        BusyOverlay { width: 600; height: 400 }
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    function findCancelButton(item) {
        if (item.objectName === "cancelButton") return item;
        for (var i = 0; i < item.children.length; ++i) {
            var found = findCancelButton(item.children[i]);
            if (found) return found;
        }
        return null;
    }

    function test_notCancellableHidesTheButton() {
        var overlay = createTemporaryObject(overlayComponent, testCase, {busy: true, label: "Scanning..."});
        verify(overlay !== null);
        waitForRendering(overlay);
        var button = findCancelButton(overlay);
        verify(button !== null);
        compare(button.visible, false);
    }

    function test_cancellableShowsTheButtonAndRequestsCancel() {
        var overlay = createTemporaryObject(overlayComponent, testCase,
                                            {busy: true, cancellable: true, current: 3, total: 10});
        verify(overlay !== null);
        waitForRendering(overlay);
        var button = findCancelButton(overlay);
        verify(button !== null);
        compare(button.visible, true);
        var spy = createTemporaryObject(spyComponent, testCase, {target: overlay, signalName: "cancelRequested"});
        button.clicked();
        compare(spy.count, 1);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(overlay).save(screenshotDir + "/busy-overlay-cancel.png");
        }
    }
}
