// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The loaded track's waveform zoomed in, the way a player shows it while
// it plays (#104): from the whole track down to a few bars across, with
// the ruler lane and the beat lines, the cues, and a fixed "this
// position" line at the golden-ratio point left of the middle, under
// which the waveform scrolls as the track plays.
//
// Opened from the track details pane (openFor(): the track is loaded into
// the player, paused if nothing was playing and playing if something
// was) and from the player bar (openLoaded()). Closing it changes nothing
// about playback.
//
// Drawn as tiles: WaveformViews a fixed width apart, each showing its own
// stretch of the track, laid side by side on a strip that is only moved
// while the track plays. A tile repaints when the zoom or the data
// changes, or when it is handed a new stretch on scrolling into view; a
// frame of playback moves the strip and paints nothing.
SeabassDialog {
    id: root
    objectName: "waveformZoomPopup"

    required property var playbackController
    readonly property var controller: root.playbackController

    showBadge: false
    // Escape closes it only while it has the focus.
    focus: true
    title: root.controller && root.controller.title ? root.controller.title : ""
    headline: root.controller && root.controller.artist ? root.controller.artist : ""
    parent: Overlay.overlay
    width: parent ? Math.round(parent.width * 0.92) : 1000

    // ---- Opening ---------------------------------------------------

    // `track`: {format, libraryPath, sourceId, filePath, title, artist,
    // artworkPath, cues, fallbackArtworkPath}, a row as a page has it.
    // The track goes into the player the way a play from Browse puts it
    // there (the page's list stays the queue), playing if the player was
    // playing and paused at its start if not. The track already loaded
    // is left as it is, playing or paused, where it is.
    function openFor(track) {
        const c = root.controller;
        const loaded = c.hasTrack === true && c.currentSourceId === track.sourceId
            && c.currentFormat === track.format && c.currentLibraryPath === track.libraryPath;
        if (!loaded) {
            c.load(track.format, track.libraryPath, track.sourceId, track.filePath, track.title, track.artist,
                   track.artworkPath || "", track.cues || [], track.fallbackArtworkPath || "", c.playing === true);
        }
        root.openLoaded();
    }

    // The track the player has, as it is.
    function openLoaded() {
        root.attached = true;
        root.spanMs = 0;
        root.resync(root.controller.position || 0);
        root.open();
    }

    // ---- Zoom ------------------------------------------------------

    readonly property real durationMs: root.controller && root.controller.duration > 0 ? root.controller.duration : 0
    // A bar of this track: four beats of its grid at their median
    // spacing, or of 120 BPM where it has no grid.
    readonly property real barMs: {
        const times = root.controller ? root.controller.beatTimesMs : undefined;
        if (!times || times.length < 2) {
            return 2000;
        }
        const gaps = [];
        for (let i = 1; i < times.length; ++i) {
            gaps.push(times[i] - times[i - 1]);
        }
        gaps.sort((a, b) => a - b);
        const beat = gaps[Math.floor(gaps.length / 2)];
        return beat > 0 ? 4 * beat : 2000;
    }
    // The closest the view goes: this many bars across (my reading of "a
    // few bars"); the furthest is the whole track.
    readonly property int minimumBars: 4
    // Where it opens.
    readonly property int openingBars: 16
    readonly property real minimumSpanMs: root.minimumBars * root.barMs
    readonly property real maximumSpanMs: Math.max(root.minimumSpanMs, root.durationMs)
    // How much of the track the view shows across its width. 0 until it
    // is zoomed: the opening span.
    property real spanMs: 0
    readonly property real shownSpanMs: root.clampSpan(root.spanMs > 0 ? root.spanMs : root.openingBars * root.barMs)
    readonly property real pxPerMs: area.width > 0 ? area.width / root.shownSpanMs : 0
    // One press of a zoom key or button, and one notch of the wheel.
    readonly property real zoomStep: 1.5

    function clampSpan(span) {
        return Math.max(root.minimumSpanMs, Math.min(root.maximumSpanMs, span));
    }

    // Zooms by `factor` (above 1 is closer) keeping the time under
    // `anchorX` where it is. While the view follows the playhead it keeps
    // the playhead under the line instead: the line is the anchor then.
    function zoomBy(factor, anchorX) {
        const before = root.pxPerMs;
        const span = root.clampSpan(root.shownSpanMs / factor);
        if (span === root.shownSpanMs || before <= 0) {
            return;
        }
        if (root.attached) {
            root.spanMs = span;
            return;
        }
        const at = root.manualMs + (anchorX - root.lineX) / before;
        root.spanMs = span;
        root.manualMs = root.clampTime(at - (anchorX - root.lineX) / root.pxPerMs);
    }
    function zoomIn() {
        root.zoomBy(root.zoomStep, root.lineX);
    }
    function zoomOut() {
        root.zoomBy(1 / root.zoomStep, root.lineX);
    }

    // ---- Position --------------------------------------------------

    // The golden-ratio point left of the middle: 1 - 1/phi of the width.
    readonly property real lineRatio: 1 - 2 / (1 + Math.sqrt(5))
    readonly property real lineX: Math.round(area.width * root.lineRatio)
    // Whether the view follows the playhead. Scrolling by hand lets go;
    // play, a seek and the follow button take hold again.
    property bool attached: true
    // The time under the line while it does not follow.
    property real manualMs: 0
    // The playhead carried forward frame by frame between the player's
    // reports, and nudged toward each report rather than snapped to it,
    // as the ring's clock is (TrackRing.smoothPositionMs): the reports
    // come a few times a second, and a waveform moved only then would
    // move in steps.
    property real smoothMs: 0
    readonly property bool playing: root.controller ? root.controller.playing === true : false
    // The time under the line.
    readonly property real centerMs: !root.attached ? root.manualMs
        : (root.playing ? root.smoothMs : (root.controller ? root.controller.position || 0 : 0))

    function clampTime(ms) {
        return Math.max(0, Math.min(root.durationMs, ms));
    }
    function resync(ms) {
        root.smoothMs = ms;
    }
    function detach() {
        if (root.attached) {
            root.manualMs = root.centerMs;
            root.attached = false;
        }
    }
    function follow() {
        root.resync(root.controller.position || 0);
        root.attached = true;
    }
    function scrollBy(px) {
        if (root.pxPerMs <= 0) {
            return;
        }
        root.detach();
        root.manualMs = root.clampTime(root.manualMs + px / root.pxPerMs);
    }
    // A click on the waveform: that moment goes to the line.
    function seekToX(x) {
        if (root.pxPerMs <= 0 || root.durationMs <= 0) {
            return;
        }
        const ms = Math.round(root.clampTime(root.centerMs + (x - root.lineX) / root.pxPerMs));
        root.controller.seek(ms);
        root.resync(ms);
        root.attached = true;
    }

    FrameAnimation {
        objectName: "waveformClock"
        running: root.opened && root.playing
        // smoothFrameTime, for the reason the ring gives (TrackRing.qml).
        onTriggered: root.smoothMs = Math.min(root.durationMs, root.smoothMs + smoothFrameTime * 1000)
    }
    Connections {
        // Only while open: closed, nothing here is shown, and openLoaded()
        // takes the position as it is then.
        target: root.opened ? root.controller : null
        ignoreUnknownSignals: true
        function onPositionChanged() {
            const reported = root.controller.position;
            const error = reported - root.smoothMs;
            if (!root.playing || Math.abs(error) > 200) {
                root.resync(reported);
            } else {
                root.smoothMs += error * 0.15;
            }
        }
        function onPlayingChanged() {
            root.resync(root.controller.position || 0);
            if (root.playing) {
                root.attached = true;
            }
        }
        function onSeeked(positionMs) {
            root.resync(positionMs);
            root.attached = true;
        }
        function onTrackChanged() {
            root.spanMs = 0;
            root.resync(root.controller.position || 0);
            root.attached = true;
        }
    }

    // ---- What is drawn ---------------------------------------------

    // The finest waveform the library has for the loaded track, read when
    // the popup opens; the overview where there is nothing finer.
    readonly property var detail: {
        if (!root.opened || !root.controller || typeof root.controller.detailWaveform !== "function") {
            return ({});
        }
        root.controller.trackSerial;
        // Read again once a write that held it off is over.
        root.controller.libraryBusy;
        return root.controller.detailWaveform();
    }
    readonly property bool hasDetail: root.detail.columns !== undefined && root.detail.columns !== null
        && root.detail.columns.byteLength >= 3
    // The beats drawn at this zoom, for the whole track: the same rule as
    // every other waveform, at the width the whole track would have.
    readonly property var ticks: {
        if (!root.opened || !root.controller || root.durationMs <= 0 || root.pxPerMs <= 0
                || typeof root.controller.beatTicksAt !== "function") {
            return [];
        }
        root.controller.trackSerial;
        return root.controller.beatTicksAt(root.durationMs, root.durationMs * root.pxPerMs);
    }
    // The marks a tile from `fromMs` to `toMs` draws: its own, and those a
    // little before it whose numbers reach into it.
    function ticksBetween(fromMs, toMs) {
        const before = Theme.scaled(40) / Math.max(root.pxPerMs, 1e-9);
        const all = root.ticks;
        const out = [];
        for (let i = 0; i < all.length; ++i) {
            const t = all[i].timeMs;
            if (t >= fromMs - before && t <= toMs) {
                out.push(all[i]);
            }
        }
        return out;
    }

    readonly property real tileWidth: 512
    readonly property int tileCount: Math.ceil(area.width / root.tileWidth) + 1
    // The strip's left edge is 0:00; it is moved so that centerMs is
    // under the line, to whole device pixels.
    readonly property real stripX: Theme.snap(root.lineX - root.centerMs * root.pxPerMs)
    readonly property int firstTile: root.tileWidth > 0 ? Math.floor(-root.stripX / root.tileWidth) : 0

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.tightSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.tightSpacing

            IconToolButton {
                objectName: "zoomPlayButton"
                focusPolicy: Qt.NoFocus
                text: root.playing ? "Pause" : "Play"
                iconName: root.playing ? "media-playback-pause" : "media-playback-start"
                onClicked: root.controller.togglePlay()
            }
            IconToolButton {
                objectName: "zoomOutButton"
                focusPolicy: Qt.NoFocus
                text: "Zoom out"
                iconName: "zoom-out"
                enabled: root.shownSpanMs < root.maximumSpanMs
                onClicked: root.zoomOut()
            }
            IconToolButton {
                objectName: "zoomInButton"
                focusPolicy: Qt.NoFocus
                text: "Zoom in"
                iconName: "zoom-in"
                enabled: root.shownSpanMs > root.minimumSpanMs
                onClicked: root.zoomIn()
            }
            IconToolButton {
                objectName: "followButton"
                focusPolicy: Qt.NoFocus
                visible: !root.attached
                text: "Follow the playhead"
                iconName: "go-jump"
                onClicked: root.follow()
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                objectName: "zoomTime"
                text: Theme.trackTime(root.controller ? root.controller.position || 0 : 0) + " / "
                    + Theme.trackTime(root.durationMs)
                color: Theme.textMuted
            }
            IconToolButton {
                objectName: "zoomCloseButton"
                focusPolicy: Qt.NoFocus
                text: "Close"
                iconName: "window-close"
                onClicked: root.close()
            }
        }

        Item {
            id: area
            objectName: "zoomArea"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.scaled(200)
            clip: true

            Item {
                id: strip
                objectName: "waveformStrip"
                x: root.stripX
                width: 1
                height: area.height

                Repeater {
                    model: root.opened && root.pxPerMs > 0 ? root.tileCount : 0
                    delegate: WaveformView {
                        id: tileView
                        required property int index
                        objectName: "zoomTile"
                        // Slot `index` shows the one tile of the visible run
                        // that falls to it: a slot keeps its tile while the
                        // strip moves, and only the slot whose tile scrolled
                        // out takes the one scrolling in.
                        readonly property int tile: root.firstTile
                            + (((tileView.index - root.firstTile) % root.tileCount) + root.tileCount) % root.tileCount
                        x: tileView.tile * root.tileWidth
                        width: root.tileWidth
                        height: strip.height
                        enabled: false
                        viewStartMs: tileView.tile * root.tileWidth / root.pxPerMs
                        viewSpanMs: root.tileWidth / root.pxPerMs
                        trackDurationMs: root.durationMs
                        columnBytes: root.hasDetail ? root.detail.columns : null
                        columnMs: root.hasDetail ? root.detail.msPerColumn || 0 : 0
                        columnStartMs: root.hasDetail ? root.detail.startMs || 0 : 0
                        waveformData: root.hasDetail || !root.controller ? [] : root.controller.waveform
                        beatsSelected: true
                        beatData: root.ticksBetween(tileView.viewStartMs, tileView.viewStartMs + tileView.viewSpanMs)
                        showRuler: root.ticks.length > 0
                        cueData: root.controller ? root.controller.cues : []
                        progress: -1
                    }
                }
            }

            // "This position": fixed, the waveform moves under it.
            Rectangle {
                objectName: "positionLine"
                x: root.lineX - width / 2
                width: Theme.snap(2)
                height: area.height
                color: Theme.accent
            }
            Rectangle {
                x: root.lineX - width / 2
                width: Theme.scaled(10)
                height: width
                rotation: 45
                transformOrigin: Item.Center
                anchors.verticalCenter: parent.top
                color: Theme.accent
            }

            // Click to seek, drag and wheel to scroll, ctrl+wheel to zoom.
            MouseArea {
                id: input
                objectName: "zoomInput"
                anchors.fill: parent
                property real pressX: -1
                property real pressMs: 0
                property bool dragging: false
                cursorShape: dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                onPressed: (mouse) => {
                    input.pressX = mouse.x;
                    input.pressMs = root.centerMs;
                    input.dragging = false;
                }
                onPositionChanged: (mouse) => {
                    if (input.pressX < 0) {
                        return;
                    }
                    if (!input.dragging && Math.abs(mouse.x - input.pressX) >= Qt.styleHints.startDragDistance) {
                        input.dragging = true;
                        root.detach();
                    }
                    if (input.dragging && root.pxPerMs > 0) {
                        root.manualMs = root.clampTime(input.pressMs - (mouse.x - input.pressX) / root.pxPerMs);
                    }
                }
                onReleased: (mouse) => {
                    if (!input.dragging && input.pressX >= 0) {
                        root.seekToX(mouse.x);
                    }
                    input.pressX = -1;
                    input.dragging = false;
                }
                onCanceled: {
                    input.pressX = -1;
                    input.dragging = false;
                }
                onWheel: (wheel) => {
                    const pixels = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0;
                    if (wheel.modifiers & Qt.ControlModifier) {
                        const steps = pixels ? wheel.pixelDelta.y / 50 : wheel.angleDelta.y / 120;
                        root.zoomBy(Math.pow(root.zoomStep, steps), wheel.x);
                    } else if (pixels) {
                        // A touchpad: the waveform follows the fingers.
                        const dx = Math.abs(wheel.pixelDelta.x) >= Math.abs(wheel.pixelDelta.y)
                            ? wheel.pixelDelta.x : wheel.pixelDelta.y;
                        root.scrollBy(-dx);
                    } else {
                        // A wheel, plain or with shift (which turns it
                        // sideways): an eighth of the view a notch, on
                        // through the track as the wheel turns down.
                        const notches = (Math.abs(wheel.angleDelta.x) > Math.abs(wheel.angleDelta.y)
                                         ? wheel.angleDelta.x : wheel.angleDelta.y) / 120;
                        root.scrollBy(-notches * area.width / 8);
                    }
                    wheel.accepted = true;
                }
            }
            PinchHandler {
                objectName: "zoomPinch"
                target: null
                property real lastScale: 1
                onActiveChanged: lastScale = 1
                onActiveScaleChanged: {
                    if (activeScale > 0) {
                        root.zoomBy(activeScale / lastScale, centroid.position.x);
                        lastScale = activeScale;
                    }
                }
            }
        }
    }

    // In the popup, so that they are not refused while it is open (a
    // window shortcut is, outside a modal popup). Space is the app's
    // own (PlayPauseShortcut), which works over every dialog.
    Shortcut {
        sequences: ["+", "=", StandardKey.ZoomIn]
        enabled: root.opened
        onActivated: root.zoomIn()
    }
    Shortcut {
        sequences: ["-", StandardKey.ZoomOut]
        enabled: root.opened
        onActivated: root.zoomOut()
    }
}
