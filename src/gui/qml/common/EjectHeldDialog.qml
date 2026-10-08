// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// An eject a program refused because it has files on the stick open
// (MediaController.ejectHeldBy): which program, and what to do about it.
// For rekordbox and Engine DJ that includes ejecting the stick from the
// program itself, which is how either of them lets go of a stick.
SeabassDialog {
    id: dialog
    required property var mediaController

    readonly property string holder: dialog.mediaController.ejectHeldBy
    readonly property string stick: dialog.mediaController.ejectHeldLabel.length > 0
        ? dialog.mediaController.ejectHeldLabel : "the USB stick"

    severity: SeabassDialog.Warning
    title: dialog.holder + " is using " + dialog.stick
    headline: dialog.mediaController.ejectHeldByDjSoftware
        ? "Quit " + dialog.holder + ", or eject " + dialog.stick + " from within " + dialog.holder
          + ", then try again."
        : "Close " + dialog.holder + ", then try again."

    footer: DialogButtonBox {
        Button {
            objectName: "ejectRetryButton"
            text: "Try Again"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                dialog.close();
                dialog.mediaController.retryHeldEject();
            }
        }
        Button {
            objectName: "ejectCancelButton"
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dialog.close()
        }
    }
    // Esc and the window's own close count as Cancel too.
    onClosed: {
        if (dialog.holder.length > 0) {
            dialog.mediaController.dismissHeldEject();
        }
    }

    Connections {
        target: dialog.mediaController
        function onEjectHeldChanged() {
            if (dialog.holder.length > 0 && !dialog.opened) {
                dialog.open();
            }
        }
    }
}
