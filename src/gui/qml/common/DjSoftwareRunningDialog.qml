// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The process guard's modal: up while rekordbox or Engine DJ is running
// *and* a library is in edit or write mode (guard.blocking). No buttons
// on purpose -- it closes itself the moment the other app is gone, and
// while it is up the guard polls every second (guard.dialogOpen).
SeabassDialog {
    id: dialog
    required property var guard

    severity: SeabassDialog.Warning
    closePolicy: Popup.NoAutoClose
    title: "Close " + dialog.guard.conflictingSoftware + " first"
    headline: dialog.guard.conflictingSoftware + " is running. Please close it until your changes have "
        + "been saved. If you have already made changes to your USB stick in "
        + dialog.guard.conflictingSoftware + ", better discard the changes made here, you might lose "
        + "data. (You have been warned!)"
    detailText: "Checking every second; this closes by itself once "
        + dialog.guard.conflictingSoftware + " is gone."

    onOpened: dialog.guard.dialogOpen = true
    onClosed: dialog.guard.dialogOpen = false

    function sync() {
        if (dialog.guard.blocking && !dialog.opened) {
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
