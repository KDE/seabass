// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for the cue lists in the rekordbox analysis
// files. Two halves of one check:
//
// - the legacy memory cue lists an XDJ-RX2 hangs on (#55): lists written
//   by a Seabass between 5282555e and 6e0f1c09 with a header that says
//   "empty", lists the player rewrote since with an empty slot, and the
//   analysis file a hung player leaves behind. The repair re-encodes each
//   list with every entry kept and removes the file nothing refers to.
// - tracks whose older (PCOB) and newer (PCO2) cue lists disagree (#60):
//   the players measured so far show the older list, Seabass reads the
//   newer one. The repair writes the list the user chose into both, the
//   player's by default.
//
// One Repair stages both halves. See infrastructure::rekordbox::
// LegacyMemoryListFinding.
HealthCheckPage {
    id: root

    checkTitle: "Cue Lists"

    readonly property bool checked: consistencyController?.legacyMemoryListsChecked ?? false
    readonly property string error: consistencyController?.legacyMemoryListError ?? ""
    readonly property int count: consistencyController?.legacyMemoryListCount ?? 0
    readonly property int fixableCount: consistencyController?.legacyMemoryListFixableCount ?? 0
    readonly property int debrisCount: consistencyController?.legacyMemoryListDebrisCount ?? 0
    readonly property int disagreeCount: consistencyController?.cueListDisagreementCount ?? 0
    readonly property int disagreeFixableCount: consistencyController?.cueListDisagreementFixableCount ?? 0
    readonly property int anyFixableCount: consistencyController?.cueListFixableCount ?? 0
    readonly property int examined: consistencyController?.cueListCounts?.examined ?? 0
    readonly property int unreadable: consistencyController?.cueListCounts?.unreadable ?? 0
    readonly property bool staged: consistencyController?.legacyMemoryListFixStaged ?? false

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

    // What was looked at. A check that could open nothing says so, rather
    // than letting the sentences below read as a clean bill.
    Label {
        objectName: "cueListCounts"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: root.examined === 0 ? Theme.warnText : Theme.textMuted
        text: {
            if (root.examined === 0) {
                return root.unreadable === 0
                    ? "No track names an analysis file, so there were no cue lists to check."
                    : "No analysis file could be read (" + root.unreadable + (root.unreadable === 1 ? " is" : " are")
                      + " missing or damaged), so no cue list was checked.";
            }
            let text = "Checked " + root.examined + (root.examined === 1 ? " analysis file." : " analysis files.");
            if (root.unreadable > 0) {
                text += " " + root.unreadable + " could not be read, named at the end of this page.";
            }
            return text;
        }
    }

    Label {
        objectName: "memoryCueListsSummary"
        visible: root.checked && (root.examined > 0 || root.count > 0)
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

    // #60: the older and the newer list of a track disagree.
    Label {
        objectName: "cueListDisagreementSummary"
        visible: root.checked && root.examined > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: {
            const malformed = consistencyController?.cueListMalformedCount ?? 0;
            const unreadLists = malformed === 0 ? ""
                : " " + malformed + (malformed === 1 ? " track has" : " tracks have")
                  + " cue lists Seabass cannot read, listed below and left alone.";
            if (root.disagreeCount === 0) {
                return (malformed === 0 ? "On every track, the player and Seabass read the same cues."
                                        : "On every track Seabass could read, the player and Seabass read the same "
                                          + "cues.") + unreadLists;
            }
            let text = root.disagreeCount + (root.disagreeCount === 1 ? " track shows" : " tracks show")
                + " different cues on the player than in Seabass, or cues only one of the two lists holds. The "
                + "analysis files hold two cue lists, an older one that an XDJ-RX2 and a CDJ-3000X show, and a "
                + "newer one that Seabass reads first. A Seabass between 2026-09-02 and 2026-09-18 wrote only the "
                + "newer one.";
            if (root.disagreeFixableCount > 0) {
                text += " Repairing writes the cues you choose below into both lists.";
            }
            if (root.disagreeFixableCount < root.disagreeCount) {
                const left = root.disagreeCount - root.disagreeFixableCount;
                text += " " + left + (left === 1 ? " track is" : " tracks are") + " left alone, as said beside "
                    + (left === 1 ? "it" : "them") + ".";
            }
            return text + unreadLists;
        }
    }

    // Which list wins: one choice for the stick. The player's is the
    // default because it is what the DJ has been playing from.
    ColumnLayout {
        objectName: "cueListChoice"
        visible: root.checked && root.disagreeFixableCount > 0
        Layout.fillWidth: true
        spacing: Theme.scaled(6)
        ButtonGroup { id: keepGroup }
        RadioButton {
            objectName: "keepPlayerCueListsButton"
            text: "Keep what the player shows (the default)"
            ButtonGroup.group: keepGroup
            enabled: !root.staged
            checked: consistencyController?.keepPlayerCueLists ?? true
            onToggled: if (checked && consistencyController) consistencyController.keepPlayerCueLists = true
        }
        RadioButton {
            objectName: "keepSeabassCueListsButton"
            text: "Keep what Seabass wrote"
            ButtonGroup.group: keepGroup
            enabled: !root.staged
            checked: !(consistencyController?.keepPlayerCueLists ?? true)
            onToggled: if (checked && consistencyController) consistencyController.keepPlayerCueLists = false
        }
        Label {
            objectName: "cueListChoiceNote"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: (consistencyController?.keepPlayerCueLists ?? true)
                ? "The cues the player shows are the ones you have been playing with, so they are kept unless you "
                  + "choose otherwise. Seabass will then read the same."
                : "The player will show the cues Seabass shows, which are probably what you set in Engine DJ. Cues "
                  + "only the older list holds are kept too, since Seabass shows them as well."
        }
    }

    RowLayout {
        visible: root.checked
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "repairMemoryCueListsButton"
            visible: root.anyFixableCount > 0 || root.staged
            text: consistencyController?.legacyMemoryListFixStaged ? "Unstage" : "Repair"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.legacyMemoryListFixStaged
                ? "Take this back out of the changes to save"
                : "Stage rebuilding each memory cue list with its cues kept, writing the chosen cues into both "
                  + "lists where they disagree, and removing the files nothing refers to. "
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

    Repeater {
        objectName: "cueListDisagreementTracks"
        model: consistencyController?.cueListDisagreementTracks
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "“" + modelData.title + "”" + (modelData.artist.length > 0 ? ", " + modelData.artist : "") + ": "
                + modelData.what
        }
    }

    // The files that could not be read, by name, so "3 could not be
    // read" above can be acted on.
    Repeater {
        objectName: "cueListUnreadableTracks"
        model: consistencyController?.cueListUnreadableTracks
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
