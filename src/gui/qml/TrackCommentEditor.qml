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
// An edit belongs to the track it was opened for: the editor keeps that
// track (`target`) and its limit from the moment editing starts, and
// accepts against them, whatever the pane shows by then. The pane ends an
// open edit before it shows another track (endEdit()), and the page
// before it saves (commitOpenEdit()).
//
// The field never takes more than every catalog can store: `limit` (the
// controller's commentLimit) says how many units fit and whether only
// plain ASCII does. Typing or pasting past that keeps only what fits, and
// a character the field cannot hold is not taken; deleting is always
// allowed, so a comment that does not fit as it is can be shortened.
Item {
    id: root
    property string text: ""
    // The text is staged and not on the stick yet.
    property bool pending: false
    property bool editable: true
    // The track shown: {filePath, title, original, rekordboxPath,
    // enginePath}. Copied when editing starts.
    property var target: ({})
    // {known, editable, limited, units, asciiOnly, reason}; see
    // TrackEditController.
    property var limit: ({known: false, editable: false, limited: false, units: 0, asciiOnly: false, reason: ""})
    // Stages `text` for `target`; returns whether it did. The field stays
    // open with its text when it did not.
    property var commitFn: null

    readonly property bool limitKnown: root.limit !== null && root.limit.known === true
    readonly property bool noRoom: root.limitKnown && root.limit.limited === true && root.limit.units === 0
    readonly property bool canEdit: root.editable && root.limitKnown && root.limit.editable === true
        && !(root.noRoom && root.text.length === 0)
    readonly property bool editing: field.visible

    // Fixed when editing starts.
    property var editTarget: null
    property var editLimit: null
    property bool userEdited: false

    implicitHeight: editing ? field.implicitHeight + (counter.visible ? counter.implicitHeight : 0)
                            : shown.implicitHeight
    implicitWidth: shown.implicitWidth

    function startEditing() {
        if (!root.canEdit || root.editing) {
            return;
        }
        root.editTarget = Object.assign({}, root.target);
        root.editLimit = Object.assign({}, root.limit);
        root.userEdited = false;
        field.text = root.text;
        // Measured against what the field holds, which is the base every
        // edit is told from.
        field.lastGood = field.text;
        field.visible = true;
        field.forceActiveFocus();
        field.selectAll();
    }
    // Ends the edit: `accept` stages what the user changed (nothing, when
    // the field was only opened). Returns false, the field still open,
    // when staging was refused.
    function finish(accept) {
        if (!root.editing) {
            return true;
        }
        // Only what the user did: a field merely opened and left stages
        // nothing, whatever the widget made of the text it was handed.
        if (accept && root.userEdited && root.commitFn !== null) {
            if (!root.commitFn(field.text, root.editTarget)) {
                return false;
            }
        }
        field.visible = false;
        root.editTarget = null;
        return true;
    }
    // Before another track is shown: what was typed goes to the track it
    // was typed for when it can, and is dropped when it cannot.
    // Returns "" when nothing was lost, else the title of the track whose
    // typed comment could not be staged.
    function endEdit() {
        const title = root.editTarget !== null ? root.editTarget.title : "";
        if (!root.finish(true)) {
            root.finish(false);
            return title;
        }
        return "";
    }
    // Before a save: staged if it can be; left open, with its text, if not.
    function commitOpenEdit() {
        return root.finish(true);
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
            text: root.text.length > 0 ? root.text
                : root.limit !== null && root.limit.reading === true ? "Reading the stick"
                : root.noRoom ? "No comment" : "Add a comment"
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
            // Not there at all where the comment is only shown (editable
            // false): a disabled pencil would promise an edit.
            visible: root.editable
            // Shown on hover only, to keep the pane light; kept in the
            // layout so the text does not reflow when it appears. Reached
            // with Tab as well, and then shown.
            opacity: hover.hovered || activeFocus ? 1 : 0
            focusPolicy: Qt.TabFocus
            Accessible.name: "Edit comment"
            onClicked: root.startEditing()
        }
    }
    HoverHandler {
        id: hover
        objectName: "commentHover"
    }
    ToolTip {
        objectName: "commentNoRoomTip"
        visible: hover.hovered && !root.editing && root.limitKnown && !root.canEdit && text.length > 0
        delay: 400
        text: !root.limitKnown ? "" : root.limit.editable !== true ? root.limit.refusal : root.limit.reason
    }

    TextField {
        id: field
        objectName: "commentField"
        visible: false
        anchors.left: parent.left
        anchors.right: parent.right
        placeholderText: "Add a comment"
        // What the field held after the last edit it took, to tell what
        // the next one inserted.
        property string lastGood: ""
        Accessible.name: "Comment"
        onTextEdited: {
            const before = field.lastGood;
            const now = field.text;
            if (now === before) {
                return;
            }
            let head = 0;
            while (head < before.length && head < now.length && before[head] === now[head]) {
                ++head;
            }
            let tail = 0;
            while (tail < before.length - head && tail < now.length - head
                   && before[before.length - 1 - tail] === now[now.length - 1 - tail]) {
                ++tail;
            }
            const prefix = before.slice(0, head);
            const suffix = before.slice(before.length - tail);
            // What was typed or pasted, one line, and of it only the
            // characters the field can hold.
            let inserted = now.slice(head, now.length - tail).replace(/[\r\n\t]+/g, " ");
            const limit = root.editLimit;
            const limited = limit !== null && limit.limited === true;
            if (limited && limit.asciiOnly === true) {
                const wide = inserted.search(/[^\x00-\x7F]/);
                if (wide >= 0) {
                    inserted = inserted.slice(0, wide);
                }
            }
            // As much of it as fits, never half of a character that takes
            // two units. With nothing inserted the edit only deleted, and
            // a deletion is always taken.
            const splitsPair = (n) => n > 0 && n < inserted.length
                && inserted.charCodeAt(n - 1) >= 0xD800 && inserted.charCodeAt(n - 1) <= 0xDBFF;
            let take = inserted.length;
            while (take > 0 && (splitsPair(take) || (limited && prefix.length + take + suffix.length > limit.units))) {
                --take;
            }
            const result = prefix + inserted.slice(0, take) + suffix;
            if (result !== now) {
                field.text = result;
                field.cursorPosition = head + take;
            }
            field.lastGood = result;
            root.userEdited = true;
        }
        Keys.onEscapePressed: root.finish(false)
        onAccepted: root.finish(true)
        onActiveFocusChanged: {
            if (!activeFocus) {
                root.finish(true);
            }
        }
        ToolTip.visible: field.hovered && root.editLimit !== null && root.editLimit.limited === true
            && root.editLimit.reason.length > 0
        ToolTip.delay: 600
        ToolTip.text: root.editLimit !== null ? root.editLimit.reason : ""
    }
    // "58 of 64", only within ten of the limit: most comments never come
    // close, and a counter on every edit would weigh the pane down.
    Label {
        id: counter
        objectName: "commentCounter"
        visible: root.editing && root.editLimit !== null && root.editLimit.limited === true
            && root.editLimit.units - field.text.length <= 10
        anchors.top: field.bottom
        anchors.right: parent.right
        text: root.editLimit !== null ? field.text.length + " of " + root.editLimit.units : ""
        color: Theme.textMuted
        font.pointSize: Theme.fontSmall
        font.family: Theme.dataFamily
    }
}
