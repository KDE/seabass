// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for playlists that do not hold the same tracks in
// every library on the stick (#61), and Engine playlist entries naming no
// track. Each playlist is matched to the library the DJ picks, which
// creates it where it is missing, or deleted from every library.
HealthCheckPage {
    id: root

    checkTitle: "Playlists"

    readonly property bool checked: consistencyController?.playlistsChecked ?? false
    readonly property string error: consistencyController?.playlistsError ?? ""
    readonly property var differences: consistencyController?.playlistDifferences ?? []
    readonly property int dangling: consistencyController?.danglingPlaylistEntryCount ?? 0
    readonly property int unresolved: consistencyController?.unresolvedPlaylistEntryCount ?? 0

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

    // "rekordbox", "rekordbox and Engine", "rekordbox, OneLibrary and Engine".
    function spokenList(items) {
        if (items.length < 2) {
            return items.join("");
        }
        return items.slice(0, items.length - 1).join(", ") + " and " + items[items.length - 1];
    }

    function librariesHaving(difference) {
        return difference.sides.filter((side) => side.hasPlaylist).map((side) => side.library);
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
        const creates = reference.creates ?? [];
        let text = "";
        if (creates.length > 0) {
            const tracks = reference.createdTracks ?? 0;
            text = "Creates it in " + root.spokenList(creates) + " with " + tracks
                + (tracks === 1 ? " track. " : " tracks. ");
        }
        text += "Make every other library's copy of this playlist hold what " + reference.library + "'s holds: "
            + (parts.length > 0 ? parts.join(", ") : "nothing to change") + ".";
        if (reference.leftOut > 0) {
            text += " " + reference.leftOut + (reference.leftOut === 1 ? " track stays" : " tracks stay")
                + " out of OneLibrary, which only takes what the rekordbox library holds.";
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
              + "library lists are compared, and order is not. A track added to a playlist goes at its end. "
              + "A playlist only some libraries have can be created in the others from the one that has it, "
              + "or deleted everywhere."
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

    // Entries a Denon player cannot see: written with the stick's own
    // library uuid where the tracks name another as their origin.
    RowLayout {
        visible: root.checked && root.unresolved > 0
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Label {
            objectName: "unresolvedSummary"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.unresolved + " Engine playlist " + (root.unresolved === 1 ? "entry" : "entries")
                + " the player cannot see ("
                + (consistencyController?.unresolvedPlaylists ?? []).map((d) => d.playlist + ": " + d.entries).join(", ")
                + "): they name their track by another library than the one the track says it came from, so a "
                + "Denon player leaves them out of the playlist. Seabass can point them at the track's own library."
        }
        Button {
            objectName: "repairUnresolvedButton"
            text: consistencyController?.unresolvedFixStaged ? "Unstage" : "Repair Them"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            onClicked: consistencyController?.unresolvedFixStaged
                ? consistencyController?.unstageUnresolvedPlaylistEntries()
                : consistencyController?.repairUnresolvedPlaylistEntries()
        }
    }

    // The playlists, as many as the stick has: a list that scrolls, with
    // room under the last row for the floating Save button (a long list
    // ran off the page and could not be scrolled, Sebastian 2026-10-16).
    Flickable {
        id: differencesView
        objectName: "playlistDifferencesView"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        contentWidth: width
        contentHeight: differencesColumn.implicitHeight
        bottomMargin: root.saveClearance
        interactive: contentHeight + bottomMargin > height
        ScrollBar.vertical: BigScrollBar {}
        ColumnLayout {
            id: differencesColumn
            width: differencesView.width
            spacing: Theme.tightSpacing
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
                    // A playlist missing somewhere used to say Seabass
                    // could not create it (#61, before Browse could, #62)
                    // and came back after every save; following a library
                    // now creates it, and Delete Playlist removes it.
                    RowLayout {
                        spacing: Theme.rowSpacing
                        Repeater {
                            model: playlistRow.modelData.references
                            delegate: Button {
                                required property var modelData
                                objectName: "match_" + modelData.format
                                readonly property bool hasWork: modelData.adds + modelData.removes + modelData.swaps > 0
                                    || (modelData.creates ?? []).length > 0
                                visible: hasWork && playlistRow.modelData.staged.length === 0
                                text: "Follow " + modelData.library
                                enabled: !consistencyController?.busy && !consistencyController?.writing
                                    && !consistencyController?.stickReadOnly
                                readonly property string tip: root.referenceTip(modelData)
                                ToolTip.visible: hovered
                                ToolTip.text: tip
                                onClicked: consistencyController?.alignPlaylist(playlistRow.modelData.name, modelData.format)
                            }
                        }
                        // The other resolution, on every row: gone from
                        // every library that has it (Sebastian 2026-10-17).
                        Button {
                            objectName: "deletePlaylistButton"
                            visible: playlistRow.modelData.staged.length === 0
                            text: "Delete Playlist"
                            enabled: !consistencyController?.busy && !consistencyController?.writing
                                && !consistencyController?.stickReadOnly
                            readonly property string tip: "Deletes \"" + playlistRow.modelData.name + "\" from "
                                + root.spokenList(root.librariesHaving(playlistRow.modelData)) + ". Tracks stay."
                            ToolTip.visible: hovered
                            ToolTip.text: tip
                            onClicked: consistencyController?.deletePlaylistEverywhere(playlistRow.modelData.name)
                        }
                        Label {
                            objectName: "stagedPlaylistNote"
                            visible: playlistRow.modelData.staged.length > 0
                            color: Theme.warnText
                            text: playlistRow.modelData.staged === "delete"
                                ? "staged: deleting from " + root.spokenList(root.librariesHaving(playlistRow.modelData))
                                  + ", not saved yet"
                                : "staged: following " + playlistRow.modelData.references
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
        }
    }
}