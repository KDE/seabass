// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The confirmation toast on its own: shown with its text for its time,
// gone by itself after it, at once on a click, and held while the pointer
// rests on it.
TestCase {
    id: testCase
    name: "ConfirmationToast"
    width: 400
    height: 300
    visible: true
    when: windowShown

    Component {
        id: toastComponent
        ConfirmationToast { duration: 400 }
    }

    function makeToast() {
        const toast = createTemporaryObject(toastComponent, testCase, {x: 100, y: 100});
        verify(toast !== null);
        return toast;
    }

    function test_itShowsItsTextAndGoesByItself() {
        const toast = makeToast();
        compare(toast.showing, false);
        compare(toast.visible, false);
        toast.show("3 cues added");
        compare(toast.showing, true);
        compare(findChild(toast, "confirmationToastLabel").text, "3 cues added");
        tryCompare(toast, "visible", true);
        tryCompare(toast, "showing", false, 3000);
        tryCompare(toast, "visible", false, 3000, "faded out");
    }

    // By the ring, not the wall clock: animations in this harness do not
    // keep step with wait().
    function test_shownAgainItRestartsWithTheNewText() {
        const toast = makeToast();
        const mark = findChild(toast, "confirmationToastMark");
        toast.show("first");
        tryVerify(() => mark.remaining < 0.7, 3000, "the ring has run down some");
        toast.show("second");
        compare(findChild(toast, "confirmationToastLabel").text, "second");
        compare(mark.remaining, 1, "the ring starts over");
        compare(toast.showing, true);
        tryCompare(toast, "showing", false, 3000);
        compare(mark.remaining, 0, "and ran all the way down before it went");
    }

    function test_aClickSendsItAway() {
        const toast = makeToast();
        let dismissed = 0;
        toast.dismissed.connect(() => { ++dismissed; });
        toast.show("saved");
        // Faded in first: at the instant of show() it is not visible yet,
        // and nothing invisible takes a click.
        tryCompare(toast, "visible", true);
        mouseClick(toast);
        compare(toast.showing, false);
        compare(dismissed, 1);
        toast.dismiss();
        compare(dismissed, 1, "dismissing what is gone says nothing");
    }

    function test_thePointerOnItHoldsIt() {
        const toast = makeToast();
        toast.show("held");
        mouseMove(testCase, toast.x + toast.width / 2, toast.y + toast.height / 2);
        tryCompare(toast, "held", true);
        const ring = findChild(toast, "confirmationToastMark").remaining;
        wait(toast.duration + 300);
        compare(toast.showing, true, "held while hovered");
        compare(findChild(toast, "confirmationToastMark").remaining, ring, "the ring stopped with it");
        mouseMove(testCase, 2, 2);
        tryCompare(toast, "held", false);
        tryCompare(toast, "showing", false, 3000, "gone once the pointer left");
    }
}
