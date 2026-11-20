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

        // The play key, after the deck the loaded track plays on: a round
        // pad for a track from either Rekordbox catalog (a CDJ's or XDJ's
        // play button), a wide rectangular key for one from Engine (a
        // Prime deck's). The face stays dark; a rim and the glyph light up in
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
            property real keyWidth: pioneerForm ? Theme.transportPadSize : Theme.transportKeyWidth
            Behavior on keyWidth {
                NumberAnimation { duration: Theme.arrivalTransitionDuration; easing.type: Easing.InOutCubic }
            }
            Layout.preferredWidth: keyWidth
            Layout.preferredHeight: Theme.transportPadSize
            radius: pioneerForm ? height / 2 : Theme.transportKeyRadius
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

            // The Pioneer pad's blink: on and off in equal halves, a
            // step and not a fade, the way the button on the deck does it.
            SequentialAnimation {
                running: playButton.visible && !root.controller.playing && playButton.pioneerForm
                loops: Animation.Infinite
                PropertyAction { target: playButton; property: "light"; value: 1 }
                PauseAnimation { duration: Theme.transportBlinkHalfPeriod }
                PropertyAction { target: playButton; property: "light"; value: 0 }
                PauseAnimation { duration: Theme.transportBlinkHalfPeriod }
            }
            // The Denon key's breath: a sine swell that never quite goes
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
                    // Where the triangle sits. On the flat Denon key it
                    // is centred on its bounding box, which is what reads
                    // as centred inside a rectangle. Inside the round
                    // Pioneer pad that left the tip 0.18h from the centre
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

        // The loaded track's face: its cover, title, time and waveform.
        // A new track's face blends in over the old one's (see faces).
        Item {
            id: faces
            objectName: "playerFaces"
            Layout.fillWidth: true
            Layout.preferredHeight: faces.current ? faces.current.implicitHeight : 0
            property Item current: null
            property Item leaving: null
        }
    }

    // AppSettingsController, for the time shown (played or left) and the
    // popup's zoom. Optional: without it both are the bar's own.
    property var settings: null

    // ---- The blend from one track to the next (#104) -----------------
    //
    // A face shows one track: cover, title and artist, time, waveform with
    // its grid and cues, all bound to the player. When the player loads
    // another track the face showing the old one is frozen (its bindings
    // replaced by the values they had), and a new face is made for the
    // new track, hidden until the player has its length (the waveform's
    // grid and cues are placed by it; a file that does not play gets a
    // moment, then is shown as it is). Then the new face blends in over
    // blendDuration while the old one blends out, and the old one is
    // destroyed. A further change meanwhile keeps whichever of the two is
    // mostly showing and starts again from it: blends never stack.
    //
    // Each face fades as one picture (layer while not opaque), for the
    // reason WaveformView gives for its own layer.
    property real blendDuration: Theme.playerBlendDuration
    // False stops the animation, for a test that moves `blend` itself.
    property bool blendAnimated: true
    // 0: only the old face shows; 1: only the new one.
    property real blend: 1
    property bool blendWaiting: false

    // A face on its way out: hidden at once, destroyed when Qt gets to it.
    function retire(face) {
        face.retired = true;
        face.destroy();
    }
    function makeFace() {
        return faceComponent.createObject(faces, {});
    }
    Component.onCompleted: faces.current = root.makeFace()

    // Before the player changes anything: the face showing the old track
    // keeps it.
    property bool frozenForChange: false
    function trackAboutToChange() {
        if (!faces.current || root.frozenForChange) {
            return;
        }
        blendAnimation.stop();
        blendFallback.stop();
        if (faces.leaving) {
            // Keep the face mostly showing, drop the other.
            if (root.blend < 0.5) {
                root.retire(faces.current);
                faces.current = null;
            } else {
                root.retire(faces.leaving);
                faces.current.freeze();
                faces.leaving = faces.current;
                faces.current = null;
            }
        } else {
            faces.current.freeze();
            faces.leaving = faces.current;
            faces.current = null;
        }
        root.frozenForChange = true;
    }
    function trackChanged() {
        // A controller that does not announce the change (a test's) is
        // frozen now, as late as it can be.
        root.trackAboutToChange();
        root.frozenForChange = false;
        if (faces.current) {
            root.retire(faces.current);
        }
        faces.current = root.makeFace();
        root.blend = 0;
        root.blendWaiting = true;
        if (root.controller.duration > 0 || !root.controller.hasTrack) {
            root.startBlend();
        } else {
            blendFallback.restart();
        }
    }
    function startBlend() {
        if (!root.blendWaiting) {
            return;
        }
        root.blendWaiting = false;
        blendFallback.stop();
        if (root.blendAnimated) {
            blendAnimation.restart();
        }
    }
    onBlendChanged: {
        if (root.blend >= 1 && !root.blendWaiting && faces.leaving) {
            root.retire(faces.leaving);
            faces.leaving = null;
        }
    }
    NumberAnimation {
        id: blendAnimation
        target: root
        property: "blend"
        from: 0
        to: 1
        duration: root.blendDuration
        easing.type: Easing.InOutQuad
    }
    Timer {
        id: blendFallback
        interval: 600
        onTriggered: root.startBlend()
    }
    Connections {
        target: root.controller
        ignoreUnknownSignals: true
        function onTrackAboutToChange() { root.trackAboutToChange(); }
        function onTrackChanged() { root.trackChanged(); }
        function onDurationChanged() {
            if (root.controller.duration > 0) {
                root.startBlend();
            }
        }
    }

    Component {
        id: faceComponent
        RowLayout {
            id: face
            objectName: "playerFace"
            width: faces.width
            spacing: 12
            readonly property bool isLeaving: faces.leaving === face
            property bool retired: false
            opacity: face.isLeaving ? 1 - root.blend : (faces.leaving ? root.blend : 1)
            visible: opacity > 0 && !face.retired
            enabled: !face.isLeaving
            z: face.isLeaving ? 1 : 0
            layer.enabled: face.opacity < 1

            // Bound to the player while this face shows the loaded track;
            // freeze() keeps what they were.
            property string title: root.controller.title || ""
            property string artist: root.controller.artist || ""
            property string artworkPath: root.controller.artworkPath || ""
            property string fallbackArtworkPath: root.controller.fallbackArtworkPath || ""
            property var waveform: root.controller.waveform
            property var cues: root.controller.cues
            property real durationMs: root.controller.duration
            property real positionMs: root.controller.position
            property string errorMessage: root.controller.errorMessage || ""
            property var beats: {
                root.controller.trackSerial;
                return root.controller.beatTicksAt(root.controller.duration, playerWaveform.width);
            }
            function freeze() {
                face.title = face.title;
                face.artist = face.artist;
                face.artworkPath = face.artworkPath;
                face.fallbackArtworkPath = face.fallbackArtworkPath;
                face.waveform = face.waveform;
                face.cues = face.cues;
                face.durationMs = face.durationMs;
                face.positionMs = face.positionMs;
                face.errorMessage = face.errorMessage;
                face.beats = face.beats;
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
                source: face.artworkPath
                fallbackSource: face.fallbackArtworkPath
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
                        objectName: "playerTitle"
                        text: face.title + (face.artist.length > 0 ? "  - " + face.artist : "")
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    // The waveform zoomed in, the way a player shows it (#104):
                    // the bar's own waveform keeps its click for seeking.
                    IconToolButton {
                        objectName: "openWaveformPopupButton"
                        implicitWidth: Theme.snap(Theme.iconSizeSmall * 0.75)
                        implicitHeight: Theme.snap(Theme.iconSizeSmall * 0.75)
                        padding: 0
                        flat: true
                        text: "Zoom into the waveform"
                        iconName: "waveform"
                        iconSize: Theme.iconSizeSmall * 0.5
                        ToolTip.visible: hovered
                        ToolTip.text: text
                        onClicked: zoomPopup.openLoaded()
                    }
                    TrackTimeLabel {
                        objectName: "playerTime"
                        positionMs: face.positionMs
                        durationMs: face.durationMs
                        settings: root.settings
                    }
                }

                Label {
                    visible: face.errorMessage.length > 0
                    text: face.errorMessage
                    color: Theme.danger
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                WaveformView {
                    id: playerWaveform
                    objectName: "playerWaveform"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    waveformData: face.waveform
                    cueData: face.cues
                    // The grid cut out of the bars (option G): a ruler
                    // lane was too heavy for the bar.
                    gridStyle: "sliced"
                    // Only the beats drawn at this width, read with the
                    // waveform when the track was loaded; laid out again for a
                    // new track, a new length or a new width.
                    beatsSelected: true
                    beatData: face.beats
                    trackDurationMs: face.durationMs
                    progress: trackDurationMs > 0 ? face.positionMs / trackDurationMs : 0
                    onSeekRequested: (ratio) => root.controller.seek(ratio * root.controller.duration)
                }
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

    WaveformZoomPopup {
        id: zoomPopup
        playbackController: root.controller
        settings: root.settings
    }

    TrackRingFullscreen {
        id: barFullscreen
        objectName: "playerBarFullscreen"
        playbackController: root.controller
        hostWindow: root.Window.window
    }
}
