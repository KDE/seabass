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
//
// Read and decoded off the GUI thread, at the size it is drawn. Every
// list of tracks shows one of these per row, and a list rebuilt (a
// playlist switched, a search typed) loaded every visible row's art from
// the stick on the GUI thread, at the image's full size: a stick on
// Windows can take far longer than a frame to answer, and the whole page
// waited for it. Now a row appears at once and its art when it is read.
Item {
    id: artwork

    property url source
    property url fallbackSource
    property int fillMode: Image.PreserveAspectCrop
    property alias sourceSize: image.sourceSize
    property alias asynchronous: image.asynchronous
    property alias smooth: image.smooth

    // Which one is on screen: "source", "fallback", or "" while neither
    // has loaded (or neither can).
    readonly property string showing: image.status !== Image.Ready ? ""
        : (image.source === artwork.source && !artwork.sourceFailed ? "source" : "fallback")

    // Set when `source` failed to load, cleared when it changes.
    property bool sourceFailed: false
    onSourceChanged: artwork.sourceFailed = false
    function noteSourceFailed() {
        if (image.status === Image.Error && image.source === artwork.source) {
            artwork.sourceFailed = true;
        }
    }

    Image {
        id: image
        objectName: "artworkImage"
        anchors.fill: parent
        fillMode: artwork.fillMode
        visible: image.status === Image.Ready
        asynchronous: true
        // Decoded to the drawn square, at the screen's pixel ratio: a
        // 1400 px cover shown at 32 px is a 32 px image's work. Square,
        // since the art is cropped to fill one; the larger side, so a
        // cropped non-square image still has the pixels it is cut from.
        // Until the item has a size, the image's own.
        readonly property real drawnSide: Math.ceil(Math.max(artwork.width, artwork.height) * Screen.devicePixelRatio)
        sourceSize: image.drawnSide > 0 ? Qt.size(image.drawnSide, image.drawnSide) : undefined
        source: artwork.source.toString().length > 0 && !artwork.sourceFailed ? artwork.source : artwork.fallbackSource
        // A load can fail while `source` is still being assigned (a local
        // file used to load synchronously, and a cached failure still
        // answers at once); switching to the fallback right there is a
        // binding loop that leaves the source as it was. Noted after the
        // assignment instead.
        onStatusChanged: {
            if (image.status === Image.Error && image.source === artwork.source) {
                Qt.callLater(artwork.noteSourceFailed);
            }
        }
    }
}
