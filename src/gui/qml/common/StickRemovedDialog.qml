// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The stick a library is being edited on was pulled. Two ways out: throw
// the staged changes away, or plug the *same* stick back in -- Understood
// only enables once the hardware identity matches (session.stickPresent).
SeabassDialog {
    id: dialog
    // A LibraryEditSession (or null when nothing is going on).
    property var session: null

    signal discardRequested()
    signal understood()

    readonly property bool hasSession: dialog.session !== null && dialog.session !== undefined
    readonly property string stickLabel: dialog.hasSession ? dialog.session.stickLabel : ""
    readonly property bool present: dialog.hasSession && dialog.session.stickPresent === true
    readonly property string strength: dialog.hasSession ? dialog.session.stickIdentityStrength : ""

    // Reads `session` directly, not `hasSession`: an onSessionChanged
    // handler can run before that binding has re-evaluated, and a stale
    // true here would reopen the dialog right after it closed.
    function sync() {
        var has = dialog.session !== null && dialog.session !== undefined;
        if (has && !dialog.opened) {
            dialog.open();
        } else if (!has && dialog.opened) {
            dialog.close();
        }
    }
    onSessionChanged: sync()
    Component.onCompleted: sync()

    severity: SeabassDialog.Warning
    closePolicy: Popup.NoAutoClose
    title: "USB stick removed"
    headline: (dialog.stickLabel.length > 0 ? "USB Stick " + dialog.stickLabel : "Your USB stick")
        + " has been removed while editing. Either discard changes or plug the stick back in. If "
        + "anything changed on the USB stick while it was unplugged, these changes will likely be lost."

    footer: DialogButtonBox {
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "discardButton"
            text: "Discard Changes"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                dialog.close();
                dialog.discardRequested();
            }
        }
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "understoodButton"
            text: "Understood"
            highlighted: true
            enabled: dialog.present
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }
    onAccepted: dialog.understood()

    // Live, and its colour changes with the answer, so it stays a real
    // Label in the content slot rather than becoming detailText.
    Label {
        objectName: "presenceLabel"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: dialog.present ? Theme.good : Theme.textMuted
        font.pointSize: Theme.fontSmall
        text: {
            if (!dialog.present) return "Waiting for the stick to be plugged back in...";
            switch (dialog.strength) {
            case "hardware": return "The same stick is back (verified by its serial number).";
            case "filesystem": return "The same stick is back (matched by its filesystem id).";
            default: return "A stick with the same label and size is back; this cannot verify it is the same one.";
            }
        }
    }
}
