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

    // ---- What the ring moves to ----
    //
    // WHEN a beat hits comes from the track's beat grid, as rekordbox or
    // Engine analysed it: the same grid the DJ's decks keep time by, known
    // ahead of time, so the display is on the beat rather than noticing
    // it afterwards. HOW HARD comes from the audio as it plays
    // (PlaybackController's live levels): a breakdown with no kick has a
    // grid all the same, and should not pulse as if it had one.
    //
    // A track with no grid falls back to the player's own beat detector
    // (beatCount), and a player with no live levels to the stored
    // waveform's column under the playhead, which changes about once a
    // second -- enough to breathe to, not to dance to.
    property var beatTimesMs: []
    property var beatNumbers: []
    // The player's position. It arrives in steps, several frames apart;
    // smoothPositionMs below is what the beat is reckoned from.
    property real positionMs: -1
    // The eye is a little behind the ear: a flash ON the beat looks late.
    property real visualLeadMs: 25
    property bool liveLevels: false
    property real liveLow: 0
    property real liveMid: 0
    property real liveHigh: 0
    // Goes up by one on every beat the player's detector hears. Used
    // only where there is no grid.
    property int beatCount: 0

    readonly property bool hasBeatGrid: !!root.beatTimesMs && root.beatTimesMs.length > 1

    readonly property var columnUnderPlayhead: {
        var n = root.waveformData ? root.waveformData.length : 0;
        if (root.progress < 0 || n === 0) {
            return null;
        }
        var col = root.waveformData[Math.min(n - 1, Math.floor(root.progress * n))];
        return typeof col === "number" ? {low: col, mid: col, high: col} : col;
    }
    // How loud each band is, from the best source there is. Only while
    // playing -- a paused track holds still.
    readonly property real loudLow: !root.playing ? 0 : root.liveLevels ? root.liveLow
        : (root.columnUnderPlayhead ? root.columnUnderPlayhead.low : 0)
    readonly property real mid: !root.playing ? 0 : root.liveLevels ? root.liveMid
        : (root.columnUnderPlayhead ? root.columnUnderPlayhead.mid : 0)
    readonly property real high: !root.playing ? 0 : root.liveLevels ? root.liveHigh
        : (root.columnUnderPlayhead ? root.columnUnderPlayhead.high : 0)

    // The position carried forward frame by frame between the player's
    // reports, and nudged toward each report rather than snapped to it:
    // the reports jitter, and a beat reckoned from them jitters with them.
    property real smoothPositionMs: 0
    onPositionMsChanged: {
        var error = root.positionMs - root.smoothPositionMs;
        if (!root.playing || Math.abs(error) > 120) {
            root.smoothPositionMs = root.positionMs;   // a seek, or not moving
        } else {
            root.smoothPositionMs += error * 0.15;
        }
        root.findBeat();
    }

    // Where the (smoothed, led) position is in the grid.
    property real msSinceBeat: 1e9
    property real beatLengthMs: 500
    property int beatInBar: 0
    function findBeat() {
        var times = root.beatTimesMs;
        if (!root.hasBeatGrid) {
            root.msSinceBeat = 1e9;
            return;
        }
        var at = root.smoothPositionMs + root.visualLeadMs;
        var lo = 0;
        var hi = times.length - 1;
        if (at < times[0]) {
            root.msSinceBeat = 1e9;   // before the first beat
            return;
        }
        while (lo < hi) {
            var middle = (lo + hi + 1) >> 1;
            if (times[middle] <= at) {
                lo = middle;
            } else {
                hi = middle - 1;
            }
        }
        var length = lo + 1 < times.length ? times[lo + 1] - times[lo] : times[lo] - times[lo - 1];
        root.beatLengthMs = length;
        // Past the last beat of the grid by more than a beat: it has ended.
        root.msSinceBeat = at - times[lo] > length * 1.5 ? 1e9 : at - times[lo];
        root.beatInBar = root.beatNumbers && lo < root.beatNumbers.length ? root.beatNumbers[lo] : 0;
    }
    onBeatTimesMsChanged: root.findBeat()

    // Asked for, the cover spins like the record it is the sleeve of, at
    // an LP's 33 1/3 rpm. Off by default: a cover is there to be
    // recognised, and that is easier upright. Reckoned from the position,
    // not from a clock: it turns while the track plays, holds when it is
    // paused, and a seek turns it as far as the record would have gone.
    // It starts from upright where it was switched on, and goes back to
    // upright when switched off. In turns, kept within one so that an hour
    // in the shader's float still has its precision.
    property bool spinning: false
    property real spinOriginMs: 0
    onSpinningChanged: root.spinOriginMs = root.smoothPositionMs
    readonly property real revolutionsPerMinute: 100 / 3
    readonly property real artTurns: !root.spinning ? 0
        : ((((root.smoothPositionMs - root.spinOriginMs) / 60000 * root.revolutionsPerMinute) % 1) + 1) % 1

    // How much the recent music has had in the low band: up fast, down
    // over half a second. It scales the grid's pulse, so that the ring
    // pulses hard in a drop and barely at all in a breakdown.
    property real energy: 0

    // The bass the shader swells and jumps to. With a grid: a pulse
    // struck on every beat, dying away over a third of it, as strong as
    // the music is. Without: the low band itself.
    readonly property real bass: !root.playing ? 0
        : root.hasBeatGrid ? Math.exp(-root.msSinceBeat / (root.beatLengthMs * 0.30)) * root.energy
        : root.loudLow

    // The ripple a beat sends out. With a grid it is a pure function of
    // the position -- nothing is triggered, so nothing can arrive late --
    // and it crosses the ring in most of a beat, whatever the tempo. It
    // used to take a fixed 0.67 s: at 135 BPM the next beat came after
    // 0.44 and cut it off halfway, every time. The downbeat's is stronger.
    property real rippleStart: -100
    readonly property real rippleAge: !root.playing ? 100
        : root.hasBeatGrid ? root.msSinceBeat / 1000 : root.time - root.rippleStart
    readonly property real rippleSpan: root.hasBeatGrid ? root.beatLengthMs * 0.85 / 1000 : 0.35
    readonly property real rippleStrength: !root.hasBeatGrid ? 0.8
        : (root.beatInBar === 1 ? 1.0 : 0.55) * Math.min(1, 0.25 + root.energy)
    onBeatCountChanged: {
        if (root.playing && !root.hasBeatGrid) {
            root.rippleStart = root.time;
        }
    }

    // The shader's clock, in seconds, and everything that runs by it. It
    // runs only while there is something to animate; `animated` off
    // leaves advance() to whoever calls it, which is how a test steps
    // through time.
    property bool animated: true
    property real time: 0
    function advance(seconds) {
        root.time += seconds;
        if (root.playing) {
            root.smoothPositionMs += seconds * 1000;
        }
        root.findBeat();
        var rate = root.loudLow > root.energy ? seconds / 0.05 : seconds / 0.5;
        root.energy += (root.loudLow - root.energy) * Math.min(1, rate);
    }
    FrameAnimation {
        running: root.animated && root.playing && root.available && root.visible
        onTriggered: root.advance(frameTime)
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
        property real mid: root.mid
        property real high: root.high
        property real time: root.time
        property real rippleAge: root.rippleAge
        property real rippleSpan: root.rippleSpan
        property real rippleStrength: root.rippleStrength
        property real artTurns: root.artTurns
        property real bars: root.bars
        property real artRadius: root.artRadius
        property real hasArt: artImage.status === Image.Ready ? 1 : 0
        property color fallbackColor: root.fallbackColor
        property color backgroundColor: root.backgroundColor
        property var wave: stripTexture
        property var art: artImage

        // From the waveform alone the bass arrives once per column, a step
        // every second or so; eased, the art breathes instead of
        // twitching. A grid's pulse and the live levels are shaped
        // already, and easing a kick again would be to miss it.
        Behavior on bass {
            enabled: !root.liveLevels && !root.hasBeatGrid
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
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
