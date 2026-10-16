// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

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
//
// And read once: a decoded cover is kept on the computer (cacheUrl()), so
// coming back to a page, or starting again, does not read it again.
Item {
    id: artwork

    property url source
    property url fallbackSource
    property int fillMode: Image.PreserveAspectCrop
    property alias sourceSize: image.sourceSize
    property alias asynchronous: image.asynchronous
    property alias smooth: image.smooth
    // The corners' radius, in pixels (Theme.coverRadius by default: half
    // the usual on a small cover). 0, square.
    property real radius: Theme.coverRadius(Math.min(artwork.width, artwork.height))

    // Drawn now, by whichever draws it (the rounding shader or the plain
    // image), and how far it has faded in: what the image's own visible
    // and opacity said before the corners were rounded.
    readonly property bool drawn: image.status === Image.Ready
    readonly property real reveal: artwork.revealTarget
    property real revealTarget: image.status === Image.Ready ? 1 : 0
    // Fades in when it arrives: read off the GUI thread, the art comes a
    // moment after its row, and popping in reads as a glitch. Short, so a
    // list scrolled past does not shimmer.
    Behavior on revealTarget {
        NumberAnimation { duration: 160; easing.type: Easing.OutQuad }
    }
    // The corners are cut by a shader where one can run (rounded_art.frag);
    // on the software scene graph, or with no shader in the build, the
    // plain image is drawn instead, square: never nothing.
    // The shader that cuts them. A test points this somewhere that does
    // not load to see the fallback; nothing else changes it.
    property url roundingShader: "qrc:/seabass/shaders/rounded_art.frag.qsb"
    readonly property bool rounded: artwork.radius > 0 && GraphicsInfo.api !== GraphicsInfo.Software
        && GraphicsInfo.api !== GraphicsInfo.Unknown && roundedArt.status !== ShaderEffect.Error

    // Which one is on screen: "source", "fallback", or "" while neither
    // has loaded (or neither can).
    readonly property string showing: image.status !== Image.Ready ? ""
        : (artwork.chosenSource === artwork.source && !artwork.sourceFailed ? "source" : "fallback")

    // The one of the two being loaded, as given: `source` first,
    // `fallbackSource` when that is empty or failed.
    readonly property url chosenSource: artwork.source.toString().length > 0 && !artwork.sourceFailed
        ? artwork.source : artwork.fallbackSource

    // A local file goes through the cover cache (gui/cover_cache.hpp), so
    // a cover read once is not read from the stick again, in this run or
    // the next; anything else loads as it is.
    function cacheUrl(url) {
        const text = url.toString();
        return text.startsWith("file:") ? "image://cover/" + encodeURIComponent(text) : text;
    }
    // `source` as the image asks for it. A property, not a call: the
    // failure is noted after the fact (noteSourceFailed()), possibly while
    // a scrolled-away row is being destroyed, where no function of it may
    // run any more.
    readonly property url sourceAsLoaded: artwork.cacheUrl(artwork.source)

    // Set when `source` failed to load, cleared when it changes.
    property bool sourceFailed: false
    onSourceChanged: artwork.sourceFailed = false
    function noteSourceFailed() {
        if (image.status === Image.Error && image.source === artwork.sourceAsLoaded) {
            artwork.sourceFailed = true;
        }
    }

    Image {
        id: image
        objectName: "artworkImage"
        anchors.fill: parent
        fillMode: artwork.fillMode
        // Drawn itself only where the shader cannot draw it; otherwise it
        // is the shader's texture, an Image being a texture provider of
        // its own (no layer).
        visible: artwork.drawn && !artwork.rounded
        opacity: artwork.reveal
        asynchronous: true
        // Decoded to the drawn square, at the screen's pixel ratio: a
        // 1400 px cover shown at 32 px is a 32 px image's work. Square,
        // since the art is cropped to fill one; the larger side, so a
        // cropped non-square image still has the pixels it is cut from.
        // Until the item has a size, the image's own.
        readonly property real drawnSide: Math.ceil(Math.max(artwork.width, artwork.height) * Screen.devicePixelRatio)
        sourceSize: image.drawnSide > 0 ? Qt.size(image.drawnSide, image.drawnSide) : undefined
        source: artwork.cacheUrl(artwork.chosenSource)
        // A load can fail while `source` is still being assigned (a local
        // file used to load synchronously, and a cached failure still
        // answers at once); switching to the fallback right there is a
        // binding loop that leaves the source as it was. Noted after the
        // assignment instead.
        onStatusChanged: {
            if (image.status === Image.Error && image.source === artwork.sourceAsLoaded) {
                Qt.callLater(artwork.noteSourceFailed);
            }
        }
    }

    ShaderEffect {
        id: roundedArt
        objectName: "roundedArtwork"
        anchors.fill: parent
        visible: artwork.drawn && artwork.rounded
        opacity: artwork.reveal
        fragmentShader: artwork.roundingShader
        property var art: image
        property size itemSize: Qt.size(width, height)
        property real radius: artwork.radius
        // The item's share of the painted picture: under 1 on the axis a
        // crop cuts, over 1 where a fit leaves room.
        property size texScale: Qt.size(image.paintedWidth > 0 ? width / image.paintedWidth : 1,
                                        image.paintedHeight > 0 ? height / image.paintedHeight : 1)
    }
}
