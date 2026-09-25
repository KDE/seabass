// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The report after a restore or clone: the controller's status or error
// line, a one-line count, the Engine database check, and problems counted
// rather than listed until asked for (a stick yanked mid-write used to
// produce one error per remaining file, a wall of text with nothing to
// do about it). `result` is the controller's result map: filesWritten,
// filesUnchanged, directoriesCreated, extrasRemoved, databaseChecked,
// missingTracks, rejected, writeErrors, warnings.
Frame {
    id: resultFrame

    property var result: ({})
    property string errorMessage: ""
    property string statusMessage: ""
    property bool busy: false
    property string startOverTooltip: ""
    // Off where whatever holds the report has its own way out that does
    // the same job (the restore overlay's Close); two exits side by side
    // read as two different answers.
    property bool startOverVisible: true
    signal startOverRequested()
    // A referenced track missing after the write is not, by itself,
    // evidence this restore/clone did anything wrong: the same database
    // row was just as capable of pointing at a file that was already gone
    // before the source stick was ever backed up (a re-numbered or
    // re-imported track leaving its old row dangling). Library Health
    // already knows how to find and fix exactly that -- a broken row
    // with a healthy same-catalog sibling, or none at all -- so this
    // hands off to it rather than only naming the problem here.
    signal repairLibraryRequested()

    visible: resultFrame.result.filesWritten !== undefined
    readonly property var problems: (resultFrame.result.rejected || []).concat(resultFrame.result.writeErrors || []).concat(resultFrame.result.warnings || [])
    property bool showProblems: false

    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Label { font.bold: true; text: "Result" }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "startOverButton"
                visible: resultFrame.startOverVisible
                text: "Start Over"
                flat: true
                enabled: !resultFrame.busy
                ToolTip.visible: hovered && resultFrame.startOverTooltip.length > 0
                ToolTip.text: resultFrame.startOverTooltip
                onClicked: {
                    resultFrame.showProblems = false;
                    resultFrame.startOverRequested();
                }
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: resultFrame.errorMessage.length > 0 ? Theme.danger : Theme.good
            text: resultFrame.errorMessage.length > 0 ? resultFrame.errorMessage : resultFrame.statusMessage
        }
        Label {
            font.family: Theme.dataFamily
            // "held back" appears only when there is something to hold
            // back. It is a restore-only outcome (a database set whose
            // parts could not all be read off the failing drive), and
            // this frame is shared with Clone, whose result has no such
            // field -- undefined there, so the term stays off.
            text: resultFrame.result.filesWritten + " written · " + resultFrame.result.filesUnchanged + " unchanged · "
                + (resultFrame.result.filesHeldBack > 0
                       ? resultFrame.result.filesHeldBack + " held back · " : "")
                + resultFrame.result.directoriesCreated + " folders created · " + resultFrame.result.extrasRemoved + " removed"
        }
        Label {
            visible: resultFrame.result.databaseChecked === true
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: (resultFrame.result.missingTracks || []).length === 0 ? Theme.good : Theme.danger
            text: (resultFrame.result.missingTracks || []).length === 0
                ? "Engine database opens and every track it references is present."
                : "The Engine database opened, but it points at " + resultFrame.result.missingTracks.length
                  + (resultFrame.result.missingTracks.length === 1 ? " file that is" : " files that are") + " not on this drive:"
        }
        Repeater {
            model: (resultFrame.result.missingTracks || []).slice(0, 20)
            // Width-bound and elided in the middle: a path label with no
            // width limit widened the whole report past the overlay, and
            // the note and the button below it were cut off (round 8,
            // TESTRIG_2 restored onto A1). The full path is the tooltip.
            delegate: Label {
                required property string modelData
                Layout.fillWidth: true
                Layout.leftMargin: 16
                font.family: Theme.dataFamily
                font.pointSize: Theme.fontSmall
                color: Theme.danger
                elide: Text.ElideMiddle
                text: modelData
                ToolTip.visible: truncated && hovered
                ToolTip.text: modelData
                HoverHandler { id: pathHover }
                readonly property bool hovered: pathHover.hovered
            }
        }
        Label {
            visible: (resultFrame.result.missingTracks || []).length > 20
            Layout.leftMargin: 16
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            text: "and " + ((resultFrame.result.missingTracks || []).length - 20) + " more"
        }
        // Said plainly, because the red list above reads as "the restore
        // broke something": it did not. The rows pointed at those files
        // before the backup was ever taken, the backup carried them as
        // they were, and this restore put them back as they were. The
        // way out is Library Health, which finds a row whose file is
        // gone and offers the repair.
        Label {
            visible: (resultFrame.result.missingTracks || []).length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            text: "This is the library as it was backed up, not damage from the restore: the backup is intact and every file it holds is on the drive. "
                  + "A re-numbered or re-imported track leaves its old row pointing at a file that is gone. "
                  + "Library Health finds those rows and can repair them."
        }
        Button {
            objectName: "repairLibraryButton"
            // Set directly (not inherited) so a test can read this
            // button's own visible property, the same convention every
            // other conditional button on this page follows.
            visible: (resultFrame.result.missingTracks || []).length > 0
            Layout.alignment: Qt.AlignRight
            text: "Repair in Library Health"
            highlighted: true
            onClicked: resultFrame.repairLibraryRequested()
        }
        RowLayout {
            visible: resultFrame.problems.length > 0
            spacing: 8
            Label {
                objectName: "problemCountLabel"
                color: Theme.conflictText
                text: resultFrame.problems.length + (resultFrame.problems.length === 1 ? " problem" : " problems")
            }
            Button {
                objectName: "toggleProblemsButton"
                flat: true
                text: resultFrame.showProblems ? "Hide details" : "Show details"
                onClicked: resultFrame.showProblems = !resultFrame.showProblems
            }
        }
        Repeater {
            objectName: "problemList"
            model: resultFrame.showProblems ? resultFrame.problems.slice(0, 200) : []
            delegate: Label { required property string modelData; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pointSize: Theme.fontSmall; color: Theme.conflictText; text: modelData }
        }
        Label {
            visible: resultFrame.showProblems && resultFrame.problems.length > 200
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
            text: "and " + (resultFrame.problems.length - 200) + " more"
        }
    }
}
