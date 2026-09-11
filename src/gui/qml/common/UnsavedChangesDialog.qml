// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Blocks leaving a page (or the app) with staged changes. Only two ways
// out, by design: save them or throw them away.
SeabassDialog {
    id: dialog
    property string message: "You have made changes to your library. Please save these changes to your library first."
    property string saveText: "Save Changes"
    property string discardText: "Discard Changes"
    property int pendingCount: 0

    signal saveRequested()
    signal discardRequested()

    severity: SeabassDialog.Warning
    closePolicy: Popup.NoAutoClose
    title: "Unsaved changes"
    headline: dialog.message
    detailText: dialog.pendingCount > 0
        ? dialog.pendingCount + " change(s) are staged and not on the stick yet." : ""

    footer: DialogButtonBox {
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "saveButton"
            text: dialog.saveText
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "discardButton"
            text: dialog.discardText
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                dialog.close();
                dialog.discardRequested();
            }
        }
    }
    onAccepted: dialog.saveRequested()
}
