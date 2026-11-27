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
// groups with their size: of tracks on the stick, and of tracks not on it
// under the name they were kept for, every group unticked: deleting is
// the person's choice, and a track not there under its name may have been
// moved or renamed outside Seabass. Removing is a direct write after a
// confirmation that says what is lost; the page shows the summary and a
// lock refusal (summaryReady, lockRefused with the lock's library).
// Hidden, taking no room, while nothing is kept.
ColumnLayout {
    id: section
    objectName: "keptAnalysisSection"

    required property string rekordboxPath
    required property string enginePath

    signal summaryReady(var summary)
    signal lockRefused(var holder, string libraryId)

    readonly property alias controller: keptController

    // No room taken and nothing shown unless something is kept, while it
    // is first listed too: most sticks have nothing kept, and the page's
    // other lists keep their height.
    visible: keptController.totalCount > 0
    // The tracks of the ticked groups that are on the stick: what the
    // confirmation warns about.
    readonly property int includedOnStick: includedWhere(group => !group.gone)
    // The ticked tracks not on the stick under their name, and the ticked
    // player's analyses (Restore Engine's Analysis goes with them).
    readonly property int includedGone: includedWhere(group => group.gone)
    readonly property int includedEngine: includedWhere(group => group.kind === "engine")

    function includedWhere(test) {
        let n = 0;
        for (const group of keptController.groups) {
            if (group.included && test(group)) {
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
        function onLockRefused(holder, libraryId) { section.lockRefused(holder, libraryId); }
    }

    function groupText(group) {
        if (group.gone) {
            return group.kind === "engine"
                ? qsTr("The player's analysis kept for %n track(s) not on the stick under that name any more", "",
                       group.count)
                : qsTr("rekordbox's analysis kept for %n track(s) not on the stick under that name any more", "",
                       group.count);
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
        detailText: {
            const parts = [];
            if (section.includedOnStick > 0) {
                parts.push(qsTr("Seabass's analysis stays on the stick and the tracks play as they do now. "
                                + "\"Restore original analysis\" will no longer be possible for these %n track(s).",
                                "", section.includedOnStick));
            }
            if (section.includedEngine > 0) {
                parts.push(qsTr("\"Restore Engine's Analysis\" on the Beat Grids & Waveforms page will no longer be "
                                + "possible for the %n track(s) whose player's analysis is removed.", "",
                                section.includedEngine));
            }
            if (section.includedGone > 0) {
                parts.push(qsTr("%n of them are not on the stick under the name they were kept for any more. If a "
                                + "track was moved or renamed outside Seabass, its original analysis is lost too.",
                                "", section.includedGone));
            }
            return parts.join("\n\n");
        }
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
                   + "their original analysis can no longer be restored. Some may be kept for tracks that are not "
                   + "on the stick under that name any more: the track was removed, or moved or renamed outside "
                   + "Seabass. Nothing is removed unless you select it.")
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
}
