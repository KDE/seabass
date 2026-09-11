// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The Preferences page, headless. Both things asserted here were real
// bugs (see the "Preferences page has no scrollbar" issue): the page had
// no scroll container at all, and its content column was anchored top and
// left only, so it sized to its own content instead of to the window.
//
// That second one is the subtle half. An unbounded column silently
// disables the two things on this page written to stay inside it -- the
// backup-path Label's elide and the description Label's wrap -- so the
// page ran off the right edge exactly when the user's own backup path was
// long. Neither symptom is visible at a comfortable window size with a
// short path, which is why they are pinned here rather than left to the
// eye. Also saves a screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "AppSettingsPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    // The real controller, not a stand-in, for the reason tst_PagesCompile
    // gives: a fake would have to guess at its properties. Safe to write
    // to because the test run redirects XDG_CONFIG_HOME away from the
    // developer's own settings (see CMakeLists.txt).
    AppSettingsController { id: settings }

    Component {
        id: pageComponent
        AppSettingsPage { appSettingsController: settings }
    }

    // Long enough that an unbounded column would be dragged well past any
    // sane window width. This is the input that actually triggered the
    // overflow: stickBackupDirectory is arbitrary-length user data.
    readonly property string longPath:
        "/home/somebody/Music/DJ/Backups/Seabass/full-stick-archives/2026/september/rehearsal-sets/RV2"

    function make(w, h) {
        return createTemporaryObject(pageComponent, testCase, {width: w, height: h});
    }

    function init() {
        settings.experimentalFeaturesEnabled = true;  // reveals the "Full stick backups" group
        settings.stickBackupDirectory = longPath;
    }

    // The page must never be wider than the window it is in. Before the
    // fix the column took its width from its widest child, so this grew
    // without bound and there was no horizontal scroll to get it back.
    function test_content_never_exceeds_the_window_width() {
        var page = make(700, 700);
        var scroll = findChild(page, "settingsScroll");
        var column = findChild(page, "settingsColumn");
        wait(50);
        verify(column.width > 0);
        verify(column.width <= scroll.width);
        compare(scroll.contentWidth, scroll.width);
    }

    // elide is a no-op without a bounded width, so this asserts the fix
    // rather than the Label: a long path must actually be shortened to
    // fit instead of pushing the page open.
    function test_long_backup_path_elides_instead_of_widening_the_page() {
        var page = make(700, 700);
        var label = findChild(page, "backupPathLabel");
        wait(50);
        verify(label.truncated);
        verify(label.width <= findChild(page, "settingsColumn").width);
    }

    // A window too short for the page must be scrollable, and must report
    // itself as such -- PageScrollView is deliberately only interactive
    // when its content actually overflows.
    function test_short_window_can_reach_the_bottom() {
        var page = make(900, 260);
        var scroll = findChild(page, "settingsScroll");
        wait(50);
        verify(scroll.contentHeight > scroll.height);
        verify(scroll.interactive);

        scroll.contentY = scroll.contentHeight - scroll.height;
        wait(50);
        compare(scroll.contentY, scroll.contentHeight - scroll.height);
    }

    // The other half of that rule: a window with room to spare must not
    // be grabbable, or the whole page slides around under the mouse with
    // nowhere to go.
    function test_tall_window_is_not_flickable() {
        var page = make(900, 1400);
        var scroll = findChild(page, "settingsScroll");
        wait(50);
        verify(!scroll.interactive);
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make(700, 520);
        wait(100);
        grabImage(page).save(screenshotDir + "/AppSettingsPage.png");
        // The bottom too: the groups that used to be unreachable, and the
        // long backup path that used to run off the right edge, are both
        // down there and neither shows in a top-of-page shot.
        var scroll = findChild(page, "settingsScroll");
        scroll.contentY = scroll.contentHeight - scroll.height;
        wait(100);
        grabImage(page).save(screenshotDir + "/AppSettingsPage-bottom.png");
    }
}
