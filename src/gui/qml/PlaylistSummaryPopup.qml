// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// What a playlist holds, at a glance: shown beside its row in Browse's
// sidebar after a moment's hover. The list owns one of these and moves
// it from row to row (PlaylistListView's summary), so a long list of
// playlists carries one popup, not one per row.
// Its cover made from its tracks' covers, how many tracks and how long
// they play, the BPM range, how many carry cue points, and the keys as
// a pie in the Camelot wheel's colours (Theme.colorForKey, as KeyBadge
// paints them) with the commonest few named beside it, so nothing rests
// on colour alone.
//
// A tooltip in all but size: it never takes focus and never closes
// itself. The list closes it: the pointer gone from every row and from
// this popup, a press or the wheel in a row. It rises a little as it
// fades in and sinks as it fades out; moved to another playlist, its
// content cross-fades rather than blinking.
//
// The pointer may come into it, and when it does the popup shows the
// rest of what it knows below its first look: every key, not the
// commonest four, and the longest and shortest track. Shown on arrival
// rather than behind a "more" to click: the pointer coming in is the
// question, and a click target in a popup that closes when the pointer
// strays is one more thing to aim at. It grows downward from where it
// stands, so nothing under the pointer moves.
PanelPopup {
    id: popup
    objectName: "playlistSummaryPopup"

    property string title: ""
    // ScanController.playlistSummary()'s map.
    property var summary: ({})
    // The middle of the row it stands beside, in its parent's (the
    // list's) coordinates. A move to another row slides it there.
    property real rowCenter: 0
    Behavior on rowCenter {
        enabled: popup.opened
        NumberAnimation { duration: Theme.shortTransitionDuration; easing.type: Easing.OutCubic }
    }

    // Shows another playlist. While open, the content fades out, changes
    // and fades back in; closed, it simply takes the new one.
    function showPlaylist(title, summary, rowCenter) {
        popup.nextTitle = title;
        popup.nextSummary = summary;
        popup.rowCenter = rowCenter;
        if (popup.visible) {
            contentSwitch.restart();
        } else {
            contentSwitch.stop();
            popup.contentItem.opacity = 1;
            popup.applyNext();
        }
    }
    property string nextTitle: ""
    property var nextSummary: ({})
    function applyNext() {
        popup.title = popup.nextTitle;
        popup.summary = popup.nextSummary;
    }
    SequentialAnimation {
        id: contentSwitch
        NumberAnimation {
            target: popup.contentItem; property: "opacity"; to: 0
            duration: Theme.shortTransitionDuration / 2; easing.type: Easing.InCubic
        }
        ScriptAction { script: popup.applyNext() }
        NumberAnimation {
            target: popup.contentItem; property: "opacity"; to: 1
            duration: Theme.shortTransitionDuration; easing.type: Easing.OutCubic
        }
    }
    // "camelot" or "traditional", as the track list names keys.
    property string notation: "camelot"

    readonly property int count: popup.summary.count || 0
    readonly property var keys: popup.summary.keys || []

    // The pointer in the popup, padding included (the list keeps it open
    // while this is true). Two handlers: the card's own
    // hears the padding, but not the pointer over the content, whose
    // labels (each with a HoverHandler under org.kde.desktop) keep it
    // from anything beneath them; the content's own, an ancestor of
    // those labels, hears it there.
    readonly property bool hovered: popupHover.hovered || contentHover.hovered
    // Set once the pointer has been in, and kept while the popup is
    // open, from playlist to playlist, so the details do not fold away
    // as the pointer crosses back to the rows.
    property bool expanded: false
    onHoveredChanged: {
        if (popup.hovered) {
            popup.expanded = true;
        }
    }
    onClosed: popup.expanded = false
    // The height of its first look: centred on the row by that, so the
    // details, once shown, grow below it and nothing under the pointer
    // moves.
    property real collapsedHeight: 0
    onHeightChanged: {
        if (!popup.expanded) {
            popup.collapsedHeight = popup.height;
        }
    }
    // How far below its place it stands while it rises in or sinks out.
    property real lift: 0

    // The BPM heatmap: bins from the summary, coloured from a faint tone
    // to the accent by how full they are. An empty bin keeps the faint
    // tone, so the strip reads as one bar from the lowest tempo to the
    // highest; a bin with anything in it is never fainter than a quarter
    // of the way, so one track among many shows.
    readonly property var bpmBins: popup.summary.bpmBins || []
    readonly property int bpmBinWidth: popup.summary.bpmBinWidth || 1
    readonly property int bpmBinMax: popup.bpmBins.reduce((most, bin) => Math.max(most, bin.count), 0)
    readonly property color emptyBinColor: Theme.mix(Theme.surface, Theme.text, 0.14)
    function binColor(count) {
        if (count <= 0 || popup.bpmBinMax <= 0) {
            return popup.emptyBinColor;
        }
        return Theme.mix(popup.emptyBinColor, Theme.accent, 0.25 + 0.75 * count / popup.bpmBinMax);
    }
    function binLabel(edge) {
        return popup.bpmBinWidth === 1 ? String(edge) : edge + " to " + (edge + popup.bpmBinWidth - 1);
    }
    function trackLine(track) {
        if (!track || !(track.durationSeconds > 0)) {
            return "";
        }
        const who = [track.title, track.artist].filter(part => part && part.length > 0).join(", ");
        return who + " (" + popup.duration(track.durationSeconds) + ")";
    }

    // Mirrors ScanPage.qml's own formatBpm().
    function formatBpm(bpm) {
        if (bpm <= 0) {
            return "";
        }
        const oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }
    // Mirrors RecordingsPage.qml's own duration(): "1:07:13" or "7:13".
    function duration(seconds) {
        const total = Math.round(seconds);
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const two = v => (v < 10 ? "0" : "") + v;
        return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
    }
    function keyLabel(entry) {
        if (entry.camelot.length === 0) {
            return "no key";
        }
        // The spelling the tracks use, not one made up from the wheel
        // position: "D♭" stays "D♭".
        return popup.notation === "traditional" ? Theme.traditionalLabel(entry.key) : entry.camelot;
    }
    // The commonest keys first, the wheel order breaking ties; the
    // legend has room for this many.
    readonly property int legendRows: 4
    readonly property var topKeys: popup.keys.slice().sort((a, b) => b.count - a.count).slice(0, popup.legendRows)

    // Beside the list, level with the row.
    x: parent ? Theme.snap(parent.width + Theme.tightSpacing) : 0
    y: Theme.snap(popup.rowCenter - popup.collapsedHeight / 2 + popup.lift)
    // Kept inside the window: the sidebar's last rows would otherwise
    // push it off the bottom.
    margins: Theme.pageMargin
    // Room for the BPM strip at its full width beside its labels.
    width: Theme.scaled(360)
    modal: false
    dim: false
    focus: false
    closePolicy: Popup.NoAutoClose

    // In: a fade and a small rise. Out: a fade and a sink, slower, so
    // the eye sees it go rather than blink off.
    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"; from: 0; to: 1
                duration: Theme.shortTransitionDuration; easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "lift"; from: Theme.scaled(8); to: 0
                duration: Theme.shortTransitionDuration; easing.type: Easing.OutCubic
            }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"; from: 1; to: 0
                duration: 2 * Theme.shortTransitionDuration; easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "lift"; from: 0; to: Theme.scaled(8)
                duration: 2 * Theme.shortTransitionDuration; easing.type: Easing.InCubic
            }
        }
    }

    // PanelPopup's card, with the pointer followed across all of it. The
    // wheel needs nothing here: a popup keeps it from what lies under it.
    background: Rectangle {
        radius: Theme.popupRadius
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        HoverHandler { id: popupHover }
    }

    contentItem: ColumnLayout {
        spacing: Theme.sectionSpacing
        HoverHandler { id: contentHover }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.rowSpacing

            PlaylistCoverMosaic {
                objectName: "coverMosaic"
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                covers: popup.summary.artwork || []
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.tightSpacing

                Label {
                    objectName: "summaryTitle"
                    Layout.fillWidth: true
                    text: popup.title
                    font.bold: true
                    font.pointSize: Theme.cardTitleSize
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "summaryCount"
                    text: popup.count === 1 ? "1 track" : popup.count + " tracks"
                    color: Theme.textMuted
                }
                Label {
                    objectName: "summaryDuration"
                    visible: (popup.summary.totalDurationSeconds || 0) > 0
                    text: popup.duration(popup.summary.totalDurationSeconds || 0)
                    font.family: Theme.dataFamily
                }
            }
        }

        // One caption column, one value column: BPM, cue points, keys.
        GridLayout {
            Layout.fillWidth: true
            visible: popup.count > 0
            columns: 2
            columnSpacing: Theme.rowSpacing
            rowSpacing: Theme.tightSpacing

            Label {
                Layout.alignment: Qt.AlignTop
                visible: bpmHeat.visible
                text: "BPM"
                color: Theme.textMuted
            }
            // The range as a strip, lowest at the left: where the
            // playlist's tempos sit, and where most of them do.
            ColumnLayout {
                id: bpmHeat
                objectName: "bpmHeat"
                visible: popup.bpmBins.length > 0
                spacing: Theme.tightSpacing / 2
                readonly property bool oneValue: popup.formatBpm(popup.summary.bpmMin || 0)
                    === popup.formatBpm(popup.summary.bpmMax || 0)
                RowLayout {
                    spacing: Theme.tightSpacing
                    Label {
                        objectName: "bpmMinLabel"
                        text: popup.formatBpm(popup.summary.bpmMin || 0)
                        font.family: Theme.dataFamily
                        font.pointSize: Theme.fontSmall
                    }
                    Item {
                        id: bpmStrip
                        objectName: "bpmStrip"
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: Theme.scaled(160)
                        implicitHeight: Theme.scaled(12)
                        Repeater {
                            model: popup.bpmBins
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                objectName: "bpmBin"
                                readonly property int count: modelData.count
                                x: Math.round(index * bpmStrip.width / popup.bpmBins.length)
                                width: Math.round((index + 1) * bpmStrip.width / popup.bpmBins.length) - x
                                height: bpmStrip.height
                                color: popup.binColor(count)
                            }
                        }
                        // The pointer over the strip: the bin under it is
                        // outlined, and a label just above the pointer
                        // gives its tempo and how many tracks sit there.
                        // At once and following the pointer, not a
                        // ToolTip with its delay. The strip lies inside
                        // the popup's content, so resting here is resting
                        // in the popup.
                        //
                        // Read from the content's own HoverHandler, not
                        // one of the strip's: under org.kde.desktop a
                        // handler on the strip never heard the pointer,
                        // while the content's, which the popup already
                        // relies on, always does.
                        readonly property point pointer: contentHover.hovered
                            ? bpmStrip.mapFromItem(popup.contentItem, contentHover.point.position.x,
                                                   contentHover.point.position.y)
                            : Qt.point(-1, -1)
                        readonly property bool pointerOn: popup.bpmBins.length > 0
                            && pointer.x >= 0 && pointer.x < bpmStrip.width
                            && pointer.y >= 0 && pointer.y < bpmStrip.height
                        readonly property int hoveredBin: bpmStrip.pointerOn
                            ? Math.max(0, Math.min(popup.bpmBins.length - 1,
                                                   Math.floor(pointer.x / bpmStrip.width * popup.bpmBins.length)))
                            : -1
                        Rectangle {
                            objectName: "bpmBinOutline"
                            visible: bpmStrip.hoveredBin >= 0
                            readonly property int bin: bpmStrip.hoveredBin
                            x: Math.round(Math.max(0, bin) * bpmStrip.width / Math.max(1, popup.bpmBins.length))
                            width: Math.round((Math.max(0, bin) + 1) * bpmStrip.width / Math.max(1, popup.bpmBins.length)) - x
                            height: bpmStrip.height
                            color: "transparent"
                            border.color: Theme.text
                            border.width: 1
                        }
                        Rectangle {
                            objectName: "bpmHoverLabel"
                            visible: bpmStrip.hoveredBin >= 0
                            z: 1
                            readonly property var bin: bpmStrip.hoveredBin >= 0 ? popup.bpmBins[bpmStrip.hoveredBin] : null
                            readonly property string text: bin === null ? ""
                                : popup.binLabel(bin.bpm) + ", "
                                  + (bin.count === 0 ? "none" : bin.count === 1 ? "1 track" : bin.count + " tracks")
                            x: Math.round(bpmStrip.pointer.x - width / 2)
                            y: -height - Theme.tightSpacing / 2
                            width: hoverText.implicitWidth + Theme.tightSpacing * 2
                            height: hoverText.implicitHeight + Theme.tightSpacing
                            radius: Theme.cornerRadius
                            color: Theme.groupBackground
                            border.color: Theme.border
                            border.width: 1
                            Label {
                                id: hoverText
                                anchors.centerIn: parent
                                text: parent.text
                                font.family: Theme.dataFamily
                                font.pointSize: Theme.fontSmall
                            }
                        }
                    }
                    Label {
                        objectName: "bpmMaxLabel"
                        visible: !bpmHeat.oneValue
                        text: popup.formatBpm(popup.summary.bpmMax || 0)
                        font.family: Theme.dataFamily
                        font.pointSize: Theme.fontSmall
                    }
                }
                Label {
                    objectName: "bpmPeakLabel"
                    visible: popup.bpmBins.length > 1
                    text: "Most at " + popup.binLabel(popup.summary.bpmPeak || 0)
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                }
            }

            Label {
                text: "Cue points"
                color: Theme.textMuted
            }
            Label {
                objectName: "summaryCues"
                text: (popup.summary.withCues || 0) + " with, " + (popup.summary.withoutCues || 0) + " without"
                font.family: Theme.dataFamily
            }

            Label {
                Layout.alignment: Qt.AlignTop
                text: "Keys"
                color: Theme.textMuted
            }
            RowLayout {
                spacing: Theme.rowSpacing

                Canvas {
                    id: keyPie
                    objectName: "keyPie"
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Theme.scaled(64)
                    Layout.preferredHeight: Theme.scaled(64)

                    // [{count, color}], round the wheel from twelve o'clock.
                    readonly property var slices: popup.keys.map(entry => ({count: entry.count,
                                                                           color: Theme.colorForKey(entry.camelot)}))
                    readonly property int total: slices.reduce((sum, slice) => sum + slice.count, 0)
                    // Tests wait on this: a Canvas paints a frame or more
                    // after it is asked to.
                    property int paintedCount: 0
                    onPainted: keyPie.paintedCount += 1
                    onSlicesChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()

                    // As CueCoveragePie draws its two slices: filled
                    // wedges, and the edges between them cut out of the
                    // disc rather than painted over.
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        const cx = width / 2;
                        const cy = height / 2;
                        const r = Math.max(0, Math.min(width, height) / 2 - 1);
                        if (r <= 0 || keyPie.total === 0) {
                            return;
                        }
                        const top = -Math.PI / 2;
                        const edges = [];
                        let from = top;
                        for (const slice of keyPie.slices) {
                            const to = from + 2 * Math.PI * slice.count / keyPie.total;
                            ctx.beginPath();
                            ctx.moveTo(cx, cy);
                            ctx.arc(cx, cy, r, from, to, false);
                            ctx.closePath();
                            ctx.fillStyle = slice.color;
                            ctx.fill();
                            edges.push(from);
                            from = to;
                        }
                        if (keyPie.slices.length < 2) {
                            return;
                        }
                        ctx.globalCompositeOperation = "destination-out";
                        ctx.strokeStyle = "black";
                        ctx.lineWidth = Theme.chartSliceGap;
                        ctx.lineCap = "butt";
                        for (const angle of edges) {
                            ctx.beginPath();
                            ctx.moveTo(cx, cy);
                            ctx.lineTo(cx + (r + 1) * Math.cos(angle), cy + (r + 1) * Math.sin(angle));
                            ctx.stroke();
                        }
                        ctx.globalCompositeOperation = "source-over";
                    }
                }

                // Swatch, key, count: the commonest keys, then how many
                // more the pie holds.
                ColumnLayout {
                    objectName: "keyLegend"
                    Layout.alignment: Qt.AlignTop
                    spacing: Theme.tightSpacing / 2

                    // The widest key name, so the counts line up.
                    TextMetrics {
                        id: keyWidth
                        font.family: Theme.dataFamily
                        text: popup.notation === "traditional" ? "F♯m" : "12A"
                    }
                    Repeater {
                        model: popup.topKeys
                        delegate: RowLayout {
                            required property var modelData
                            objectName: "keyLegendEntry"
                            spacing: Theme.tightSpacing
                            Rectangle {
                                implicitWidth: Theme.chartSwatchSize
                                implicitHeight: Theme.chartSwatchSize
                                radius: 2
                                color: Theme.colorForKey(modelData.camelot)
                            }
                            Label {
                                objectName: "keyLegendKey"
                                Layout.minimumWidth: Math.ceil(keyWidth.advanceWidth)
                                text: popup.keyLabel(modelData)
                                font.family: Theme.dataFamily
                            }
                            Label {
                                objectName: "keyLegendCount"
                                text: modelData.count
                                font.family: Theme.dataFamily
                                color: Theme.textMuted
                            }
                        }
                    }
                    Label {
                        objectName: "keyLegendMore"
                        readonly property int more: popup.keys.length - popup.topKeys.length
                        // Answered below once the popup has expanded.
                        visible: more > 0 && !popup.expanded
                        text: more === 1 ? "and 1 more" : "and " + more + " more"
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                    }
                }
            }

            // ---- What the pointer coming in reveals ----
            Label {
                Layout.alignment: Qt.AlignTop
                visible: allKeys.visible
                text: "All keys"
                color: Theme.textMuted
            }
            Flow {
                id: allKeys
                objectName: "allKeys"
                Layout.fillWidth: true
                visible: popup.expanded && popup.keys.length > 0
                spacing: Theme.rowSpacing
                Repeater {
                    model: popup.expanded ? popup.keys : []
                    delegate: Row {
                        required property var modelData
                        objectName: "allKeysEntry"
                        spacing: Theme.tightSpacing / 2
                        readonly property string label: popup.keyLabel(modelData) + " " + modelData.count
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.chartSwatchSize
                            height: Theme.chartSwatchSize
                            radius: 2
                            color: Theme.colorForKey(modelData.camelot)
                        }
                        Label {
                            text: popup.keyLabel(modelData)
                            font.family: Theme.dataFamily
                        }
                        Label {
                            text: modelData.count
                            font.family: Theme.dataFamily
                            color: Theme.textMuted
                        }
                    }
                }
            }

            Label {
                visible: longestLabel.visible
                text: "Longest"
                color: Theme.textMuted
            }
            Label {
                id: longestLabel
                objectName: "longestTrack"
                Layout.fillWidth: true
                visible: popup.expanded && text.length > 0
                text: popup.trackLine(popup.summary.longest)
                elide: Text.ElideRight
            }
            Label {
                visible: shortestLabel.visible
                text: "Shortest"
                color: Theme.textMuted
            }
            Label {
                id: shortestLabel
                objectName: "shortestTrack"
                Layout.fillWidth: true
                // One track is its own longest and shortest: said once.
                visible: popup.expanded && text.length > 0 && popup.count > 1
                text: popup.trackLine(popup.summary.shortest)
                elide: Text.ElideRight
            }
        }
    }
}
