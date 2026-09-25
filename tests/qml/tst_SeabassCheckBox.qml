// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// SeabassCheckBox can be seen: measured, not assumed. The KDE style's own
// indicator measured 1.8:1 on a list row and vanished on a real Plasma
// session. This suite runs under Basic and, as
// seabass_qml_desktop_style_tests, under the KDE style, and each box has to
// clear 3:1 against the colour it sits on, the floor for a control's
// boundary, unticked and ticked, on every surface a box appears on.
TestCase {
    id: testCase
    name: "SeabassCheckBox"
    when: windowShown
    visible: true
    width: 240
    height: 60

    Component {
        id: sceneComponent
        Rectangle {
            property alias box: box
            width: 220
            height: 40
            SeabassCheckBox {
                id: box
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Include this track"
            }
        }
    }

    function luminance(c) {
        function channel(v) {
            return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
        }
        return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
    }
    function contrast(a, b) {
        var la = luminance(a), lb = luminance(b);
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
    }

    // The best contrast along the indicator's left edge, where the outline
    // is: an antialiased edge spreads over a pixel or two, and the eye
    // takes the strongest of them.
    function edgeContrast(scene, image) {
        var ind = findChild(scene.box, "seabassCheckIndicator");
        verify(ind !== null, "no indicator");
        var p = ind.mapToItem(scene, 0, 0);
        var ground = image.pixel(2, 2);
        var best = 0;
        for (var dx = 0; dx < 3; ++dx) {
            for (var dy = Math.round(ind.height * 0.3); dy < Math.round(ind.height * 0.7); ++dy) {
                best = Math.max(best, contrast(image.pixel(Math.round(p.x) + dx, Math.round(p.y) + dy), ground));
            }
        }
        return best;
    }

    function test_theBoxCanBeSeenOnEverySurface_data() {
        return [
            {tag: "row, even", ground: Theme.rowEven},
            {tag: "row, odd", ground: Theme.rowOdd},
            {tag: "row, hovered", ground: Theme.rowHover},
            {tag: "surface", ground: Theme.surface},
            {tag: "page", ground: Theme.background},
        ];
    }
    function test_theBoxCanBeSeenOnEverySurface(data) {
        var scene = createTemporaryObject(sceneComponent, testCase, {color: data.ground});
        verify(scene !== null);
        waitForRendering(scene);
        var unticked = edgeContrast(scene, grabImage(scene));
        verify(unticked >= 3, "unticked outline " + unticked.toFixed(2) + ":1 on " + data.tag);

        scene.box.checked = true;
        waitForRendering(scene);
        var ticked = edgeContrast(scene, grabImage(scene));
        verify(ticked >= 3, "ticked box " + ticked.toFixed(2) + ":1 on " + data.tag);
    }

    function test_theLabelClearsTheBox() {
        var scene = createTemporaryObject(sceneComponent, testCase, {color: Theme.rowEven});
        waitForRendering(scene);
        var ind = findChild(scene.box, "seabassCheckIndicator");
        var label = scene.box.contentItem;
        verify(label !== null);
        // Where the text itself starts, padding included.
        var textStart = label.x + (label.leftPadding || 0);
        verify(textStart >= ind.x + ind.width,
               "the label starts at " + textStart + ", inside the box ending at " + (ind.x + ind.width));
    }

    function test_theMarkShowsOnlyWhenTicked() {
        var scene = createTemporaryObject(sceneComponent, testCase, {color: Theme.rowEven});
        var mark = findChild(scene.box, "seabassCheckMark");
        verify(mark !== null);
        verify(!mark.visible);
        mouseClick(scene.box);
        verify(scene.box.checked, "a click ticks it, as any CheckBox");
        verify(mark.visible);
    }
}
