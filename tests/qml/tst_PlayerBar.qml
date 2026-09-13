// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

TestCase {
    id: testCase
    name: "PlayerBar"
    width: 820
    height: 200
    visible: true
    when: windowShown

    Component {
        id: barComponent
        PlayerBar { width: 800 }
    }

    function fakeController() {
        return {title: "Major Tom", artist: "DJ Amador", artworkPath: "", position: 0, duration: 372000,
                playing: false, errorMessage: "", waveform: [], cues: [], calls: [],
                togglePlay: function() { this.calls.push("togglePlay"); },
                seek: function(ms) { this.calls.push("seek"); },
                stop: function() { this.calls.push("stop"); }};
    }

    // Out of the way until the pointer is over the player, in its top
    // right corner, and closing stops playback.
    function test_closeAppearsOnHoverAndStopsPlayback() {
        var bar = createTemporaryObject(barComponent, testCase, {controller: fakeController()});
        verify(bar !== null);
        waitForRendering(bar);
        var close = findChild(bar, "closePlayerButton");
        verify(close !== null, "the player must have a close button");
        compare(close.visible, false, "hidden until the pointer is over the player");

        mouseMove(bar, bar.width / 2, bar.height / 2);
        tryCompare(close, "visible", true);
        var corner = close.mapToItem(bar, close.width, 0);
        compare(Math.round(corner.x), Math.round(bar.width), "flush with the right edge");
        compare(Math.round(corner.y), 0, "flush with the top edge");
        // And clear of the time beside it.
        var time = findChild(bar, "playerTime");
        verify(time !== null);
        var timeRight = time.mapToItem(bar, time.width, 0).x;
        var closeLeft = close.mapToItem(bar, 0, 0).x;
        verify(timeRight <= closeLeft, "the close button must not cover the time (" + timeRight + " vs " + closeLeft + ")");

        mouseClick(close);
        verify(bar.controller.calls.indexOf("stop") >= 0, "closing must stop playback");
    }
}
