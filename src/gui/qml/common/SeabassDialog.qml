// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The shared frame every Seabass dialog wears, in the shape KDE's
// KMessageBox settled on: a severity badge on the left, the question in
// the title bar, the consequence first and the reassurance under it, and
// the buttons on the right named by their verb.
//
// This carries chrome only. It has no buttons of its own and no idea what
// the dialog is for -- MessageDialog sits on top of it for the ordinary
// cases, and the purpose-built dialogs (StickRemoved, LockedLibrary,
// UnsavedChanges...) derive from it directly and keep their own logic and
// their own footer.
//
//   SeabassDialog {
//       severity: SeabassDialog.Warning
//       title: "USB stick removed"
//       headline: "WHALESHARK has been removed while editing."
//       detailText: "Either discard changes or plug the stick back in."
//       footer: DialogButtonBox { ... }
//   }
//
// Anything declared as a child is placed under the message, indented to
// the text column so it lines up rather than sitting under the badge.
Dialog {
    id: root

    enum Severity {
        Info,
        Warning,
        Error,
        Question,
        // "It worked" is a result too, and the one case where a dialog is
        // worth putting in someone's way only to reassure them: a repair
        // that ran while the stick vanished and came back needs an
        // unambiguous ending.
        Success
    }

    property int severity: SeabassDialog.Info
    // The consequence, in one sentence. The title bar carries the question.
    property string headline: ""
    // The qualifier under it, quieter. Optional.
    property string detailText: ""
    // A progress dialog has no severity to report; everything else does.
    property bool showBadge: true
    // Only ever offered on a question -- never on a destructive action and
    // never on an error, where "do not tell me again" is not a safe answer.
    property bool dontAskVisible: false
    property alias dontAskChecked: dontAskBox.checked

    // Re-based dialogs keep whatever names their tests already look for.
    property string messageObjectName: "messageLabel"
    property string detailObjectName: "messageDetailLabel"

    default property alias extraContent: extraColumn.data

    readonly property color severityColor: {
        switch (root.severity) {
        case SeabassDialog.Warning: return Theme.warnIcon;
        case SeabassDialog.Error: return Theme.danger;
        case SeabassDialog.Question: return Theme.accent;
        case SeabassDialog.Success: return Theme.good;
        default: return Theme.info;
        }
    }
    readonly property string severityGlyph: {
        switch (root.severity) {
        case SeabassDialog.Warning: return "!";
        case SeabassDialog.Error: return "✕";
        case SeabassDialog.Question: return "?";
        case SeabassDialog.Success: return "\u2713";
        default: return "i";
        }
    }

    anchors.centerIn: parent
    modal: true
    width: 520
    padding: Theme.cardPadding

    // --- One palette for the whole dialog ---
    //
    // The message, the badge and the content are drawn in Theme colours,
    // so the frame, the title and the button row are too. Left to the
    // style, they followed the system colour scheme instead: on a light
    // scheme the frame came out white under Kelp's light grey text, and
    // the headline was all but invisible; under Material rendered
    // offscreen the body had no background at all, so the message sat on
    // top of whatever the dialog covered. Only the buttons stay the
    // style's, like every other button in the app.
    background: Rectangle {
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        radius: Theme.scaled(6)
    }
    header: Label {
        text: root.title
        visible: text.length > 0
        color: Theme.text
        font.pointSize: Theme.fontMedium
        elide: Text.ElideRight
        leftPadding: Theme.cardPadding
        rightPadding: Theme.cardPadding
        topPadding: Theme.cardPadding
    }

    // Every Seabass dialog puts its buttons on the right, inset from the
    // edge. Enforced here rather than left to each footer, so a derived
    // dialog that supplies its own DialogButtonBox cannot quietly get it
    // wrong -- and several did: the buttons sat flush against the
    // dialog's right border with the frame drawn right up against them.
    //
    // The padding is applied alongside the alignment because they are
    // the same decision. Aligning right without insetting just moves the
    // problem to the other edge.
    function alignFooter() {
        if (!root.footer) {
            return;
        }
        if (root.footer.alignment !== undefined) {
            root.footer.alignment = Qt.AlignRight | Qt.AlignVCenter;
        }
        // The frame above is the dialog's one background; a style that
        // paints the button row as a band of its own would split it.
        // Hidden rather than replaced: assigning null (or another item)
        // orphans the style's background, whose own bindings to its parent
        // then throw on every layout pass (Fusion does exactly that).
        if (root.footer.background) {
            root.footer.background.visible = false;
        }
        if (root.footer.rightPadding !== undefined) {
            root.footer.rightPadding = Theme.scaled(16);
            root.footer.bottomPadding = Theme.scaled(16);
            // A little above them too, so the buttons read as separate
            // from the message rather than as the end of it.
            root.footer.topPadding = Theme.scaled(8);
        }
    }
    onFooterChanged: { root.alignFooter(); root.wireFooterKeys(); }
    Component.onCompleted: { root.alignFooter(); root.wireFooterKeys(); }

    // --- Keyboard: every dialog answers the same way ---
    //
    // Return presses the selected button, Left/Right move the selection,
    // Escape cancels (closePolicy, which for a Dialog is reject() -- the
    // same thing the Cancel button does, so a dialog dismissed either way
    // is indistinguishable to whatever is waiting on the answer).
    //
    // Selection IS keyboard focus, and the highlight follows it. If the
    // arrows moved focus without moving the highlight, the dialog would
    // be showing one answer while Return performed another.
    //
    // The keys are taken on the footer's BUTTONS, wired here once for
    // every dialog, never on the dialog itself: a dialog with a text
    // field in it (TypedConfirmDialog asks you to type a word) must let
    // Left/Right move the cursor, and a key the field takes never reaches
    // a button. On the buttons, not on the footer either: under every
    // style but Basic the DialogButtonBox's own list view takes the
    // arrows before the box sees them. Wired from here rather than
    // declared on each button, because each button declaring four
    // handlers is how several footers came to declare none (Browse's
    // playlist dialogs, the software-running and eject-held questions)
    // and their arrows went nowhere. A footer declaring them as well
    // would move the selection twice per press, so none does.
    property Item wiredFooter: null
    property var wiredButtons: []
    function wireFooterKeys() {
        const box = root.footer;
        if (!box || box.contentChildren === undefined) {
            return;
        }
        if (box !== root.wiredFooter) {
            root.wiredFooter = box;
            root.wiredButtons = [];
            box.contentChildrenChanged.connect(root.wireFooterKeys);
        }
        for (let i = 0; i < box.contentChildren.length; ++i) {
            const button = box.contentChildren[i];
            if (!button || button.Keys === undefined || root.wiredButtons.indexOf(button) >= 0) {
                continue;
            }
            root.wiredButtons.push(button);
            // A footer declaring its default (`highlighted: true`) has it
            // remembered, and every button keeps to the remembered one
            // (markFooterDefault).
            if (button.highlighted === true && root.footerDefaultButton === null) {
                root.markFooterDefault(button);
            }
            button.highlightedChanged.connect(() => root.keepFooterDefault(button));
            root.keepFooterDefault(button);
            button.Keys.leftPressed.connect((event) => { root.moveFooterSelection(-1); event.accepted = true; });
            button.Keys.rightPressed.connect((event) => { root.moveFooterSelection(1); event.accepted = true; });
            button.Keys.returnPressed.connect((event) => { root.activateFooterSelection(); event.accepted = true; });
            button.Keys.enterPressed.connect((event) => { root.activateFooterSelection(); event.accepted = true; });
        }
    }

    function footerButtons() {
        var box = root.footer;
        if (!box || box.contentChildren === undefined) {
            return [];
        }
        var out = [];
        for (var i = 0; i < box.contentChildren.length; ++i) {
            var b = box.contentChildren[i];
            if (b && b.visible && b.enabled !== false) {
                out.push(b);
            }
        }
        return out;
    }

    // The footer's default: `target` highlighted, every other button not,
    // a disabled one included (Create Playlist marks OK before a name is
    // typed). Remembered, and put back whenever a button's highlight
    // changes behind it: since Qt 6.11 DialogButtonBox re-highlights its
    // buttons on every polish (a button joining, the alignment set, the
    // box shown) from a defaultButton of its own, so a highlight assigned
    // without that was wiped at the next layout. Its defaultButton is not
    // used instead: the box also hands that button the focus on every
    // polish, out of a text field the dialog asks to be typed in.
    // Assigned, not bound: a binding on `highlighted` is discarded too.
    property Item footerDefaultButton: null
    function markFooterDefault(target) {
        root.footerDefaultButton = target;
        const box = root.footer;
        const buttons = box && box.contentChildren !== undefined ? box.contentChildren : [];
        for (let i = 0; i < buttons.length; ++i) {
            root.keepFooterDefault(buttons[i]);
        }
        root.keepFooterDefault(target);
    }
    function keepFooterDefault(button) {
        if (!button || button.highlighted === undefined || root.footerDefaultButton === null) {
            return;
        }
        const wanted = button === root.footerDefaultButton;
        if (button.highlighted !== wanted) {
            button.highlighted = wanted;
        }
    }

    function selectFooterButton(target) {
        root.markFooterDefault(target);
        if (target) {
            target.forceActiveFocus();
        }
    }

    function moveFooterSelection(delta) {
        var list = root.footerButtons();
        if (list.length < 2) {
            return;
        }
        var index = 0;
        for (var i = 0; i < list.length; ++i) {
            if (list[i].activeFocus) {
                index = i;
            }
        }
        root.selectFooterButton(list[(index + delta + list.length) % list.length]);
    }

    function activateFooterSelection() {
        var list = root.footerButtons();
        for (var i = 0; i < list.length; ++i) {
            if (list[i].activeFocus) {
                list[i].clicked();
                return;
            }
        }
        if (list.length > 0) {
            list[0].clicked();
        }
    }

    // Whatever the dialog marked as its default, so Return does the
    // expected thing the instant it opens without anyone having to click
    // into the dialog first.
    function focusDefaultFooterButton() {
        var list = root.footerButtons();
        for (var i = 0; i < list.length; ++i) {
            if (list[i].highlighted) {
                list[i].forceActiveFocus();
                return;
            }
        }
        // Nothing marked as default: take the LAST button rather than the
        // first. Every footer here declares the affirmative action first
        // and the way out last, so the last one is the safe answer, and a
        // dialog that failed to mark a default must not hand Return to
        // the destructive button by accident.
        if (list.length > 0) {
            root.selectFooterButton(list[list.length - 1]);
        }
    }
    onOpened: Qt.callLater(root.focusDefaultFooterButton)

    RowLayout {
        width: parent.width
        spacing: 14

        Rectangle {
            objectName: "severityBadge"
            visible: root.showBadge
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: Theme.iconSizeSmall
            Layout.preferredHeight: Theme.iconSizeSmall
            radius: width / 2
            color: Qt.rgba(root.severityColor.r, root.severityColor.g, root.severityColor.b, 0.14)
            border.width: 1
            border.color: Qt.rgba(root.severityColor.r, root.severityColor.g, root.severityColor.b, 0.45)

            Label {
                anchors.centerIn: parent
                text: root.severityGlyph
                color: root.severityColor
                font.bold: true
                font.pointSize: Theme.fontMedium
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                objectName: root.messageObjectName
                Layout.fillWidth: true
                visible: root.headline.length > 0
                text: root.headline
                color: Theme.text
                wrapMode: Text.WordWrap
            }

            // Selectable: an Error dialog's detail is exactly the string
            // someone needs to quote back, and Label cannot be selected.
            SelectableText {
                objectName: root.detailObjectName
                visible: root.detailText.length > 0
                text: root.detailText
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }

            ColumnLayout {
                id: extraColumn
                Layout.fillWidth: true
                spacing: 8
            }

            SeabassCheckBox {
                id: dontAskBox
                objectName: "dontAskCheckBox"
                visible: root.dontAskVisible
                text: "Do not ask again"
                font.pointSize: Theme.fontSmall
            }
        }
    }
}
