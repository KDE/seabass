// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// ScrollCollapse on a plain list with rows of a known height, so the
// numbers that decide folding are exact: a 200 px header above a list
// of 40 px rows in a 400 px window.
//
// The case this file exists for is the list that only just overflows.
// Folding the header makes the list taller, a taller list scrolls less
// far, and a header that folded on the scroll position alone would be
// unfolded again by its own fold.
TestCase {
    id: testCase
    name: "ScrollCollapse"
    width: 400
    height: 400
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        Column {
            id: page
            width: testCase.width
            height: testCase.height
            property alias list: list
            property alias collapse: collapse
            property int rows: 40
            readonly property real headerFull: 200

            ScrollCollapse {
                id: collapse
                flickable: list
                collapsibleHeight: page.headerFull
            }
            Rectangle {
                width: parent.width
                height: page.headerFull * (1 - collapse.progress)
                color: "steelblue"
            }
            ListView {
                id: list
                width: parent.width
                height: page.height - y
                clip: true
                model: page.rows
                delegate: Rectangle {
                    required property int index
                    width: ListView.view.width
                    height: 40
                    color: index % 2 ? "white" : "lightgrey"
                }
            }
        }
    }

    function makePage(rows) {
        const page = createTemporaryObject(pageComponent, testCase, {rows: rows});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        return page;
    }

    // Whatever an animation is doing, done: the fold's longest run is the
    // unfold, and this waits it out with room to spare.
    function settle() {
        wait(Theme.arrivalTransitionDuration + 200);
    }

    function test_scrollingPastTheThresholdFoldsAndTheTopUnfolds() {
        const page = makePage(40);
        verify(!page.collapse.collapsed);

        // A keypress or the scroll bar: contentY moves, nothing else.
        page.list.contentY = page.collapse.threshold + 1;
        tryCompare(page.collapse, "progress", 1);
        // The layout hands the list its last pixel a frame after the fold
        // ends, so wait for it rather than read it at once.
        tryCompare(page.list, "height", testCase.height, 1000, "the list took the header's room");
        compare(page.list.contentY, page.collapse.threshold + 1, "folding did not move the list");

        // Back up, not yet at the top: stays folded.
        page.list.contentY = 1;
        settle();
        verify(page.collapse.collapsed, "unfolded before the top");

        page.list.contentY = 0;
        tryCompare(page.collapse, "progress", 0);
    }

    function test_theWheelFoldsAndUnfolds() {
        const page = makePage(40);
        for (let i = 0; i < 5 && !page.collapse.collapsed; ++i) {
            mouseWheel(page.list, page.list.width / 2, page.list.height / 2, 0, -120);
            wait(50);
        }
        verify(page.collapse.collapsed, "the wheel never folded the header");
        tryCompare(page.collapse, "progress", 1);
        for (let i = 0; i < 40 && !page.list.atYBeginning; ++i) {
            mouseWheel(page.list, page.list.width / 2, page.list.height / 2, 0, 120);
            wait(50);
        }
        verify(page.list.atYBeginning);
        tryCompare(page.collapse, "progress", 0);
    }

    // One notch past the threshold, held there: folded, and still folded
    // and still the same height a while later.
    function test_foldedJustPastTheThresholdIsStable() {
        const page = makePage(40);
        page.list.contentY = page.collapse.threshold + 1;
        tryCompare(page.collapse, "progress", 1);
        const heights = [];
        for (let i = 0; i < 10; ++i) {
            wait(50);
            heights.push(page.list.height);
        }
        verify(heights.every((h) => h === testCase.height), "the list kept resizing: " + heights);
        verify(page.collapse.collapsed);
    }

    // Ten rows are 400 px in a 200 px list: scrolled to the bottom it is
    // 200 px down, far past the threshold, but folded the list is 400 px
    // tall and has nowhere to scroll at all. Folding there would throw
    // the list back to its top and unfold it. So it does not fold, and
    // nothing moves or changes size while it sits there.
    function test_aListThatOnlyJustOverflowsDoesNotFold() {
        const page = makePage(10);
        const bottom = page.list.contentHeight - page.list.height;
        verify(bottom > page.collapse.threshold, "the list must scroll past the threshold for this to mean anything");
        page.list.contentY = bottom;
        const heights = [];
        let everFolded = false;
        for (let i = 0; i < 12; ++i) {
            wait(50);
            heights.push(page.list.height);
            everFolded = everFolded || page.collapse.collapsed;
        }
        verify(!everFolded, "folded where folding unfolds it again");
        verify(heights.every((h) => h === heights[0]), "the list kept resizing: " + heights);
        compare(page.list.contentY, bottom, "the list was moved");
    }
}
