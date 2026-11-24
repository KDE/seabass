// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A track row's cover with its play key (#133): every row that shows a
// cover plays its track from it, the same way. The key shows while the
// pointer is on the cover, or anywhere on the row where the row says so
// (rowHovered), and while the row is the track loaded, as a pause key
// then. A track that cannot be played from here gets the key disabled,
// and the cover says why (unavailableText).
//
// The page plays the track (playRequested): what it hands the player,
// cues included, is the page's to read. A press on the track already
// loaded plays or pauses it.
//
// A Rectangle, so a page sizes and colours it as it did its own tile;
// anything declared inside lands over the key (Browse's drag area).
Rectangle {
    id: cover

    // The cover image, as ArtworkImage takes it.
    property alias source: art.source
    property alias fallbackSource: art.fallbackSource
    property alias asynchronous: art.asynchronous
    property alias artworkObjectName: art.objectName

    property var playbackController: null
    // Who the track is to the player, {format, libraryPath, sourceId}: what
    // the page hands load(), and how the key knows its track is loaded. No
    // format, no key.
    property var playSource: ({})
    // The audio file, or empty where it cannot be played from here.
    property string playFilePath: ""
    property string unavailableText: ""
    // What the key says over a track not loaded.
    property string playText: "Play this track"
    // The row is hovered, for a row that shows the key from anywhere on
    // it; false shows it only over the cover itself.
    property bool rowHovered: false

    readonly property bool isCurrentTrack: cover.playbackController !== null
        && cover.playbackController.hasTrack
        && (cover.playSource.format || "").length > 0
        && cover.playbackController.currentFormat === cover.playSource.format
        && cover.playbackController.currentSourceId === (cover.playSource.sourceId || "")
        && cover.playbackController.currentLibraryPath === (cover.playSource.libraryPath || "")
    // Its track is the one loaded, and it is playing: the key pauses.
    readonly property bool playingThis: cover.isCurrentTrack && cover.playbackController.playing
    readonly property bool coverHovered: coverHover.hovered
    readonly property alias playKey: key

    // The key, pressed over a track that is not the one loaded.
    signal playRequested()
    // The key's press, as the key does it: pauses or plays the track
    // loaded, asks for any other. For a page whose own area over the cover
    // takes the click (Browse's drag area).
    function press() {
        if (cover.isCurrentTrack) {
            cover.playbackController.togglePlay();
            return;
        }
        cover.playRequested();
    }

    radius: Theme.coverRadius(Math.min(width, height))
    color: Theme.surface

    // A cover the catalog names but the stick does not have draws
    // nothing; the tile behind it shows.
    ArtworkImage {
        id: art
        objectName: "rowArtwork"
        anchors.fill: parent
    }

    // On the cover rather than the key: a disabled key gets no hover, and
    // that is the one that most needs explaining.
    HoverHandler { id: coverHover }
    // And only with something to say: a page may give a disabled key no
    // reason, and an empty bubble is noise.
    ToolTip.visible: coverHover.hovered && key.visible && ToolTip.text.length > 0
    ToolTip.delay: 400
    ToolTip.text: cover.isCurrentTrack
        ? (cover.playingThis ? "Pause" : "Play")
        : (key.enabled ? cover.playText : cover.unavailableText)

    ToolButton {
        id: key
        objectName: "rowPlayButton"
        anchors.fill: parent
        // Every side, not `padding`: a style that sets a side itself
        // (FluentWinUI3 does) wins over it, and moved this off the line.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        display: AbstractButton.IconOnly
        text: cover.playingThis ? "Pause" : "Play"
        visible: cover.playbackController !== null && (cover.playSource.format || "").length > 0
            && (cover.rowHovered || coverHover.hovered || cover.isCurrentTrack)
        enabled: cover.isCurrentTrack || cover.playFilePath.length > 0
        contentItem: Item {
            SeabassIcon {
                anchors.centerIn: parent
                iconName: cover.playingThis ? "media-playback-pause" : "media-playback-start"
                size: Theme.iconSizeSmall * 0.6
                color: "white"
                opacity: key.enabled ? 1 : 0.4
            }
        }
        background: Rectangle {
            radius: cover.radius
            color: "black"
            opacity: key.hovered ? 0.55 : 0.4
        }
        onClicked: cover.press()
    }
}
