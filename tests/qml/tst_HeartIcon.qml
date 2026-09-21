// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest

import "PixelScale.js" as PixelScale
import SeabassGui

TestCase {
    id: testCase
    name: "HeartIcon"
    width: 120
    height: 120
    visible: true
    when: windowShown

    Component {
        id: heartComponent
        HeartIcon { width: 44; height: 44; color: "#ff0000" }
    }

    // Filled, not outlined: the middle of the heart is painted. Breeze's
    // own "love" icon is an outline, and there the middle is empty.
    function test_theHeartIsFilled() {
        var heart = createTemporaryObject(heartComponent, testCase);
        waitForRendering(heart);
        var image = grabImage(heart);
        // (11, 11) on Breeze's 22 grid, twice over: inside the inner
        // contour an outline leaves open.
        // Logical coordinates through PixelScale: on a 2x display the
        // grab of this 44x44 icon is 88x88, and pixel(22, 22) reads the
        // top-left quadrant rather than the middle.
        var middle = PixelScale.pixel(image, heart, 22, 22);
        verify(middle.r > 0.8 && middle.g < 0.2 && middle.b < 0.2,
               "the inside of the heart must be painted, got " + middle);
        // And it is a heart, not a filled square: the corners stay clear.
        var corner = PixelScale.pixel(image, heart, 1, 42);
        verify(!(corner.r > 0.8 && corner.g < 0.2 && corner.b < 0.2), "the bottom corner must be left unpainted, got " + corner);
    }
}
