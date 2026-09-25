// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// The breadcrumbs of the two Housekeeping pages that have no test file
// of their own (Clean Up Duplicates and Clean Up Stray Cues assert theirs
// in tst_CleanupPage.qml and tst_JunkCuePage.qml). Each is opened from
// the Housekeeping hub only, so each reads Home > stick > Housekeeping >
// page, with Housekeeping the way back.
TestCase {
    id: testCase
    name: "HousekeepingCrumbs"
    width: 900
    height: 600
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: duplicatesComponent
        DuplicatesPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    Component {
        id: pendingComponent
        PendingDeletionsPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            appSettingsController: realAppSettings
        }
    }

    Component {
        id: stackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: fillerComponent
        Item {}
    }

    function test_breadcrumb_data() {
        return [
            {tag: "duplicates", page: duplicatesComponent, title: "Match Duplicate Cues"},
            {tag: "pending", page: pendingComponent, title: "Delete Orphaned Files"},
        ];
    }

    function test_breadcrumb(data) {
        const stack = createTemporaryObject(stackComponent, testCase);
        // Home, then the Housekeeping hub, then the page.
        stack.push(fillerComponent, {}, StackView.Immediate);
        stack.push(fillerComponent, {}, StackView.Immediate);
        const page = stack.push(data.page, {}, StackView.Immediate);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, "Housekeeping");
        verify(crumb.middleIsLink);
        compare(crumb.title, data.title);
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/crumb-" + data.tag + ".png");
        }
    }
}
