// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Window
import QtMultimedia
import SeabassGui

// The metronome (`seabass --metronome`): a robot that jumps to the beat
// of the music file dropped on it. A small show of the track analysis:
// the beats it finds are the only thing the robot goes by.

Window {
    id: root

    width: 520
    height: 860
    minimumWidth: 420
    minimumHeight: 860
    visible: true
    title: "Seabass Metronome"
    color: "#12141b"

    // Where the music is, in ms: the player reports its position only
    // now and then, so the time since its last report is added.
    property real reportedMs: 0
    property real reportedAt: 0
    // 0 on a beat, rising to 1 just before the next; -1 without one.
    property real phase: -1
    // The same moment, kept for the two strips at the bottom.
    property real nowMs: 0
    // How much of the track the upper strip shows, in ms.
    readonly property real stripMs: 8000
    // The length of a beat here, for the buttons that skip by beats.
    property real beatMs: 500

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60)
    }
    // The player said where it is.
    function heard(ms) {
        root.reportedMs = ms
        root.reportedAt = Date.now()
        if (!root.playing) {
            root.nowMs = ms
        }
    }
    function skipTo(ms) {
        if (track.beatCount < 2 || player.duration <= 0) {
            return
        }
        const to = Math.max(0, Math.min(ms, player.duration - 50))
        player.position = to
        root.reportedMs = to
        root.reportedAt = Date.now()
        root.nowMs = to
    }
    property int beat: -1
    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState
    // How high the robot is, 0 to 1: it leaves the floor on a beat and
    // lands on the next.
    readonly property real lift: phase < 0 ? 0 : Math.sin(Math.PI * phase)
    // 1 on the beat, gone a sixth of a beat later.
    readonly property real hit: phase < 0 ? 0 : Math.max(0, 1 - phase * 6)

    MetronomeTrack {
        id: track
        objectName: "metronomeTrack"
        onReady: {
            player.source = track.source
            root.heard(0)
            player.play()
        }
        // A file that is being analysed, or could not be: the one
        // before it is not there to be played any more.
        onSourceChanged: {
            if (track.source.toString() === "") {
                player.stop()
                player.source = ""
            }
        }
    }

    MediaPlayer {
        id: player
        audioOutput: AudioOutput {}
        loops: MediaPlayer.Infinite
        onPositionChanged: root.heard(position)
        // Paused and played again: the last report is as old as the pause.
        onPlaybackStateChanged: root.heard(position)
    }

    // Only while there is something to move to: a frame animation
    // keeps the window drawing.
    FrameAnimation {
        running: root.playing && track.beatCount >= 2
        onRunningChanged: if (!running) root.phase = -1
        onTriggered: {
            // No further from the last report than the player goes
            // between two, and never past the end.
            const since = Math.max(0, Math.min(Date.now() - root.reportedAt, 250))
            let now = Math.min(root.reportedMs + since, player.duration > 0 ? player.duration : root.reportedMs + since)
            // A report that lands a little behind what was shown does
            // not take the robot back a step; a skip or the start of
            // the next round is further than that.
            if (now < root.nowMs && root.nowMs - now < 120) {
                now = root.nowMs
            }
            root.nowMs = now
            const near = track.beatsBetween(now - 2000, now + 2000)
            if (near.length >= 4) {
                root.beatMs = near[2] - near[0]
            }
            root.phase = track.phaseAt(now)
            root.beat = track.beatAt(now)
        }
    }

    // A file named on the command line: analysed at once.
    property url firstFile
    Component.onCompleted: {
        if (firstFile.toString() !== "") {
            track.load(firstFile)
        }
    }

    DropArea {
        id: drop
        anchors.fill: parent
        onDropped: (event) => {
            if (event.hasUrls) {
                player.stop()
                track.load(event.urls[0])
            }
        }
    }

    // The floor, and the robot's shadow on it.
    Rectangle {
        id: floor
        anchors.horizontalCenter: parent.horizontalCenter
        y: 440
        width: 360
        height: 2
        color: "#2a2e3b"
    }
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: floor.y + 6
        width: 170 * (1 - 0.45 * root.lift)
        height: 14
        radius: 7
        color: "#000000"
        opacity: 0.45 * (1 - 0.5 * root.lift)
    }

    Item {
        id: robot
        objectName: "metronomeRobot"

        readonly property color shell: "#d9dee8"
        readonly property color shade: "#9aa3b5"
        readonly property color glow: drop.containsDrag ? "#ffd166" : "#4fd1c5"
        // Thinking while the file is analysed: the head tilts to and fro.
        property real think: 0

        width: 200
        height: 300
        anchors.horizontalCenter: parent.horizontalCenter
        y: floor.y - height - 120 * root.lift
        // Squashed for a moment as it lands on the beat.
        transform: Scale {
            origin.x: robot.width / 2
            origin.y: robot.height
            yScale: 1 - 0.09 * root.hit
            xScale: 1 + 0.06 * root.hit
        }

        TapHandler {
            onTapped: root.playing ? player.pause() : player.play()
        }

        SequentialAnimation on think {
            running: track.busy
            loops: Animation.Infinite
            NumberAnimation { to: 1; duration: 500; easing.type: Easing.InOutSine }
            NumberAnimation { to: -1; duration: 1000; easing.type: Easing.InOutSine }
            NumberAnimation { to: 0; duration: 500; easing.type: Easing.InOutSine }
            onRunningChanged: if (!running) robot.think = 0
        }

        // Legs and feet: tucked up in the air.
        Repeater {
            model: 2
            Item {
                required property int index
                x: index === 0 ? 62 : 118
                y: 222
                Rectangle {
                    width: 20
                    height: 62 - 22 * root.lift
                    radius: 6
                    color: robot.shade
                }
                Rectangle {
                    x: -10
                    y: 56 - 22 * root.lift
                    width: 40
                    height: 22
                    radius: 8
                    color: robot.shell
                }
            }
        }

        // Arms: up as it rises.
        Repeater {
            model: 2
            Rectangle {
                required property int index
                x: index === 0 ? 14 : 166
                y: 132
                width: 20
                height: 78
                radius: 10
                color: robot.shade
                transformOrigin: Item.Top
                rotation: (index === 0 ? 1 : -1) * (12 + 120 * root.lift)
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: parent.height - 14
                    width: 28
                    height: 28
                    radius: 14
                    color: robot.shell
                }
            }
        }

        // Body, with a row of lights that counts the four beats.
        Rectangle {
            x: 40
            y: 120
            width: 120
            height: 112
            radius: 22
            color: robot.shell
            Rectangle {
                anchors.centerIn: parent
                width: 88
                height: 40
                radius: 12
                color: "#1b1f2a"
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Repeater {
                        model: 4
                        Rectangle {
                            required property int index
                            readonly property bool lit: root.phase >= 0 && root.beat % 4 === index
                            width: 12
                            height: 12
                            radius: 6
                            color: lit ? robot.glow : "#343a4a"
                            scale: lit ? 1 + 0.4 * root.hit : 1
                        }
                    }
                }
            }
        }

        // Head, antenna, eyes and mouth.
        Item {
            x: 30
            y: 8
            width: 140
            height: 108
            rotation: 7 * robot.think
            transformOrigin: Item.Bottom

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: -4
                width: 4
                height: 24
                color: robot.shade
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: -22
                width: 18 + 8 * root.hit
                height: width
                radius: width / 2
                color: root.hit > 0 || track.busy || drop.containsDrag ? robot.glow : "#566078"
            }
            Rectangle {
                y: 16
                width: parent.width
                height: 92
                radius: 26
                color: robot.shell
                Rectangle {
                    anchors.centerIn: parent
                    width: 112
                    height: 62
                    radius: 18
                    color: "#1b1f2a"
                    Repeater {
                        model: 2
                        Rectangle {
                            required property int index
                            x: index === 0 ? 22 : 68
                            y: track.busy ? 18 : 14 - 3 * root.hit
                            width: 22
                            height: track.busy ? 6 : 22 + 6 * root.hit
                            radius: track.busy ? 3 : 11
                            color: robot.glow
                        }
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: 46
                        width: 26 + 18 * root.lift
                        height: 5
                        radius: 2.5
                        color: robot.glow
                        opacity: 0.8
                    }
                }
            }
        }
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: floor.y + 46
        width: parent.width - 48
        spacing: 8

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideMiddle
            objectName: "metronomeTitle"
            color: "#e8ebf2"
            font.pixelSize: 20
            font.weight: Font.DemiBold
            text: track.busy ? "Listening..." : track.title !== "" ? track.title : "Drop a music file on me"
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            objectName: "metronomeSummary"
            wrapMode: Text.WordWrap
            color: track.error !== "" ? "#ff8a80" : "#8b93a7"
            font.pixelSize: 14
            text: track.busy ? track.title : track.error !== "" ? track.error : track.summary !== "" ? track.summary : "I jump on every beat Seabass finds"
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            color: "#8b93a7"
            objectName: "metronomeTiming"
            font.pixelSize: 13
            visible: text !== ""
            text: track.busy ? "" : track.timing
        }
    }

    // The lower part: the beat grid around this moment, the whole track
    // to skip through, and the controls.
    Column {
        id: lower
        objectName: "metronomeLower"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        width: parent.width - 48
        spacing: 12
        visible: track.beatCount >= 2
        onVisibleChanged: whole.requestPaint()

        // Eight seconds around now: the audio, every beat as a line
        // (each fourth one taller), now in the middle. Drag to move.
        Rectangle {
            width: parent.width
            height: 110
            radius: 10
            color: "#1b1f2a"
            clip: true

            Canvas {
                id: strip
                anchors.fill: parent
                readonly property real now: root.nowMs
                onNowChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    const from = root.nowMs - root.stripMs / 2
                    const to = from + root.stripMs
                    const columns = Math.floor(width / 3)
                    const levels = track.levels(from, to, columns)
                    const middle = height / 2
                    ctx.fillStyle = "#566078"
                    for (let i = 0; i < levels.length; ++i) {
                        const h = Math.max(1, levels[i] * (height - 34))
                        ctx.fillRect(i * 3, middle - h / 2, 2, h)
                    }
                    const beats = track.beatsBetween(from, to)
                    for (let i = 0; i < beats.length; i += 2) {
                        const x = Math.round((beats[i] - from) / root.stripMs * width)
                        const first = beats[i + 1] % 4 === 0
                        ctx.fillStyle = first ? "#ffd166" : "#4fd1c5"
                        ctx.fillRect(x - 1, first ? 6 : 16, 2, height - (first ? 12 : 32))
                    }
                    ctx.fillStyle = "#ffffff"
                    ctx.fillRect(width / 2 - 1, 0, 2, height)
                }
            }
            DragHandler {
                target: null
                property real startedAt: 0
                onActiveChanged: if (active) startedAt = root.nowMs
                onTranslationChanged: if (active) root.skipTo(startedAt - translation.x / strip.width * root.stripMs)
            }
        }

        // The whole track: click or drag to skip to a part of it.
        Rectangle {
            width: parent.width
            height: 64
            radius: 10
            color: "#1b1f2a"
            clip: true

            Canvas {
                id: whole
                anchors.fill: parent
                readonly property url of: track.source
                onOfChanged: requestPaint()
                onWidthChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    const columns = Math.floor(width / 2)
                    const levels = track.levels(0, track.lengthMs, columns)
                    ctx.fillStyle = "#8b93a7"
                    for (let i = 0; i < levels.length; ++i) {
                        const h = Math.max(1, levels[i] * (height - 12))
                        ctx.fillRect(i * 2, (height - h) / 2, 1, h)
                    }
                }
            }
            // What has been played, and where the upper strip is.
            Rectangle {
                width: player.duration > 0 ? parent.width * root.nowMs / player.duration : 0
                height: parent.height
                color: "#4fd1c5"
                opacity: 0.22
            }
            Rectangle {
                x: player.duration > 0 ? parent.width * root.nowMs / player.duration - 1 : 0
                width: 2
                height: parent.height
                color: "#ffffff"
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                function skip(mouse) { root.skipTo(mouse.x / width * player.duration) }
                onPressed: (mouse) => skip(mouse)
                onPositionChanged: (mouse) => { if (pressed) skip(mouse) }
            }
        }

        Item {
            width: parent.width
            height: 48

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                color: "#e8ebf2"
                font.pixelSize: 15
                font.features: { "tnum": 1 }
                text: root.clock(root.nowMs)
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                color: "#8b93a7"
                font.pixelSize: 15
                font.features: { "tnum": 1 }
                text: root.clock(player.duration)
            }
            Row {
                anchors.centerIn: parent
                spacing: 14

                // Back to the start, play or pause, on by a beat less or more.
                Repeater {
                    model: [
                        { label: "Start", width: 70, act: () => root.skipTo(0) },
                        { label: "Back 16", width: 84, act: () => root.skipTo(root.nowMs - 16 * root.beatMs) },
                        { label: "", width: 48, act: () => root.playing ? player.pause() : player.play() },
                        { label: "On 16", width: 84, act: () => root.skipTo(root.nowMs + 16 * root.beatMs) }
                    ]
                    Rectangle {
                        required property var modelData
                        objectName: modelData.label === "" ? "metronomePlay" : "metronome" + modelData.label.replace(" ", "")
                        width: modelData.width
                        height: 48
                        radius: 24
                        color: modelData.label === "" ? "#4fd1c5" : press.pressed ? "#343a4a" : "#232836"
                        Text {
                            anchors.centerIn: parent
                            color: "#e8ebf2"
                            font.pixelSize: 14
                            text: parent.modelData.label
                        }
                        // Play: a triangle. Pause: two bars.
                        Canvas {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            visible: parent.modelData.label === ""
                            readonly property bool playing: root.playing
                            onPlayingChanged: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d")
                                ctx.clearRect(0, 0, width, height)
                                ctx.fillStyle = "#12141b"
                                if (root.playing) {
                                    ctx.fillRect(2, 1, 5, 16)
                                    ctx.fillRect(11, 1, 5, 16)
                                } else {
                                    ctx.beginPath()
                                    ctx.moveTo(4, 1)
                                    ctx.lineTo(17, 9)
                                    ctx.lineTo(4, 17)
                                    ctx.closePath()
                                    ctx.fill()
                                }
                            }
                        }
                        MouseArea {
                            id: press
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: parent.modelData.act()
                        }
                    }
                }
            }
        }
    }
}
