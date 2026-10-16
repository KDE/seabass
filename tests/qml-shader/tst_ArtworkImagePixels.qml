// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../qml/PixelScale.js" as PixelScale

// Cover art's rounded corners, read back pixel by pixel: the corner is
// the page behind, the middle is the cover, at a small size (half the
// radius) and a large one; and where the shader cannot load, the plain
// square image is drawn, never nothing.
TestCase {
    id: testCase
    name: "ArtworkImagePixels"
    width: 400
    height: 400
    visible: true
    when: windowShown

    readonly property color behind: "#00ff00"

    Component {
        id: stageComponent
        Rectangle {
            width: 200
            height: 200
            color: testCase.behind
            property alias art: artItem
            ArtworkImage {
                id: artItem
                x: 20
                y: 20
                asynchronous: false
                source: browseFixture.presentArtworkUrl()
            }
        }
    }

    function make(side, properties) {
        const stage = createTemporaryObject(stageComponent, testCase);
        verify(stage !== null);
        stage.art.width = side;
        stage.art.height = side;
        for (const key in properties) {
            stage.art[key] = properties[key];
        }
        tryCompare(stage.art, "drawn", true, 5000);
        tryCompare(stage.art, "reveal", 1, 2000);
        waitForRendering(stage);
        return stage;
    }

    // The fixture's cover is a flat (200, 40, 40).
    function isCover(c) {
        return Math.abs(c.r * 255 - 200) <= 6 && Math.abs(c.g * 255 - 40) <= 6 && Math.abs(c.b * 255 - 40) <= 6;
    }
    function isBehind(c) {
        return c.r * 255 <= 6 && Math.abs(c.g * 255 - 255) <= 6 && c.b * 255 <= 6;
    }

    function test_theCornersAreRounded_data() {
        return [{tag: "small, half the radius", side: 40}, {tag: "large", side: 120}];
    }
    function test_theCornersAreRounded(data) {
        const stage = make(data.side, {});
        const art = stage.art;
        verify(art.rounded, "this suite exists to run the shader: no GPU scene graph, or no shader in the binary");
        compare(art.radius, Theme.coverRadius(data.side));
        verify(art.radius > 0);
        compare(findChild(art, "roundedArtwork").visible, true, "drawn by the shader");
        compare(findChild(art, "artworkImage").visible, false, "not also as a square under it");
        const image = grabImage(stage);
        const px = (x, y) => PixelScale.pixel(image, stage, art.x + x, art.y + y);
        verify(isCover(px(data.side / 2, data.side / 2)), "the middle is the cover: " + px(data.side / 2, data.side / 2));
        verify(isCover(px(art.radius + 1, 0.5)), "the top edge beside the corner too: " + px(art.radius + 1, 0.5));
        // The very corner pixel: outside the rounding, the page shows. On
        // the small cover the half radius leaves the corner pixel a soft
        // blend, mostly the page.
        const corner = px(0.25, 0.25);
        if (art.radius >= 4) {
            verify(isBehind(corner), "the corner is the page behind: " + corner);
        } else {
            verify(corner.g > 0.5 && corner.r < 0.5, "the corner is mostly the page behind: " + corner);
        }
        const far = px(data.side - 0.25, data.side - 0.25);
        verify(far.g > 0.5 && far.r < 0.5, "and so is the opposite one: " + far);
        saveScreenshot(stage, "artwork-rounded-" + data.side);
    }

    // No shader to load: the plain image, square, corner and all.
    function test_withoutTheShaderTheCoverIsSquare() {
        const stage = make(120, {roundingShader: "qrc:/seabass/shaders/not-there.frag.qsb"});
        const art = stage.art;
        tryCompare(art, "rounded", false, 2000, "the shader did not load");
        compare(findChild(art, "artworkImage").visible, true, "the plain image is drawn");
        waitForRendering(stage);
        const image = grabImage(stage);
        const corner = PixelScale.pixel(image, stage, art.x + 0.25, art.y + 0.25);
        verify(isCover(corner), "square: the corner is the cover: " + corner);
        verify(isCover(PixelScale.pixel(image, stage, art.x + 60, art.y + 60)));
    }

    // Radius 0 is asked for square, and drawn square.
    function test_radiusZeroIsSquare() {
        const stage = make(120, {radius: 0});
        const image = grabImage(stage);
        verify(isCover(PixelScale.pixel(image, stage, stage.art.x + 0.25, stage.art.y + 0.25)));
    }

    function saveScreenshot(item, name) {
        if (typeof screenshotDir !== "undefined" && screenshotDir && screenshotDir.length > 0) {
            grabImage(item).save(screenshotDir + "/" + name + ".png");
        }
    }
}
