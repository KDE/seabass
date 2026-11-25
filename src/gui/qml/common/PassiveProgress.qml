// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Progress of work that goes on beside what the person is doing: one
// line and a thin bar at the edge of a pane, never in the way. A click
// opens it into a panel with room to read: the steps of the work with
// their counts, what is in hand right now by name, what was done last,
// and Cancel. It closes again with a click, so it may take the room.
Rectangle {
    id: root

    // Shown while this is true.
    property bool running: false
    // One line for the closed strip, e.g. "Reading 12 of 40 files".
    property string summary: ""
    // Of the step in hand; total 0 for one that cannot count ahead.
    property real done: 0
    property real total: 0
    // A line under what is in hand, e.g. the file done last; "" for none.
    property string detail: ""
    // What is in hand right now, a line each: "Analysing a.mp3".
    property var activity: []
    // A line of small print at the foot of the open panel; "" for none.
    property string note: ""
    // The steps of the work, in order: [{label, state, count}], state
    // one of "done", "now", "later"; count ("3 of 20") may be missing.
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
                font.pointSize: root.expanded ? Theme.fontNormal : Theme.fontSmall
                font.bold: root.expanded
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
            barHeight: root.expanded ? 8 : 6
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
                Label {
                    objectName: "passiveProgressStepCount"
                    visible: text.length > 0
                    text: stepRow.modelData.count ?? ""
                    font.pointSize: Theme.fontSmall
                    color: Theme.textMuted
                }
            }
        }
        Rectangle {
            visible: root.expanded
            Layout.fillWidth: true
            Layout.topMargin: Theme.tightSpacing
            implicitHeight: 1
            color: Theme.borderSubtle
        }
        // What is in hand, by name. The room of two lines is kept while
        // the panel is open, so it does not jump as files come and go.
        ColumnLayout {
            objectName: "passiveProgressActivity"
            visible: root.expanded
            Layout.fillWidth: true
            Layout.minimumHeight: 2 * activityMetrics.height + Theme.tightSpacing
            spacing: Theme.tightSpacing
            Repeater {
                model: root.expanded ? root.activity : []
                delegate: Label {
                    required property string modelData
                    objectName: "passiveProgressActivityLine"
                    Layout.fillWidth: true
                    text: modelData
                    elide: Text.ElideMiddle
                    font.pointSize: Theme.fontSmall
                    color: Theme.text
                }
            }
            Item {
                Layout.fillHeight: true
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
        // The foot: the note, beside the room the Cancel button takes.
        RowLayout {
            visible: root.expanded
            Layout.fillWidth: true
            Layout.topMargin: Theme.tightSpacing
            spacing: Theme.rowSpacing
            Label {
                objectName: "passiveProgressNote"
                Layout.fillWidth: true
                text: root.note
                wrapMode: Text.WordWrap
                font.pointSize: Theme.fontSmall
                color: Theme.textMuted
            }
            Item {
                implicitWidth: root.cancellable ? cancelButton.width : 0
                implicitHeight: root.cancellable ? cancelButton.height : 0
            }
        }
    }
    FontMetrics {
        id: activityMetrics
        font.pointSize: Theme.fontSmall
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
        id: cancelButton
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
