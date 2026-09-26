// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import SeabassGui

Frame {
    id: root
    required property var controller

    // Over the player, so the close button can show itself only then.
    HoverHandler { id: playerHover }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        // A big, backlit-looking transport button, styled after a hardware
        // DJ controller's play button rather than a flat UI button.
        Rectangle {
            id: playButton
            Layout.preferredWidth: 64
            Layout.preferredHeight: 64
            radius: width / 2
            color: playMouseArea.pressed ? Qt.darker(Material.accent, 1.4) : Material.accent
            border.color: Theme.background
            border.width: 2

            // Hand-drawn rather than a font glyph: icon-font play/pause
            // characters carry their own (inconsistent, per-font) internal
            // padding, so centering them by anchoring the Text item never
            // lines the visible ink up with the circle -- only exact
            // geometry does. Both shapes are built with a bounding box
            // exactly centered on (cx, cy).
            Canvas {
                id: playIcon
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.reset();
                    ctx.fillStyle = "white";
                    var cx = width / 2, cy = height / 2;
                    if (root.controller.playing) {
                        var barW = width * 0.13;
                        var barH = height * 0.42;
                        var gap = width * 0.12;
                        ctx.fillRect(cx - gap / 2 - barW, cy - barH / 2, barW, barH);
                        ctx.fillRect(cx + gap / 2, cy - barH / 2, barW, barH);
                    } else {
                        var w = width * 0.36, h = height * 0.42;
                        ctx.beginPath();
                        ctx.moveTo(cx - w / 2, cy - h / 2);
                        ctx.lineTo(cx - w / 2, cy + h / 2);
                        ctx.lineTo(cx + w / 2, cy);
                        ctx.closePath();
                        ctx.fill();
                    }
                }
                Connections {
                    target: root.controller
                    function onPlayingChanged() { playIcon.requestPaint(); }
                }
            }

            MouseArea {
                id: playMouseArea
                anchors.fill: parent
                onClicked: root.controller.togglePlay()
            }
        }

        // Its square only while there is art to show in it: a cover the
        // stick does not have leaves no empty gap before the title.
        ArtworkImage {
            id: playerArtwork
            objectName: "playerArtwork"
            Layout.preferredWidth: 64
            Layout.preferredHeight: 64
            fillMode: Image.PreserveAspectFit
            visible: playerArtwork.showing.length > 0
            source: root.controller.artworkPath
            fallbackSource: root.controller.fallbackArtworkPath || ""
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            RowLayout {
                Layout.fillWidth: true
                // Room for the close button in the corner above it, so it
                // never covers the time.
                Layout.rightMargin: closeButton.width
                Label {
                    text: root.controller.title + (root.controller.artist.length > 0 ? "  - " + root.controller.artist : "")
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "playerTime"
                    text: Theme.trackTime(root.controller.position) + " / " + Theme.trackTime(root.controller.duration)
                    color: Theme.textMuted
                }
            }

            Label {
                visible: root.controller.errorMessage.length > 0
                text: root.controller.errorMessage
                color: Theme.danger
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            WaveformView {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
                waveformData: root.controller.waveform
                cueData: root.controller.cues
                trackDurationMs: root.controller.duration
                progress: trackDurationMs > 0 ? root.controller.position / trackDurationMs : 0
                onSeekRequested: (ratio) => root.controller.seek(ratio * root.controller.duration)
            }
        }
    }

    // Closing is one click, in the top right corner, shown while the
    // pointer is over the player. Stopping unloads the track, and the bar
    // goes with it: Main.qml shows it only while one is loaded.
    ToolButton {
        id: closeButton
        objectName: "closePlayerButton"
        // The Frame's own corner, not its content's: declared children go
        // into the contentItem, inside the padding.
        parent: root
        anchors.top: parent.top
        anchors.right: parent.right
        z: 1
        implicitWidth: Theme.snap(Theme.iconSizeSmall * 0.75)
        implicitHeight: Theme.snap(Theme.iconSizeSmall * 0.75)
        padding: 0
        flat: true
        display: AbstractButton.IconOnly
        text: "Close the player"
        icon.source: Theme.iconUrl("window-close")
        icon.color: Theme.text
        icon.width: Theme.iconSizeSmall * 0.5
        icon.height: Theme.iconSizeSmall * 0.5
        opacity: playerHover.hovered || hovered ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        ToolTip.visible: hovered
        ToolTip.text: "Close the player"
        onClicked: root.controller.stop()
    }
}
