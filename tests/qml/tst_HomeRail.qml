// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

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
        // The stick section has no accent bar: that marks the tool.
        compare(findChild(card, "railAccentBar").visible, false);

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
        compare(rail.rowCount(), 3);
        compare(rail.keyOf(rail.rowAt(1)), "/media/CARD");
        verify(rail.hasKey("/dev/sdc1"));
        verify(!rail.hasKey("/media/GONE"));
        const unmounted = findChild(rail, "railStick:/dev/sdc1");
        verify(unmounted !== null);
        compare(unmounted.selected, true);
        compare(text(unmounted).text, "UNMOUNTED");
        compare(findChild(findChild(rail, "railStick:/media/CARD"), "railIcon").iconName, "media-flash-sd-mmc");

        // And the plain array shape answers the same.
        const arrayFrame = makeFrame(threeSticks(), {});
        compare(arrayFrame.rail.rowCount(), 3);
        compare(arrayFrame.rail.keyOf(arrayFrame.rail.rowAt(2)), "/dev/sdc1");
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

    // Narrow window: wrapped rows of chips, the sticks first, then the four
    // groups, each section starting on a new line under its label.
    function test_theCompactForm() {
        const frame = makeFrame(threeSticks(), {selectedStickKey: "/media/MAIN", selectedGroup: "sync", compact: true});
        frame.width = 640;
        waitForRendering(frame);
        const rail = frame.rail;
        const main = findChild(rail, "railStick:/media/MAIN");
        const card = findChild(rail, "railStick:/media/CARD");
        const explore = findChild(rail, "railGroup:explore");
        const maintain = findChild(rail, "railGroup:maintain");
        verify(main.width < rail.width / 2, "a chip is as wide as its text");
        compare(card.y, main.y, "the sticks run across");
        verify(card.x > main.x);
        compare(maintain.y, explore.y, "the tools run across");
        verify(maintain.x > explore.x);
        verify(explore.y > main.y + main.height, "the tools come after the sticks");
        compare(explore.x, 0, "the tools start their own line");
        compare(findChild(rail, "railToolsLabel").width, rail.width, "each label takes a line of its own");
        compare(findChild(card, "railPill").border.width, 1, "an unselected chip has an edge");
        compare(findChild(main, "railPill").border.width, 0, "the selected one is its fill");
        compare(findChild(findChild(rail, "railGroup:sync"), "railAccentBar").visible, true);
        saveScreenshot(frame, "home-rail-compact");

        // And wraps when the row runs out.
        frame.width = 260;
        waitForRendering(frame);
        verify(maintain.y > explore.y, "the tools wrap onto a second line in a narrow rail");
        saveScreenshot(frame, "home-rail-compact-wrapped");
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
        compare(rail.rowCount(), 0);
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
        // Left and Right belong to the compact form only.
        keyClick(Qt.Key_Right);
        compare(toolKeys.cursor, 2);
        rail.compact = true;
        keyClick(Qt.Key_Right);
        compare(toolKeys.cursor, 3);
        keyClick(Qt.Key_Left);
        compare(toolKeys.cursor, 2);
        saveScreenshot(frame, "home-rail-keyboard");
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
