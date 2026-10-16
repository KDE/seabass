// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

TestCase {
    id: testCase
    name: "PlaylistPickerCombo"
    width: 400
    height: 300
    visible: true
    when: windowShown

    property var testModel: [
        { name: "All tracks", count: 42 },
        { name: "Techno", count: 12 },
        { name: "House", count: 8 },
    ]

    Component {
        id: comboComponent
        PlaylistPickerCombo {}
    }

    // No static target -- each test that needs it points this at whatever
    // combo it just created (createTemporaryObject()'d instances don't
    // exist yet when this component tree is built).
    SignalSpy {
        id: pickedSpy
        signalName: "playlistPicked"
    }

    // A regression guard for exactly today's bug: redeclaring a property
    // ComboBox already owns (`model` is FINAL in Qt 6's QQC2) fails object
    // creation outright. This alone would have caught it -- no click
    // needed, just instantiation.
    function test_instantiatesWithModel() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel });
        verify(combo !== null);
        compare(combo.model.length, 3);
    }

    function test_currentIndexReflectsExternalSelection() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel, currentIndex: 1 });
        verify(combo !== null);
        compare(combo.currentIndex, 1);
        compare(combo.model[combo.currentIndex].name, "Techno");
    }

    function test_playlistPickedOnRowClick() {
        var combo = createTemporaryObject(comboComponent, testCase, { model: testModel });
        verify(combo !== null);

        pickedSpy.target = combo;
        pickedSpy.clear();

        mouseClick(combo);
        tryVerify(function() { return combo.popup.visible; });
        // The ListView's delegates aren't necessarily instantiated the
        // instant the popup becomes visible -- poll rather than assume
        // one synchronous check after popup.visible is enough.
        tryVerify(function() { return combo.popup.contentItem.itemAtIndex(2) !== null; });

        var row = combo.popup.contentItem.itemAtIndex(2);
        verify(row !== null);
        mouseClick(row);

        tryVerify(function() { return pickedSpy.count === 1; });
        compare(pickedSpy.signalArguments[0][0], 2);
        compare(pickedSpy.signalArguments[0][1].name, "House");
        tryVerify(function() { return !combo.popup.visible; });
    }

    // A row shades with the pointer over its name, not only over its
    // margins: under org.kde.desktop a Label has a HoverHandler of its
    // own, and the MouseArea's hover under it never fired.
    function test_aRowShadesUnderItsName() {
        const combo = createTemporaryObject(comboComponent, testCase, {model: testModel});
        mouseClick(combo);
        tryVerify(() => combo.popup.visible && combo.popup.contentItem.itemAtIndex(2) !== null);
        const row = combo.popup.contentItem.itemAtIndex(2);
        const name = findChild(row, "pickerRowName");
        compare(name.text, "House");
        verify(!Qt.colorEqual(row.color, Theme.rowHover));
        mouseMove(name, 4, name.height / 2);
        tryVerify(() => Qt.colorEqual(row.color, Theme.rowHover), 1000, "shaded under the name: " + row.color);
        combo.popup.close();
    }

    // ---- Blank rows (Sebastian's testing): a row now and then came up
    // an empty gap, the right height and no text. ----

    function manyNames(n, prefix) {
        const names = [{name: "All tracks", count: n}];
        for (let i = 1; i < n; ++i) {
            names.push({name: (prefix || "Playlist ") + String(i).padStart(3, "0"), count: i});
        }
        return names;
    }

    // Every row the popup's list has built: its name shown, with its
    // text, and the row the height of a row.
    function checkRows(combo, what) {
        const list = combo.popup.contentItem;
        tryVerify(() => list.count === combo.model.length
                 && (list.itemAtIndex(0) !== null || list.itemAtIndex(list.count - 1) !== null), 3000, what + ": rows built");
        waitForRendering(list);
        let built = 0;
        for (let i = 0; i < list.count; ++i) {
            const row = list.itemAtIndex(i);
            if (row === null) {
                continue;  // not built: scrolled out of the popup
            }
            ++built;
            const name = findChild(row, "pickerRowName");
            verify(name !== null, what + ": row " + i + " has its name label");
            compare(name.text, combo.model[i].name, what + ": row " + i + "'s text");
            verify(name.visible && name.width > 0 && name.height > 0 && name.opacity > 0,
                   what + ": row " + i + "'s name is drawn (" + name.width + " x " + name.height + ")");
            compare(row.height, 32, what + ": row " + i + "'s height");
            verify(row.width > 0 && row.visible, what + ": row " + i + " is laid out");
            const before = i > 0 ? list.itemAtIndex(i - 1) : null;
            verify(before === null || row.y >= before.y + before.height, what + ": row " + i + " below the one before");
        }
        verify(built > 5, what + ": rows built " + built);
        checkRowsDrawn(list, "pickerRowName", what);
    }

    function test_noBlankRowsOverRepeatedOpens() {
        const combo = createTemporaryObject(comboComponent, testCase, {model: manyNames(30), width: 240});
        for (let round = 0; round < 20; ++round) {
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            checkRows(combo, "round " + round);
            // And scrolled to its end, the rows below built as they come.
            combo.popup.contentItem.positionViewAtEnd();
            checkRows(combo, "scrolled, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
        }
    }

    // The names arrive (a scan finishing) and the popup opens straight
    // away, the first time and after a change of the list.
    function test_noBlankRowsRightAfterTheNamesArrive() {
        const combo = createTemporaryObject(comboComponent, testCase, {model: [], width: 240});
        for (let round = 0; round < 20; ++round) {
            combo.model = manyNames(30 + round % 3, round % 2 ? "Crate " : "Playlist ");
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            checkRows(combo, "after the names changed, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
        }
    }

    // The names change while the popup is open (a scan finishing under it).
    function test_noBlankRowsWhenTheNamesChangeWhileOpen() {
        const combo = createTemporaryObject(comboComponent, testCase, {model: manyNames(5), width: 240});
        for (let round = 0; round < 20; ++round) {
            mouseClick(combo);
            tryVerify(() => combo.popup.opened, 3000, "opens, round " + round);
            combo.model = manyNames(30 + round % 3, round % 2 ? "Crate " : "Playlist ");
            checkRows(combo, "changed while open, round " + round);
            combo.popup.close();
            tryVerify(() => !combo.popup.visible, 3000, "closes, round " + round);
            combo.model = manyNames(5 + round % 4);
        }
    }

    // ---- Typing while the popup is open ----

    readonly property var crateModel: ["All tracks", "Peak Time", "Warmup", "Deep House", "Peaky", "Techno Peak",
                                      "Afterhours", "Ambient", "Broken Beat", "Warehouse"].map((n, i) => ({name: n, count: i}))

    function openCrates(props) {
        const combo = createTemporaryObject(comboComponent, testCase,
                                            Object.assign({model: crateModel, width: 240}, props || {}));
        mouseClick(combo);
        tryVerify(() => combo.popup.opened, 3000, "opens");
        return combo;
    }

    function type(text) {
        for (const c of text) {
            keyClick(c);
        }
    }

    function shownNames(combo) {
        return combo.shownRows.map(i => combo.textAt(i)).join(", ");
    }

    function currentName(combo) {
        const list = combo.popup.contentItem;
        return list.currentIndex < 0 ? "" : combo.textAt(combo.shownRows[list.currentIndex]);
    }

    // Typed quickly, the letters narrow the list to the names holding
    // them anywhere, case folded; the first is current, and the field
    // says what is typed.
    function test_typingNarrowsToTheNamesHoldingIt() {
        const combo = openCrates();
        type("eak");
        compare(combo.typedFilter, "eak");
        compare(shownNames(combo), "Peak Time, Peaky, Techno Peak", "inside a name, not only at its start");
        compare(combo.popup.contentItem.count, 3, "the popup lists those");
        compare(currentName(combo), "Peak Time", "the first match is current");
        compare(combo.displayText, "Filter: eak");
        verify(combo.popup.visible, "still open");
        combo.popup.close();
    }

    function test_theFilterFoldsCase() {
        const combo = openCrates();
        type("PEAK");
        compare(shownNames(combo), "Peak Time, Peaky, Techno Peak");
        combo.popup.close();
    }

    // After a pause longer than the interval the typing starts over: a
    // letter alone then jumps, it does not narrow.
    function test_aPauseStartsTheTypingOver() {
        const combo = openCrates({typeAheadInterval: 150});
        type("w");
        compare(combo.typedFilter, "", "one letter alone narrows nothing");
        compare(currentName(combo), "Warmup", "it jumps to the next name starting with it");
        wait(400);
        type("a");
        compare(combo.typedFilter, "", "after the pause: started over, not \"wa\"");
        compare(shownNames(combo).split(", ").length, 10, "the whole list");
        compare(currentName(combo), "Afterhours");
        type("m");
        compare(combo.typedFilter, "am", "within the interval: a filter");
        compare(shownNames(combo), "Ambient");
        combo.popup.close();
    }

    // A letter after each pause goes to the next name with that initial,
    // round the list.
    function test_aLetterAloneJumpsToTheNextInitial() {
        const combo = openCrates({typeAheadInterval: 100});
        compare(currentName(combo), "All tracks", "opens on the playlist picked");
        const seen = [];
        for (let i = 0; i < 3; ++i) {
            type("a");
            seen.push(currentName(combo));
            wait(300);
        }
        compare(seen.join(", "), "Afterhours, Ambient, All tracks");
        compare(combo.typedFilter, "");
        combo.popup.close();
    }

    function test_backspaceTakesALetterOff() {
        const combo = openCrates();
        type("eak");
        keyClick(Qt.Key_Backspace);
        compare(combo.typedFilter, "ea");
        compare(shownNames(combo), "Peak Time, Peaky, Techno Peak, Broken Beat", "wider again");
        keyClick(Qt.Key_Backspace);
        compare(combo.typedFilter, "e", "still a filter at one letter");
        verify(combo.shownRows.length < 10 && combo.shownRows.length > 3, shownNames(combo));
        keyClick(Qt.Key_Backspace);
        compare(combo.typedFilter, "");
        compare(combo.shownRows.length, 10, "the whole list again");
        combo.popup.close();
    }

    function test_escapeClearsTheFilterThenCloses() {
        const combo = openCrates();
        type("pe");
        compare(combo.shownRows.length, 3);
        keyClick(Qt.Key_Escape);
        compare(combo.typedFilter, "");
        compare(combo.shownRows.length, 10);
        verify(combo.popup.visible, "the first Escape only clears");
        keyClick(Qt.Key_Escape);
        tryVerify(() => !combo.popup.visible, 2000, "the second closes");
    }

    function test_enterPicksTheCurrentRow() {
        const combo = openCrates();
        pickedSpy.target = combo;
        pickedSpy.clear();
        type("tech");
        keyClick(Qt.Key_Return);
        compare(pickedSpy.count, 1);
        compare(pickedSpy.signalArguments[0][0], 5, "the model's index, not the filtered list's");
        compare(pickedSpy.signalArguments[0][1].name, "Techno Peak");
        tryVerify(() => !combo.popup.visible, 2000, "and closes");
        compare(combo.typedFilter, "", "the filter goes with it");
    }

    function test_theFilterIsClearedOnReopen() {
        const combo = openCrates();
        type("pe");
        compare(combo.shownRows.length, 3);
        combo.popup.close();
        tryVerify(() => !combo.popup.visible);
        compare(combo.typedFilter, "", "cleared on closing");
        mouseClick(combo);
        tryVerify(() => combo.popup.opened, 3000);
        compare(combo.shownRows.length, 10);
        compare(combo.popup.contentItem.count, 10);
        compare(combo.displayText, "All tracks");
        combo.popup.close();
    }

    // The text of every row in sight, as drawn: the window's own frame
    // (the popup is in its overlay), read under each row's name label,
    // must hold pixels unlike the row's ground beside it.
    function checkRowsDrawn(list, labelName, what) {
        const ground = list.Window.contentItem;
        const image = grabImage(ground);
        const s = PixelScale.scale(image, ground);
        const lum = c => 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        let looked = 0;
        for (let i = 0; i < list.count; ++i) {
            const row = list.itemAtIndex(i);
            if (row === null || row.y - list.contentY < 0 || row.y - list.contentY + row.height > list.height) {
                continue;
            }
            if (row.mapToItem(ground, 0, row.height).y > ground.height) {
                continue;  // below the window's edge
            }
            const name = findChild(row, labelName);
            const back = lum(PixelScale.pixel(image, ground, row.mapToItem(ground, 4, row.height / 2).x,
                                              row.mapToItem(ground, 4, row.height / 2).y));
            const origin = name.mapToItem(ground, 0, 0);
            let ink = 0;
            for (let y = 0; y < name.height; y += 1 / s) {
                for (let x = 0; x < Math.min(name.width, 60); x += 1 / s) {
                    ink += Math.abs(lum(PixelScale.pixel(image, ground, origin.x + x, origin.y + y)) - back) > 0.15 ? 1 : 0;
                }
            }
            verify(ink >= 3, what + ": row " + i + " (" + name.text + ") is drawn with its text, " + ink + " pixels of ink");
            ++looked;
        }
        verify(looked > 4, what + ": rows looked at " + looked);
    }
}
