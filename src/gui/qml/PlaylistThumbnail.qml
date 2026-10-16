// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// A playlist's small cover in a list of playlists: one image composed from
// its tracks' covers (PlaylistThumbnailProvider, the source
// ScanController.playlistThumbnails gives), on a tile of its own size, so
// the row does not move while it is composed. The tile, with a track
// glyph, is also what a playlist without covers shows ("" for a source).
Rectangle {
    id: root
    property string source: ""

    implicitWidth: Theme.scaled(18)
    implicitHeight: Theme.scaled(18)
    radius: Theme.cornerRadius / 2
    color: Theme.groupBackground

    SeabassIcon {
        objectName: "playlistThumbnailGlyph"
        anchors.centerIn: parent
        visible: root.source.length === 0
        iconName: "view-media-track"
        size: Theme.snap(root.width * 0.6)
        color: Theme.textMuted
    }
    Image {
        objectName: "playlistThumbnail"
        anchors.fill: parent
        // Composed at the pixels it is drawn at, and rounded there: a
        // mask on every row's image would cost each row a layer.
        readonly property int side: Math.ceil(root.width * Screen.devicePixelRatio)
        sourceSize: Qt.size(side, side)
        source: root.source
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
    }
}
