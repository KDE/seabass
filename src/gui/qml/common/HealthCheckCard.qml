// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// One check on the Library Health hub: what was looked at, what was found
// in a sentence or two, and the one thing to do about it.
//
// The summary is written out in full rather than shown as a count and a
// label, because "27" means nothing until you know 27 of what, and this
// page is read once and acted on. A check that found nothing still gets a
// card: "nothing wrong here" is a result, and a page that only lists
// problems cannot tell you the difference between a clean library and a
// check that never ran.
Rectangle {
    id: card

    required property string title
    // What was found, in one or two sentences. Complete sentences, ending
    // in a full stop -- this is prose, not a status line.
    required property string summary
    // False means the check found something worth a person's attention.
    // It does not mean the check failed; see `failed` for that.
    property bool ok: true
    // The check itself could not run (a catalog would not open, say).
    // Distinct from ok=false, which is a real finding.
    property bool failed: false
    property bool running: false

    property string actionLabel: ""
    property bool actionEnabled: true
    // Why the action cannot be taken, shown instead of silently disabling
    // it. Empty when actionEnabled is true.
    property string actionDisabledReason: ""
    signal actionRequested()

    readonly property color statusColor: card.failed ? Theme.danger
        : card.running ? Theme.textMuted
        : card.ok ? Theme.good : Theme.warnIcon

    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + 28
    radius: 8
    color: Theme.surface
    border.width: 1
    border.color: card.failed || !card.ok ? Qt.rgba(card.statusColor.r, card.statusColor.g, card.statusColor.b, 0.45)
                                          : Theme.borderSubtle

    // A quiet stripe rather than a filled card: several of these sit
    // together, and filling each one makes the page shout uniformly.
    Rectangle {
        width: 4
        radius: 2
        color: card.statusColor
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 6 }
        visible: !card.ok || card.failed
    }

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 14
        anchors.leftMargin: 20
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                objectName: "checkTitle"
                text: card.title
                color: Theme.text
                font.family: Theme.titleFamily
                font.weight: Theme.cardTitleWeight
                font.pointSize: Theme.fontMedium
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: card.running
                visible: card.running
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }

        Label {
            objectName: "checkSummary"
            Layout.fillWidth: true
            text: card.summary
            color: card.running ? Theme.textMuted : Theme.text
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10
            visible: card.actionLabel.length > 0 && !card.running

            Button {
                objectName: "checkAction"
                text: card.actionLabel
                enabled: card.actionEnabled
                onClicked: card.actionRequested()
            }
            Label {
                objectName: "checkActionReason"
                Layout.fillWidth: true
                visible: !card.actionEnabled && card.actionDisabledReason.length > 0
                text: card.actionDisabledReason
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}
