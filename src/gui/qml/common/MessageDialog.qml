// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The ordinary case, on top of SeabassDialog's frame: a message, a verb,
// and a way out. This is what the hand-rolled `Dialog { title; footer;
// Label }` blocks scattered through the pages become.
//
//   MessageDialog {
//       severity: SeabassDialog.Warning
//       title: "Delete This Backup?"
//       headline: "This permanently deletes this one backup copy."
//       detailText: "It never touches the stick's live data."
//       acceptText: "Delete"
//       destructive: true
//       onAccepted: backupsController.deleteBackup(targetId)
//   }
//
// `destructive` is the important one. It marks the action button, and it
// moves the default to Cancel -- so Return cancels rather than deletes.
// Several of the dialogs this replaces had Delete on AcceptRole, which
// made the destructive action the default.
SeabassDialog {
    id: root

    property string acceptText: "OK"
    property string rejectText: "Cancel"
    // An acknowledgement rather than a decision: OK on its own.
    property bool showReject: true
    property bool destructive: false
    // Ported dialogs keep whatever objectName their tests already look for,
    // the way TypedConfirmDialog does.
    property string acceptObjectName: "acceptButton"
    property string rejectObjectName: "rejectButton"

    // Escape cancels, a click outside does not. Cancelling is reject(),
    // the same thing the Cancel button does, so a dialog dismissed by
    // Escape and one dismissed by the button are indistinguishable to
    // whatever is waiting on the answer. The dialogs this replaces
    // mostly took Qt's default, which dismissed them on an outside click --
    // easy to do by accident when the answer matters.
    closePolicy: Popup.CloseOnEscape

    // DialogButtonBox writes to its buttons' `highlighted` itself, which
    // breaks a binding declared on it -- both buttons come out unhighlighted
    // whatever the expression says. So the default button is assigned rather
    // than bound, and re-assigned on open, because the box writes to it again
    // every time the dialog is shown. Caught by a screenshot: the property
    // read correctly right after construction and was gone by the time
    // anyone could see it.
    function applyDefaultButton() {
        // Assigned directly by id, NOT through the shared
        // selectFooterButton(): that reads footer.contentChildren, which
        // is empty while Component.onCompleted runs, so routing this
        // through it left a destructive dialog with neither button
        // highlighted -- Return would then have fallen to whatever the
        // base class guessed. Caught by tst_MessageDialog, which asserts
        // the default before the dialog is even opened.
        acceptButton.highlighted = !(root.destructive && root.showReject);
        rejectButton.highlighted = root.destructive && root.showReject;
    }
    onDestructiveChanged: root.applyDefaultButton()
    Component.onCompleted: root.applyDefaultButton()
    // Both, explicitly: a handler declared here REPLACES the one
    // SeabassDialog declares for the same signal, so the base class's
    // focus call has to be repeated rather than assumed.
    onOpened: {
        root.applyDefaultButton();
        Qt.callLater(root.focusDefaultFooterButton);
    }

    footer: DialogButtonBox {
        alignment: Qt.AlignRight | Qt.AlignVCenter

        Button {
            id: acceptButton
            objectName: root.acceptObjectName
            text: root.acceptText
            focus: !root.destructive
            KeyNavigation.right: rejectButton.visible ? rejectButton : null
            KeyNavigation.left: rejectButton.visible ? rejectButton : null
            onActiveFocusChanged: if (activeFocus) root.selectFooterButton(acceptButton)
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            // DialogButtonBox turns AcceptRole into accepted() by itself,
            // but DestructiveRole into rejected() -- so the destructive
            // path has to say what it means explicitly.
            DialogButtonBox.buttonRole: root.destructive ? DialogButtonBox.DestructiveRole
                                                         : DialogButtonBox.AcceptRole
            onClicked: {
                if (root.destructive) {
                    root.accept();
                }
            }
        }

        Button {
            id: rejectButton
            objectName: root.rejectObjectName
            visible: root.showReject
            text: root.rejectText
            focus: root.destructive
            KeyNavigation.left: acceptButton
            KeyNavigation.right: acceptButton
            onActiveFocusChanged: if (activeFocus) root.selectFooterButton(rejectButton)
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
