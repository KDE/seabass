// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The shading behind a hovered list row. The point of it is that it
// fades, so that is what these check: the value on the way, not only the
// value it ends on.
TestCase {
    id: testCase
    name: "RowHoverShade"
    width: 200
    height: 60
    visible: true
    when: windowShown

    Component {
        id: rowComponent
        Rectangle {
            width: 200
            height: 60
            property alias shade: shadeItem
            RowHoverShade { id: shadeItem }
        }
    }

    function make() {
        var row = createTemporaryObject(rowComponent, testCase);
        verify(row !== null, "the row did not instantiate");
        waitForRendering(row);
        return row;
    }

    function test_theShadeFadesInAndOutRatherThanSnapping() {
        var row = make();
        var shade = row.shade;
        compare(shade.opacity, 0, "an untouched row carries no shading");
        compare(shade.visible, false, "and nothing to draw or hit-test");

        shade.hovered = true;
        // The teeth: without the animation this is already 1.
        verify(shade.opacity < 1, "the shade must fade in, not appear at once");
        verify(shade.visible, "and it must be drawn while it fades in");
        tryCompare(shade, "opacity", 1, 2000);

        shade.hovered = false;
        verify(shade.opacity > 0, "the shade must fade out, not vanish at once");
        tryCompare(shade, "opacity", 0, 2000);
        compare(shade.visible, false, "a faded-out shade is gone again");
    }

    // Pressed is the stronger of the two colours, and it wins while both
    // are true -- a row is always hovered while it is being pressed.
    function test_pressedShadesStrongerThanHovered() {
        var row = make();
        var shade = row.shade;
        shade.hovered = true;
        tryCompare(shade, "opacity", 1, 2000);
        var hoveredColour = String(shade.color);
        shade.pressed = true;
        verify(String(shade.color) !== hoveredColour, "pressing must shade deeper than hovering");
        compare(String(shade.color), String(Theme.rowPressed));
        shade.pressed = false;
        compare(String(shade.color), String(Theme.rowHover));
    }
}
