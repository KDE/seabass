// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Text a person needs to copy, not merely read.
//
// Label cannot be selected: it is a Text, and Text has no selection at
// all. Every error this app has ever shown was therefore unreachable by
// mouse -- someone hitting a failure had to retype it, or screenshot it,
// to report it. For a message whose whole purpose is to be sent to
// someone else, that is the wrong end of the trade.
//
// A read-only TextEdit is the smallest thing that fixes it and still
// lays out like a Label: same wrapping, same colour, same font. It does
// not take focus by click-through the way an editable field would, and
// it shows the I-beam only over text that is actually there.
TextEdit {
    id: selectable

    readOnly: true
    selectByMouse: true
    // Without this the caret blinks in a field nobody can type into.
    cursorVisible: false
    activeFocusOnPress: true
    wrapMode: TextEdit.Wrap
    textFormat: TextEdit.PlainText

    color: Theme.text
    font.pointSize: Theme.fontNormal
    Layout.fillWidth: true

    // Ctrl+C without needing a selection first: a failure message is
    // read as one thing, so copying it should be one gesture.
    Keys.onPressed: function(event) {
        if (event.matches(StandardKey.Copy) && !selectable.selectedText) {
            selectable.selectAll();
            selectable.copy();
            selectable.deselect();
            event.accepted = true;
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.IBeamCursor
        onClicked: {
            selectable.selectAll();
            selectable.copy();
            selectable.deselect();
            copiedHint.show();
        }
    }

    // Right-click copies the whole message; say so, because a silent
    // clipboard write is indistinguishable from a click that did nothing.
    ToolTip {
        id: copiedHint
        text: "Copied"
        timeout: 1200
        function show() { visible = true; }
    }
}
