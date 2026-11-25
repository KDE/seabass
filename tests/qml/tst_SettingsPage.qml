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

    // The fixture as a whole stick (PIONEER and Engine Library), for the
    // tests that write to it or take it away: a copy, never the fixture.
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component { id: mediaComponent; MediaController {} }

    // A page on a copy of the fixture stick, loaded, with one setting
    // staged to its other value. Returns what the tests need of it.
    function stagedOnACopy() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const pioneer = stick + "/PIONEER";
        const page = createTemporaryObject(pageComponent, testCase, {pioneerRoot: pioneer});
        verify(page, "page did not instantiate");
        const controller = findChild(page, "settingsController");
        tryVerify(() => controller.groups.length > 0 && !controller.busy, 5000, "the copy's settings load");
        let target = null;
        for (let g = 0; g < controller.groups.length && target === null; ++g) {
            const fields = controller.groups[g].fields;
            for (let f = 0; f < fields.length; ++f) {
                const at = fields[f].options.indexOf(fields[f].value);
                if (fields[f].isSwitch && at >= 0) {
                    target = {fileName: fields[f].fileName, label: fields[f].label, value: fields[f].value,
                              other: fields[f].options[(at + 1) % fields[f].options.length]};
                    break;
                }
            }
        }
        verify(target !== null, "precondition: an off/on setting to change");
        controller.setField(target.fileName, target.label, target.other);
        compare(controller.pendingCount, 1, "the change is staged");
        const libraryId = EditSessionRegistry.libraryIdForPath(pioneer);
        verify(libraryId.length > 0, "the copy has a library id");
        const session = EditSessionRegistry.sessionFor(libraryId);
        verify(session !== null, "staging opened the session");
        tryCompare(session, "pendingCount", 1, 5000);
        return {stick: stick, pioneer: pioneer, page: page, controller: controller, target: target,
                libraryId: libraryId, session: session};
    }

    // The stick pulled (its PIONEER folder moved aside), `whileGone` run on
    // the folder, and the stick back at `mountPoint` (its old root when
    // empty) with an identity match of `strength`.
    function pullAndReturn(media, env, strength, mountPoint, whileGone) {
        const aside = stickFixture.scratchDirectory() + "/PIONEER";
        verify(stickFixture.moveFile(env.pioneer, aside), "the stick goes");
        media.stickRemoved(env.libraryId, "FIXTURE");
        compare(env.session.stickPresent, false);
        if (whileGone) {
            whileGone(aside);
        }
        const root = mountPoint.length > 0 ? mountPoint : env.stick;
        verify(stickFixture.moveFile(aside, root + "/PIONEER"), "the stick comes back");
        media.stickReturned(env.libraryId, strength, root);
        compare(env.session.stickPresent, true);
        tryCompare(env.controller, "busy", false, 5000);
        return root + "/PIONEER";
    }

    function savedIn(session) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 60000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // #114: back at another mount point (/media/x/A3 then /media/x/A31),
    // it is a new library there, not this page's. The page does not read
    // its old path again (whatever is mounted there now is not this
    // stick), says what to do, and what is staged cannot be saved: the
    // session's stick is not at its root.
    function test_theStickBackAtAnotherMountPointIsNotFollowed() {
        const media = createTemporaryObject(mediaComponent, testCase);
        EditSessionRegistry.mediaController = media;
        const env = stagedOnACopy();
        try {
            const elsewhere = stickFixture.scratchDirectory();
            pullAndReturn(media, env, "hardware", elsewhere, null);
            EditSessionRegistry.acknowledgeStickReturned();
            wait(200);
            compare(env.controller.busy, false, "not read again");
            const status = findChild(env.page, "statusLabel");
            verify(status.visible && status.text.indexOf("at another place") >= 0, "and it says why: " + status.text);
            compare(env.controller.pendingCount, 1, "what is staged stays staged");
            const summary = savedIn(env.session);
            verify(summary.error.indexOf("at another place") >= 0, "and cannot be saved: " + summary.error);
            compare(summary.written, 0);
            verify(!stickFixture.exists(elsewhere + "/Seabass"), "nothing written where the stick is now");
        } finally {
            EditSessionRegistry.acknowledgeStickReturned();
            env.session.discard();
            EditSessionRegistry.mediaController = null;
        }
    }

    // #114: what is staged is squared with the stick that came back. A
    // setting it already holds is no longer staged ("was X" against X
    // says nothing), and one it no longer has is said, not hidden.
    function test_theStickBackSquaresWhatIsStaged() {
        const media = createTemporaryObject(mediaComponent, testCase);
        EditSessionRegistry.mediaController = media;
        const env = stagedOnACopy();
        try {
            // The stick as it will come back, made by a save: the setting
            // holds the staged value. Then the original under it again, and
            // the same change staged once more.
            compare(savedIn(env.session).written, 1, "precondition: saved");
            const holdsIt = stickFixture.scratchDirectory() + "/" + env.target.fileName;
            verify(stickFixture.copyFile(env.pioneer + "/" + env.target.fileName, holdsIt));
            verify(stickFixture.copyFile(testCase.fixturePioneerRoot + "/" + env.target.fileName,
                                         env.pioneer + "/" + env.target.fileName));
            tryCompare(env.controller, "busy", false, 5000);
            env.controller.load(env.pioneer);
            tryCompare(env.controller, "busy", false, 5000);
            env.controller.setField(env.target.fileName, env.target.label, env.target.other);
            compare(env.controller.pendingCount, 1, "precondition: staged again");

            pullAndReturn(media, env, "hardware", "", (aside) => {
                verify(stickFixture.copyFile(holdsIt, aside + "/" + env.target.fileName));
            });
            EditSessionRegistry.acknowledgeStickReturned();
            tryVerify(() => env.controller.pendingCount === 0, 5000, "a value the stick holds is not staged");
            compare(env.session.pendingCount, 0, "nor in the session");
            const field = fieldIn(env.controller, env.target.fileName, env.target.label);
            compare(field.unsaved, false);
            compare(field.value, env.target.other);
            const status = findChild(env.page, "statusLabel");
            verify(status.visible && status.text.indexOf("Already set that way") >= 0
                   && status.text.indexOf(env.target.label) >= 0, "and it is said: " + status.text);

            // Staged again, and this time its file is gone while the stick is out.
            const other = env.target.value;
            env.controller.setField(env.target.fileName, env.target.label, other);
            compare(env.controller.pendingCount, 1);
            pullAndReturn(media, env, "hardware", "", (aside) => {
                verify(stickFixture.moveFile(aside + "/" + env.target.fileName,
                                             stickFixture.scratchDirectory() + "/" + env.target.fileName));
            });
            EditSessionRegistry.acknowledgeStickReturned();
            tryVerify(() => env.controller.pendingCount === 0, 5000, "a setting the stick lacks is not staged");
            compare(env.session.pendingCount, 0);
            compare(fieldIn(env.controller, env.target.fileName, env.target.label), null, "no row for it");
            verify(status.visible && status.text.indexOf("Not on the stick any more") >= 0
                   && status.text.indexOf(env.target.label) >= 0, "and it is said: " + status.text);
        } finally {
            EditSessionRegistry.acknowledgeStickReturned();
            env.session.discard();
            EditSessionRegistry.mediaController = null;
        }
    }

    // #114: matched by label and size only, the stick may be another one.
    // The page stays as it was, staged change and all, and says why.
    function test_aStickMatchedByLabelOnlyIsNotReadAgain() {
        const media = createTemporaryObject(mediaComponent, testCase);
        EditSessionRegistry.mediaController = media;
        const env = stagedOnACopy();
        try {
            // What a read would see: the staged setting's file is gone.
            pullAndReturn(media, env, "weak", "", (aside) => {
                verify(stickFixture.moveFile(aside + "/" + env.target.fileName,
                                             stickFixture.scratchDirectory() + "/" + env.target.fileName));
            });
            wait(200);
            compare(env.controller.busy, false, "not read again");
            verify(fieldIn(env.controller, env.target.fileName, env.target.label) !== null,
                   "the page still shows what it read before");
            compare(env.controller.pendingCount, 1, "the staged change is kept");
            compare(env.session.pendingCount, 1);
            const status = findChild(env.page, "statusLabel");
            verify(status.visible && status.text.indexOf("cannot tell it is the same one") >= 0,
                   "and it says why: " + status.text);
        } finally {
            EditSessionRegistry.acknowledgeStickReturned();
            env.session.discard();
            EditSessionRegistry.mediaController = null;
        }
    }

    function fieldIn(controller, fileName, label) {
        for (let g = 0; g < controller.groups.length; ++g) {
            const fields = controller.groups[g].fields;
            for (let f = 0; f < fields.length; ++f) {
                if (fields[f].fileName === fileName && fields[f].label === label) {
                    return fields[f];
                }
            }
        }
        return null;
    }

    // #114: the stick pulled mid-save and the page read it while it was
    // gone (the save's own re-read), so it said "No recognized settings
    // files found on this stick." The same stick back, matched by its
    // serial, must show its settings again without leaving the page, and
    // a change still staged keeps its UNSAVED row against what the stick
    // really holds.
    function test_theSameStickBackShowsItsSettingsAgain() {
        const media = createTemporaryObject(mediaComponent, testCase);
        EditSessionRegistry.mediaController = media;
        const env = stagedOnACopy();
        try {
            // Pulled: the folder is gone and the watcher says so.
            verify(stickFixture.moveFile(env.pioneer, env.stick + "/PIONEER.pulled"), "the stick goes");
            media.stickRemoved(env.libraryId, "FIXTURE");
            compare(env.session.stickPresent, false, "the session knows the stick is gone");
            // Read while gone, as the save's re-read was.
            env.controller.load(env.pioneer);
            tryCompare(env.controller, "busy", false, 5000);
            compare(env.controller.groups.length, 0, "precondition: nothing to read while it is gone");
            verify(findChild(env.page, "errorLabel").visible, "precondition: the page says so");

            // Plugged back in: the same stick, by its serial.
            verify(stickFixture.moveFile(env.stick + "/PIONEER.pulled", env.pioneer), "the stick comes back");
            media.stickReturned(env.libraryId, "hardware", env.stick);
            compare(env.session.stickPresent, true);
            tryVerify(() => env.controller.groups.length > 0 && !env.controller.busy, 5000,
                      "the same stick back must show its settings again");
            compare(findChild(env.page, "errorLabel").visible, false, "and the empty-stick message goes");
            const field = fieldIn(env.controller, env.target.fileName, env.target.label);
            verify(field !== null, env.target.label + " is shown again");
            compare(field.unsaved, true, "the staged change is kept");
            compare(field.pendingValue, env.target.other);
            compare(field.value, env.target.value, "\"was\" is what the stick holds");
        } finally {
            EditSessionRegistry.acknowledgeStickReturned();
            env.session.discard();
            EditSessionRegistry.mediaController = null;
        }
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
