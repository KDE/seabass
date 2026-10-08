// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The process guard's modal: up while rekordbox or Engine DJ is running
// *and* a library is in edit or write mode (guard.blocking). It closes
// itself the moment the other app is gone, and while it is up the guard
// polls every second (guard.dialogOpen). Cancel puts it away until the
// other app has gone and come back: the warning has been read, and the
// save itself still refuses to write while the app runs (write_guard).
SeabassDialog {
    id: dialog
    required property var guard

    severity: SeabassDialog.Warning
    closePolicy: Popup.CloseOnEscape
    title: "Close " + dialog.guard.conflictingSoftware + " first"
    headline: dialog.guard.conflictingSoftware + " is running. Please close it until your changes have "
        + "been saved. If you have already made changes to your USB stick in "
        + dialog.guard.conflictingSoftware + ", better discard the changes made here, you might lose "
        + "data. (You have been warned!)"
    detailText: "Checking every second; this closes by itself once "
        + dialog.guard.conflictingSoftware + " is gone."

    property bool dismissed: false

    footer: DialogButtonBox {
        Button {
            objectName: "djGuardCancelButton"
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dialog.close()
        }
    }

    onOpened: dialog.guard.dialogOpen = true
    // Closed while the app still runs is Cancel (the button or Esc).
    onClosed: {
        dialog.guard.dialogOpen = false;
        if (dialog.guard.blocking) {
            dialog.dismissed = true;
        }
    }

    function sync() {
        if (!dialog.guard.blocking) {
            dialog.dismissed = false;
        }
        if (dialog.guard.blocking && !dialog.dismissed && !dialog.opened) {
            dialog.open();
        } else if (!dialog.guard.blocking && dialog.opened) {
            dialog.close();
        }
    }

    Connections {
        target: dialog.guard
        function onBlockingChanged() { dialog.sync(); }
    }
    Component.onCompleted: sync()
}
