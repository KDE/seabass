// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for playlists that do not hold the same tracks in
// every library on the stick (#61), and Engine playlist entries naming no
// track. Each playlist is matched to the library the DJ picks; export.pdb
// only loses entries here, since adding one is #62.
HealthCheckPage {
    id: root

    checkTitle: "Playlists"

    readonly property bool checked: consistencyController?.playlistsChecked ?? false
    readonly property string error: consistencyController?.playlistsError ?? ""
    readonly property var differences: consistencyController?.playlistDifferences ?? []
    readonly property int dangling: consistencyController?.danglingPlaylistEntryCount ?? 0

    function sideLine(side) {
        if (!side.hasPlaylist) {
            return side.library + ": no such playlist";
        }
        const parts = [side.library + ": " + side.members + (side.members === 1 ? " track" : " tracks")];
        if (side.lacking.length > 0) {
            parts.push("lacks " + side.lacking.length);
        }
        if (side.extra.length > 0) {
            parts.push(side.extra.length + " the others leave out");
        }
        return parts.join(", ");
    }

    function referenceTip(reference) {
        const parts = [];
        if (reference.swaps > 0) {
            parts.push(reference.swaps + " copies replaced by the copy " + reference.library + " plays");
        }
        if (reference.adds > 0) {
            parts.push(reference.adds + " added");
        }
        if (reference.removes > 0) {
            parts.push(reference.removes + " taken out");
        }
        let text = "Make every other library's copy of this playlist hold what " + reference.library + "'s holds: "
            + (parts.length > 0 ? parts.join(", ") : "nothing to change") + ".";
        if (reference.leftOut > 0) {
            text += " " + reference.leftOut + (reference.leftOut === 1 ? " track" : " tracks")
                + " cannot be added to the rekordbox library yet, so rekordbox and OneLibrary keep them out.";
        }
        return text;
    }

    Label {
        objectName: "playlistsNotChecked"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: !root.checked && root.error.length === 0 && !consistencyController?.busy
        color: Theme.textMuted
        text: "This stick has only one library, so there is nothing to compare its playlists with."
    }

    Label {
        objectName: "playlistsError"
        visible: root.error.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.danger
        text: root.error
    }

    Label {
        objectName: "playlistsSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.differences.length === 0
            ? "Every playlist holds the same tracks in every library on this stick."
            : (root.differences.length === 1 ? "One playlist does" : root.differences.length + " playlists do")
              + " not hold the same tracks in every library on this stick. A Pioneer player reads the "
              + "rekordbox side and a Denon player the Engine side, so the set changes with the player. Pick "
              + "which library each playlist should follow; Seabass makes the others match. Only tracks every "
              + "library lists are compared, and order is not. Seabass cannot add a track to the rekordbox "
              + "library's playlists yet, only take one out."
    }

    RowLayout {
        visible: root.checked && root.dangling > 0
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Label {
            objectName: "danglingSummary"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.dangling + " Engine playlist " + (root.dangling === 1 ? "entry points" : "entries point")
                + " at a track the library no longer has ("
                + (consistencyController?.danglingPlaylists ?? []).map((d) => d.playlist + ": " + d.entries).join(", ")
                + "). A player skips them."
        }
        Button {
            objectName: "removeDanglingButton"
            text: consistencyController?.danglingFixStaged ? "Unstage" : "Remove Them"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            onClicked: consistencyController?.danglingFixStaged
                ? consistencyController?.unstageDanglingPlaylistEntries()
                : consistencyController?.removeDanglingPlaylistEntries()
        }
    }

    Repeater {
        objectName: "playlistDifferences"
        model: root.differences
        delegate: ColumnLayout {
            id: playlistRow
            required property var modelData
            Layout.fillWidth: true
            Layout.topMargin: Theme.tightSpacing
            spacing: Theme.tightSpacing

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.bold: true
                text: playlistRow.modelData.name
            }
            Repeater {
                model: playlistRow.modelData.sides
                delegate: Label {
                    required property var modelData
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: root.sideLine(modelData)
                }
            }
            Label {
                visible: playlistRow.modelData.missingSomewhere
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: "Seabass cannot create a playlist in a library that lacks it yet."
            }
            RowLayout {
                spacing: Theme.rowSpacing
                Repeater {
                    model: playlistRow.modelData.references
                    delegate: Button {
                        required property var modelData
                        objectName: "match_" + modelData.format
                        readonly property bool hasWork: modelData.adds + modelData.removes + modelData.swaps > 0
                        visible: hasWork && playlistRow.modelData.staged.length === 0
                        text: "Follow " + modelData.library
                        enabled: !consistencyController?.busy && !consistencyController?.writing
                            && !consistencyController?.stickReadOnly
                        ToolTip.visible: hovered
                        ToolTip.text: root.referenceTip(modelData)
                        onClicked: consistencyController?.alignPlaylist(playlistRow.modelData.name, modelData.format)
                    }
                }
                Label {
                    objectName: "stagedPlaylistNote"
                    visible: playlistRow.modelData.staged.length > 0
                    color: Theme.warnText
                    text: "staged: following " + playlistRow.modelData.references
                        .filter((r) => r.format === playlistRow.modelData.staged)
                        .map((r) => r.library).join("") + ", not saved yet"
                }
                Button {
                    objectName: "unstagePlaylistButton"
                    visible: playlistRow.modelData.staged.length > 0
                    text: "Unstage"
                    onClicked: consistencyController?.unstagePlaylist(playlistRow.modelData.name)
                }
            }
        }
    }

    Item { Layout.fillHeight: true }
}
