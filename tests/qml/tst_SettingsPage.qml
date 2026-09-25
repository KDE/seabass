// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The Device Profile page against the settings files of a real stick
// (the anonymized fixture keeps them byte for byte), checked for the
// three things this page promises: headings that say what is under them,
// a switch wherever a setting is only off or on, and an explanation
// behind every setting. Also saves a screenshot when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "SettingsPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    // tests/qml/ -> tests/fixtures/anonymized_library/rekordbox, which
    // holds MYSETTING.DAT, MYSETTING2.DAT and DJMMYSETTING.DAT directly,
    // the way a stick's PIONEER folder does.
    // A local path, not a URL. On Windows the URL is file:///C:/..., so
    // taking off "file://" alone leaves "/C:/...", which is no path at all
    // there -- the mistake gui/local_file_url.hpp's localPathFromUrl()
    // exists to avoid -- and every test in this file found no settings.
    readonly property string fixturePioneerRoot: {
        var url = Qt.resolvedUrl("../fixtures/anonymized_library/rekordbox").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        SettingsPage {
            width: 980
            height: 780
            stickLabel: "FIXTURE"
            pioneerRoot: testCase.fixturePioneerRoot
        }
    }

    function makeLoaded(size) {
        var page = createTemporaryObject(pageComponent, testCase, size || {});
        verify(page, "page did not instantiate");
        // Loading runs on a worker thread; wait for the groups, not a delay.
        tryVerify(function () { return findAll(page, "settingsGroup").length > 0; }, 5000,
                  "the fixture's settings files never produced a group");
        waitForRendering(page);
        return page;
    }

    // Every descendant with this objectName. findChild stops at the first.
    function findAll(item, name) {
        var out = [];
        function walk(node) {
            if (!node || !node.children) {
                return;
            }
            for (var i = 0; i < node.children.length; ++i) {
                var child = node.children[i];
                if (child.objectName === name) {
                    out.push(child);
                }
                walk(child);
            }
        }
        walk(item);
        return out;
    }

    function test_headingsNameWhatIsUnderThemNotTheFile() {
        var page = makeLoaded();
        var headings = findAll(page, "groupHeading").map(function (h) { return h.text; });
        verify(headings.length >= 4, "expected player and mixer groups, got " + JSON.stringify(headings));
        for (var i = 0; i < headings.length; ++i) {
            verify(headings[i].indexOf(".DAT") < 0, "a heading still names a file: " + headings[i]);
            verify(headings[i].indexOf("(2)") < 0, "a heading still numbers a file: " + headings[i]);
        }
        verify(headings.indexOf("Player: DJ setting") >= 0, "the player's DJ settings must have a heading");
        verify(headings.indexOf("Mixer: faders") >= 0, "the mixer's faders must have a heading");
    }

    function test_offOnSettingsAreSwitchesAndTheRestStayDropDowns() {
        var page = makeLoaded();
        var rows = findAll(page, "settingRow");
        verify(rows.length > 20, "expected every decoded setting as a row, got " + rows.length);
        var switches = 0;
        var combos = 0;
        for (var i = 0; i < rows.length; ++i) {
            var row = rows[i];
            var options = row.modelData.options;
            var isOffOn = options.length === 2 && options[0] === "off" && options[1] === "on";
            var sw = findChild(row, "settingSwitch");
            var combo = findChild(row, "settingCombo");
            compare(sw.visible, isOffOn, row.modelData.label + ": a switch exactly when the values are off/on");
            compare(combo.visible, !isOffOn, row.modelData.label + ": a drop-down for everything else");
            if (isOffOn) {
                compare(sw.checked, row.shownValue === "on", row.modelData.label + ": the switch shows the stick");
                switches++;
            } else {
                combos++;
            }
        }
        verify(switches > 0 && combos > 0, "both kinds must be on the page");
    }

    function test_everySettingExplainsItself() {
        var page = makeLoaded();
        var rows = findAll(page, "settingRow");
        for (var i = 0; i < rows.length; ++i) {
            var info = findChild(rows[i], "settingInfoButton");
            verify(info, rows[i].modelData.label + " has no (i)");
            verify(info.summaryText.length > 0, rows[i].modelData.label + "'s (i) is empty");
            verify(info.explanationText.indexOf(rows[i].modelData.fileName) >= 0,
                   rows[i].modelData.label + "'s (i) must say which file it is stored in");
        }
    }

    function test_everyControlStartsOnOneLine() {
        // One left line for the controls, whatever the length of the name
        // beside them: the label column is a fixed width, not the label's.
        var page = makeLoaded();
        var rows = findAll(page, "settingRow");
        var firstX = -1;
        var firstInfoX = -1;
        for (var i = 0; i < rows.length; ++i) {
            var control = rows[i].modelData.isSwitch ? findChild(rows[i], "settingSwitch")
                                                     : findChild(rows[i], "settingCombo");
            var x = control.mapToItem(page, 0, 0).x;
            // And the (i) after it. A switch is far narrower than a
            // drop-down, and when each control took its own width the (i)
            // of every off/on setting sat a couple of hundred pixels left
            // of its neighbours' -- in the first screenshot, not in any
            // assertion, because only the controls were checked.
            var infoX = findChild(rows[i], "settingInfoButton").mapToItem(page, 0, 0).x;
            if (firstX < 0) {
                firstX = x;
                firstInfoX = infoX;
            }
            compare(x, firstX, rows[i].modelData.label + "'s control is off the line");
            compare(infoX, firstInfoX, rows[i].modelData.label + "'s (i) is off the line");
        }
    }

    // Centred the way Preferences is: the settings column stops at a
    // maximum width and sits in the middle of a wide window, with the
    // scroll view still spanning the window so its bar stays on the
    // edge. It used to fill the window, so on a wide one every control
    // sat far left under the breadcrumb with the rest of the page empty,
    // while Preferences, one click away, stood in the middle. Same
    // measurement as tst_AppSettingsPage's, against the same idiom.
    function test_aWideWindowCapsTheColumnAndCentresItLikePreferences() {
        const page = makeLoaded({width: 1600, height: 780});
        const scroll = findChild(page, "settingsScroll");
        const column = findChild(page, "settingsColumn");
        verify(column.maxWidth > 0, "the column carries a maximum width");
        compare(column.width, column.maxWidth, "the column stops at its maximum");
        const holder = column.parent;
        const leftGap = column.x;
        const rightGap = holder.width - (column.x + column.width);
        verify(leftGap > 0, "and does not sit against the left edge");
        fuzzyCompare(leftGap, rightGap, 1, "with equal space either side");
        compare(scroll.contentWidth, scroll.width,
                "while the scroll view still spans the window, so its bar stays on the edge");
        // Every row inside the column, not only the column itself: a
        // row that kept the old full width would reach past it.
        const rows = findAll(page, "settingRow");
        verify(rows.length > 0);
        for (let i = 0; i < rows.length; ++i) {
            const r = rows[i].mapToItem(column, 0, 0);
            verify(r.x >= 0 && r.x + rows[i].width <= column.width + 0.5,
                   rows[i].modelData.label + " is outside the centred column");
        }
    }

    // The cap is not a floor: narrower than the maximum, the column fills
    // what there is and starts on the page's left line.
    function test_aNarrowWindowStillFillsTheWidth() {
        const page = makeLoaded({width: 520, height: 780});
        const column = findChild(page, "settingsColumn");
        verify(column.width < column.maxWidth);
        compare(column.width, column.parent.width, "no cap to apply, so it uses what it is given");
        compare(column.x, 0);
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = makeLoaded();
        grabImage(page).save(screenshotDir + "/SettingsPage.png");
        // PageScrollView is a Flickable, so it scrolls itself.
        var scroll = findChild(page, "settingsScroll");
        verify(scroll, "the page's scroll view must exist");
        verify(scroll.contentHeight > scroll.height, "every setting on one screen: nothing to scroll to");
        scroll.contentY = scroll.contentHeight - scroll.height;
        waitForRendering(page);
        grabImage(page).save(screenshotDir + "/SettingsPage-bottom.png");
        // Wide, where the centring shows: at 900, the width
        // tst_AppSettingsPage grabs Preferences at, so the two can be laid
        // side by side, and at 1600 with the window widened to match (an
        // item wider than the window is clipped by it).
        const wasWidth = testCase.width;
        const widths = [900, 1600];
        for (let i = 0; i < widths.length; ++i) {
            testCase.width = Math.max(wasWidth, widths[i]);
            const wide = makeLoaded({width: widths[i], height: 780});
            grabImage(wide).save(screenshotDir + "/SettingsPage-" + widths[i] + ".png");
        }
        testCase.width = wasWidth;
    }
}
