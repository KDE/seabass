// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

TestCase {
    id: testCase
    name: "ProgressTrack"
    width: 700
    height: 100
    visible: true
    when: windowShown

    Component {
        id: trackComponent
        ProgressTrack { x: 10; y: 10; width: 600; from: 0; to: 100 }
    }

    function isAccent(c) {
        return Math.abs(c.r - Theme.accent.r) + Math.abs(c.g - Theme.accent.g) + Math.abs(c.b - Theme.accent.b) < 0.1;
    }

    // A sliver of progress still starts with the track's round end. A fill
    // narrower than the bar is tall used to have its corner radius shrunk
    // to fit, which drew a square-ended sliver in the round track.
    function test_aSmallFillKeepsTheRoundedEnd() {
        var track = createTemporaryObject(trackComponent, testCase, {value: 1});  // 6 of 600 px
        verify(track !== null);
        waitForRendering(track);
        var fill = findChild(track, "progressFill");
        verify(fill !== null, "the fill must exist");
        verify(fill.width > 0 && fill.width < fill.height, "the case under test: a fill narrower than it is tall");
        var image = grabImage(track);
        var origin = track.contentItem.mapToItem(track, 0, 0);
        var h = track.contentItem.height;
        var middle = image.pixel(Math.round(origin.x) + 3, Math.round(origin.y + h / 2));
        verify(isAccent(middle), "the fill must be drawn at mid height, got " + middle);
        var top = image.pixel(Math.round(origin.x) + 2, Math.round(origin.y) + 1);
        var bottom = image.pixel(Math.round(origin.x) + 2, Math.round(origin.y + h) - 2);
        verify(!isAccent(top), "the top left corner must stay round, got " + top);
        verify(!isAccent(bottom), "the bottom left corner must stay round, got " + bottom);
    }

    // A full bar is still the whole pill.
    function test_aFullFillIsTheWholeTrack() {
        var track = createTemporaryObject(trackComponent, testCase, {value: 100});
        waitForRendering(track);
        var fill = findChild(track, "progressFill");
        compare(Math.round(fill.width), Math.round(track.contentItem.width));
    }
}
