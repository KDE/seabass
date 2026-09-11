// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Write mode: up for exactly as long as a session is saving. Shows what
// is being written right now, the progress, the "do not remove the
// stick" warning, and a Cancel that stops at the next consistent point.
SeabassDialog {
    id: dialog
    required property var session

    readonly property bool hasSession: dialog.session !== null && dialog.session !== undefined
    readonly property bool writing: dialog.hasSession && dialog.session.writing === true

    showBadge: false   // progress, not a severity
    closePolicy: Popup.NoAutoClose
    title: "Saving changes" + (dialog.hasSession && dialog.session.stickLabel.length > 0
        ? " to " + dialog.session.stickLabel : "")

    function sync() {
        if (dialog.writing && !dialog.opened) {
            dialog.open();
        } else if (!dialog.writing && dialog.opened) {
            dialog.close();
        }
    }
    onWritingChanged: sync()
    Component.onCompleted: sync()

    footer: DialogButtonBox {
        Button {
            objectName: "cancelButton"
            text: dialog.hasSession && dialog.session.cancelRequested ? "Stopping after the current item..." : "Cancel"
            enabled: dialog.hasSession && !dialog.session.cancelRequested
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: dialog.session.cancelWrite()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 12

        Label {
            objectName: "writeLabel"
            color: Theme.text
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            elide: Text.ElideMiddle
            maximumLineCount: 2
            text: dialog.hasSession ? dialog.session.writeLabel : ""
        }
        ProgressBar {
            id: bar
            Layout.fillWidth: true
            Layout.preferredHeight: 14
            indeterminate: !dialog.hasSession || dialog.session.writeTotal <= 0
            value: dialog.hasSession && dialog.session.writeTotal > 0
                ? dialog.session.writeCurrent / dialog.session.writeTotal : 0
            background: Rectangle {
                implicitHeight: 14
                radius: height / 2
                color: Theme.surface
                border.color: Theme.borderSubtle
            }
            contentItem: Item {
                implicitHeight: 14
                clip: true
                Rectangle {
                    visible: !bar.indeterminate
                    height: parent.height
                    width: bar.visualPosition * parent.width
                    radius: height / 2
                    color: Theme.accent
                }
                Rectangle {
                    visible: bar.indeterminate
                    width: parent.width * 0.3
                    height: parent.height
                    radius: height / 2
                    color: Theme.accent
                    SequentialAnimation on x {
                        running: bar.indeterminate && dialog.opened
                        loops: Animation.Infinite
                        NumberAnimation { from: -parent.width * 0.3; to: parent.width; duration: 1100; easing.type: Easing.InOutQuad }
                    }
                }
            }
        }
        Label {
            objectName: "countLabel"
            visible: dialog.hasSession && dialog.session.writeTotal > 0
            color: Theme.textMuted
            text: dialog.hasSession ? dialog.session.writeCurrent + " / " + dialog.session.writeTotal : ""
        }
        StickWriteWarning {
            objectName: "stickWarning"
            visible: true
            text: "Do not remove your USB stick until this finishes."
        }
    }
}
