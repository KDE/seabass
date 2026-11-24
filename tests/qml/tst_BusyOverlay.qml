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

    function findWhere(item, predicate) {
        if (predicate(item)) {
            return item;
        }
        const kids = item.children ? item.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findWhere(kids[i], predicate);
            if (found) {
                return found;
            }
        }
        return null;
    }

    function yIn(overlay, item) {
        return item.mapToItem(overlay, 0, 0).y;
    }

    // #118: the remaining time comes in after ten seconds. It used to be a
    // line of its own under Cancel, in a column centred on the page, so
    // its arrival pushed the title, the bar and the button all up. It sits
    // beside Cancel now, on a line that is there from the start, and
    // nothing above it moves; nor does the button.
    function test_theRemainingTimeMovesNothing_data() {
        return [
            {tag: "cancellable", cancellable: true},
            {tag: "not cancellable", cancellable: false},
        ];
    }

    function test_theRemainingTimeMovesNothing(row) {
        const overlay = createTemporaryObject(overlayComponent, testCase,
                                              {busy: true, cancellable: row.cancellable, current: 3, total: 10,
                                               label: "Scanning..."});
        verify(overlay !== null);
        waitForRendering(overlay);
        compare(overlay.etaText, "", "no estimate in the first ten seconds");
        const title = findWhere(overlay, (item) => item.text === "Scanning..." && item.visible);
        const bar = findChild(overlay, "progressTrack");
        const button = findCancelButton(overlay);
        verify(title !== null && bar !== null && button !== null);
        const titleY = yIn(overlay, title);
        const barY = yIn(overlay, bar);
        const buttonY = yIn(overlay, button);

        // Twenty seconds on, three of ten done: an estimate.
        overlay._startTimeMs = Date.now() - 20000;
        overlay._nowMs = Date.now();
        verify(overlay.etaText.length > 0, "an estimate after twenty seconds");
        const eta = findWhere(overlay, (item) => item.visible && item.text === overlay.etaText);
        verify(eta !== null, "the remaining time is shown");
        waitForPolish(overlay);
        waitForRendering(overlay);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(overlay).save(screenshotDir + "/busy-overlay-remaining-"
                                    + (row.cancellable ? "cancellable" : "not-cancellable") + ".png");
        }

        compare(yIn(overlay, title), titleY, "the title stays put");
        compare(yIn(overlay, bar), barY, "the bar stays put");
        if (row.cancellable) {
            compare(yIn(overlay, button), buttonY, "the Cancel line stays put");
            // On Cancel's line, to its left, and not under it.
            const etaTop = yIn(overlay, eta);
            verify(etaTop < buttonY + button.height && etaTop + eta.height > buttonY,
                   "the remaining time shares Cancel's line: " + etaTop + " vs " + buttonY);
            verify(eta.mapToItem(overlay, eta.width, 0).x <= button.mapToItem(overlay, 0, 0).x,
                   "and sits left of the button");
        }
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
