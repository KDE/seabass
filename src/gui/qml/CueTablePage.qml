// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for OneLibrary rows whose cue table holds cues
// their analysis file does not (#57). The players read the file, so the
// file is right: the repair removes the cues only the table holds, row by
// row or all at once, and nothing else changes.
HealthCheckPage {
    id: root

    checkTitle: "OneLibrary cue tables"

    readonly property bool checked: consistencyController?.cueTablesChecked ?? false
    readonly property var rows: consistencyController?.cueTableRows ?? []
    readonly property int stagedCount: consistencyController?.cueTableStagedCount ?? 0
    readonly property int leftAloneCount: consistencyController?.cueTableLeftAloneCount ?? 0
    readonly property bool canStage: !consistencyController?.busy && !consistencyController?.writing
        && !consistencyController?.stickReadOnly

    Label {
        objectName: "cueTableSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.rows.length === 0
            ? "No OneLibrary cue table on this stick holds cues its analysis file does not."
            : (root.rows.length === 1 ? "One track's" : root.rows.length + " tracks'")
              + " OneLibrary cue table holds cues its analysis file does not."
    }

    Label {
        objectName: "cueTableExplanation"
        visible: root.checked && root.rows.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: "Players take a track's cues from its analysis file, so that is what you have been playing. "
            + "An earlier Seabass build wrote other cues into the table beside it. Repairing removes the cues "
            + "only the table holds; nothing else changes. A backup is made first. Nothing changes until you Save."
    }

    Label {
        objectName: "cueTableLeftAlone"
        visible: root.checked && root.leftAloneCount > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: (root.leftAloneCount === 1 ? "One track is" : root.leftAloneCount + " tracks are")
            + " left alone: Seabass could not read the analysis file, or the table holds cues of a kind "
            + "Seabass does not know."
    }

    RowLayout {
        visible: root.checked && root.rows.length > 0
        spacing: Theme.rowSpacing
        Button {
            objectName: "repairAllButton"
            text: root.stagedCount === root.rows.length ? "Unstage All" : "Repair All"
            enabled: root.canStage
            onClicked: {
                if (root.stagedCount === root.rows.length) {
                    consistencyController?.unstageCueTables();
                } else {
                    consistencyController?.repairCueTables([]);
                }
            }
        }
    }

    Repeater {
        objectName: "cueTableRows"
        model: root.rows
        delegate: RowLayout {
            id: tableRow
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.bold: true
                    text: tableRow.modelData.title.length > 0 ? tableRow.modelData.title : tableRow.modelData.filePath
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    color: Theme.textMuted
                    text: (tableRow.modelData.artist.length > 0 ? tableRow.modelData.artist + " · " : "")
                        + tableRow.modelData.filePath
                }
                Label {
                    objectName: "extra_" + tableRow.modelData.contentId
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: "Only in the table: " + tableRow.modelData.extra
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: "In the analysis file: " + tableRow.modelData.fileCues
                }
            }
            Button {
                objectName: "repair_" + tableRow.modelData.contentId
                Layout.alignment: Qt.AlignTop
                text: tableRow.modelData.staged ? "Unstage" : "Repair"
                enabled: root.canStage
                onClicked: {
                    if (tableRow.modelData.staged) {
                        consistencyController?.unstageCueTable(tableRow.modelData.contentId);
                    } else {
                        consistencyController?.repairCueTables([tableRow.modelData.contentId]);
                    }
                }
            }
        }
    }
}
