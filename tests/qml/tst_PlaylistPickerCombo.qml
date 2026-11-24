// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
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

    // ---- Boxes on the rows (checkable, Beat Grids & Waveforms) ----

    // A host keeping the ticks, as the page's controller does: a set of
    // names ("" for All tracks), and some boxes off.
    property var ticked: ({})
    property var offBoxes: []
    property int tickRevision: 0
    SignalSpy {
        id: toggledSpy
        signalName: "playlistCheckToggled"
    }
    function openCheckable(props) {
        testCase.ticked = {};
        testCase.offBoxes = [];
        const combo = openCrates(Object.assign({
            checkable: true,
            checkStateOf: name => testCase.tickRevision >= 0 && testCase.ticked[name] ? Qt.Checked : Qt.Unchecked,
            checkEnabledFor: name => testCase.offBoxes.indexOf(name) < 0,
        }, props || {}));
        combo.checkRevision = Qt.binding(() => testCase.tickRevision);
        combo.playlistCheckToggled.connect((name, checked) => {
            testCase.ticked[name] = checked;
            ++testCase.tickRevision;
        });
        toggledSpy.target = combo;
        toggledSpy.clear();
        pickedSpy.target = combo;
        pickedSpy.clear();
        return combo;
    }
    function rowNamed(combo, name) {
        const list = combo.popup.contentItem;
        let found = null;
        tryVerify(() => {
            for (let i = 0; i < list.count; ++i) {
                const row = list.itemAtIndex(i);
                if (row !== null && row.name === name) {
                    found = row;
                    return true;
                }
            }
            return false;
        }, 3000, "a row " + name);
        return found;
    }

    // Space ticks the current row's box, as a click on it does, and again
    // unticks it; the popup stays open and nothing is picked. Letters
    // still move the current row.
    function test_spaceTicksTheCurrentRowsBox() {
        const combo = openCheckable();
        keyClick(Qt.Key_Down);
        compare(currentName(combo), "Peak Time");
        keyClick(Qt.Key_Space);
        compare(toggledSpy.count, 1);
        compare(toggledSpy.signalArguments[0][0], "Peak Time");
        compare(toggledSpy.signalArguments[0][1], true);
        compare(rowNamed(combo, "Peak Time").checkState, Qt.Checked);
        compare(findChild(rowNamed(combo, "Peak Time"), "playlistRowCheck").checkState, Qt.Checked);
        compare(combo.typedFilter, "", "Space typed nothing");
        verify(combo.popup.visible, "still open");
        compare(pickedSpy.count, 0, "and picked nothing");
        keyClick(Qt.Key_Space);
        compare(toggledSpy.signalArguments[1][1], false, "Space again unticks");
        compare(rowNamed(combo, "Peak Time").checkState, Qt.Unchecked);
        // A letter still jumps, and Space right after it ticks that row:
        // one letter is no word to type a space into.
        type("w");
        compare(currentName(combo), "Warmup");
        keyClick(Qt.Key_Space);
        compare(toggledSpy.signalArguments[2][0], "Warmup");
        compare(combo.typedFilter, "", "the letter alone filters nothing, and Space added nothing");
        keyClick(Qt.Key_Space);
        compare(toggledSpy.count, 4, "the next Space ticks too");
        compare(toggledSpy.signalArguments[3][0], "Warmup");
        compare(toggledSpy.signalArguments[3][1], false);
        // All tracks' box stands for "".
        keyClick(Qt.Key_Escape);
        combo.popup.close();
        tryVerify(() => !combo.popup.visible);
        mouseClick(combo);
        tryVerify(() => combo.popup.opened, 3000);
        keyClick(Qt.Key_Space);
        compare(toggledSpy.signalArguments[4][0], "", "the first row's box is All tracks'");
        combo.popup.close();
    }

    // Without boxes, Space is the type-ahead's, as before.
    function test_spaceTypesWithoutBoxes() {
        const combo = openCrates({typeAheadInterval: 2000});
        toggledSpy.target = combo;
        toggledSpy.clear();
        type("p");
        keyClick(Qt.Key_Space);
        type("t");
        compare(combo.typedFilter, "p t");
        compare(toggledSpy.count, 0);
        combo.popup.close();
    }

    // Space on a row whose box is off does nothing, and neither does a
    // click on that box: it does not pick the row or close the popup.
    function test_aBoxThatIsOffTakesNoClickOrSpace() {
        const combo = openCheckable({checksEnabled: false});
        const row = rowNamed(combo, "Warmup");
        const box = findChild(row, "playlistRowCheck");
        verify(box.visible && !box.enabled);
        mouseClick(box);
        wait(50);
        compare(pickedSpy.count, 0, "the click picks nothing");
        verify(combo.popup.visible, "and leaves the combo open");
        compare(toggledSpy.count, 0);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Space);
        compare(toggledSpy.count, 0, "Space ticks nothing");
        // One playlist's box off, the others on.
        combo.checksEnabled = true;
        testCase.offBoxes = ["Warmup"];
        combo.checkEnabledFor = name => testCase.offBoxes.indexOf(name) < 0;
        compare(findChild(rowNamed(combo, "Warmup"), "playlistRowCheck").enabled, false);
        mouseClick(findChild(rowNamed(combo, "Warmup"), "playlistRowCheck"));
        wait(50);
        compare(pickedSpy.count, 0);
        compare(toggledSpy.count, 0);
        // The name still picks.
        mouseClick(findChild(rowNamed(combo, "Warmup"), "pickerRowName"));
        tryVerify(() => pickedSpy.count === 1, 3000);
        tryVerify(() => !combo.popup.visible, 3000);
    }

    // A screen reader hears the playlist a box ticks and how much of it.
    function test_aBoxSaysItsPlaylistAndState() {
        const combo = openCheckable();
        const box = () => findChild(rowNamed(combo, "Deep House"), "playlistRowCheck");
        compare(box().Accessible.name, "Deep House: no tracks ticked");
        mouseClick(box());
        compare(box().Accessible.name, "Deep House: all tracks ticked");
        compare(findChild(rowNamed(combo, "All tracks"), "playlistRowCheck").Accessible.name,
                "All tracks: no tracks ticked");
        combo.checkStateOf = name => name === "" ? Qt.PartiallyChecked : Qt.Unchecked;
        compare(findChild(rowNamed(combo, "All tracks"), "playlistRowCheck").Accessible.name,
                "All tracks: some tracks ticked");
        combo.popup.close();
    }

    // A box clicked keeps the keys the combo's: typing still filters,
    // Space ticks the current row, Enter picks it, and Escape closes with
    // the focus back on the combo.
    function test_aClickedBoxLeavesTheKeysToTheCombo() {
        const combo = openCheckable({typeAheadInterval: 300});
        const box = findChild(rowNamed(combo, "Deep House"), "playlistRowCheck");
        mouseClick(box);
        compare(toggledSpy.count, 1);
        verify(!box.activeFocus, "the box took no focus");
        type("wa");
        compare(combo.typedFilter, "wa", "the letters reach the combo");
        compare(shownNames(combo), "Warmup, Warehouse");
        compare(currentName(combo), "Warmup");
        wait(450);  // past the type-ahead's pause: Space ticks
        keyClick(Qt.Key_Space);
        compare(toggledSpy.count, 2);
        compare(toggledSpy.signalArguments[1][0], "Warmup", "Space ticks the current row");
        keyClick(Qt.Key_Return);
        compare(pickedSpy.count, 1);
        compare(pickedSpy.signalArguments[0][1].name, "Warmup", "Enter picks it");
        tryVerify(() => !combo.popup.visible, 2000);

        mouseClick(combo);
        tryVerify(() => combo.popup.opened, 3000);
        mouseClick(findChild(rowNamed(combo, "Peak Time"), "playlistRowCheck"));
        keyClick(Qt.Key_Escape);
        tryVerify(() => !combo.popup.visible, 2000, "Escape closes");
        verify(combo.activeFocus, "the focus is the combo's again");
    }

    // With boxes, Space in the middle of a name is typed; after the pause
    // it ticks again.
    function test_spaceInANameIsTyped() {
        const combo = openCheckable({typeAheadInterval: 2000});
        type("deep");
        keyClick(Qt.Key_Space);
        type("house");
        compare(combo.typedFilter, "deep house");
        compare(shownNames(combo), "Deep House");
        compare(currentName(combo), "Deep House");
        compare(toggledSpy.count, 0, "nothing ticked");
        combo.typeAheadInterval = 100;
        type("x");  // restarts the pause at the shorter interval
        keyClick(Qt.Key_Backspace);
        wait(250);
        keyClick(Qt.Key_Space);
        compare(toggledSpy.count, 1, "after the pause, Space ticks");
        compare(toggledSpy.signalArguments[0][0], "Deep House");
        combo.popup.close();
    }

    // A box that is off says why, on hover, though it hears no hover of
    // its own; without a reason it says its state.
    function test_aBoxThatIsOffSaysWhy() {
        const combo = openCheckable({checksEnabled: false, checkOffReason: name => "Unstage first: " + name});
        const box = findChild(rowNamed(combo, "Warmup"), "playlistRowCheck");
        verify(!box.enabled);
        compare(box.ToolTip.text, "Unstage first: Warmup");
        mouseMove(box, box.width / 2, box.height / 2);
        tryVerify(() => box.ToolTip.visible, 3000, "the reason shows on hover");
        tryVerify(() => box.ToolTip.toolTip.visible && box.ToolTip.toolTip.text === "Unstage first: Warmup", 3000);
        mouseMove(findChild(rowNamed(combo, "Warmup"), "pickerRowName"), 5, 5);
        tryVerify(() => !box.ToolTip.visible, 3000, "and goes off the box");
        combo.checkOffReason = name => "";
        compare(box.ToolTip.text, "No tracks of this playlist are ticked. Click to tick them all.");
        combo.checksEnabled = true;
        compare(box.ToolTip.text, "No tracks of this playlist are ticked. Click to tick them all.");
        combo.popup.close();
    }

    // A press on a box that is off does not shade the row as a press on
    // the row does.
    function test_aBoxThatIsOffDoesNotFlash() {
        const combo = openCheckable({checksEnabled: false});
        const row = rowNamed(combo, "Warmup");
        const box = findChild(row, "playlistRowCheck");
        mousePress(box);
        verify(!Qt.colorEqual(row.color, Theme.rowPressed), "no pressed shade on the box's press");
        mouseRelease(box);
        const name = findChild(row, "pickerRowName");
        mousePress(name, 5, 5);
        verify(Qt.colorEqual(row.color, Theme.rowPressed), "the name's press shades, as before");
        mouseRelease(name, 5, 5);
        tryVerify(() => !combo.popup.visible, 2000);
    }

    // The text of every row in sight, as drawn: the window's own frame
    // (the popup is in its overlay), read under each row's name label,
    // must hold pixels unlike the row's ground beside it.
    function checkRowsDrawn(list, labelName, what) {
        // The list's own window: the overlay of the test's window, or a
        // window of the popup's own where popups are native windows.
        const ground = list.Window.contentItem;
        const image = grabImage(ground);
        // Device pixels per logical one (PixelScale.js): 2 on a Retina Mac.
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
            // Ink is measured against the label's own ground, the commonest
            // shade under it, not a pixel taken beside it: whatever paints
            // the row (a style's own shading, a hover, the current row's
            // outline) is then what the text is told apart from.
            const origin = name.mapToItem(ground, 0, 0);
            const shades = [];
            for (let y = 0; y < name.height; y += 1 / s) {
                for (let x = 0; x < Math.min(name.width, 60); x += 1 / s) {
                    shades.push(lum(PixelScale.pixel(image, ground, origin.x + x, origin.y + y)));
                }
            }
            const sorted = shades.slice().sort((p, q) => p - q);
            const back = sorted[Math.floor(sorted.length / 2)];
            const ink = shades.filter(v => Math.abs(v - back) > 0.15).length;
            if (ink < 3) {
                // What a run elsewhere (a Mac) needs to say why: the grab,
                // and the numbers the sampling worked from.
                const file = screenshotDir && screenshotDir.length > 0
                    ? screenshotDir + "/blank-row-combo-" + what.replace(/[^A-Za-z0-9]+/g, "-") + "-row" + i + ".png" : "";
                if (file.length > 0) {
                    image.save(file);
                }
                fail(what + ": row " + i + " (" + name.text + ") shows no text: " + ink + " pixels of ink in "
                     + shades.length + " sampled, shades " + sorted[0].toFixed(3) + " to "
                     + sorted[sorted.length - 1].toFixed(3) + ", label at " + origin.x + "," + origin.y + " "
                     + name.width + "x" + name.height + ", grab " + image.width + "x" + image.height + " of "
                     + ground.width + "x" + ground.height + " (scale " + s + ", dpr " + Screen.devicePixelRatio
                     + "), popup window " + (list.Window.window === testCase.Window.window ? "the test's" : "its own")
                     + (file.length > 0 ? ", saved " + file : ""));
            }
            ++looked;
        }
        verify(looked > 4, what + ": rows looked at " + looked);
    }

    // Entries that carry a thumbnail show it left of the name, on every
    // row (an empty one as the neutral tile); a model without shows none.
    function test_rowsShowThumbnailsWhenTheModelHasThem() {
        const plain = createTemporaryObject(comboComponent, testCase, {model: testModel});
        verify(!plain.showsThumbnails);
        const withThumbs = [
            {name: "All tracks", count: 42, thumbnail: browseFixture.colouredArtworkUrl("#c02020")},
            {name: "Techno", count: 12, thumbnail: ""},
        ];
        const combo = createTemporaryObject(comboComponent, testCase, {model: withThumbs});
        verify(combo.showsThumbnails);
        mouseClick(combo);
        tryVerify(() => combo.popup.visible && combo.popup.contentItem.itemAtIndex(1) !== null);
        const first = combo.popup.contentItem.itemAtIndex(0);
        const second = combo.popup.contentItem.itemAtIndex(1);
        const tile = findChild(first, "playlistThumbnail").parent;
        compare(tile.visible, true);
        compare(findChild(first, "playlistThumbnail").source.toString(), withThumbs[0].thumbnail);
        compare(findChild(second, "playlistThumbnailGlyph").visible, true, "no covers: the neutral tile");
        combo.popup.close();
    }
}
