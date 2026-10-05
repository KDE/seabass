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

        // The play key, after the deck the loaded track plays on: a wide
        // flat key for a track from either Rekordbox catalog (a CDJ's or
        // XDJ's transport key), a round pad for one from Engine (a Prime
        // deck's). The face stays dark; a rim and the glyph light up in
        // transport green while playing, and while paused the key gives
        // the deck's own idle signal, a blink or a breath. Switching
        // library morphs one form into the other. Sizes and colours are
        // Theme's transport* tokens; see there for what is evoked and
        // what is deliberately not reproduced.
        Rectangle {
            id: playButton
            objectName: "playButton"
            // Both Rekordbox catalogs play on the same decks.
            readonly property bool pioneerForm: root.controller.currentFormat !== "engine"
            // 0 is the play triangle, 1 the pause bars; in between, the
            // fold from one to the other.
            property real morph: root.controller.playing ? 1 : 0
            Behavior on morph {
                NumberAnimation { duration: Theme.transportMorphDuration; easing.type: Easing.InOutQuad }
            }
            // How lit the rim and glyph are, 0 to 1. Full while playing;
            // paused, the animations below own it.
            property real light: root.controller.playing ? 1 : 0
            property real keyWidth: pioneerForm ? Theme.transportKeyWidth : Theme.transportPadSize
            Behavior on keyWidth {
                NumberAnimation { duration: Theme.arrivalTransitionDuration; easing.type: Easing.InOutCubic }
            }
            Layout.preferredWidth: keyWidth
            Layout.preferredHeight: Theme.transportPadSize
            radius: pioneerForm ? Theme.transportKeyRadius : height / 2
            Behavior on radius {
                NumberAnimation { duration: Theme.arrivalTransitionDuration; easing.type: Easing.InOutCubic }
            }
            color: playMouseArea.pressed ? Qt.darker(Theme.transportBody, 1.3) : Theme.transportBody

            Connections {
                target: root.controller
                function onPlayingChanged() {
                    if (root.controller.playing) {
                        playButton.light = 1;
                    }
                }
            }

            // The Pioneer form's blink: on and off in equal halves, a
            // step and not a fade, the way the key on the deck does it.
            SequentialAnimation {
                running: playButton.visible && !root.controller.playing && playButton.pioneerForm
                loops: Animation.Infinite
                PropertyAction { target: playButton; property: "light"; value: 1 }
                PauseAnimation { duration: Theme.transportBlinkHalfPeriod }
                PropertyAction { target: playButton; property: "light"; value: 0 }
                PauseAnimation { duration: Theme.transportBlinkHalfPeriod }
            }
            // The Denon pad's breath: a sine swell that never quite goes
            // out and never reaches the playing light.
            SequentialAnimation {
                running: playButton.visible && !root.controller.playing && !playButton.pioneerForm
                loops: Animation.Infinite
                NumberAnimation { target: playButton; property: "light"; to: 0.7; duration: Theme.transportBreathHalfPeriod; easing.type: Easing.InOutSine }
                NumberAnimation { target: playButton; property: "light"; to: 0.1; duration: Theme.transportBreathHalfPeriod; easing.type: Easing.InOutSine }
            }

            Rectangle {
                id: playFace
                objectName: "playFace"
                anchors.fill: parent
                anchors.margins: Theme.transportFaceInset
                radius: Math.max(Theme.scaled(4), playButton.radius - Theme.transportFaceInset)
                color: Theme.transportFace
            }
            Rectangle {
                id: playRim
                objectName: "playRim"
                anchors.fill: playFace
                radius: playFace.radius
                color: "transparent"
                border.color: Theme.transportLit
                border.width: Theme.transportRimWidth
                opacity: playButton.light
            }

            // Hand-drawn rather than a font glyph: icon-font play/pause
            // characters carry their own (inconsistent, per-font) internal
            // padding, so centering them by anchoring the Text item never
            // lines the visible ink up with the key; only exact geometry
            // does. The triangle is two quadrilaterals that meet along
            // its middle, the bars are two more, and each corner of one
            // is moved to its counterpart in the other by `morph`, so
            // the glyph folds instead of swapping. Both shapes' bounding
            // boxes are centred on (cx, cy); the play shape keeps the
            // proportions this key has always drawn.
            Canvas {
                id: playIcon
                objectName: "playIcon"
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.fillStyle = Theme.mix(Theme.transportInkOff, Theme.transportLit, playButton.light).toString();
                    const h = height;
                    const cx = width / 2, cy = height / 2;
                    const top = cy - 0.21 * h, bottom = cy + 0.21 * h;
                    // Where the triangle sits. On the flat Pioneer key it
                    // is centred on its bounding box, which is what reads
                    // as centred inside a rectangle. Inside the round
                    // Denon pad that left the tip 0.18h from the centre
                    // and the two back corners 0.28h, so the triangle
                    // looked pushed off the ring; there it is centred on
                    // its circumcentre instead, all three corners on one
                    // circle concentric with the rim (an equilateral
                    // triangle of the same height). The key's own
                    // roundness blends the two, so the form morph
                    // carries the glyph with it.
                    const round = Math.max(0, Math.min(1, playButton.radius / (playButton.height / 2)));
                    const circR = 0.21 * h / (Math.sqrt(3) / 2);
                    const backX = cx + ((-0.18 * h) + ((-circR / 2) - (-0.18 * h)) * round);
                    const tipX = cx + (0.18 * h + (circR - 0.18 * h) * round);
                    // Where the triangle's edges cross the vertical through
                    // cx, which is where its two halves meet.
                    const midHalf = 0.21 * h * (tipX - cx) / (tipX - backX);
                    // Left half of the triangle to the left bar, right
                    // half to the right bar, corner for corner.
                    const play = [
                        [[backX, top], [backX, bottom], [cx, cy + midHalf], [cx, cy - midHalf]],
                        [[cx, cy - midHalf], [cx, cy + midHalf], [tipX, cy], [tipX, cy]]
                    ];
                    const pause = [
                        [[cx - 0.19 * h, top], [cx - 0.19 * h, bottom], [cx - 0.06 * h, bottom], [cx - 0.06 * h, top]],
                        [[cx + 0.06 * h, top], [cx + 0.06 * h, bottom], [cx + 0.19 * h, bottom], [cx + 0.19 * h, top]]
                    ];
                    const t = playButton.morph;
                    for (let q = 0; q < 2; ++q) {
                        ctx.beginPath();
                        for (let i = 0; i < 4; ++i) {
                            const x = play[q][i][0] + (pause[q][i][0] - play[q][i][0]) * t;
                            const y = play[q][i][1] + (pause[q][i][1] - play[q][i][1]) * t;
                            if (i === 0) {
                                ctx.moveTo(x, y);
                            } else {
                                ctx.lineTo(x, y);
                            }
                        }
                        ctx.closePath();
                        ctx.fill();
                    }
                }
                Connections {
                    target: playButton
                    function onMorphChanged() { playIcon.requestPaint(); }
                    function onLightChanged() { playIcon.requestPaint(); }
                    function onRadiusChanged() { playIcon.requestPaint(); }
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
            // The cover here is the same cover as the ring's on the detail
            // pane, and a click on it opens the same fullscreen view.
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler {
                objectName: "playerArtworkTap"
                onTapped: barFullscreen.open()
            }
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
    IconToolButton {
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
        text: "Close the player"
        iconName: "window-close"
        iconSize: Theme.iconSizeSmall * 0.5
        opacity: playerHover.hovered || hovered ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        onClicked: root.controller.stop()
    }

    TrackRingFullscreen {
        id: barFullscreen
        objectName: "playerBarFullscreen"
        playbackController: root.controller
        hostWindow: root.Window.window
    }
}
