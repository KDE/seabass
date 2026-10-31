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

    function test_theArrowsMoveTheSelectionAndReturnPressesIt() {
        const dialog = createTemporaryObject(plainFooterComponent, testCase);
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "opened", true);
        const first = findChild(dialog.footer, "first");
        const second = findChild(dialog.footer, "second");
        // Which button the dialog takes as its default is its own rule
        // (SeabassDialog.focusDefaultFooterButton); what is tested here is
        // the walk from wherever it starts.
        tryVerify(() => first.activeFocus || second.activeFocus, 5000, "the dialog focused a button");
        first.forceActiveFocus();
        tryCompare(first, "activeFocus", true);
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
        tryVerify(() => first.activeFocus || second.activeFocus, 5000, "the dialog focused a button first");
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
