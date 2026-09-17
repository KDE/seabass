// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Clean Up Stray Cues: the floating save button says what this page's
// save does.
//
// The page stages one kind of change and nothing else, so "Save" told
// the user less than it could -- the button is the moment a stack of
// stray cues stops being a list and gets written to the stick.
//
// Paths point nowhere on purpose, the same as tst_PagesCompile: the page
// must build without a stick, and the label is not a property of one.
TestCase {
    id: testCase
    name: "JunkCuePage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        JunkCuePage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            appSettingsController: realAppSettings
        }
    }

    function test_theSaveButtonSaysCleanUp() {
        var page = createTemporaryObject(pageComponent, testCase);
        verify(page !== null, "the page must instantiate");
        var overlay = findChild(page, "saveOverlay");
        verify(overlay, "the standard save overlay must exist");
        compare(overlay.label, "Clean Up", "the save button says what this page's save does");
    }
}
