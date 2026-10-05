// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The window's header row over the page stack: still while pages slide,
// shown on the pages that name a place, the menu on the home alone, the
// current place's button off, and the page's header told how much room
// to keep. Fake pages here; the real ones say their place in their own
// tests.
TestCase {
    id: testCase
    name: "AppHeaderOverlay"
    width: 900
    height: 600
    visible: true
    when: windowShown

    Component {
        id: harness
        Item {
            width: 900; height: 600
            property alias stack: stack
            property alias overlay: overlay
            StackView { id: stack; anchors.fill: parent }
            AppHeaderOverlay {
                id: overlay
                objectName: "overlay"
                stackView: stack
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Theme.pageMargin
            }
        }
    }
    // A page that wants the row, and one that guards leaving.
    Component {
        id: homeLike
        Item { readonly property string appHeaderPlace: "home"; property real appHeaderReserve: 0 }
    }
    Component {
        id: aboutLike
        Item { readonly property string appHeaderPlace: "about"; property real appHeaderReserve: 0 }
    }
    Component {
        id: guardedLike
        Item {
            readonly property string appHeaderPlace: "preferences"
            property real appHeaderReserve: 0
            property var held: null
            function leaveTo(fn) { held = fn; }
        }
    }
    Component { id: toolLike; Item {} }
    Component { id: spyComponent; SignalSpy {} }

    function test_shownWhereAPageNamesItsPlaceAndStillAcrossAReplace() {
        const h = createTemporaryObject(harness, testCase);
        h.stack.push(homeLike);
        tryCompare(h.stack, "busy", false);
        waitForRendering(h);
        compare(h.overlay.visible, true);
        compare(h.overlay.place, "home");
        // Arriving home plays the buttons' entrance; stillness is about
        // what follows it.
        tryCompare(h.overlay, "entranceRunning", false, 3000);
        compare(findChild(h.overlay, "homeMenuButton").visible, true, "the menu is on the home");
        verify(h.overlay.width > 0);
        // Still means the buttons keep their place: the row is anchored
        // at its right, so its right edge and the About button are what
        // must not move (the row narrows when the menu leaves).
        const aboutButton = findChild(h.overlay, "aboutButton");
        const before = aboutButton.mapToItem(h, 0, 0);
        const rightBefore = h.overlay.mapToItem(h, h.overlay.width, 0).x;
        const widthBefore = h.overlay.width;
        // The home's header keeps the row's width clear.
        compare(h.stack.currentItem.appHeaderReserve, h.overlay.width);

        h.stack.replace(aboutLike);
        // Mid-transition and after: the row has not moved. It is not a
        // child of the stack, so the page's slide cannot carry it.
        const during = aboutButton.mapToItem(h, 0, 0);
        compare(during.x, before.x); compare(during.y, before.y);
        tryCompare(h.stack, "busy", false);
        waitForRendering(h);
        const after = aboutButton.mapToItem(h, 0, 0);
        compare(after.x, before.x); compare(after.y, before.y);
        compare(h.overlay.mapToItem(h, h.overlay.width, 0).x, rightBefore, "the right edge stays");
        compare(h.overlay.place, "about");
        compare(findChild(h.overlay, "homeMenuButton").visible, false, "no menu away from the home");
        compare(findChild(h.overlay, "aboutButton").enabled, false, "the page one is on is off");
        verify(h.overlay.width < widthBefore, "narrower without the menu button");
        compare(h.stack.currentItem.appHeaderReserve, h.overlay.width);

        h.stack.push(toolLike);
        tryCompare(h.stack, "busy", false);
        compare(h.overlay.visible, false, "a tool page gets no row");
    }

    function test_theButtonsLeadAndAGuardedPageIsAskedFirst() {
        const h = createTemporaryObject(harness, testCase);
        h.stack.push(homeLike);
        tryCompare(h.stack, "busy", false);
        waitForRendering(h);
        const about = createTemporaryObject(spyComponent, testCase, {target: h.overlay, signalName: "aboutRequested"});
        mouseClick(findChild(h.overlay, "aboutButton"));
        compare(about.count, 1);

        const menuButton = findChild(h.overlay, "homeMenuButton");
        const menu = findChild(h.overlay, "homeMenu");
        mouseClick(menuButton);
        tryCompare(menu, "opened", true);
        compare(menu.count, 2, "two entries: what the no-stick Backup group cannot offer");
        const full = findChild(h.overlay, "browseFullBackupItem");
        const folder = findChild(h.overlay, "openFolderItem");
        for (const entry of [full, folder]) {
            verify(entry.implicitWidth <= entry.width, entry.text + " needs " + entry.implicitWidth + " px and got " + entry.width);
        }
        // The button closes what it opened: a press on it no longer
        // closes the menu only for the click to open it again.
        mouseClick(menuButton);
        tryCompare(menu, "visible", false);
        const browse = createTemporaryObject(spyComponent, testCase, {target: h.overlay, signalName: "browseFullBackupRequested"});
        full.triggered();
        compare(browse.count, 1);

        h.stack.replace(guardedLike);
        tryCompare(h.stack, "busy", false);
        waitForRendering(h);
        const support = createTemporaryObject(spyComponent, testCase, {target: h.overlay, signalName: "supportRequested"});
        mouseClick(findChild(h.overlay, "donateButton"));
        compare(support.count, 0, "the guarded page holds the move");
        verify(h.stack.currentItem.held !== null, "the page was asked");
        h.stack.currentItem.held();
        compare(support.count, 1, "and the move follows its answer");
    }

    function test_theHeaderIconsAreBundledAndTheHeartIsFilled() {
        const h = createTemporaryObject(harness, testCase);
        h.stack.push(homeLike);
        tryCompare(h.stack, "busy", false);
        waitForRendering(h);
        const menu = findChild(h.overlay, "homeMenuButton");
        const donate = findChild(h.overlay, "donateButton");
        // Drawn, not a theme icon: Breeze's heart ("love") is an outline.
        const heart = findChild(h.overlay, "donateHeart");
        verify(heart !== null, "the donate button must draw the filled heart");
        compare(donate.contentItem, heart);
        compare(heart.color, Qt.color("#aa0000"));
        // Every icon one size, the menu's included, and the heart DRAWN
        // at it: a button stretches its contentItem, and under KDE's style
        // the heart filled the button.
        verify(h.overlay.iconSize > 0);
        compare(menu.icon.width, h.overlay.iconSize);
        compare(heart.drawnSize, h.overlay.iconSize);
        const buttons = [{name: "homeMenuButton", icon: "application-menu"}, {name: "aboutButton", icon: "help-about"},
                         {name: "preferencesButton", icon: "configure"}];
        for (const b of buttons) {
            const button = findChild(h.overlay, b.name);
            verify(button !== null, b.name + " must exist");
            // An IconToolButton: the glyph is a bundled icon Seabass draws
            // itself, since KDE's style ignores a ToolButton's icon.color.
            compare(button.icon.name, "", b.name + " must not look the icon up in the theme");
            compare(button.icon.source.toString(), "", b.name + " must not have the style paint its icon");
            compare(button.iconName, b.icon);
            const glyph = findChild(button, "iconToolButtonGlyph");
            verify(glyph !== null, b.name + " must draw its own glyph");
            compare(glyph.source.toString(), Theme.iconUrl(b.icon));
            compare(glyph.color, Theme.text, b.name + " must be tinted flat");
            compare(glyph.size, h.overlay.iconSize);
            compare(button.icon.width, h.overlay.iconSize);
            compare(button.icon.height, h.overlay.iconSize);
        }
    }

    // Arriving home, the buttons make their entrance; moving between the
    // places that share the row, they do not.
    function test_arrivingHomeThePlayersMakeAnEntrance() {
        const h = createTemporaryObject(harness, testCase);
        h.stack.push(toolLike);
        tryCompare(h.stack, "busy", false);
        verify(!h.overlay.visible, "no row on a tool page");
        h.stack.replace(homeLike);
        tryCompare(h.overlay, "entranceRunning", true, 1000, "back home, the entrance plays");
        if (typeof screenshotDir !== "undefined" && screenshotDir && screenshotDir.length > 0) {
            let elapsed = 0;
            for (const ms of [120, 260, 420, 700]) {
                wait(ms - elapsed);
                elapsed = ms;
                grabImage(h.overlay).save(screenshotDir + "/header-entrance-" + ms + ".png");
            }
        }
        tryCompare(h.overlay, "entranceRunning", false, 3000, "and ends");
        // At rest the buttons are where the layout put them: no transform left over.
        const about = findChild(h.overlay, "aboutButton");
        const slot = about.parent.parent;
        const p = about.mapToItem(slot, 0, 0);
        compare(Math.round(p.x), 0, "no sideways offset left");
        compare(Math.round(p.y), 0, "no drop left");
        compare(about.parent.opacity, 1);

        h.stack.replace(aboutLike);
        tryCompare(h.stack, "busy", false);
        h.stack.replace(guardedLike);
        tryCompare(h.stack, "busy", false);
        wait(50);
        verify(!h.overlay.entranceRunning, "between About and Preferences the row stays put");
    }
}
