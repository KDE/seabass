// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// What every dialog built on SeabassDialog answers to the keyboard, with
// a footer of plain buttons that declare no key handlers of their own:
// the arrows move the selection (focus and highlight together), Return
// presses it, and a text field in the body keeps its arrows.
TestCase {
    id: testCase
    name: "SeabassDialog"
    width: 600
    height: 400
    visible: true
    when: windowShown

    Component {
        id: plainFooterComponent
        SeabassDialog {
            title: "Plain footer"
            property int firstClicks: 0
            property int secondClicks: 0
            footer: DialogButtonBox {
                Button { objectName: "first"; text: "First"; highlighted: true; onClicked: ++firstClicks }
                Button { objectName: "second"; text: "Second"; onClicked: ++secondClicks }
            }
        }
    }

    Component {
        id: fieldComponent
        SeabassDialog {
            title: "A field in the body"
            TextField { objectName: "field"; text: "abc" }
            footer: DialogButtonBox {
                Button { objectName: "first"; text: "First"; highlighted: true }
                Button { objectName: "second"; text: "Second" }
            }
        }
    }

    Component {
        id: disabledDefaultComponent
        SeabassDialog {
            title: "A default that is not enabled yet"
            TextField { objectName: "field" }
            footer: DialogButtonBox {
                Button { objectName: "ok"; text: "OK"; enabled: false }
                Button { objectName: "cancel"; text: "Cancel" }
            }
        }
    }

    // A relayout of the button box, as a button joining it, its alignment
    // changing or it being shown again would cause.
    function relayout(box) {
        const was = box.alignment;
        box.alignment = Qt.AlignLeft | Qt.AlignVCenter;
        wait(50);
        box.alignment = was;
        wait(50);
    }

    // The default survives a relayout of the box. Since Qt 6.11 the box
    // re-highlights on every polish from its own defaultButton, and a
    // highlight assigned without it was wiped: the Create Playlist
    // dialog lost OK as its default whenever the box laid itself out
    // again after opening.
    function test_theDefaultSurvivesARelayout() {
        const dialog = createTemporaryObject(plainFooterComponent, testCase);
        dialog.open();
        tryCompare(dialog, "opened", true);
        const first = findChild(dialog.footer, "first");
        const second = findChild(dialog.footer, "second");
        tryCompare(first, "activeFocus", true, 5000, "the declared default takes the focus");
        relayout(dialog.footer);
        compare(first.highlighted, true, "the declared default, after a relayout");
        compare(second.highlighted, false);
        // The relayout also takes the keyboard focus out of the footer
        // (Qt 6.11's box focuses only a default of its own); given back,
        // the arrows walk on from the default.
        first.forceActiveFocus();
        keyClick(Qt.Key_Right);
        compare(second.highlighted, true);
        relayout(dialog.footer);
        compare(second.highlighted, true, "the selection, after a relayout");
        compare(first.highlighted, false);
        dialog.close();

        // A default marked while it cannot be pressed yet (no name typed):
        // marked all the same, and kept.
        const asking = createTemporaryObject(disabledDefaultComponent, testCase);
        asking.open();
        tryCompare(asking, "opened", true);
        const ok = findChild(asking.footer, "ok");
        const cancel = findChild(asking.footer, "cancel");
        // After the dialog's own pick on opening (the last enabled button),
        // as Create Playlist marks it.
        tryCompare(cancel, "activeFocus", true);
        asking.markFooterDefault(ok);
        relayout(asking.footer);
        compare(ok.highlighted, true, "the disabled default stays marked");
        compare(cancel.highlighted, false);
        asking.close();
    }

    function test_theArrowsMoveTheSelectionAndReturnPressesIt() {
        const dialog = createTemporaryObject(plainFooterComponent, testCase);
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "opened", true);
        const first = findChild(dialog.footer, "first");
        const second = findChild(dialog.footer, "second");
        // The declared default takes the focus on opening, and the walk
        // starts there.
        tryCompare(first, "activeFocus", true, 5000, "the declared default is focused");
        compare(first.highlighted, true);
        keyClick(Qt.Key_Right);
        compare(second.activeFocus, true, "Right moves the focus to the next button");
        compare(second.highlighted, true, "and the highlight goes with it");
        compare(first.highlighted, false);
        keyClick(Qt.Key_Left);
        compare(first.activeFocus, true, "Left moves it back");
        compare(first.highlighted, true);
        keyClick(Qt.Key_Right);
        keyClick(Qt.Key_Return);
        compare(dialog.secondClicks, 1, "Return presses the selected button");
        compare(dialog.firstClicks, 0);
        dialog.close();
    }

    function test_aFieldInTheBodyKeepsItsArrows() {
        const dialog = createTemporaryObject(fieldComponent, testCase);
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "opened", true);
        const field = findChild(dialog, "field");
        const first = findChild(dialog.footer, "first");
        const second = findChild(dialog.footer, "second");
        tryCompare(first, "activeFocus", true, 5000, "the declared default is focused first");
        field.forceActiveFocus();
        tryCompare(field, "activeFocus", true);
        field.cursorPosition = 3;
        keyClick(Qt.Key_Left);
        compare(field.cursorPosition, 2, "Left moved the cursor");
        compare(field.activeFocus, true, "and the focus stayed in the field");
        compare(first.activeFocus, false);
        dialog.close();
    }
}
