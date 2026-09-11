// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Another Seabass instance is editing this library. The safe button is
// the default; the other one is deliberately blunt about what it does.
SeabassDialog {
    id: dialog
    property string libraryId: ""
    // {instanceId, hostname, pid, stickLabel, startedAtUtc}, possibly empty
    property var holder: ({})

    signal removeLockRequested()

    function openFor(id, holderMap) {
        dialog.libraryId = id;
        dialog.holder = holderMap || {};
        dialog.open();
    }

    readonly property string holderLine: {
        if (!dialog.holder || dialog.holder.hostname === undefined || dialog.holder.hostname === "") return "";
        var line = "Held by Seabass on " + dialog.holder.hostname;
        if (dialog.holder.pid !== undefined && dialog.holder.pid > 0) line += " (process " + dialog.holder.pid + ")";
        if (dialog.holder.startedAtUtc !== undefined && dialog.holder.startedAtUtc !== "") line += " since " + dialog.holder.startedAtUtc;
        return line + ".";
    }

    severity: SeabassDialog.Error
    closePolicy: Popup.NoAutoClose
    title: "This library is locked"
    headline: "This library is currently locked by another instance of Seabass. Finish or discard the "
        + "work in the other instance first.\n\nIf you are absolutely sure that you want to continue "
        + "from here: Fine. Your call, remove the lock. (Don't come complaining.)"
    detailText: dialog.holderLine
    detailObjectName: "holderLabel"

    footer: DialogButtonBox {
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "removeLockButton"
            text: "Remove Lock"
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                dialog.close();
                dialog.removeLockRequested();
            }
        }
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "staySafeButton"
            text: "Stay on the Safe Side"
            highlighted: true
            focus: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

}
