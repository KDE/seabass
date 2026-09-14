// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The row both metadata pages share.
TestCase {
    id: testCase
    name: "MetadataTrackDelegate"
    width: 620
    height: 160
    visible: true
    when: windowShown

    Component {
        id: rowComponent
        MetadataTrackDelegate { width: 600; title: "Major Tom"; artist: "DJ Amador" }
    }

    // The tick box shows what the page decided, not what was clicked. A page
    // that refuses to stage a row (the library is held elsewhere, the row has
    // nothing it can write) leaves `selected` alone, and the box used to stay
    // ticked anyway -- over nothing, and out of Select None's reach.
    function test_aRefusedTickDoesNotStay() {
        var row = createTemporaryObject(rowComponent, testCase, {index: 0, selected: false});
        verify(row !== null);
        var asked = [];
        var accept = false;
        row.selectionToggled.connect(function(wanted) {
            asked.push(wanted);
            if (accept) {
                row.selected = wanted;
            }
        });
        var box = findChild(row, "selectCheckBox");
        verify(box !== null);

        mouseClick(box);
        compare(asked.length, 1);
        compare(asked[0], true, "the page is asked to stage the row");
        compare(box.checked, false, "a refused tick must not stay ticked");

        accept = true;
        mouseClick(box);
        compare(box.checked, true, "an accepted tick stays");
        row.selected = false;
        compare(box.checked, false, "and the page can still clear it, as Select None does");
    }
}
