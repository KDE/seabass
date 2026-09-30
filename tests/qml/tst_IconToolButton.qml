// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale
import "../qml-live/LiveHelpers.js" as Live

// An icon-only tool button's glyph is drawn in Theme.text under every
// style. Under KDE's desktop style a raw ToolButton's icon is painted by
// QStyle, which ignores icon.color: on Kelp the header's Preferences and
// the stick header's eject came out a dim grey, almost impossible to read,
// while Basic (this suite's other lane) drew them in full ink. So this
// matters in seabass_qml_desktop_style_tests, and runs in both.
//
// Measured on the picture: the button's ink is its brightest pixel (the
// ground is Kelp's near-black), and its luminance has to be within 15%
// of Theme.text's. The faint icon this guards against measured well
// under half.
TestCase {
    id: testCase
    name: "IconToolButton"
    width: 1000
    height: 400
    visible: true
    when: windowShown

    Component {
        id: headerComponent
        // Grabbed from the ground at 0,0 of the window, not from the
        // buttons: a grab of an item inside another came back offset (see
        // tst_PlayerBar's rimPixel).
        Rectangle {
            property alias buttons: buttons
            width: buttons.implicitWidth + 2 * Theme.pageMargin
            height: buttons.implicitHeight + 2 * Theme.pageMargin
            color: Theme.background
            AppHeaderButtons {
                id: buttons
                x: Theme.pageMargin
                y: Theme.pageMargin
            }
        }
    }
    Component {
        id: stickComponent
        Rectangle {
            property alias row: row
            width: 860
            height: row.implicitHeight + 2 * Theme.pageMargin
            color: Theme.background
            StickHeaderRow {
                id: row
                x: Theme.pageMargin
                y: Theme.pageMargin
                width: 820
                row: ({label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1", mounted: true,
                       hasRekordbox: true, hasEngine: true, rekordboxPath: "/media/MAIN/PIONEER",
                       enginePath: "/media/MAIN/Engine Library", isSdCard: false, isFolder: false,
                       isBrowsedBackup: false, libraryId: "lib-main", safeToUnplug: false,
                       hasOneLibrary: false, readOnly: false})
                mediaController: ({busy: false, busyDevicePath: "",
                                   mountStick: function() {}, unmountStick: function() {}})
                playbackController: ({stop: function() {}})
            }
        }
    }

    function channel(c) {
        return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
    }

    function luminance(color) {
        return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b);
    }

    // The brightest pixel's luminance inside `button`, read off a grab of
    // `ground`, which holds it.
    function inkLuminance(ground, button) {
        const image = grabImage(ground);
        const origin = button.mapToItem(ground, 0, 0);
        const step = 1 / PixelScale.scale(image, ground);
        let ink = 0;
        for (let y = 0; y < button.height; y += step) {
            for (let x = 0; x < button.width; x += step) {
                ink = Math.max(ink, luminance(PixelScale.pixel(image, ground, origin.x + x, origin.y + y)));
            }
        }
        return ink;
    }

    function saveScreenshot(ground, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(ground).save(screenshotDir + "/" + name + ".png");
        }
    }

    function checkInk(ground, button, what) {
        verify(button !== null, what + " exists");
        verify(button.visible && button.width > 0, what + " is shown");
        const expected = luminance(Theme.text);
        const ink = inkLuminance(ground, button);
        console.log(what + ": ink luminance " + ink.toFixed(3) + ", Theme.text " + expected.toFixed(3));
        verify(Math.abs(ink - expected) <= 0.15 * expected,
               what + "'s glyph is drawn in Theme.text: luminance " + ink.toFixed(3)
               + " against " + expected.toFixed(3));
    }

    function test_theHeadersPreferencesGlyphIsInk() {
        const ground = createTemporaryObject(headerComponent, testCase);
        verify(ground !== null);
        waitForRendering(ground);
        saveScreenshot(ground, "app-header-buttons");
        checkInk(ground, Live.findByObjectName(ground.buttons, "preferencesButton"), "Preferences");
        checkInk(ground, Live.findByObjectName(ground.buttons, "aboutButton"), "About");
    }

    function test_theStickHeadersEjectGlyphIsInk() {
        const ground = createTemporaryObject(stickComponent, testCase);
        verify(ground !== null);
        waitForRendering(ground);
        checkInk(ground, Live.findByObjectName(ground.row, "ejectButton"), "Eject");
    }
}
