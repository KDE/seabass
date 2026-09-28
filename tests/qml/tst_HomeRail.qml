// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "../../src/gui/qml/common/HomeModel.js" as HomeModel

// The home screen's rail on its own: what it shows as selected in each
// section, both shapes of stick model, the column and the compact form,
// the keyboard, and sticks arriving and leaving. Screenshots of both
// forms when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "HomeRail"
    width: 900
    height: 600
    visible: true
    when: windowShown

    // The rail on the page's ground, inset by the page margin, so the
    // screenshots show it the way the home page will.
    Component {
        id: frameComponent
        Rectangle {
            id: frame
            property alias rail: rail
            property alias sticks: rail.sticks
            width: 360
            height: 520
            color: Theme.background
            HomeRail {
                id: rail
                x: Theme.pageMargin
                y: Theme.pageMargin
                width: frame.width - 2 * Theme.pageMargin
                sticks: []
            }
        }
    }

    Component {
        id: listModelComponent
        ListModel {}
    }

    Component {
        id: spyComponent
        SignalSpy {}
    }

    function makeStick(overrides) {
        const s = {label: "MAIN", mountPoint: "/media/MAIN", devicePath: "/dev/sdb1",
                   isSdCard: false, isFolder: false, isBrowsedBackup: false};
        for (const key in overrides) {
            s[key] = overrides[key];
        }
        return s;
    }

    function threeSticks() {
        return [makeStick({}),
                makeStick({label: "CARD", mountPoint: "/media/CARD", devicePath: "/dev/mmcblk0p1", isSdCard: true}),
                makeStick({label: "UNMOUNTED", mountPoint: "", devicePath: "/dev/sdc1"})];
    }

    function makeFrame(sticks, props) {
        // The sticks at creation, as the page hands them over: set
        // afterwards they would all be arriving sticks.
        const frame = createTemporaryObject(frameComponent, testCase, {sticks: sticks});
        verify(frame !== null);
        for (const key in (props || {})) {
            frame.rail[key] = props[key];
        }
        waitForRendering(frame);
        return frame;
    }

    function saveScreenshot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) {
            return;
        }
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    function text(entry) {
        return findChild(entry, "railText");
    }

    // At 8 bits a channel, which is what the screen gets: Theme's mixed
    // colours carry more precision than that, and two routes to the same
    // colour can differ below it.
    function sameColor(a, b) {
        return Qt.colorEqual(String(a), String(b));
    }

    // The selected stick is the one row drawn on the group background in
    // full-strength DemiBold text; every other row is muted and plain.
    function test_selectedStickIsDrawnAsSelected() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/CARD"});
        const rail = frame.rail;
        const main = findChild(rail, "railStick:/media/MAIN");
        const card = findChild(rail, "railStick:/media/CARD");
        const unmounted = findChild(rail, "railStick:/dev/sdc1");
        verify(main !== null && card !== null, "every row is named by its key");
        verify(unmounted !== null, "an unmounted stick is named by its device");

        compare(card.selected, true);
        verify(sameColor(findChild(card, "railPill").color, Theme.groupBackground));
        verify(sameColor(text(card).color, Theme.text));
        compare(text(card).font.weight, Font.DemiBold);

        for (const other of [main, unmounted]) {
            compare(other.selected, false);
            verify(sameColor(text(other).color, Theme.textMuted));
            compare(text(other).font.weight, Font.Normal);
            verify(sameColor(findChild(other, "railPill").color, "transparent"));
        }
        // The selected stick is marked the way the selected tool is: the
        // accent bar and an accent icon, not the pill alone.
        compare(findChild(card, "railAccentBar").visible, true);
        verify(sameColor(findChild(card, "railIcon").color, Theme.accent), "the selected stick's icon is accent");
        for (const other of [main, unmounted]) {
            compare(findChild(other, "railAccentBar").visible, false);
            verify(sameColor(findChild(other, "railIcon").color, Theme.textMuted));
        }

        rail.selectedStickKey = "/dev/sdc1";
        compare(card.selected, false);
        compare(unmounted.selected, true);
        saveScreenshot(frame, "home-rail-column");
    }

    // The selected tool: group background, DemiBold, the accent bar at its
    // left edge and its icon in the accent colour. Exactly one at a time.
    function test_selectedGroupIsDrawnAsSelected() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN", selectedGroup: "sync"});
        const rail = frame.rail;
        for (const group of ["explore", "sync", "backup", "maintain"]) {
            const entry = findChild(rail, "railGroup:" + group);
            verify(entry !== null, group + " is in the rail");
            const on = group === "sync";
            compare(entry.selected, on, group);
            compare(findChild(entry, "railAccentBar").visible, on, group + "'s accent bar");
            verify(sameColor(text(entry).color, on ? Theme.text : Theme.textMuted), group + "'s text");
            compare(text(entry).font.weight, on ? Font.DemiBold : Font.Normal, group + "'s weight");
        }
        const sync = findChild(rail, "railGroup:sync");
        const bar = findChild(sync, "railAccentBar");
        compare(bar.x, 0, "the bar is at the entry's left edge");
        compare(bar.height, sync.height, "and runs its full height");
        verify(sameColor(bar.color, Theme.accent));
        compare(findChild(rail, "railGroup:explore").text, "Explore");
        compare(findChild(rail, "railGroup:maintain").text, "Maintain");

        rail.selectedGroup = "maintain";
        compare(sync.selected, false);
        compare(findChild(rail, "railGroup:maintain").selected, true);
    }

    // The group icons are the four the design names, and the selected one
    // is drawn in the accent colour.
    function test_groupIcons() {
        const frame = makeFrame([], {selectedGroup: "backup"});
        const expected = {explore: "view-media-track", sync: "exchange-positions",
                          backup: "backup", maintain: "kt-check-data"};
        for (const group in expected) {
            const entry = findChild(frame.rail, "railGroup:" + group);
            compare(entry.iconName, expected[group], group);
            let icon = null;
            const row = text(entry).parent;
            for (let i = 0; i < row.children.length; ++i) {
                const child = row.children[i];
                if (child.visible && child.iconName !== undefined) {
                    icon = child;
                }
            }
            verify(icon !== null, group + " draws its icon");
            compare(icon.iconName, expected[group]);
            verify(sameColor(icon.color, group === "backup" ? Theme.accent : Theme.textMuted), group + "'s icon colour");
            verify(icon.status === Image.Ready, group + "'s icon is a bundled one that loads");
        }
    }

    // The same rows through the other model shape: count and get(i), as
    // the real stick model and a ListModel have.
    function test_theListModelShape() {
        const model = createTemporaryObject(listModelComponent, testCase);
        for (const s of threeSticks()) {
            model.append(s);
        }
        const frame = makeFrame(model, {selectedStickKey: "/dev/sdc1"});
        const rail = frame.rail;
        compare(HomeModel.rowCount(rail.sticks), 3);
        compare(HomeModel.keyOf(HomeModel.rowAt(rail.sticks, 1)), "/media/CARD");
        verify(rail.hasKey("/dev/sdc1"));
        verify(!rail.hasKey("/media/GONE"));
        const unmounted = findChild(rail, "railStick:/dev/sdc1");
        verify(unmounted !== null);
        compare(unmounted.selected, true);
        compare(text(unmounted).text, "UNMOUNTED");
        compare(findChild(findChild(rail, "railStick:/media/CARD"), "railIcon").iconName, "media-flash-sd-mmc");

        // And the plain array shape answers the same.
        const arrayFrame = makeFrame(threeSticks(), {});
        compare(HomeModel.rowCount(arrayFrame.rail.sticks), 3);
        compare(HomeModel.keyOf(HomeModel.rowAt(arrayFrame.rail.sticks, 2)), "/dev/sdc1");
        verify(arrayFrame.rail.hasKey("/dev/sdc1"));
    }

    Component {
        id: appSettingsComponent
        AppSettingsController {}
    }

    // The rail's groups are the ones the settings accept as the group to
    // remember, in the same order: HomeModel.js on the QML side and
    // AppSettingsController::homeGroupKeys on the C++ side each spell
    // them out once, and this is what keeps the two from drifting.
    function test_theGroupsAreTheOnesTheSettingsAccept() {
        const frame = makeFrame([], {});
        const keys = frame.rail.groups.map((group) => group.key);
        compare(keys.length, 4);
        const settings = createTemporaryObject(appSettingsComponent, testCase);
        compare(JSON.stringify(Array.from(settings.homeGroupKeys)), JSON.stringify(keys));
        for (let i = 0; i < keys.length; ++i) {
            verify(findChild(frame.rail, "railGroup:" + keys[i]) !== null, keys[i] + " is on the rail");
        }
    }

    // A click reports the entry; it does not change the selection itself.
    function test_clicksReportTheEntry() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN", selectedGroup: "explore"});
        const rail = frame.rail;
        const stickSpy = createTemporaryObject(spyComponent, testCase, {target: rail, signalName: "stickActivated"});
        const groupSpy = createTemporaryObject(spyComponent, testCase, {target: rail, signalName: "groupActivated"});

        mouseClick(findChild(rail, "railStick:/dev/sdc1"));
        compare(stickSpy.count, 1);
        compare(stickSpy.signalArguments[0][0], "/dev/sdc1");
        compare(rail.selectedStickKey, "/media/MAIN", "the page decides; the rail only reports");

        mouseClick(findChild(rail, "railGroup:backup"));
        compare(groupSpy.count, 1);
        compare(groupSpy.signalArguments[0][0], "backup");
        compare(rail.selectedGroup, "explore");
    }

    // The column: every entry the rail's full width, one under the other,
    // the sticks above the tools, and every line of text on one inset.
    function test_theColumnForm() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN"});
        const rail = frame.rail;
        compare(rail.implicitWidth, Theme.scaled(200));
        const names = ["railSticksLabel", "railStick:/media/MAIN", "railStick:/media/CARD", "railStick:/dev/sdc1",
                       "railToolsLabel", "railGroup:explore", "railGroup:sync", "railGroup:backup", "railGroup:maintain"];
        let lastBottom = -1;
        for (const name of names) {
            const item = findChild(rail, name);
            verify(item !== null, name);
            compare(item.x, 0, name + " starts on the rail's left edge");
            compare(item.width, rail.width, name + " is the rail's width");
            verify(item.y >= lastBottom, name + " is below the one before");
            lastBottom = item.y + item.height;
        }
        compare(findChild(rail, "railNoSticks").visible, false);
        // One text line: the labels' text and every entry's text start at
        // the same inset from the rail's edge.
        const labelInset = findChild(rail, "railSticksLabel").leftPadding;
        compare(labelInset, Theme.crumbTextInset);
        const stickIcon = findChild(findChild(rail, "railStick:/media/MAIN"), "railIcon");
        compare(stickIcon.mapToItem(rail, 0, 0).x, labelInset, "a stick's icon starts on the text line");
        const toolRow = text(findChild(rail, "railGroup:sync")).parent;
        compare(toolRow.mapToItem(rail, 0, 0).x, labelInset, "a tool's icon starts on the text line");
        verify(rail.implicitHeight > 0);
        compare(rail.implicitHeight, lastBottom);
    }

    // Narrow window: a grid of chips, the sticks first, then the four
    // groups, each section starting on a new line under its label. Two
    // columns when two chips as wide as the widest fit side by side, each
    // chip filling its column with its text on the left after its icon.
    function test_theCompactForm() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN", selectedGroup: "sync", compact: true});
        frame.width = 640;
        waitForRendering(frame);
        const rail = frame.rail;
        compare(rail.chipColumns, 2);
        const main = findChild(rail, "railStick:/media/MAIN");
        const card = findChild(rail, "railStick:/media/CARD");
        const unmounted = findChild(rail, "railStick:/dev/sdc1");
        const explore = findChild(rail, "railGroup:explore");
        const sync = findChild(rail, "railGroup:sync");
        const backup = findChild(rail, "railGroup:backup");
        const maintain = findChild(rail, "railGroup:maintain");
        const cell = Math.floor((rail.width - Theme.tightSpacing) / 2);
        for (const chip of [main, card, unmounted, explore, sync, backup, maintain]) {
            compare(chip.width, cell, chip.objectName + " fills its column");
            verify(chip.x + chip.width <= rail.width, chip.objectName + " stays inside the rail");
            compare(Math.round(text(chip).parent.mapToItem(chip, 0, 0).x), Math.round(Theme.crumbTextInset),
                    chip.objectName + "'s icon and text start on the left");
        }
        compare(main.x, 0);
        compare(card.y, main.y, "two sticks to a row");
        compare(card.x, cell + Theme.tightSpacing, "the second in the second column");
        compare(unmounted.x, 0, "the third starts the next row");
        verify(unmounted.y > main.y);
        verify(explore.y > unmounted.y + unmounted.height, "the tools come after the sticks");
        compare(explore.x, 0, "the tools start their own line");
        compare(sync.y, explore.y);
        compare(sync.x, card.x, "the columns line up across the two sections");
        compare(backup.x, 0);
        verify(backup.y > explore.y);
        compare(maintain.y, backup.y);
        compare(findChild(rail, "railToolsLabel").width, rail.width, "each label takes a line of its own");
        compare(findChild(card, "railPill").border.width, 1, "an unselected chip has an edge");
        compare(findChild(main, "railPill").border.width, 0, "the selected one is its fill");
        compare(findChild(sync, "railAccentBar").visible, true);
        saveScreenshot(frame, "home-rail-compact");

        // Two of the widest chip side by side decide it, at the pixel.
        frame.width = 2 * rail.widestChip + Theme.tightSpacing + 2 * Theme.pageMargin;
        waitForRendering(frame);
        compare(rail.chipColumns, 2, "room for exactly two of the widest");
        frame.width -= 1;
        waitForRendering(frame);
        compare(rail.chipColumns, 1, "a pixel short of two");
        let lastBottom = -1;
        for (const chip of [main, card, unmounted, explore, sync, backup, maintain]) {
            compare(chip.x, 0, chip.objectName + " is in the one column");
            compare(chip.width, rail.width, chip.objectName + " fills it");
            verify(chip.y >= lastBottom, chip.objectName + " is below the one before");
            lastBottom = chip.y + chip.height;
        }
        saveScreenshot(frame, "home-rail-compact-narrow");

        // A long stick name widens every chip's claim: one column, even in
        // a rail that took two for the short names.
        frame.width = 640;
        waitForRendering(frame);
        compare(rail.chipColumns, 2);
        frame.sticks = [makeStick({label: "A STICK WITH A VERY LONG NAME INDEED, TOO LONG TO SHARE"})];
        waitForRendering(frame);
        verify(2 * rail.widestChip + Theme.tightSpacing > rail.width, "the long name is wider than half the rail");
        compare(rail.chipColumns, 1);
        // Never in the column form.
        rail.compact = false;
        compare(rail.chipColumns, 1);
    }

    // No stick: one muted line where the sticks were, the tools as ever,
    // and no tab stop on a section with nothing in it.
    function test_noSticks() {
        const frame = makeFrame([], {selectedGroup: "backup"});
        const rail = frame.rail;
        const none = findChild(rail, "railNoSticks");
        compare(none.visible, true);
        compare(none.text, "No USB sticks detected");
        verify(sameColor(none.color, Theme.textMuted));
        compare(findChild(rail, "railGroup:backup").selected, true);
        compare(findChild(rail, "railStickKeys").activeFocusOnTab, false);
        compare(findChild(rail, "railToolKeys").activeFocusOnTab, true);
        verify(findChild(rail, "railToolsLabel").y > none.y);
        saveScreenshot(frame, "home-rail-no-sticks");

        // A null model (no controller yet) is the same, not an error.
        rail.sticks = null;
        compare(HomeModel.rowCount(rail.sticks), 0);
        compare(none.visible, true);
    }

    // Tab reaches the sticks, then the tools. Up and Down move a cursor
    // that starts on the selected entry and stops at the ends; Enter and
    // Space activate what is under it.
    function test_keyboard() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/CARD", selectedGroup: "sync"});
        const rail = frame.rail;
        const stickKeys = findChild(rail, "railStickKeys");
        const toolKeys = findChild(rail, "railToolKeys");
        const stickSpy = createTemporaryObject(spyComponent, testCase, {target: rail, signalName: "stickActivated"});
        const groupSpy = createTemporaryObject(spyComponent, testCase, {target: rail, signalName: "groupActivated"});

        stickKeys.forceActiveFocus();
        verify(stickKeys.activeFocus);
        compare(stickKeys.cursor, 1, "the cursor starts on the selected stick");
        compare(findChild(findChild(rail, "railStick:/media/CARD"), "railCursor").visible, true);
        keyClick(Qt.Key_Down);
        compare(stickKeys.cursor, 2);
        compare(findChild(findChild(rail, "railStick:/dev/sdc1"), "railCursor").visible, true);
        compare(findChild(findChild(rail, "railStick:/media/CARD"), "railCursor").visible, false);
        keyClick(Qt.Key_Down);
        compare(stickKeys.cursor, 2, "and stops at the end");
        compare(stickSpy.count, 0, "moving is not choosing");
        keyClick(Qt.Key_Return);
        compare(stickSpy.count, 1);
        compare(stickSpy.signalArguments[0][0], "/dev/sdc1");
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_Up);
        keyClick(Qt.Key_Up);
        compare(stickKeys.cursor, 0);
        keyClick(Qt.Key_Space);
        compare(stickSpy.signalArguments[1][0], "/media/MAIN");

        keyClick(Qt.Key_Tab);
        verify(toolKeys.activeFocus, "Tab goes from the sticks to the tools");
        compare(findChild(findChild(rail, "railStick:/media/MAIN"), "railCursor").visible, false,
                "the sticks' cursor goes with their focus");
        compare(toolKeys.cursor, 1, "the cursor starts on the selected tool");
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Enter);
        compare(groupSpy.count, 1);
        compare(groupSpy.signalArguments[0][0], "backup");
        // Left and Right belong to the chip grid only.
        keyClick(Qt.Key_Right);
        compare(toolKeys.cursor, 2);
        // The grid, two columns: Explore Sync / Backup Maintain. Left and
        // Right move between the columns of a row and stop at its ends;
        // Up and Down move a row, and stay in the section.
        rail.compact = true;
        waitForRendering(frame);
        compare(rail.chipColumns, 2);
        compare(toolKeys.columns, 2);
        keyClick(Qt.Key_Right);
        compare(toolKeys.cursor, 3, "Backup to Maintain, across the row");
        keyClick(Qt.Key_Right);
        compare(toolKeys.cursor, 3, "the row ends at Maintain");
        keyClick(Qt.Key_Left);
        compare(toolKeys.cursor, 2);
        keyClick(Qt.Key_Left);
        compare(toolKeys.cursor, 2, "and starts at Backup");
        keyClick(Qt.Key_Up);
        compare(toolKeys.cursor, 0, "Up goes to Explore, above Backup");
        keyClick(Qt.Key_Up);
        compare(toolKeys.cursor, 0, "and no further");
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_Down);
        compare(toolKeys.cursor, 3, "Down goes from Sync to Maintain");
        keyClick(Qt.Key_Down);
        compare(toolKeys.cursor, 3, "and no further: the sticks are another section");
        saveScreenshot(frame, "home-rail-keyboard");

        // Three sticks in two columns: Down from the second lands on the
        // third, the shorter row's only chip.
        keyClick(Qt.Key_Backtab);
        verify(stickKeys.activeFocus);
        compare(stickKeys.columns, 2);
        stickKeys.cursor = 1;
        keyClick(Qt.Key_Down);
        compare(stickKeys.cursor, 2);
        keyClick(Qt.Key_Right);
        compare(stickKeys.cursor, 2, "nothing beside it");
        keyClick(Qt.Key_Up);
        compare(stickKeys.cursor, 0);

        // One column: Left and Right have nowhere to go.
        frame.width = 2 * rail.widestChip + Theme.tightSpacing + 2 * Theme.pageMargin - 1;
        waitForRendering(frame);
        compare(stickKeys.columns, 1);
        keyClick(Qt.Key_Right);
        compare(stickKeys.cursor, 0);
        keyClick(Qt.Key_Down);
        compare(stickKeys.cursor, 1, "Down is the next chip in one column");
    }

    // A stick plugged in arrives visibly; one pulled out shrinks away. The
    // rows there from the start do not animate, and a model replacing
    // itself with the same sticks is not an arrival or a departure.
    function test_sticksArriveAndLeave() {
        const model = createTemporaryObject(listModelComponent, testCase);
        model.append(makeStick({}));
        const frame = makeFrame(model, {selectedStickKey: "/media/MAIN"});
        const rail = frame.rail;
        const main = findChild(rail, "railStick:/media/MAIN");
        compare(main.opacity, 1, "a stick present from the start is simply there");
        compare(main.scale, 1);

        model.append(makeStick({label: "NEW", mountPoint: "/media/NEW", devicePath: "/dev/sdd1"}));
        const arrived = findChild(rail, "railStick:/media/NEW");
        verify(arrived !== null);
        verify(arrived.opacity < 1, "an arriving stick fades in");
        tryCompare(arrived, "opacity", 1, Theme.arrivalTransitionDuration * 4);
        tryCompare(arrived, "scale", 1, Theme.arrivalTransitionDuration * 4);

        const newY = arrived.y;
        model.remove(0);
        const ghost = findChild(rail, "railStickLeaving:/media/MAIN");
        verify(ghost !== null, "a leaving stick is shown going");
        compare(ghost.text, "MAIN");
        compare(ghost.selected, true, "as it was drawn");
        tryVerify(() => findChild(rail, "railStickLeaving:/media/MAIN") === null,
                  Theme.departureTransitionDuration * 6, "and is gone once it has");
        tryVerify(() => arrived.y < newY, Theme.arrivalTransitionDuration * 4, "the row below moves up");

        // Mounting changes a row's key from its device to its mount point:
        // the same stick, so nothing arrives.
        model.append(makeStick({label: "LATE", mountPoint: "", devicePath: "/dev/sde1"}));
        const late = findChild(rail, "railStick:/dev/sde1");
        tryCompare(late, "opacity", 1, Theme.arrivalTransitionDuration * 4);
        model.setProperty(1, "mountPoint", "/media/LATE");
        compare(late.objectName, "railStick:/media/LATE");
        compare(late.opacity, 1);
    }

    // A stick is known by two keys over its life, its device path until
    // it is mounted and its mount point after: pulling it forgets both,
    // so plugging it in again is an arrival, however it was keyed when
    // it went.
    function test_aRepluggedStickArrivesAgain() {
        const model = createTemporaryObject(listModelComponent, testCase);
        model.append(makeStick({}));
        const frame = makeFrame(model, {selectedStickKey: "/media/MAIN"});
        const rail = frame.rail;

        model.append(makeStick({label: "PLUG", mountPoint: "", devicePath: "/dev/sdg1"}));
        const plugged = findChild(rail, "railStick:/dev/sdg1");
        verify(plugged.opacity < 1, "plugged in: it arrives");
        tryCompare(plugged, "opacity", 1, Theme.arrivalTransitionDuration * 4);
        model.setProperty(1, "mountPoint", "/media/PLUG");
        compare(plugged.objectName, "railStick:/media/PLUG");
        model.remove(1);
        tryVerify(() => findChild(rail, "railStickLeaving:/media/PLUG") === null,
                  Theme.departureTransitionDuration * 6, "pulled: it has gone");

        model.append(makeStick({label: "PLUG", mountPoint: "", devicePath: "/dev/sdg1"}));
        const again = findChild(rail, "railStick:/dev/sdg1");
        verify(again !== null);
        verify(again.opacity < 1, "plugged in again under its device key: it arrives again");
        tryCompare(again, "opacity", 1, Theme.arrivalTransitionDuration * 4);

        // And the other way round: unmounted before it is pulled, then
        // mounted by the system as it comes back.
        model.setProperty(1, "mountPoint", "/media/PLUG");
        model.setProperty(1, "mountPoint", "");
        model.remove(1);
        tryVerify(() => findChild(rail, "railStickLeaving:/dev/sdg1") === null,
                  Theme.departureTransitionDuration * 6, "pulled again: gone");
        model.append(makeStick({label: "PLUG", mountPoint: "/media/PLUG", devicePath: "/dev/sdg1"}));
        verify(findChild(rail, "railStick:/media/PLUG").opacity < 1,
               "back already mounted, under the key it had before: it arrives");
    }

    function test_aReplacedArrayIsNotAnArrival() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN"});
        const rail = frame.rail;
        rail.sticks = threeSticks();
        compare(findChild(rail, "railStickLeaving:/media/MAIN"), null, "nothing left");
        const main = findChild(rail, "railStick:/media/MAIN");
        compare(main.opacity, 1, "nothing arrived");

        // But a stick that is new in the replacement did arrive.
        const more = threeSticks();
        more.push(makeStick({label: "FOURTH", mountPoint: "/media/FOURTH", devicePath: "/dev/sdf1"}));
        rail.sticks = more;
        verify(findChild(rail, "railStick:/media/FOURTH").opacity < 1);
    }
}
