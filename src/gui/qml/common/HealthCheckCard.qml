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

    // How much of what this check found Seabass can put right itself,
    // drawn big and coloured beside the title: read before the sentence
    // is, and the number a person actually wants off this page. Both -1
    // (the default) on a check with nothing to count.
    property int fixableCount: -1
    property int foundCount: -1
    readonly property bool hasTally: card.foundCount > 0 && card.fixableCount >= 0
    readonly property color tallyColor: card.fixableCount === 0 ? Theme.danger
        : card.fixableCount >= card.foundCount ? Theme.good
        : Theme.warnIcon
    property string actionLabel: ""
    property bool actionEnabled: true
    // Why the action cannot be taken, shown instead of silently disabling
    // it. Empty when actionEnabled is true.
    property string actionDisabledReason: ""
    signal actionRequested()

    readonly property color statusColor: card.failed ? Theme.danger
        : card.running ? Theme.textMuted
        : card.ok ? Theme.good : Theme.warnIcon

    // The inset on every side but the left, which is Theme.cardTextInset
    // so the text meets the page's left line. The tally, the tick and the
    // action all end on this inset at the right, so every card on the hub
    // shares one right edge as well as one left one.
    readonly property real contentInset: Theme.cardPadding

    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + 2 * card.contentInset
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

    // Two columns: the text on the left, on one left line, and on the
    // right the tally or tick (on the title's row) with the one thing to
    // do about it under it, both ending on one right edge. The action used to take a row of its
    // own under the summary, so a page of findings read as a column of
    // buttons interleaved with the prose; beside the text it is where the
    // eye ends up after reading the sentence, and the card is a line
    // shorter.
    //
    // Every cell is placed by Layout.row/column rather than by flow: a
    // GridLayout's flow skips invisible children, so a card without an
    // action would otherwise slide its reason up into the action's cell.
    GridLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: card.contentInset
        anchors.leftMargin: Theme.cardTextInset
        columns: 2
        columnSpacing: Theme.rowSpacing
        rowSpacing: Theme.tightSpacing

        // The title and the tally share a row across both columns, so
        // the tally's width, which is wider than most buttons, does not
        // also narrow the summary under it.
        RowLayout {
            Layout.row: 0
            Layout.column: 0
            Layout.columnSpan: 2
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            Label {
                objectName: "checkTitle"
                Layout.fillWidth: true
                text: card.title
                color: Theme.text
                elide: Text.ElideRight
                font.family: Theme.titleFamily
                font.weight: Theme.cardTitleWeight
                font.pointSize: Theme.fontMedium
            }
            // <fixable> / <found>, with the half that matters carrying the
            // weight: the big coloured number is what Seabass can do, the
            // quieter one what it found.
            Row {
                objectName: "checkTally"
                visible: card.hasTally && !card.running
                spacing: 3
                Label {
                    anchors.baseline: parent.children[1].baseline
                    text: card.fixableCount
                    color: card.tallyColor
                    font.family: Theme.dataFamily
                    font.pointSize: Theme.fontXLarge
                    font.weight: Font.DemiBold
                }
                Label {
                    id: tallyRest
                    text: "/ " + card.foundCount
                    color: Theme.textMuted
                    font.family: Theme.dataFamily
                    font.pointSize: Theme.fontMedium
                }
                Label {
                    anchors.baseline: tallyRest.baseline
                    leftPadding: 4
                    text: "Seabass can fix"
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                }
            }
            // A check that passed says so with a mark, not only by the
            // absence of a warning stripe: a page of cards where nothing
            // is wrong should read as a row of green ticks at a glance,
            // rather than as a page where the checks may not have run.
            SeabassIcon {
                objectName: "checkPassedMark"
                visible: card.ok && !card.failed && !card.running
                iconName: "checkmark"
                size: Theme.iconSizeSmall
                color: Theme.good
            }
            BusyIndicator {
                running: card.running
                visible: card.running
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }

        Label {
            objectName: "checkSummary"
            Layout.row: 1
            Layout.column: 0
            // The whole width when there is no action beside it.
            Layout.columnSpan: action.visible ? 1 : 2
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignLeft | Qt.AlignTop
            text: card.summary
            color: card.running ? Theme.textMuted : Theme.text
            wrapMode: Text.WordWrap
        }
        Button {
            id: action
            objectName: "checkAction"
            Layout.row: 1
            Layout.column: 1
            Layout.alignment: Qt.AlignRight | Qt.AlignTop
            visible: card.actionLabel.length > 0 && !card.running
            text: card.actionLabel
            enabled: card.actionEnabled
            onClicked: card.actionRequested()
        }

        Label {
            objectName: "checkActionReason"
            Layout.row: 2
            Layout.column: 0
            Layout.fillWidth: true
            visible: !card.running && card.actionLabel.length > 0
                && !card.actionEnabled && card.actionDisabledReason.length > 0
            text: card.actionDisabledReason
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
}
