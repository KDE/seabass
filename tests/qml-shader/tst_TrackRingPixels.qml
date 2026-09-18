// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// What TrackRing's shader actually draws, read back pixel by pixel. Runs
// as seabass_qml_shader_tests, on a scene graph that can run a shader --
// see the top-level CMakeLists.txt. On the main suite's offscreen
// platform every one of these would find an empty item.
TestCase {
    id: testCase
    name: "TrackRingPixels"
    width: 600
    height: 600
    visible: true
    when: windowShown

    // Every column the same, low band full: every bar reaches the rim,
    // so a pixel near the rim is in the low band's colour all the way
    // round and only the playhead decides how bright it is.
    function flatWaveform() {
        var columns = [];
        for (var i = 0; i < 400; ++i) {
            columns.push({low: 1.0, mid: 0.5, high: 0.2});
        }
        return columns;
    }

    Component {
        id: stageComponent
        Rectangle {
            width: 600
            height: 600
            color: "black"
            property alias ring: ringItem
            TrackRing {
                id: ringItem
                anchors.fill: parent
                waveformData: testCase.flatWaveform()
                trackDurationMs: 400000
                fallbackColor: "#00ff00"
                backgroundColor: "black"
            }
        }
    }

    function make(properties) {
        var stage = createTemporaryObject(stageComponent, testCase);
        verify(stage !== null, "the stage did not instantiate");
        for (var key in properties) {
            stage.ring[key] = properties[key];
        }
        verify(stage.ring.available, "this suite exists to run the shader: no GPU scene graph, or no shader in the binary");
        // The waveform reaches the shader through a Canvas, which paints
        // when it gets round to it.
        wait(400);
        return stage;
    }

    // The middle of the bar `fraction` of the way round, `radius` out
    // (in units of the outer radius). Bars are 200 to the turn; the
    // middle of one is solid, its edges are antialiased gap.
    function pixelAt(image, fraction, radius) {
        var bar = Math.floor(fraction * 200);
        var angle = ((bar + 0.5) / 200) * 2 * Math.PI;
        var x = Math.round(300 + Math.sin(angle) * radius * 300);
        var y = Math.round(300 - Math.cos(angle) * radius * 300);
        return {r: image.red(x, y), g: image.green(x, y), b: image.blue(x, y)};
    }

    function test_whatIsPlayedIsLitAndWhatIsToComeIsDimmed() {
        var stage = make({progress: 0.5});
        var image = grabImage(stage);
        var played = pixelAt(image, 0.25, 0.93);
        var toCome = pixelAt(image, 0.75, 0.93);
        verify(played.g > 100, "a played bar is in the fallback colour at half strength, got g=" + played.g);
        compare(played.r, 0, "and in no other colour");
        verify(toCome.g > 10 && toCome.g < 70, "a bar still to come is dimmed, not gone, got g=" + toCome.g);
        verify(played.g > toCome.g * 2, "and the difference is plain to see");
    }

    // Seabass can follow a light system theme. Dimmed must then mean
    // nearer the page, not nearer black: dark bars on a light page are
    // the loudest thing on it.
    function test_onALightPageWhatIsToComeFadesTowardThePage() {
        var stage = make({progress: 0.5, backgroundColor: "white"});
        stage.color = "white";
        var image = grabImage(stage);
        var toCome = pixelAt(image, 0.75, 0.93);
        var played = pixelAt(image, 0.25, 0.93);
        verify(toCome.r > 150 && toCome.g > 170, "a bar still to come is pale on a white page, got " + JSON.stringify(toCome));
        verify(played.r < toCome.r - 30, "and a played bar is stronger in colour than it, got " + JSON.stringify(played));
    }

    // With no playhead there is no played and unplayed: all of it is lit.
    function test_withNoPlayheadTheWholeRingIsLit() {
        var stage = make({progress: -1});
        var image = grabImage(stage);
        verify(pixelAt(image, 0.25, 0.93).g > 100);
        verify(pixelAt(image, 0.75, 0.93).g > 100, "got g=" + pixelAt(image, 0.75, 0.93).g);
    }

    function test_theBandsStackFromTheRootOut() {
        var stage = make({progress: -1});
        var image = grabImage(stage);
        // high reaches 0.2 * 0.45 of the room, mid 0.5 * 0.72, low all of it.
        var high = pixelAt(image, 0.25, 0.57 + 0.43 * 0.04);
        var mid = pixelAt(image, 0.25, 0.57 + 0.43 * 0.25);
        var low = pixelAt(image, 0.25, 0.57 + 0.43 * 0.80);
        verify(high.r > 150 && high.g > 200, "the high band is near white, got " + JSON.stringify(high));
        verify(mid.g > 220 && mid.r < 40, "the mid band is the full colour, got " + JSON.stringify(mid));
        verify(low.g > 100 && low.g < 160 && low.r < 40, "the low band is the colour at half strength, got " + JSON.stringify(low));
    }

    function test_aQuietTrackDrawsShortBars() {
        var quiet = [];
        for (var i = 0; i < 400; ++i) {
            quiet.push({low: 0.2, mid: 0.1, high: 0.0});
        }
        var stage = make({progress: -1, waveformData: quiet});
        var image = grabImage(stage);
        verify(pixelAt(image, 0.25, 0.57 + 0.43 * 0.10).g > 100, "inside a bar a fifth of the full length");
        compare(pixelAt(image, 0.25, 0.57 + 0.43 * 0.40).g, 0, "and nothing beyond its tip");
    }

    function test_outsideTheRingAndBetweenItsPartsNothingIsDrawn() {
        var stage = make({progress: 0.5});
        var image = grabImage(stage);
        compare(image.pixel(4, 4), Qt.rgba(0, 0, 0, 1), "the corner is the page behind");
        // Without art the disc is the fallback colour, very dark.
        var centre = {r: image.red(300, 300), g: image.green(300, 300), b: image.blue(300, 300)};
        verify(centre.g > 30 && centre.g < 60 && centre.r === 0, "the disc with no art, got " + JSON.stringify(centre));
    }

    function test_cuesSitOnTheRingAtTheirMoment() {
        var stage = make({progress: -1, cueData: [{positionMs: 100000, color: "#ff0000"}]});
        var mark = findChild(stage.ring, "ringCueMark");
        verify(mark !== null, "a drawn ring carries its cues");
        // A quarter of the way through is three o'clock, on the base line.
        fuzzyCompare(mark.x + mark.width / 2, 300 + 300 * 0.535, 1);
        fuzzyCompare(mark.y + mark.height / 2, 300, 1);
        var image = grabImage(stage);
        verify(image.red(Math.round(300 + 300 * 0.535), 300) > 200, "and is drawn in its own colour");
    }

    function test_aClickOnTheDiscIsAClickAndItsCornersAreNot() {
        var stage = make({progress: 0.1});
        var clicks = 0;
        stage.ring.clicked.connect(function() { clicks += 1; });
        mouseClick(stage.ring, 300 + 240, 300);
        compare(clicks, 1, "on the bars");
        mouseClick(stage.ring, 300, 300);
        compare(clicks, 2, "on the cover");
        mouseClick(stage.ring, 8, 8);
        compare(clicks, 2, "the square's corner is not the ring");
    }
}
