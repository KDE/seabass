// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for the legacy memory cue lists an XDJ-RX2 hangs
// on (#55): lists written by a Seabass between 5282555e and 6e0f1c09 with
// a header that says "empty", lists the player rewrote since with an
// empty slot, and the analysis file a hung player leaves behind. The
// repair re-encodes each list with every entry kept and removes the
// file nothing refers to. See infrastructure::rekordbox::
// LegacyMemoryListFinding.
HealthCheckPage {
    id: root

    checkTitle: "Memory Cue Lists"

    readonly property bool checked: consistencyController?.legacyMemoryListsChecked ?? false
    readonly property string error: consistencyController?.legacyMemoryListError ?? ""
    readonly property int count: consistencyController?.legacyMemoryListCount ?? 0
    readonly property int fixableCount: consistencyController?.legacyMemoryListFixableCount ?? 0
    readonly property int debrisCount: consistencyController?.legacyMemoryListDebrisCount ?? 0

    Label {
        objectName: "memoryCueListsNotChecked"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: !root.checked && root.error.length === 0 && !consistencyController?.busy
        color: Theme.textMuted
        text: "There is no rekordbox library on this stick, so there is nothing for this check to look at."
    }

    Label {
        objectName: "memoryCueListsError"
        visible: root.error.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.danger
        text: root.error
    }

    Label {
        objectName: "memoryCueListsSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: {
            if (root.count === 0) {
                return "Every memory cue list in the rekordbox analysis files is in the shape a player reads.";
            }
            let text = root.count + (root.count === 1 ? " track has" : " tracks have")
                + " a memory cue list an XDJ-RX2 can hang on, or an analysis file a hung player left behind. "
                + "A Seabass between 2026-09-18 and 2026-09-30 wrote the list with a header that says it is empty. "
                + "The player then drops the cues on its next memory save and hangs on the memory loop save after "
                + "that, leaving a second analysis file nothing refers to.";
            if (root.fixableCount > 0) {
                text += " Repairing rebuilds each list with every cue kept, the player's own included"
                    + (root.debrisCount > 0 ? ", and removes the file left behind" : "") + ".";
            }
            if (root.fixableCount < root.count) {
                text += " " + (root.count - root.fixableCount) + " listed below " + (root.count - root.fixableCount === 1
                    ? "is" : "are") + " left alone: Seabass cannot read the list well enough to rebuild it.";
            }
            return text;
        }
    }

    RowLayout {
        visible: root.checked
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "repairMemoryCueListsButton"
            visible: root.fixableCount > 0 || (consistencyController?.legacyMemoryListFixStaged ?? false)
            text: consistencyController?.legacyMemoryListFixStaged ? "Unstage" : "Repair"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.legacyMemoryListFixStaged
                ? "Take this back out of the changes to save"
                : "Stage rebuilding each list with its cues kept and removing the files nothing refers to. "
                  + "The analysis files are backed up first. Save writes it."
            onClicked: consistencyController?.legacyMemoryListFixStaged
                ? consistencyController?.unstageLegacyMemoryListFix()
                : consistencyController?.repairLegacyMemoryLists()
        }
        Label {
            objectName: "stagedMemoryCueListsNote"
            visible: consistencyController?.legacyMemoryListFixStaged ?? false
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    Repeater {
        objectName: "memoryCueListTracks"
        model: consistencyController?.legacyMemoryListTracks
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "“" + modelData.title + "”" + (modelData.artist.length > 0 ? ", " + modelData.artist : "") + ": "
                + modelData.what
        }
    }

    Item { Layout.fillHeight: true }
}
