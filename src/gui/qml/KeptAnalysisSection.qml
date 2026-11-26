// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Free Up Space's kept original analyses: what rekordbox and the player
// made of tracks before Seabass analysed them again (or copied rekordbox's
// analysis over the player's), kept so it can be put back. Listed in
// groups with their size: of tracks on the stick, unticked, and of tracks
// gone from it, ticked, since nothing can be restored from those. Removing
// is a direct write after a confirmation that says what is lost; the page
// shows the summary and a lock refusal (summaryReady, lockRefused).
ColumnLayout {
    id: section
    objectName: "keptAnalysisSection"

    required property string rekordboxPath
    required property string enginePath

    signal summaryReady(var summary)
    signal lockRefused(var holder)

    readonly property alias controller: keptController
    // The tracks of the ticked groups that are on the stick: what the
    // confirmation warns about.
    readonly property int includedOnStick: {
        let n = 0;
        for (const group of keptController.groups) {
            if (group.included && !group.gone) {
                n += group.count;
            }
        }
        return n;
    }

    spacing: 8

    KeptAnalysisSpaceController {
        id: keptController
    }
    Component.onCompleted: keptController.refresh(section.rekordboxPath, section.enginePath)
    Connections {
        target: keptController
        function onRemoveFinished(summary) { section.summaryReady(summary); }
        function onLockRefused(holder) { section.lockRefused(holder); }
    }

    function groupText(group) {
        if (group.gone) {
            return group.kind === "engine"
                ? qsTr("The player's analysis kept for %n track(s) no longer on the stick", "", group.count)
                : qsTr("rekordbox's analysis kept for %n track(s) no longer on the stick", "", group.count);
        }
        return group.kind === "engine"
            ? qsTr("The player's analysis of %n track(s) on the stick", "", group.count)
            : qsTr("rekordbox's analysis of %n track(s) on the stick", "", group.count);
    }

    MessageDialog {
        id: confirmRemoveDialog
        objectName: "confirmRemoveKeptDialog"
        severity: SeabassDialog.Warning
        destructive: true
        title: qsTr("Remove the Original Analysis of %n Track(s)?", "", keptController.includedCount)
        headline: qsTr("This frees %1. The grids and waveforms rekordbox or the player made for %n track(s), "
                       + "before Seabass analysed them again, are removed for good.", "", keptController.includedCount)
            .arg(keptController.includedBytesHuman)
        detailText: section.includedOnStick > 0
            ? qsTr("Seabass's analysis stays on the stick and the tracks play as they do now. \"Restore original "
                   + "analysis\" will no longer be possible for these %n track(s).", "", section.includedOnStick)
            : qsTr("These tracks are no longer on the stick, so nothing could be restored from what is kept of them.")
        acceptText: qsTr("Remove")
        onAccepted: keptController.removeIncluded()
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8
        spacing: 12
        Label {
            objectName: "keptAnalysisCountLabel"
            text: qsTr("Original analyses kept for %n track(s)", "", keptController.totalCount)
            font.bold: true
        }
        Label {
            visible: keptController.totalCount > 0
            text: "(" + qsTr("%1 total").arg(keptController.totalBytesHuman) + ")"
            color: Theme.textMuted
        }
        Item { Layout.fillWidth: true }
        Button {
            objectName: "removeKeptAnalysisButton"
            text: qsTr("Remove Selected")
            enabled: !keptController.busy && keptController.includedCount > 0
            onClicked: confirmRemoveDialog.open()
        }
    }
    Label {
        text: qsTr("Before Seabass analyses a track again, it keeps the beat grid, waveforms and tempo rekordbox or "
                   + "the player made, so that \"Restore original analysis\" can put them back. Removing them deletes "
                   + "them for good: Seabass's analysis stays on the stick and the tracks play as they do now, but "
                   + "their original analysis can no longer be restored. What is kept for tracks no longer on the "
                   + "stick is safe to remove.")
        color: Theme.textMuted
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Label {
        visible: keptController.errorMessage.length > 0
        text: keptController.errorMessage
        color: Theme.danger
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }
    Repeater {
        objectName: "keptAnalysisGroups"
        model: keptController.groups
        delegate: RowLayout {
            id: groupRow
            required property var modelData
            required property int index
            Layout.fillWidth: true
            spacing: 8
            SeabassCheckBox {
                objectName: "keptAnalysisGroupCheck"
                checked: groupRow.modelData.included
                enabled: !keptController.busy
                onToggled: keptController.setIncluded(groupRow.index, checked)
            }
            Label {
                text: section.groupText(groupRow.modelData)
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: groupRow.modelData.sizeHuman
                color: Theme.textMuted
                Layout.preferredWidth: 70
                horizontalAlignment: Text.AlignRight
            }
        }
    }
    Label {
        objectName: "keptAnalysisEmpty"
        visible: keptController.totalCount === 0 && !keptController.busy
        text: qsTr("No original analysis is kept on this stick.")
        color: Theme.textMuted
    }
}
