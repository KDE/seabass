// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The page stack cross-fades: the page pushed fades in while the one it
// covers fades out, and a pop does the same the other way. Sampled while
// the fade runs, so a stack that cut from one page to the next would fail.
TestCase {
    id: testCase
    name: "FadeStackView"
    width: 300
    height: 200
    visible: true
    when: windowShown

    Component {
        id: stackComponent
        FadeStackView {
            anchors.fill: parent
            arriveDuration: 400
            leaveDuration: 300
        }
    }
    Component {
        id: pageComponent
        Rectangle { color: "steelblue" }
    }

    // Opacities seen while the stack is busy: {entering: [...], leaving: [...]}.
    function sampleFade(stack, entering, leaving) {
        const seen = {entering: [], leaving: []};
        const deadline = Date.now() + 2000;
        while (stack.busy && Date.now() < deadline) {
            seen.entering.push(entering.opacity);
            if (leaving) {
                seen.leaving.push(leaving.opacity);
            }
            wait(16);
        }
        return seen;
    }
    function between(values) {
        return values.some(v => v > 0.05 && v < 0.95);
    }

    function test_aPushFadesTheNewPageInAndTheOldOneOut() {
        const stack = createTemporaryObject(stackComponent, testCase, {initialItem: pageComponent});
        verify(stack !== null);
        tryCompare(stack, "busy", false);
        const first = stack.currentItem;
        const second = stack.push(pageComponent);
        const seen = sampleFade(stack, second, first);
        verify(seen.entering.length > 2, "the fade was long enough to be seen: " + seen.entering.length + " samples");
        verify(between(seen.entering), "the new page passed through part opacity: " + seen.entering.join(","));
        verify(between(seen.leaving), "the old page faded rather than vanishing: " + seen.leaving.join(","));
        compare(second.opacity, 1, "arrived whole");
        compare(stack.depth, 2);
    }

    function test_aPopFadesBack() {
        const stack = createTemporaryObject(stackComponent, testCase, {initialItem: pageComponent});
        tryCompare(stack, "busy", false);
        const first = stack.currentItem;
        const second = stack.push(pageComponent);
        tryCompare(stack, "busy", false, 3000);
        stack.pop();
        const seen = sampleFade(stack, first, second);
        verify(between(seen.entering), "the page coming back fades in: " + seen.entering.join(","));
        verify(between(seen.leaving), "the page leaving fades out: " + seen.leaving.join(","));
        compare(first.opacity, 1);
        compare(stack.depth, 1);
    }

    function test_theAppKeepsThemesTimes() {
        const stack = createTemporaryObject(stackComponent, testCase, {initialItem: pageComponent,
                                            arriveDuration: Theme.arrivalTransitionDuration,
                                            leaveDuration: Theme.departureTransitionDuration});
        compare(stack.arriveDuration, 320, "arriving takes a third of a second, as things arriving do");
        compare(stack.leaveDuration, 240, "leaving a touch quicker");
    }
}
