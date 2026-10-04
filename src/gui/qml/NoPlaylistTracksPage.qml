// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for tracks no library on the stick has in any
// playlist. The DJ picks which to delete; deleting works like Clean Up:
// each track out of every library, its file listed for Delete Orphaned
// Files. A stick with no playlists at all lists every track, which says
// nothing about what is unwanted, so nothing is offered then.
HealthCheckPage {
    id: root

    checkTitle: "Tracks not in any playlist"

    readonly property bool checked: consistencyController?.playlistsChecked ?? false
    readonly property var tracks: consistencyController?.noPlaylistTracks ?? []
    readonly property bool hasPlaylists: consistencyController?.stickHasPlaylists ?? false
    readonly property int stagedCount: consistencyController?.noPlaylistStagedCount ?? 0
    // filePath -> true for the rows ticked on this page.
    property var picked: ({})
    property int pickedCount: 0

    function setPicked(filePath, on) {
        const next = Object.assign({}, root.picked);
        if (on) {
            next[filePath] = true;
        } else {
            delete next[filePath];
        }
        root.picked = next;
        root.pickedCount = Object.keys(next).length;
    }

    function pickAll(on) {
        const next = {};
        if (on) {
            for (const t of root.tracks) {
                next[t.filePath] = true;
            }
        }
        root.picked = next;
        root.pickedCount = Object.keys(next).length;
    }

    Label {
        objectName: "noPlaylistSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.tracks.length === 0
            ? "Every track on this stick is in at least one playlist."
            : (root.tracks.length === 1 ? "One track is" : root.tracks.length + " tracks are")
              + " in no playlist in any library on this stick."
    }

    Label {
        objectName: "noPlaylistsOnStick"
        visible: root.checked && !root.hasPlaylists && root.tracks.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: "This stick has no playlists, so every track is listed here. That says nothing about which "
            + "tracks are unwanted, so Seabass offers no deletion."
    }

    Label {
        objectName: "noPlaylistExplanation"
        visible: root.checked && root.hasPlaylists && root.tracks.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: "Deleting takes a track out of every library on the stick and lists its file under Delete Orphaned "
            + "Files, as Clean Up does. A stick backup is made first. Nothing changes until you Save."
    }

    RowLayout {
        visible: root.checked && root.hasPlaylists && root.tracks.length > 0
        spacing: Theme.rowSpacing
        Button {
            objectName: "pickAllButton"
            text: "Select All"
            enabled: root.stagedCount === 0
            onClicked: root.pickAll(true)
        }
        Button {
            objectName: "pickNoneButton"
            text: "Select None"
            enabled: root.stagedCount === 0 && root.pickedCount > 0
            onClicked: root.pickAll(false)
        }
        Button {
            objectName: "deleteNoPlaylistButton"
            text: root.stagedCount > 0 ? "Unstage"
                : (root.pickedCount === 1 ? "Delete 1 Track" : "Delete " + root.pickedCount + " Tracks")
            enabled: (root.stagedCount > 0 || root.pickedCount > 0) && !consistencyController?.busy
                && !consistencyController?.writing && !consistencyController?.stickReadOnly
            onClicked: {
                if (root.stagedCount > 0) {
                    consistencyController?.unstageTracksInNoPlaylist();
                } else {
                    consistencyController?.deleteTracksInNoPlaylist(Object.keys(root.picked));
                }
            }
        }
    }

    Repeater {
        objectName: "noPlaylistTracks"
        model: root.tracks
        delegate: RowLayout {
            id: trackRow
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.rowSpacing
            SeabassCheckBox {
                objectName: "pick_" + trackRow.modelData.filePath
                visible: root.hasPlaylists
                enabled: root.stagedCount === 0
                checked: trackRow.modelData.staged || root.picked[trackRow.modelData.filePath] === true
                onToggled: root.setPicked(trackRow.modelData.filePath, checked)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font.bold: true
                    font.strikeout: trackRow.modelData.staged
                    text: trackRow.modelData.title.length > 0 ? trackRow.modelData.title : trackRow.modelData.filePath
                }
                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    text: (trackRow.modelData.artist.length > 0 ? trackRow.modelData.artist + " · " : "")
                        + trackRow.modelData.libraries
                }
            }
        }
    }
}
