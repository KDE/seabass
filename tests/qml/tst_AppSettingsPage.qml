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
        // The "Full stick backups" group is not gated any more (the feature
        // graduated on 2026-09-17); the long path must elide with the flag
        // off, the state most users are in.
        settings.experimentalFeaturesEnabled = false;
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

    // Wide enough for the cap to bite, which 700 is not: at 1600 the
    // column must stop at its maximum and sit in the middle, with the
    // scroll bar still on the window's edge rather than floating in
    // beside the text. A page that simply filled the window was where
    // the long lines came from.
    function test_a_wide_window_caps_the_column_and_centres_it() {
        const page = make(1600, 700);
        const scroll = findChild(page, "settingsScroll");
        const column = findChild(page, "settingsColumn");
        wait(50);
        compare(column.width, column.maxWidth, "the column stops at its maximum");
        // Measured against the column's own parent, not the scroll view:
        // PageScrollView insets its content by its padding and leaves a
        // gutter for the scroll bar, so the space the column is centred
        // in is narrower than the view by both.
        const holder = column.parent;
        const leftGap = column.x;
        const rightGap = holder.width - (column.x + column.width);
        verify(leftGap > 0, "and does not sit against the left edge");
        fuzzyCompare(leftGap, rightGap, 1, "with equal space either side");
        compare(scroll.contentWidth, scroll.width,
                "while the scroll view still spans the window, so its bar stays on the edge");
    }

    // The cap must not become a floor: in a window narrower than the
    // maximum the column still fills what there is.
    function test_a_narrow_window_still_fills_the_width() {
        const page = make(500, 700);
        const column = findChild(page, "settingsColumn");
        wait(50);
        verify(column.width < column.maxWidth);
        compare(column.width, column.parent.width, "no cap to apply, so it uses what it is given");
        compare(column.x, 0);
    }

    // elide is a no-op without a bounded width, so this asserts the fix
    // rather than the Label: a long path must actually be shortened to
    // fit instead of pushing the page open.
    function test_long_backup_path_elides_instead_of_widening_the_page() {
        var page = make(700, 700);
        var label = findChild(page, "backupPathLabel");
        wait(50);
        verify(label.visible);
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
        // Tall enough to hold the whole page with room to spare. The
        // number was 1400 while the page had four groups; the Music
        // section pushed it past that, and a height picked to be "surely
        // enough" quietly turns this test into a second copy of the one
        // above. So the window is sized from the content instead, and
        // the test asserts the rule rather than a number.
        var page = make(900, 1400);
        var scroll = findChild(page, "settingsScroll");
        wait(50);
        page.height = scroll.contentHeight + 200;
        wait(50);
        verify(scroll.contentHeight <= scroll.height,
               "the window was made tall enough: content " + scroll.contentHeight + " in " + scroll.height);
        verify(!scroll.interactive);
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make(700, 520);
        wait(100);
        grabImage(page).save(screenshotDir + "/AppSettingsPage.png");
        // And once wide enough for the cap to show. 900 rather than
        // something larger: this TestCase's own window is 900 across,
        // and an item wider than the window is clipped by it, so a
        // 1600-wide grab would show the left two thirds of a centred
        // column and look like a column pushed right.
        const wide = make(900, 700);
        wait(100);
        grabImage(wide).save(screenshotDir + "/AppSettingsPage-wide.png");
        // The Music section, which is the one with controls rather than
        // radio buttons: two spin boxes in a sentence and a checkbox,
        // each with a "?" beside it. Whether that row still reads as a
        // sentence at this width is not something an assertion can say.
        var scroll = findChild(page, "settingsScroll");
        var music = findChild(page, "exactMatchSpin");
        scroll.contentY = Math.min(scroll.contentHeight - scroll.height,
                                    Math.max(0, music.mapToItem(scroll.contentItem, 0, 0).y - 80));
        wait(100);
        grabImage(page).save(screenshotDir + "/AppSettingsPage-music.png");
        // And the two help popups open, since their whole job is to be
        // read and a wall of Markdown that does not wrap, or that is
        // clipped by the window, looks fine in the source either way.
        var helpButtons = [];
        function collectHelp(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                if (child.explanationTitle !== undefined) {
                    helpButtons.push(child);
                }
                collectHelp(child);
            }
        }
        collectHelp(page);
        for (var h = 0; h < helpButtons.length; ++h) {
            // clicked() rather than mouseClick(): the buttons sit inside
            // a Flickable, which takes the press for a drag, and a
            // synthetic click on one of them opened nothing at all.
            helpButtons[h].clicked();
            wait(200);
            // The whole window, not the page: a Popup is parented to
            // the window's overlay, so grabbing the page returns the
            // page with the popup neatly absent -- a screenshot that
            // proves nothing while looking like it did.
            grabImage(testCase).save(screenshotDir + "/AppSettingsPage-help-" + h + ".png");
            keyClick(Qt.Key_Escape);
            wait(100);
        }
        // The bottom too: the groups that used to be unreachable, and the
        // long backup path that used to run off the right edge, are both
        // down there and neither shows in a top-of-page shot.
        scroll.contentY = scroll.contentHeight - scroll.height;
        wait(100);
        grabImage(page).save(screenshotDir + "/AppSettingsPage-bottom.png");
    }

    // What the Updates section reads and writes on the real UpdateChecker.
    function fakeUpdateChecker(overrides) {
        const c = {
            automatic: false, includeTesting: false, testingOptionRevealed: false, runningPreRelease: false,
            state: "idle", message: "Not checked yet.", updateAvailable: false, runningWithdrawn: false,
            latestVersion: "", latestChannel: "", latestNote: "", latestNoteLevel: "",
            currentVersion: "0.8.0", currentChannel: "stable", currentCommit: "",
            downloadPage: "https://vizzzion.org/seabass/get-it.html",
            calls: [], checkNow: function() { this.calls.push("checkNow"); },
        };
        for (const key in (overrides || {})) {
            c[key] = overrides[key];
        }
        return c;
    }

    function test_the_result_is_said_where_the_check_is_switched_on() {
        const checker = fakeUpdateChecker({updateAvailable: true, state: "updateAvailable", latestVersion: "0.8.1",
                                           latestChannel: "stable",
                                           message: "Seabass 0.8.1 (stable) is available. You have 0.8.0."});
        const page = createTemporaryObject(pageComponent, testCase, {width: 900, height: 700, updateChecker: checker});
        const result = findChild(page, "updateCheckResult");
        verify(result.visible);
        compare(result.text, "Seabass 0.8.1 (stable) is available. You have 0.8.0.");
        verify(Qt.colorEqual(result.color, Theme.good), "good news in the good colour");
        const box = findChild(page, "automaticUpdateCheck");
        // The same section: the result sits under the box that enables it.
        const boxPos = box.mapToItem(page, 0, 0);
        const resultPos = result.mapToItem(page, 0, 0);
        verify(resultPos.y > boxPos.y, "the result follows the checkbox");
        findChild(page, "checkForUpdatesNow").clicked();
        // Through the page: it holds its own reference to the object.
        compare(page.updateChecker.calls.indexOf("checkNow") >= 0, true);
    }


    // The real checker, for the reason the settings controller above is
    // real: the tap sequence and what it reveals live in it, and the
    // settings it writes go to the redirected XDG_CONFIG_HOME.
    Component {
        id: checkerComponent
        UpdateChecker {}
    }

    function test_ten_quick_taps_on_the_version_line_reveal_the_testing_checkbox() {
        const checker = createTemporaryObject(checkerComponent, testCase);
        // Whatever an earlier run left in the settings: a fresh stable
        // install has never seen the option.
        checker.forgetTestingChoice();
        verify(!checker.includeTesting);
        verify(!checker.testingOptionRevealed);
        const page = createTemporaryObject(pageComponent, testCase, {width: 900, height: 700, updateChecker: checker});
        const box = findChild(page, "includeTestingUpdates");
        const dialog = findChild(page, "testingRevealedDialog");
        const version = findChild(page, "currentVersionLabel");
        verify(box !== null && dialog !== null && version !== null);
        verify(!box.visible, "nothing on the page says alphas and betas exist");
        verify(version.text.indexOf("Seabass ") === 0, "the version line reads: " + version.text);
        // The Updates section is below the fold at this height; bring the
        // line into view, or the taps land outside the window.
        const scroll = findChild(page, "settingsScroll");
        scroll.contentY = Math.max(0, Math.min(scroll.contentHeight - scroll.height,
                                               version.mapToItem(scroll.contentItem, 0, 0).y - 100));
        wait(50);
        const inWindow = version.mapToItem(testCase, 0, version.height / 2);
        verify(inWindow.y > 0 && inWindow.y < testCase.height, "the version line is on screen at y " + inWindow.y);

        for (let i = 0; i < 9; ++i) {
            mouseClick(version, 10, version.height / 2);
        }
        verify(!box.visible, "nine taps are not ten");
        verify(!dialog.visible);
        mouseClick(version, 10, version.height / 2);
        tryVerify(() => dialog.visible, 1000, "the tenth tap says what happened");
        verify(checker.includeTesting, "and switched testing on");
        verify(box.visible, "and the checkbox is there now");
        verify(box.checked);
        verify(box.enabled, "a stable (or dev) build gets to switch it off");
        dialog.close();
        tryVerify(() => !dialog.visible && !dialog.opened, 2000, "the popup goes away");

        // Off again through the box, which stays where it is. A real
        // click: toggle() from script never emits toggled.
        mouseClick(box);
        tryVerify(() => !checker.includeTesting, 1000, "unticking turns it off");
        verify(box.visible, "once found, the option stays in view");
        verify(checker.testingOptionRevealed);
        // Ticking works too, and the popup is for the discovery only.
        mouseClick(box);
        tryVerify(() => checker.includeTesting, 1000, "ticking turns it back on");
        verify(!dialog.visible);
        checker.forgetTestingChoice();
    }

    function test_the_version_line_is_not_advertised_as_a_switch() {
        const checker = createTemporaryObject(checkerComponent, testCase);
        checker.forgetTestingChoice();
        const page = createTemporaryObject(pageComponent, testCase, {width: 900, height: 700, updateChecker: checker});
        const version = findChild(page, "currentVersionLabel");
        // A plain label: no link, no button, nothing that invites a click.
        compare(version.textFormat, Text.AutoText);
        verify(version.text.indexOf("<a ") < 0);
    }

}
