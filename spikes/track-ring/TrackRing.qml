// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick

// SPIKE. A whole track as a ring around its cover art -- see ring.frag.
Item {
    id: root

    property var waveformData: []   // [{low, mid, high}] or [number]
    property var cueData: []        // [{positionMs, color}]
    property real trackDurationMs: 0
    property url artworkSource: ""
    property real progress: -1      // 0..1, below 0 for "not playing"
    property color fallbackColor: "#35c4b0"
    property int bars: 200
    readonly property real artRadius: 0.50

    // The low band under the playhead: what the art and its halo move to.
    readonly property real bass: {
        var n = root.waveformData ? root.waveformData.length : 0;
        if (root.progress < 0 || n === 0) return 0;
        var col = root.waveformData[Math.min(n - 1, Math.floor(root.progress * n))];
        return typeof col === "number" ? col : col.low;
    }

    implicitWidth: 360
    implicitHeight: 360

    // The waveform as an N x 1 strip of pixels, which is all the shader
    // needs of it. A Canvas is the spike's way of making one; the real
    // thing would hand over a QImage from C++.
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
        source: root.artworkSource
        visible: false
        asynchronous: false
        sourceSize: Qt.size(512, 512)
        fillMode: Image.PreserveAspectCrop
    }

    ShaderEffect {
        id: ring
        width: Math.min(root.width, root.height)
        height: width
        anchors.centerIn: parent
        fragmentShader: Qt.resolvedUrl("ring.frag.qsb")

        property real progress: root.progress
        property real bass: root.bass
        property real bars: root.bars
        property real columns: strip.width
        property real artRadius: root.artRadius
        property real hasArt: artImage.status === Image.Ready ? 1 : 0
        property color fallbackColor: root.fallbackColor
        property var wave: stripTexture
        property var art: artImage
    }

    // Cues sit on the ring's base line, each in its own colour.
    Repeater {
        model: root.trackDurationMs > 0 ? root.cueData : []
        Item {
            id: cueMark
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
                color: (cueMark.modelData.color && cueMark.modelData.color.charAt(0) === "#") ? cueMark.modelData.color : "#ffcc00"
                border.color: "#101418"
                border.width: 1
            }
        }
    }
}
