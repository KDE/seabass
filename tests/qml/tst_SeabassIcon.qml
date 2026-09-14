// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import SeabassGui

// The bundled Breeze icons load, and draw in one flat colour.
TestCase {
    id: testCase
    name: "SeabassIcon"
    width: 400
    height: 200
    visible: true
    when: windowShown

    Component {
        id: iconComponent
        SeabassIcon { iconName: "edit-delete"; size: 44; color: "#00ff00" }
    }
    Component {
        id: iconLabelComponent
        IconLabel { width: 300; text: "3 of 40 entries"; iconName: "dialog-warning" }
    }

    // Each file compiled in loads from the URL Theme gives for it. The
    // list comes from the resources themselves (qml_test_main.cpp), so a
    // file listed in CMake under one path and asked for under another
    // fails here.
    function test_everyBundledIconLoads() {
        verify(bundledIcons.length >= 40, "only " + bundledIcons.length + " bundled icons were found");
        for (var i = 0; i < bundledIcons.length; ++i) {
            var icon = createTemporaryObject(iconComponent, testCase, {iconName: bundledIcons[i]});
            tryCompare(icon, "status", Image.Ready, 2000, bundledIcons[i] + " did not load");
            compare(icon.implicitWidth, 44, bundledIcons[i] + " is not rasterised at the size asked for");
        }
    }

    // One colour, whatever Breeze painted: edit-delete is red in Breeze
    // (ColorScheme-NegativeText). Every visible pixel must come out green,
    // and there must be some -- a tint that drew nothing would pass the
    // first half on its own.
    function test_theWholeShapeTakesTheOneColour() {
        var icon = createTemporaryObject(iconComponent, testCase);
        tryCompare(icon, "status", Image.Ready);
        waitForRendering(icon);
        var image = grabImage(icon);
        var painted = 0;
        for (var y = 0; y < 44; ++y) {
            for (var x = 0; x < 44; ++x) {
                var p = image.pixel(x, y);
                if (p.g > 0.5 && p.r < 0.3 && p.b < 0.3) {
                    ++painted;
                }
                verify(!(p.r > 0.5 && p.g < 0.3), "a red pixel survived the tint at " + x + "," + y + ": " + p);
            }
        }
        verify(painted > 50, "only " + painted + " pixels were painted in the icon's colour");
    }

    // The text starts past the icon, and the icon sits on the first line.
    function test_iconLabelIndentsItsText() {
        var label = createTemporaryObject(iconLabelComponent, testCase);
        verify(label.leftPadding > 0, "the text must start past the icon");
        label.iconName = "";
        compare(label.leftPadding, 0, "without an icon there is nothing to indent past");
    }
}
