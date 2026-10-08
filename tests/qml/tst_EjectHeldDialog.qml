// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// An eject a program refused: the dialog names the program and says what
// to do -- for rekordbox and Engine DJ, quit it or eject the stick from
// within it -- and offers Try Again and Cancel. The real MediaController,
// told an eject was held through its test seam.
TestCase {
    id: testCase
    name: "EjectHeldDialog"
    width: 800
    height: 600
    visible: true
    when: windowShown

    Component { id: controllerComponent; MediaController {} }
    Component { id: dialogComponent; EjectHeldDialog {} }

    function footerButton(dialog, name) {
        var kids = dialog.footer ? dialog.footer.contentChildren : [];
        for (var i = 0; i < kids.length; ++i) {
            if (kids[i].objectName === name) return kids[i];
        }
        return null;
    }

    function test_rekordboxHoldingTheStick() {
        var media = createTemporaryObject(controllerComponent, testCase);
        var dialog = createTemporaryObject(dialogComponent, testCase, {mediaController: media});
        compare(dialog.opened, false);
        media.noteEjectHeldForTesting("/dev/disk5s1", "WHALESHARK", "rekordbox", true);
        tryCompare(dialog, "opened", true);
        compare(dialog.title, "rekordbox is using WHALESHARK");
        compare(dialog.headline, "Quit rekordbox, or eject WHALESHARK from within rekordbox, then try again.");
        compare(media.errorMessage, "", "a dialog, not the red line");
        verify(footerButton(dialog, "ejectRetryButton") !== null);
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(testCase);
            grabImage(testCase).save(screenshotDir + "/eject-held-rekordbox.png");
        }
        footerButton(dialog, "ejectCancelButton").clicked();
        tryCompare(dialog, "opened", false);
        compare(media.ejectHeldBy, "", "Cancel lets it be");
    }

    function test_anotherProgramAndTryAgain() {
        var media = createTemporaryObject(controllerComponent, testCase);
        var dialog = createTemporaryObject(dialogComponent, testCase, {mediaController: media});
        media.noteEjectHeldForTesting("/dev/disk5s1", "WHALESHARK", "Preview", false);
        tryCompare(dialog, "opened", true);
        compare(dialog.title, "Preview is using WHALESHARK");
        compare(dialog.headline, "Close Preview, then try again.");
        footerButton(dialog, "ejectRetryButton").clicked();
        tryCompare(dialog, "opened", false);
        compare(media.ejectHeldBy, "", "asked again, the held state starts over");
    }
}
