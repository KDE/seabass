// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The "type the drive's name to confirm" dialog in front of every
// operation that overwrites a drive. The warning block and the typed
// field appear only when `needsTypedConfirmation` is set (a blank drive
// needs neither); the page puts its own summary rows (a GridLayout,
// usually) inside as children.
SeabassDialog {
    id: dialog

    property string confirmTarget: ""          // what has to be typed, and what the warning names
    property bool needsTypedConfirmation: true
    property string acceptText: "Confirm"
    property string acceptObjectName: "acceptButton"
    property bool showWarning: dialog.needsTypedConfirmation
    property string warningTitle: ""
    property string warningText: ""
    default property alias summary: summaryColumn.data

    severity: SeabassDialog.Warning
    // It draws its own, deliberately heavier warning block below; a second
    // badge beside it would just be two warning icons.
    showBadge: false

    footer: DialogButtonBox {
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: dialog.acceptObjectName
            text: dialog.acceptText
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: !dialog.needsTypedConfirmation || confirmField.text === dialog.confirmTarget
        }
        Button { text: "Cancel"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    onOpened: confirmField.text = ""

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 14
        Rectangle {
            Layout.fillWidth: true
            visible: dialog.showWarning
            implicitHeight: warnColumn.implicitHeight + 24
            radius: 4
            color: Theme.dangerBg
            border.color: Theme.dangerBorder
            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10
                Label { text: "⚠"; font.family: "Noto Sans Symbols2"; font.pointSize: Theme.fontHuge; color: Theme.dangerText; Layout.alignment: Qt.AlignTop }
                ColumnLayout {
                    id: warnColumn
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.dangerText
                        font.family: Theme.titleFamily
                        font.weight: Font.Bold
                        font.pointSize: Theme.fontMedium
                        text: dialog.warningTitle
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.dangerText
                        text: dialog.warningText
                    }
                }
            }
        }
        ColumnLayout {
            id: summaryColumn
            Layout.fillWidth: true
            spacing: 4
        }
        Label {
            visible: dialog.needsTypedConfirmation
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "Type " + dialog.confirmTarget + " to confirm"
        }
        TextField {
            id: confirmField
            objectName: "confirmField"
            visible: dialog.needsTypedConfirmation
            Layout.fillWidth: true
        }
    }
}
