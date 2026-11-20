// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A track's comment as text that turns into a field when clicked (#105).
// Hovering shows a small edit button at its end; clicking the text or the
// button edits it in place. Enter, or leaving the field, accepts; Escape
// puts the text back and accepts nothing. One line, as the players and
// rekordbox show a comment.
//
// The field never holds more than every catalog can store: `limit` (the
// controller's commentLimit) says how many units fit and whether only
// plain ASCII does, and anything typed or pasted past that is cut where
// it stops fitting. Close to the limit a counter says how close.
Item {
    id: root
    property string text: ""
    // The text is staged and not on the stick yet.
    property bool pending: false
    property bool editable: true
    // {known, limited, units, asciiOnly, reason}; see TrackEditController.
    property var limit: ({known: true, limited: false, units: 0, asciiOnly: false, reason: ""})
    readonly property bool limitKnown: root.limit !== null && root.limit.known === true
    readonly property bool limited: root.limitKnown && root.limit.limited === true
    readonly property int units: root.limited ? root.limit.units : 0
    // Nothing more fits than is there: the comment can be shortened or
    // cleared, and is otherwise only shown.
    readonly property bool noRoom: root.limited && root.units === 0
    readonly property bool canEdit: root.editable && root.limitKnown && !(root.noRoom && root.text.length === 0)
    readonly property bool editing: field.visible
    signal accepted(string text)

    implicitHeight: editing ? field.implicitHeight + (counter.visible ? counter.implicitHeight : 0)
                            : shown.implicitHeight
    implicitWidth: shown.implicitWidth

    // What fits of `text`: line breaks as spaces, a character an ASCII
    // field cannot hold ends it, and no more units than the limit (never
    // half of a character that takes two).
    function fitted(text) {
        let t = text.replace(/[\r\n\t]+/g, " ");
        if (root.limited && root.limit.asciiOnly) {
            const firstWide = t.search(/[^\x00-\x7F]/);
            if (firstWide >= 0) {
                t = t.slice(0, firstWide);
            }
        }
        if (root.limited && t.length > root.units) {
            t = t.slice(0, root.units);
            const last = t.charCodeAt(t.length - 1);
            if (last >= 0xD800 && last <= 0xDBFF) {
                t = t.slice(0, t.length - 1);
            }
        }
        return t;
    }

    function startEditing() {
        if (!root.canEdit || root.editing) {
            return;
        }
        field.text = root.text;
        field.lastGood = root.text;
        field.visible = true;
        field.forceActiveFocus();
        field.selectAll();
    }
    function finish(accept) {
        if (!root.editing) {
            return;
        }
        field.visible = false;
        if (accept && field.text !== root.text) {
            root.accepted(field.text);
        }
    }

    // The text as it is shown, or the muted invitation when there is none.
    RowLayout {
        id: shown
        visible: !root.editing
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.tightSpacing
        Label {
            id: shownText
            objectName: "commentText"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.text.length > 0 ? root.text : (root.noRoom ? "No comment" : "Add a comment")
            color: root.text.length > 0 ? (root.pending ? Theme.textMuted : Theme.text) : Theme.textMuted
            font.italic: root.pending || root.text.length === 0
            TapHandler {
                enabled: root.canEdit
                cursorShape: Qt.IBeamCursor
                onTapped: root.startEditing()
            }
        }
        IconToolButton {
            id: editButton
            objectName: "commentEditButton"
            text: "Edit comment"
            iconName: "document-edit"
            iconSize: Theme.scaled(16)
            iconColor: Theme.textMuted
            enabled: root.canEdit
            // Shown on hover only, to keep the pane light; kept in the
            // layout so the text does not reflow when it appears. Reached
            // with Tab as well, and then shown.
            opacity: hover.hovered || activeFocus ? 1 : 0
            focusPolicy: Qt.TabFocus
            Accessible.name: "Edit comment"
            onClicked: root.startEditing()
            ToolTip.visible: (hovered || hover.hovered) && root.limited && root.limit.reason.length > 0 && !root.canEdit
            ToolTip.text: root.limited ? root.limit.reason : ""
        }
    }
    HoverHandler {
        id: hover
        objectName: "commentHover"
    }
    ToolTip {
        objectName: "commentNoRoomTip"
        visible: hover.hovered && root.noRoom && !root.editing && root.limit.reason.length > 0
        delay: 400
        text: root.limited ? root.limit.reason : ""
    }

    TextField {
        id: field
        objectName: "commentField"
        visible: false
        anchors.left: parent.left
        anchors.right: parent.right
        placeholderText: "Add a comment"
        // The text last known to fit, to tell what an edit inserted.
        property string lastGood: ""
        Accessible.name: "Comment"
        // Any change that fits, the user's or the program's, is the base
        // the next edit is measured against.
        onTextChanged: {
            if (root.fitted(field.text) === field.text) {
                field.lastGood = field.text;
            }
        }
        onTextEdited: {
            if (root.fitted(field.text) === field.text) {
                field.lastGood = field.text;
                return;
            }
            // Keep what was there and as much of what was inserted
            // (typed or pasted) as fits.
            const before = field.lastGood;
            const now = field.text;
            let head = 0;
            while (head < before.length && head < now.length && before[head] === now[head]) {
                ++head;
            }
            let tail = 0;
            while (tail < before.length - head && tail < now.length - head
                   && before[before.length - 1 - tail] === now[now.length - 1 - tail]) {
                ++tail;
            }
            const inserted = now.slice(head, now.length - tail);
            const prefix = before.slice(0, head);
            const suffix = before.slice(before.length - tail);
            // Never half of a character that takes two units.
            const splitsPair = (n) => n > 0 && n < inserted.length
                && inserted.charCodeAt(n - 1) >= 0xD800 && inserted.charCodeAt(n - 1) <= 0xDBFF;
            const fits = (n) => root.fitted(prefix + inserted.slice(0, n) + suffix) === prefix + inserted.slice(0, n) + suffix;
            let take = root.fitted(inserted).length;
            while (take > 0 && (splitsPair(take) || !fits(take))) {
                --take;
            }
            const result = prefix + inserted.slice(0, take) + suffix;
            field.text = root.fitted(result) === result ? result : before;
            field.cursorPosition = Math.min(field.text.length, head + take);
            field.lastGood = field.text;
        }
        Keys.onEscapePressed: root.finish(false)
        onAccepted: root.finish(true)
        onActiveFocusChanged: {
            if (!activeFocus) {
                root.finish(true);
            }
        }
        ToolTip.visible: field.hovered && root.limited && root.limit.reason.length > 0
        ToolTip.delay: 600
        ToolTip.text: root.limited ? root.limit.reason : ""
    }
    // "58 of 64", only within ten of the limit: most comments never come
    // close, and a counter on every edit would weigh the pane down.
    Label {
        id: counter
        objectName: "commentCounter"
        visible: root.editing && root.limited && root.units - field.text.length <= 10
        anchors.top: field.bottom
        anchors.right: parent.right
        text: field.text.length + " of " + root.units
        color: Theme.textMuted
        font.pointSize: Theme.fontSmall
        font.family: Theme.dataFamily
    }
}
