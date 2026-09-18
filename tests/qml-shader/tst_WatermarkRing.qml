// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The watermark as a ring: there while a track is loaded, faint, in its
// corner, drawn -- and no obstacle to the page under it.
TestCase {
    id: testCase
    name: "WatermarkRing"
    width: 1100
    height: 720
    visible: true
    when: windowShown

    property var loaded: ({
        hasTrack: true, playing: false, position: 60000, duration: 240000, artworkPath: "", cues: [],
        waveform: (function() { var c = []; for (var i = 0; i < 400; ++i) c.push({low: 0.9, mid: 0.5, high: 0.2}); return c; })(),
    })
    property var idle: ({hasTrack: false})

    Component {
        id: stageComponent
        Rectangle {
            id: stage
            width: 1100
            height: 720
            color: "black"
            property alias mark: watermark
            property int clicks: 0
            property var player: testCase.loaded
            // A page's button, under the corner the watermark covers.
            Button {
                objectName: "underTheWatermark"
                x: 1100 - 300; y: 720 - 200; width: 120; height: 60
                text: "Save"
                onClicked: stage.clicks += 1
            }
            WatermarkRing { id: watermark; playbackController: stage.player }
        }
    }

    function test_itIsThereFaintInItsCornerWhileATrackIsLoaded() {
        var stage = createTemporaryObject(stageComponent, testCase);
        var mark = stage.mark;
        compare(mark.shows, true);
        tryCompare(mark, "opacity", mark.shownOpacity, 2000);
        verify(mark.opacity < 0.4, "a watermark is faint");
        compare(mark.width, 720 * 0.75, "the cover art watermark's square");
        fuzzyCompare(mark.x + mark.width, 1100 + mark.width * 0.08, 0.5, "bleeding off the right edge as that does");
        fuzzyCompare(mark.y + mark.height, 720 + mark.height * 0.08, 0.5, "and off the bottom");

        // And it is drawn: a played bar is lit a little -- the accent colour
        // at the watermark's strength. One near twelve o'clock: further
        // round, the ring is off the edge of the window.
        wait(400);
        var image = grabImage(stage);
        var cx = mark.x + mark.width / 2;
        var cy = mark.y + mark.height / 2;
        var angle = (10.5 / 200) * 2 * Math.PI;                 // the middle of a bar, 5% round: played
        var px = Math.round(cx + Math.sin(angle) * mark.width / 2 * 0.9);
        var py = Math.round(cy - Math.cos(angle) * mark.width / 2 * 0.9);
        verify(px < 1100 && py > 0, "the point looked at is in the window");
        verify(image.blue(px, py) > 15 && image.blue(px, py) < 110, "faintly there, got b=" + image.blue(px, py));
    }

    function test_aClickGoesThroughItToThePage() {
        var stage = createTemporaryObject(stageComponent, testCase);
        tryCompare(stage.mark, "opacity", stage.mark.shownOpacity, 2000);
        var button = findChild(stage, "underTheWatermark");
        var inMark = stage.mark.mapFromItem(button, 60, 30);
        verify(inMark.x > 0 && inMark.y > 0 && inMark.x < stage.mark.width && inMark.y < stage.mark.height,
               "the button really is under the watermark");
        mouseClick(stage, button.x + 60, button.y + 30);
        compare(stage.clicks, 1, "a faint picture in the corner must not swallow the page's clicks");
    }

    function test_withNothingLoadedItIsGone() {
        var stage = createTemporaryObject(stageComponent, testCase, {player: testCase.idle});
        compare(stage.mark.shows, false);
        compare(stage.mark.visible, false);
    }
}
