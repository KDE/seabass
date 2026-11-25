// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Progress of work that goes on beside what the person is doing: one
// line and a thin bar at the edge of a pane, never in the way. A click
// opens it: the steps of the work, the file in hand, and Cancel.
Rectangle {
    id: root

    // Shown while this is true.
    property bool running: false
    // One line for the closed strip, e.g. "Reading 12 of 40 files".
    property string summary: ""
    // Of the step in hand; total 0 for one that cannot count ahead.
    property real done: 0
    property real total: 0
    // The file in hand; "" between files.
    property string detail: ""
    // The steps of the work, in order: [{label, state}], state one of
    // "done", "now", "later".
    property var steps: []
    property bool expanded: false
    property bool cancellable: true

    signal cancelRequested()

    visible: root.running
    implicitHeight: root.running ? column.implicitHeight + 2 * Theme.tightSpacing : 0
    color: Theme.surface
    onRunningChanged: {
        if (!root.running) {
            root.expanded = false;
        }
    }

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.borderSubtle
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.rowSpacing
        anchors.rightMargin: Theme.rowSpacing
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.tightSpacing
            Label {
                objectName: "passiveProgressSummary"
                Layout.fillWidth: true
                text: root.summary
                elide: Text.ElideRight
                font.pointSize: Theme.fontSmall
                color: Theme.text
            }
            Label {
                objectName: "passiveProgressToggle"
                text: root.expanded ? qsTr("Less") : qsTr("More")
                font.pointSize: Theme.fontSmall
                color: Theme.accent
            }
        }
        ProgressTrack {
            objectName: "passiveProgressBar"
            Layout.fillWidth: true
            barHeight: 6
            // As a share: a ProgressBar clamps its value to the range it
            // has at that moment, and done and total do not arrive at once.
            from: 0
            to: 1
            value: root.total > 0 ? Math.min(1, root.done / root.total) : 0
            indeterminate: root.total <= 0
        }
        Repeater {
            model: root.expanded ? root.steps : []
            delegate: RowLayout {
                id: stepRow
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.tightSpacing
                Label {
                    text: stepRow.modelData.state === "done" ? "✓" : stepRow.modelData.state === "now" ? "▸" : "·"
                    font.pointSize: Theme.fontSmall
                    color: stepRow.modelData.state === "later" ? Theme.textMuted : Theme.accent
                    Layout.preferredWidth: Theme.scaled(14)
                    horizontalAlignment: Text.AlignHCenter
                }
                Label {
                    objectName: "passiveProgressStep"
                    Layout.fillWidth: true
                    text: stepRow.modelData.label
                    elide: Text.ElideRight
                    font.pointSize: Theme.fontSmall
                    color: stepRow.modelData.state === "later" ? Theme.textMuted : Theme.text
                }
            }
        }
        Label {
            objectName: "passiveProgressDetail"
            visible: root.expanded && root.detail.length > 0
            Layout.fillWidth: true
            text: root.detail
            elide: Text.ElideMiddle
            font.pointSize: Theme.fontSmall
            color: Theme.textMuted
        }
    }

    // Under the button, over the rest: a click anywhere else opens and
    // closes the strip.
    MouseArea {
        objectName: "passiveProgressArea"
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.expanded = !root.expanded
    }
    Button {
        objectName: "passiveProgressCancel"
        visible: root.expanded && root.cancellable
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Theme.rowSpacing
        anchors.bottomMargin: Theme.tightSpacing
        text: qsTr("Cancel")
        font.pointSize: Theme.fontSmall
        onClicked: root.cancelRequested()
    }
}
