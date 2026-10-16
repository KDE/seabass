// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Browse's drop to reorder (Experimental): a track of the playlist shown,
// dragged over the track list and held over the gap between two rows for
// hoverDelay, opens that gap. The rows below it move down by a row
// (shiftFor), a faded copy of the dragged row sits in it saying "Move
// track here", and release() asks for the move to that place
// (moveRequested). Moving on follows the pointer at once; leaving the
// list, or a drag that is not one of the playlist's tracks, closes it.
//
// Every input is explicit, so the page wires it to whatever drag there is:
// the drag item (Drag.keys ["seabass/track"], carrying the row's roles),
// where the pointer is, and the list's geometry. Laid over the list,
// filling it: the copy is drawn in the list's own coordinates.
Item {
    id: gap

    // The drag item, or null: filePath, title, artist, artworkPath,
    // fallbackArtworkPath and playlistNames are read from it.
    property var draggedTrack: null
    // Its row in the list, or -1 when the list does not show it.
    property int draggedIndex: -1
    // The playlist shown, and whether the list shows it whole in its own
    // order: a place in a sorted or searched list is no place in the
    // playlist, so nothing opens then.
    property string playlist: ""
    property bool orderShown: false
    // A drag is over the list, the pointer at pointerY in the list's
    // coordinates.
    property bool hovering: false
    property real pointerY: 0
    // The list's geometry: its scroll position, where its first row
    // starts, a row's height and how many rows there are.
    property real contentY: 0
    property real originY: 0
    property real rowHeight: 56
    property int rowCount: 0
    // How long the pointer has to stay over one gap before it opens.
    property int hoverDelay: 1500

    // The open gap, before the row of that index (rowCount: after the
    // last), or -1.
    property int gapIndex: -1
    readonly property real gapHeight: rowHeight

    // The drag is a track of the playlist shown, which the list shows.
    readonly property bool applies: orderShown && playlist.length > 0 && draggedTrack !== null
        && draggedTrack !== undefined && draggedIndex >= 0 && !!draggedTrack.playlistNames
        && draggedTrack.playlistNames.indexOf(playlist) >= 0

    // The move a release at the open gap asks for: the place the track
    // takes in the playlist afterwards (the dragged row's own place no
    // longer counts below it).
    signal moveRequested(string filePath, string title, int toIndex)

    // How far the row at `index` moves down to make the gap.
    function shiftFor(index) {
        return gapIndex >= 0 && index >= gapIndex ? gapHeight : 0;
    }

    // Where the track goes when dropped at gap `g`, or -1 when that leaves
    // it where it is (the gaps on either side of its own row).
    function toIndexFor(g) {
        if (g < 0 || draggedIndex < 0) {
            return -1;
        }
        const to = g > draggedIndex ? g - 1 : g;
        return to === draggedIndex ? -1 : to;
    }

    // The drag let go over the list: the move, when a gap is open and it
    // moves the track. Returns whether it asked for one.
    function release() {
        const to = applies ? toIndexFor(gapIndex) : -1;
        const track = draggedTrack;
        collapse();
        if (to < 0) {
            return false;
        }
        gap.moveRequested(track.filePath, track.title, to);
        return true;
    }

    function collapse() {
        delay.stop();
        pending = -1;
        gapIndex = -1;
    }

    // The gap at content position `y`. With a gap open, the rows below it
    // sit a row lower and the copy fills it, so the pointer over the copy
    // stays at that gap rather than flickering between two.
    function gapAt(y) {
        let row = y - originY;
        if (gapIndex >= 0) {
            const top = gapIndex * rowHeight;
            if (row >= top && row < top + gapHeight) {
                return gapIndex;
            }
            if (row >= top + gapHeight) {
                row -= gapHeight;
            }
        }
        return Math.max(0, Math.min(rowCount, Math.round(row / rowHeight)));
    }

    // The gap the pointer waits over until the delay opens it.
    property int pending: -1

    function update() {
        if (!hovering || !applies) {
            collapse();
            return;
        }
        const g = gapAt(pointerY + contentY);
        if (gapIndex >= 0) {
            gapIndex = g;
        } else if (g !== pending) {
            pending = g;
            delay.restart();
        }
    }

    onHoveringChanged: update()
    onAppliesChanged: update()
    onPointerYChanged: update()
    onContentYChanged: update()

    Timer {
        id: delay
        interval: gap.hoverDelay
        onTriggered: {
            if (gap.hovering && gap.applies && gap.pending >= 0) {
                gap.gapIndex = gap.pending;
            }
        }
    }

    // The faded copy of the dragged row, in the gap.
    Rectangle {
        id: ghost
        objectName: "dropGapGhost"
        visible: gap.gapIndex >= 0
        x: 0
        y: gap.originY + gap.gapIndex * gap.rowHeight - gap.contentY
        width: gap.width
        height: gap.gapHeight
        radius: Theme.cornerRadius
        color: Theme.rowHover
        border.color: Theme.accent
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.rowSpacing
            anchors.rightMargin: Theme.rowSpacing
            spacing: Theme.rowSpacing
            opacity: 0.45
            ArtworkImage {
                Layout.preferredWidth: gap.gapHeight - 2 * Theme.tightSpacing
                Layout.preferredHeight: gap.gapHeight - 2 * Theme.tightSpacing
                source: gap.draggedTrack && gap.draggedTrack.artworkPath ? gap.draggedTrack.artworkPath : ""
                fallbackSource: gap.draggedTrack && gap.draggedTrack.fallbackArtworkPath
                    ? gap.draggedTrack.fallbackArtworkPath : ""
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    objectName: "dropGapTitle"
                    text: gap.draggedTrack ? gap.draggedTrack.title : ""
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    text: gap.draggedTrack ? gap.draggedTrack.artist : ""
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }

        Label {
            objectName: "dropGapLabel"
            anchors.centerIn: parent
            text: qsTr("Move track here")
            font.bold: true
            color: Theme.text
        }
    }
}
