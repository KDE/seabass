// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// Cover art with a second source to fall back on. The readers name the
// art a catalog names without looking for the file (a stat per image on
// every read cost seconds on a stick), so whether it is there is found
// out here, when it is drawn: `source` first, `fallbackSource` when that
// is empty or does not load, and nothing at all when neither does. Never
// a broken-image frame: a failed image is simply not shown, and whatever
// is behind this item (a row's placeholder square) shows through.
Item {
    id: artwork

    property url source
    property url fallbackSource
    property int fillMode: Image.PreserveAspectCrop
    property alias sourceSize: image.sourceSize

    // Which one is on screen: "source", "fallback", or "" while neither
    // has loaded (or neither can).
    readonly property string showing: image.status !== Image.Ready ? ""
        : (image.source === artwork.source && !artwork.sourceFailed ? "source" : "fallback")

    // Set when `source` failed to load, cleared when it changes.
    property bool sourceFailed: false
    onSourceChanged: artwork.sourceFailed = false

    Image {
        id: image
        objectName: "artworkImage"
        anchors.fill: parent
        fillMode: artwork.fillMode
        visible: image.status === Image.Ready
        source: artwork.source.toString().length > 0 && !artwork.sourceFailed ? artwork.source : artwork.fallbackSource
        onStatusChanged: {
            if (image.status === Image.Error && image.source === artwork.source) {
                artwork.sourceFailed = true;
            }
        }
    }
}
