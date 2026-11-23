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
// about playback. While open it follows the player: another track loaded
// is shown at once, at the same zoom in bars.
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
    // AppSettingsController, for the zoom and the time shown, which are
    // kept between openings and runs. Optional: without it both are this
    // popup's own.
    property var settings: null

    showBadge: false
    // Escape closes it only while it has the focus.
    focus: true
    // The title and artist are drawn in the popup's own top row, beside
    // the controls (the dialog's title bar would put them above them).
    title: ""
    readonly property string trackTitle: root.controller && root.controller.title ? root.controller.title : ""
    readonly property string trackArtist: root.controller && root.controller.artist ? root.controller.artist : ""
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

    // The track the player has, as it is, at the zoom last used.
    function openLoaded() {
        root.attached = true;
        root.spanBars = root.settings && root.settings.waveformZoomBars > 0
            ? root.settings.waveformZoomBars : root.localZoomBars;
        root.resync(root.controller.position || 0);
        root.open();
    }

    // Everything here runs from the moment open() is called, not only
    // once the dialog has finished fading in: a report of the player's
    // that arrives while it opens is taken.
    readonly property bool showing: root.visible

    onShowingChanged: {
        if (root.showing) {
            root.askForDetail();
        } else {
            flick.cancelFlick();
            root.storeZoom();
            root.releaseDetail();
        }
    }
    // Whether this popup asked for the detail and has not let it go: the
    // player bar and the details pane each have a popup on one player,
    // and one that never asked must not cancel the other's read.
    property bool detailAsked: false
    function askForDetail() {
        if (root.controller && typeof root.controller.requestDetailWaveform === "function") {
            root.detailAsked = true;
            root.controller.requestDetailWaveform();
        }
    }
    function releaseDetail() {
        if (root.detailAsked && root.controller && typeof root.controller.releaseDetailWaveform === "function") {
            root.controller.releaseDetailWaveform();
        }
        root.detailAsked = false;
    }

    // ---- Zoom ------------------------------------------------------

    // The loaded track's length, once the player has reported it for this
    // track: until then `duration` may still be the previous track's (see
    // durationSerial), and a grid, cues or columns laid out to it would be
    // laid out wrong. A controller without durationSerial (a test's fake)
    // is taken at its word, as PlayerBar does.
    readonly property bool lengthKnown: !!root.controller && root.controller.duration > 0
        && (root.controller.durationSerial === undefined
            || root.controller.durationSerial === root.controller.trackSerial)
    readonly property real durationMs: root.lengthKnown ? root.controller.duration : 0
    // How many tiles are drawn (for the tests).
    readonly property int tilesShown: tileRepeater.count
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
    // Where it opens the first time ever.
    readonly property int openingBars: 16
    readonly property real minimumSpanMs: root.minimumBars * root.barMs
    readonly property real maximumSpanMs: Math.max(root.minimumSpanMs, root.durationMs)
    // How many bars the view shows across its width: bars, not time, so
    // that another track, faster or slower, is shown at the same zoom.
    property real spanBars: root.openingBars
    // The zoom kept without a settings controller.
    property real localZoomBars: root.openingBars
    readonly property real shownSpanMs: root.clampSpan(root.spanBars * root.barMs)
    readonly property real pxPerMs: area.width > 0 ? area.width / root.shownSpanMs : 0
    // One press of a zoom key, and one notch of the wheel.
    readonly property real zoomStep: 1.5
    // The zoom as the slider shows it: 0 is the whole track, 1 is four
    // bars across. Even on a log scale, so equal moves of the handle are
    // equal zoom factors wherever on the slider they are made.
    readonly property real zoomLevel: root.levelOfSpan(root.shownSpanMs)

    function clampSpan(span) {
        return Math.max(root.minimumSpanMs, Math.min(root.maximumSpanMs, span));
    }
    function setSpan(span) {
        root.spanBars = span / root.barMs;
        root.localZoomBars = root.spanBars;
        zoomStore.restart();
    }
    // The zoom is written to the settings once it has been left alone for
    // a moment, and when the popup closes: not at every notch of a wheel.
    function storeZoom() {
        zoomStore.stop();
        if (root.settings && root.settings.waveformZoomBars !== root.spanBars) {
            root.settings.waveformZoomBars = root.spanBars;
        }
    }
    Timer {
        id: zoomStore
        interval: 500
        onTriggered: root.storeZoom()
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
        flick.cancelFlick();
        if (root.attached) {
            root.setSpan(span);
            return;
        }
        const at = root.manualMs + (anchorX - root.lineX) / before;
        root.setSpan(span);
        root.manualMs = root.clampTime(at - (anchorX - root.lineX) / root.pxPerMs);
    }
    function zoomIn() {
        root.zoomBy(root.zoomStep, root.lineX);
    }
    function zoomOut() {
        root.zoomBy(1 / root.zoomStep, root.lineX);
    }
    // The span a slider level stands for.
    function spanAtLevel(level) {
        return root.maximumSpanMs * Math.pow(root.minimumSpanMs / root.maximumSpanMs, level);
    }
    function levelOfSpan(span) {
        return root.maximumSpanMs > root.minimumSpanMs
            ? Math.log(root.maximumSpanMs / span) / Math.log(root.maximumSpanMs / root.minimumSpanMs)
            : 1;
    }
    // The slider's zoom: as a key does it, around the line.
    function zoomToLevel(level) {
        root.zoomBy(root.shownSpanMs / root.spanAtLevel(level), root.lineX);
    }
    // A drag reports far more often than the view can follow: every zoom
    // redraws each tile and works out the track's beat marks again. So
    // while the handle is dragged the zoom follows it at most once per
    // interval, the first move at once and the latest one at the end of
    // it; letting go applies where the handle is.
    readonly property int zoomFollowInterval: 50
    property real pendingZoomLevel: -1
    function followSlider(level) {
        root.pendingZoomLevel = level;
        if (!zoomFollow.running) {
            root.applyPendingZoom();
        }
    }
    function applyPendingZoom() {
        if (root.pendingZoomLevel < 0) {
            return;
        }
        const level = root.pendingZoomLevel;
        root.pendingZoomLevel = -1;
        root.zoomToLevel(level);
        zoomFollow.restart();
    }
    Timer {
        id: zoomFollow
        interval: root.zoomFollowInterval
        onTriggered: root.applyPendingZoom()
    }
    // A mark on the slider at every doubling from four bars, and one at
    // the whole track unless a doubling sits next to it.
    readonly property var zoomTicks: {
        if (root.maximumSpanMs <= root.minimumSpanMs) {
            return [];
        }
        const out = [];
        for (let bars = root.minimumBars; bars * root.barMs < root.maximumSpanMs; bars *= 2) {
            out.push(root.levelOfSpan(bars * root.barMs));
        }
        if (out.length === 0 || out[out.length - 1] > 0.08) {
            out.push(0);
        }
        return out;
    }

    // ---- Position --------------------------------------------------

    // The golden-ratio point left of the middle: 1 - 1/phi of the width.
    readonly property real lineRatio: 1 - 2 / (1 + Math.sqrt(5))
    // On the device grid, so the line is sharp at a fractional scale.
    readonly property real lineX: Theme.snap(area.width * root.lineRatio)
    // Whether the view follows the playhead. Scrolling by hand lets go;
    // play, a seek, the follow button and another track take hold again.
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
    function attach() {
        flick.cancelFlick();
        root.attached = true;
    }
    function follow() {
        root.resync(root.controller.position || 0);
        root.attach();
    }
    function scrollBy(px) {
        if (root.pxPerMs <= 0) {
            return;
        }
        flick.cancelFlick();
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
        root.attach();
    }

    // The frames. frameClock false stops the clock on the wall, so that a
    // test can hand it frames of its own through advanceClock().
    property bool frameClock: true
    function advanceClock(seconds) {
        root.smoothMs = Math.min(root.durationMs, root.smoothMs + seconds * 1000);
    }
    FrameAnimation {
        objectName: "waveformClock"
        running: root.showing && root.playing && root.frameClock
        // smoothFrameTime, for the reason the ring gives (TrackRing.qml).
        onTriggered: root.advanceClock(smoothFrameTime)
    }
    Connections {
        // Only while shown: hidden, nothing here is drawn, and
        // openLoaded() takes the position as it is then.
        target: root.showing ? root.controller : null
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
                root.attach();
            }
        }
        function onSeeked(positionMs) {
            root.resync(positionMs);
            root.attach();
        }
        // Another track: shown at the zoom in bars it had, from where the
        // player is, following it, and its finer waveform asked for.
        function onTrackChanged() {
            root.resync(root.controller.position || 0);
            root.attach();
            root.askForDetail();
        }
    }

    // ---- What is drawn ---------------------------------------------

    // The finest waveform the library has for the loaded track, as the
    // player hands it over once it has been read (off the GUI thread);
    // the overview until then, and where there is nothing finer.
    readonly property var detail: root.showing && root.controller && root.controller.detailWaveform
        ? root.controller.detailWaveform : ({})
    // Each library's detail in its own player's colours: an Engine
    // track as a Denon player draws it, a rekordbox one in rekordbox's
    // three-band colours (WaveformView.colorScheme).
    readonly property string colorScheme: root.controller && root.controller.currentFormat === "engine"
        ? "engine" : "vivid"
    readonly property bool hasDetail: root.detail.columns !== undefined && root.detail.columns !== null
        && root.detail.columns.byteLength >= 3
    // The beats drawn at this zoom, for the whole track: the same rule as
    // every other waveform, at the width the whole track would have.
    readonly property var ticks: {
        if (!root.showing || !root.controller || root.durationMs <= 0 || root.pxPerMs <= 0
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
    // The lane's height, the same on every tile.
    readonly property real laneHeight: root.ticks.length > 0 ? Theme.beatRulerMaximumHeight : 0

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.tightSpacing

        // Title and artist on the left; the controls on the right, then
        // close in the corner.
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.tightSpacing

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    objectName: "zoomTitle"
                    Layout.fillWidth: true
                    text: root.trackTitle
                    font.bold: true
                    font.pointSize: Theme.fontMedium
                    color: Theme.text
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "zoomArtist"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.trackArtist
                    color: Theme.textMuted
                    elide: Text.ElideRight
                }
            }
            IconToolButton {
                objectName: "zoomPlayButton"
                focusPolicy: Qt.NoFocus
                text: root.playing ? "Pause" : "Play"
                iconName: root.playing ? "media-playback-pause" : "media-playback-start"
                onClicked: root.controller.togglePlay()
            }
            // Left is the whole track, right four bars across. No focus,
            // as the buttons beside it: the arrow keys stay the waveform's.
            Slider {
                id: zoomSlider
                objectName: "zoomSlider"
                Layout.preferredWidth: Theme.scaled(140)
                focusPolicy: Qt.NoFocus
                from: 0
                to: 1
                enabled: root.maximumSpanMs > root.minimumSpanMs
                // The handle moves through the slider's own C++ setter,
                // which leaves this binding in place; and the zoom a drag
                // applies is always the handle's own latest level, so it
                // never pulls the handle back.
                value: root.zoomLevel
                Accessible.name: "Zoom"
                // While dragged it says how much it shows.
                ToolTip.visible: pressed || hovered
                ToolTip.delay: pressed ? 0 : 600
                ToolTip.text: !pressed ? "Zoom, or Shift+scroll over the waveform"
                    : value <= 0.001 ? "Whole track"
                    : Math.round(root.spanAtLevel(value) / root.barMs) + " bars"
                onMoved: root.followSlider(value)
                onPressedChanged: {
                    if (!pressed) {
                        root.applyPendingZoom();
                    }
                }

                // The marks, under the groove where the handle's centre
                // passes them. Some styles draw the handle without an item
                // (org.kde.desktop): there the groove's ends are used.
                Repeater {
                    model: root.zoomTicks
                    Rectangle {
                        required property real modelData
                        objectName: "zoomTick"
                        readonly property real handleWidth: zoomSlider.handle ? zoomSlider.handle.width : 0
                        width: Math.max(1, Theme.snap(1))
                        height: Theme.scaled(4)
                        x: Theme.snap(zoomSlider.leftPadding + handleWidth / 2
                                      + modelData * (zoomSlider.availableWidth - handleWidth) - width / 2)
                        y: Theme.snap(zoomSlider.height - height)
                        color: Theme.textMuted
                    }
                }
            }
            IconToolButton {
                objectName: "followButton"
                focusPolicy: Qt.NoFocus
                visible: !root.attached
                text: "Follow the playhead"
                iconName: "go-jump"
                onClicked: root.follow()
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
                    id: tileRepeater
                    // No tiles until the length is known: a tile with no
                    // length to place its columns by would spread the
                    // whole track across its own width, once per tile.
                    model: root.showing && root.durationMs > 0 && root.pxPerMs > 0 ? root.tileCount : 0
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
                        // Read once the tile is made, as showRuler is.
                        beatData: tileView.made
                            ? root.ticksBetween(tileView.viewStartMs, tileView.viewStartMs + tileView.viewSpanMs) : []
                        gridStyle: "ruler"
                        // Evaluated once more in a tile the Repeater is
                        // dropping as the player reports the length (a
                        // real one, opened from a page), when `root` is
                        // already gone from its context. And evaluated
                        // first, as the popup opens, before `root` can be
                        // reached from a new tile: a binding that found no
                        // `root` never looks again, and the tile kept no
                        // lane (Sebastian's screenshot, 2026-11-21). So it
                        // is decided once the tile is made, and only after
                        // the change that made it has been handed out in
                        // full: a binding that starts listening while a
                        // change is still being sent misses it.
                        property bool made: false
                        function markMade() {
                            tileView.made = true;
                        }
                        Component.onCompleted: Qt.callLater(tileView.markMade)
                        showRuler: tileView.made && root ? root.ticks.length > 0 : false
                        // `root` can be gone already, as for showRuler.
                        colorScheme: root ? root.colorScheme : "vivid"
                        // Played or not is shown by the dimming below, which
                        // moves with the playhead without a repaint.
                        unplayedOpacity: 1
                        cueData: root.controller ? root.controller.cues : []
                        progress: -1
                    }
                }
            }

            // The part already played, dimmed: what is ahead of the
            // playhead is what matters on a player.
            Rectangle {
                objectName: "playedShade"
                readonly property real playheadX: root.stripX
                    + (root.controller ? (root.playing && root.attached ? root.smoothMs
                                                                       : root.controller.position || 0) : 0)
                      * root.pxPerMs
                x: Math.max(0, root.stripX)
                y: root.laneHeight
                width: Math.max(0, Math.min(area.width, playheadX) - x)
                height: area.height - y
                color: Theme.background
                opacity: Theme.waveformPlayedShade
            }

            // "This position": fixed, the waveform moves under it.
            Rectangle {
                objectName: "positionLine"
                x: Theme.snap(root.lineX - width / 2)
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

            // Dragging and flicking scroll it with Qt's own flick physics:
            // let go with speed and it glides on and slows to a stop, at
            // the track's ends at the latest; a press stops it. The
            // Flickable is a proxy, never drawn: its contentX is the time
            // under the line in pixels, kept in step while it is still.
            Flickable {
                id: flick
                objectName: "zoomFlick"
                anchors.fill: parent
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                contentWidth: root.durationMs * root.pxPerMs + width
                contentHeight: height
                pressDelay: 0
                onMovementEnded: root.gliding = false
                onContentXChanged: {
                    if (flick.moving && root.pxPerMs > 0) {
                        root.detach();
                        root.manualMs = root.clampTime(flick.contentX / root.pxPerMs);
                    }
                }
                // A press holds the waveform where it is (it no longer
                // follows the playhead under the hand, so a drag starts
                // from what was grabbed); a click seeks there, unless the
                // press only stopped a glide.
                MouseArea {
                    objectName: "zoomInput"
                    width: flick.contentWidth
                    height: flick.height
                    property bool stoppedGlide: false
                    cursorShape: flick.dragging ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                    onPressed: {
                        stoppedGlide = flick.moving || root.gliding;
                        root.gliding = false;
                        flick.cancelFlick();
                        root.detach();
                    }
                    onClicked: (mouse) => {
                        if (!stoppedGlide) {
                            root.seekToX(mouse.x - flick.contentX);
                        }
                    }
                }
            }
            Binding {
                target: flick
                property: "contentX"
                value: root.centerMs * root.pxPerMs
                when: !flick.moving
                restoreMode: Binding.RestoreNone
            }

            // The wheel: shift or ctrl zooms, otherwise it scrolls; presses go on
            // to the Flickable underneath.
            MouseArea {
                objectName: "zoomWheel"
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                // Pixels a millisecond of the last touchpad scroll, for its
                // fling when the fingers leave.
                property real fingerVelocity: 0
                property real lastWheelTime: 0
                onWheel: (wheel) => {
                    wheel.accepted = true;
                    const pixels = wheel.pixelDelta.x !== 0 || wheel.pixelDelta.y !== 0;
                    const angles = wheel.angleDelta.x !== 0 || wheel.angleDelta.y !== 0;
                    // The phase first: the end of a touchpad scroll usually
                    // carries no delta at all.
                    // Flung only if the fingers were still moving when they
                    // left: Wayland sends nothing while they rest on the
                    // pad, then an end with no delta, and the last
                    // velocity would be a stale one.
                    if (wheel.phase === Qt.ScrollEnd) {
                        if (root.now() - lastWheelTime <= root.flingRecentMs
                                && Math.abs(fingerVelocity) > root.flingMinimumVelocity) {
                            root.flingAt(fingerVelocity * 1000);
                        }
                        fingerVelocity = 0;
                        return;
                    }
                    if (!pixels && !angles) {
                        return;   // nothing to scroll by
                    }
                    // Shift or ctrl zooms. Shift often turns a wheel
                    // sideways (macOS, some Linux setups): whichever way
                    // it comes, up or left is closer.
                    if (wheel.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) {
                        const along = (d) => Math.abs(d.x) > Math.abs(d.y) ? d.x : d.y;
                        const steps = pixels ? along(wheel.pixelDelta) / 50 : along(wheel.angleDelta) / 120;
                        root.zoomBy(Math.pow(root.zoomStep, steps), wheel.x);
                        return;
                    }
                    if (pixels) {
                        const dx = Math.abs(wheel.pixelDelta.x) >= Math.abs(wheel.pixelDelta.y)
                            ? wheel.pixelDelta.x : wheel.pixelDelta.y;
                        if (wheel.phase === Qt.ScrollMomentum) {
                            // The system's own glide (macOS): followed as it
                            // comes, with no glide of ours on top.
                            root.scrollBy(-dx);
                            return;
                        }
                        // A touchpad: the waveform follows the fingers, and
                        // flung (ScrollEnd above) glides on as a drag does.
                        const now = root.now();
                        const dt = Math.max(1, now - lastWheelTime);
                        fingerVelocity = dt < 100 ? -dx / dt : 0;
                        lastWheelTime = now;
                        root.scrollBy(-dx);
                        return;
                    }
                    // A wheel: an eighth of the view a notch, on through
                    // the track as the wheel turns down.
                    const notches = (Math.abs(wheel.angleDelta.x) > Math.abs(wheel.angleDelta.y)
                                     ? wheel.angleDelta.x : wheel.angleDelta.y) / 120;
                    root.scrollBy(-notches * area.width / 8);
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

        // The time, under the waveform at the right.
        TrackTimeLabel {
            objectName: "zoomTime"
            Layout.alignment: Qt.AlignRight
            positionMs: root.controller ? root.controller.position || 0 : 0
            durationMs: root.durationMs
            settings: root.settings
        }
    }

    // A glide of `velocity` pixels a second, positive on through the
    // track, as a flick of the waveform gives it.
    // A touchpad's lift flings only if its last motion was this recent
    // (milliseconds) and at least this fast (pixels a millisecond).
    readonly property int flingRecentMs: 80
    readonly property real flingMinimumVelocity: 0.2
    // The wall clock in milliseconds; a test sets clockNow to hand it
    // times of its own.
    property real clockNow: -1
    function now() {
        return root.clockNow >= 0 ? root.clockNow : Date.now();
    }

    // Set while a glide of flingAt() runs; a press during it stops it and
    // does not seek.
    property bool gliding: false
    function flingAt(velocity) {
        root.detach();
        flick.contentX = root.manualMs * root.pxPerMs;
        flick.flick(-velocity, 0);
        root.gliding = flick.moving;
    }

    // Gone or the app quitting: a zoom not yet written is written, and the
    // detail is no longer wanted.
    Component.onDestruction: {
        root.storeZoom();
        root.releaseDetail();
    }
    Connections {
        target: Qt.application
        function onAboutToQuit() { root.storeZoom(); }
    }

    // In the popup, so that they are not refused while it is open (a
    // window shortcut is, outside a modal popup). Space is the app's
    // own (PlayPauseShortcut), which works over every dialog.
    Shortcut {
        sequences: ["+", "=", StandardKey.ZoomIn]
        enabled: root.showing
        onActivated: root.zoomIn()
    }
    Shortcut {
        sequences: ["-", StandardKey.ZoomOut]
        enabled: root.showing
        onActivated: root.zoomOut()
    }
}
