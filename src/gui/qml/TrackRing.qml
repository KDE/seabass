// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// A whole track as a ring around its cover art: twelve o'clock is the
// start, clockwise is time, a bar's length is that moment's energy, and
// the colour is the cover's own. shaders/track_ring.frag draws all of it
// from two textures, so a frame costs two uniforms.
//
// It needs a GPU scene graph. Where there is none -- the software
// backend, or a build without Qt's shader tools -- `available` is false
// and the caller shows the plain art instead; nothing is drawn here.
Item {
    id: root

    property var waveformData: []   // [{low, mid, high}] or [number]
    property var cueData: []        // [{positionMs, color}]
    property real trackDurationMs: 0
    property string artworkSource: ""
    property real progress: -1      // 0..1 played; below 0 for "no playhead"
    property bool playing: false
    property color fallbackColor: Theme.accent
    // The page behind the ring. What is dimmed fades toward this, so
    // the ring sits as well on a light system theme as on the dark one.
    property color backgroundColor: Theme.background
    property int bars: 200

    // The art disc's radius, in units of the ring's outer radius.
    readonly property real artRadius: 0.50

    readonly property bool available: GraphicsInfo.api !== GraphicsInfo.Software
        && GraphicsInfo.api !== GraphicsInfo.Unknown
        && ring.status !== ShaderEffect.Error

    // Clicked anywhere on the disc the ring fills.
    signal clicked()

    // The low band under the playhead: what the art and its halo move to.
    // Only while playing -- a paused track holds still.
    readonly property real bass: {
        var n = root.waveformData ? root.waveformData.length : 0;
        if (!root.playing || root.progress < 0 || n === 0) {
            return 0;
        }
        var col = root.waveformData[Math.min(n - 1, Math.floor(root.progress * n))];
        return typeof col === "number" ? col : col.low;
    }

    // rekordbox writes every cover twice, 80 px as aNN.jpg and 240 px as
    // aNN_m.jpg next to it, and the library names the small one. A disc
    // this size wants the large one; if it is not there the small one
    // will do.
    readonly property string largeArtworkSource: /\/Artwork\/.*\/a\d+\.jpg$/.test(root.artworkSource)
        ? root.artworkSource.replace(/\.jpg$/, "_m.jpg") : ""
    property bool largeArtworkMissing: false
    onArtworkSourceChanged: root.largeArtworkMissing = false

    implicitWidth: Theme.iconSizeLarge * 6
    implicitHeight: implicitWidth

    // The waveform as an N x 1 strip of pixels -- r, g, b are low, mid,
    // high -- which is all the shader needs of it.
    Canvas {
        id: strip
        width: Math.max(1, root.waveformData ? root.waveformData.length : 1)
        height: 1
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            var data = root.waveformData || [];
            for (var i = 0; i < data.length; ++i) {
                var col = data[i];
                var low = typeof col === "number" ? col : col.low;
                var mid = typeof col === "number" ? col : col.mid;
                var high = typeof col === "number" ? col : col.high;
                ctx.fillStyle = Qt.rgba(low, mid, high, 1);
                ctx.fillRect(i, 0, 1, 1);
            }
        }
        Connections {
            target: root
            function onWaveformDataChanged() { strip.requestPaint(); }
        }
    }
    ShaderEffectSource {
        id: stripTexture
        sourceItem: strip
        hideSource: true
        smooth: true
        visible: false
        width: strip.width
        height: 1
        textureSize: Qt.size(strip.width, 1)
    }

    Image {
        id: artImage
        objectName: "ringArtwork"
        source: root.largeArtworkSource.length > 0 && !root.largeArtworkMissing
            ? root.largeArtworkSource : root.artworkSource
        visible: false
        sourceSize: Qt.size(512, 512)
        onStatusChanged: {
            if (status === Image.Error && source.toString() === root.largeArtworkSource) {
                root.largeArtworkMissing = true;
            }
        }
    }

    ShaderEffect {
        id: ring
        objectName: "ringShader"
        width: Math.min(root.width, root.height)
        height: width
        anchors.centerIn: parent
        visible: root.available
        fragmentShader: "qrc:/seabass/shaders/track_ring.frag.qsb"

        property real progress: root.progress
        property real bass: root.bass
        property real bars: root.bars
        property real artRadius: root.artRadius
        property real hasArt: artImage.status === Image.Ready ? 1 : 0
        property color fallbackColor: root.fallbackColor
        property color backgroundColor: root.backgroundColor
        property var wave: stripTexture
        property var art: artImage

        // The bass arrives once per waveform column, a step every second
        // or so; eased, the art breathes instead of twitching.
        Behavior on bass { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    }

    // Cues sit on the ring's base line, each in its own colour.
    Repeater {
        model: root.available && root.trackDurationMs > 0 ? root.cueData : []
        Item {
            id: cueMark
            objectName: "ringCueMark"
            required property var modelData
            readonly property real angle: (modelData.positionMs / root.trackDurationMs) * 2 * Math.PI
            readonly property real orbit: ring.width / 2 * (root.artRadius + 0.035)
            x: root.width / 2 + Math.sin(angle) * orbit - width / 2
            y: root.height / 2 - Math.cos(angle) * orbit - height / 2
            width: Math.max(9, ring.width * 0.032)
            height: width
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: (cueMark.modelData.color && cueMark.modelData.color.charAt(0) === "#")
                    ? cueMark.modelData.color : "#ffcc00"
                border.color: Theme.background
                border.width: 1
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.available
        cursorShape: Qt.PointingHandCursor
        onClicked: function(mouse) {
            // The ring is round and this area is square: its corners are
            // not the ring.
            var dx = mouse.x - root.width / 2;
            var dy = mouse.y - root.height / 2;
            if (Math.sqrt(dx * dx + dy * dy) <= ring.width / 2) {
                root.clicked();
            }
        }
    }
}
