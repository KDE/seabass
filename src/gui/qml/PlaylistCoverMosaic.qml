// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// A playlist's cover, made from its tracks' own: the way a music player
// shows a playlist it has no picture for. One cover fills the square, two
// split it down the middle, three put the first on the left half and the
// other two stacked on the right, four or more make a 2 x 2 grid. With no
// cover at all, a neutral tile with a track glyph, so a playlist without
// art still has something in the place its picture goes.
//
// Each tile is an ArtworkImage over the tile's own ground: a cover that
// does not load leaves that tile plain rather than showing a broken
// frame, and the rest of the mosaic stands.
Rectangle {
    id: root

    // [{source, fallbackSource}], as ScanController.playlistSummary()
    // serves them. Only the first four are drawn.
    property var covers: []

    readonly property int tileCount: Math.min(4, root.covers ? root.covers.length : 0)

    // Where tile `i` of `n` goes, in fractions of the square.
    function tileRect(i, n) {
        if (n <= 1) {
            return {x: 0, y: 0, w: 1, h: 1};
        }
        if (n === 2) {
            return {x: i * 0.5, y: 0, w: 0.5, h: 1};
        }
        if (n === 3) {
            return i === 0 ? {x: 0, y: 0, w: 0.5, h: 1} : {x: 0.5, y: (i - 1) * 0.5, w: 0.5, h: 0.5};
        }
        return {x: (i % 2) * 0.5, y: Math.floor(i / 2) * 0.5, w: 0.5, h: 0.5};
    }

    implicitWidth: Theme.scaled(96)
    implicitHeight: implicitWidth
    // The gaps between the tiles are this ground showing through.
    color: Theme.surface
    border.color: Theme.border
    border.width: root.tileCount === 0 ? 1 : 0

    Repeater {
        model: root.tileCount
        delegate: Rectangle {
            id: tile
            required property int index
            objectName: "coverTile" + index
            readonly property var rect: root.tileRect(index, root.tileCount)
            // Half the gap on each inner edge, none on the outer ones, so
            // the mosaic's edge is the square's edge.
            readonly property real half: Theme.chartSliceGap / 2
            x: Theme.snap(rect.x * root.width + (rect.x > 0 ? half : 0))
            y: Theme.snap(rect.y * root.height + (rect.y > 0 ? half : 0))
            width: Theme.snap(rect.w * root.width - (rect.w < 1 ? half : 0))
            height: Theme.snap(rect.h * root.height - (rect.h < 1 ? half : 0))
            color: Theme.groupBackground
            radius: Theme.coverRadius(Math.min(width, height))
            ArtworkImage {
                objectName: "coverArt" + tile.index
                anchors.fill: parent
                source: root.covers[tile.index].source || ""
                fallbackSource: root.covers[tile.index].fallbackSource || ""
            }
        }
    }

    // No art in the whole playlist.
    Rectangle {
        objectName: "neutralCover"
        anchors.fill: parent
        anchors.margins: parent.border.width
        visible: root.tileCount === 0
        color: Theme.groupBackground
        SeabassIcon {
            anchors.centerIn: parent
            iconName: "view-media-track"
            size: Theme.snap(root.width * 0.45)
            color: Theme.textMuted
        }
    }
}
