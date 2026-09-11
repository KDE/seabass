// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The Matching panel (Experimental, see docs/experimental-features.md):
// finds tracks compatible in key/BPM/
// rating with whichever Browse row was last marked as the anchor (via
// that row's own edit button), scoped to This Playlist -- a filter here
// just like key/rating/BPM, not a separate write-target concept, so
// every result is already a member and Before/After only ever moves it
// to a new position (never inserts a track from elsewhere; see
// findCompatibleTracks()'s own doc comment). The PREVIEW badge below is
// deliberately separate from the page-level EXPERIMENTAL gate this whole
// panel already sits behind: the search/filter side here is fully real,
// only the actual Before/After write is a stub -- no format has a
// playlist writer yet, so clicking one just reports previewStatus rather
// than touching anything on disk.
ColumnLayout {
    id: root
    required property var scanController
    required property string keyNotation
    required property string anchorSourceId
    required property string anchorTitle
    required property string anchorArtist
    required property string anchorKey
    required property double anchorBpm
    required property string anchorArtworkPath
    // Every playlist the anchor track already belongs to -- shown as
    // quick picks at the top of the This Playlist combo (see
    // playlistComboModel below).
    required property var anchorPlaylistNames
    // Browse's own current playlist filter ("the playlist we're
    // currently in") -- read-only reference here, only used to seed
    // targetPlaylistIndex below each time a new track becomes the
    // anchor. This Playlist is a genuinely separate selection from
    // Browse's own: picking a different write target here must never
    // change what Browse itself is showing.
    required property int browseSelectedPlaylistIndex
    signal closeRequested()

    spacing: 0

    // This panel's own target playlist -- defaults to whatever Browse is
    // currently showing each time a new track is picked as the anchor
    // (see onAnchorSourceIdChanged below), but from then on is entirely
    // independent: the combo only ever writes to this, never back to
    // Browse's own selectedPlaylistIndex.
    property int targetPlaylistIndex: 0
    onAnchorSourceIdChanged: root.targetPlaylistIndex = root.browseSelectedPlaylistIndex

    // "" if "All tracks" is the target -- there's no real playlist to
    // write into, so Before/After stay disabled until a real one is
    // picked.
    readonly property string targetPlaylistName: root.targetPlaylistIndex === 0
        ? "" : (root.scanController.playlistNames[root.targetPlaylistIndex - 1] ?? "")

    // Additive: any subset of "match"/"relative"/"harmonic"/"energymix"
    // may be active at once (a track matches if its relation to the
    // anchor is any tier in the set), unlike the old single cumulative
    // keyMode this replaced -- see toggleKeyTier()/clearKeyTiers() below
    // and domain::keyRelationMatchesAnyMode(). Empty means no key
    // filtering at all ("All").
    property var selectedKeyTiers: ["match", "relative", "harmonic"]
    property int minRating: 0
    property string resultQuery: ""
    property string previewStatus: ""
    property var candidates: []
    // Driven by hovering a wedge or a legend row in a Camelot Wheel
    // popup opened from any KeyBadge on this panel (the anchor's own, or
    // a candidate row's): wheelHoverCamelot (a specific wedge) and
    // wheelHoverRelationLabel (a legend row -- every candidate at that
    // exact relation tier) both highlight matching rows live while
    // hovering, whichever's active. Purely ephemeral -- the wheel is a
    // visual aid only, it never sets a real filter (see
    // CamelotWheelPopup.qml's own doc comment).
    property string wheelHoverCamelot: ""
    property string wheelHoverRelationLabel: ""

    // BPM Range's steps aren't linear: 1% granularity close in, where it
    // actually matters for beatmatching, widening to 5% jumps further out
    // where the exact number stops mattering, ending in "ignore BPM
    // entirely" -- the same fixed-tolerance/ignore-scale relationship
    // Ignore Key already has, so the two sliders read consistently.
    readonly property var bpmSteps: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 30, -1]
    property int bpmStepIndex: 4
    readonly property bool bpmIgnored: root.bpmSteps[root.bpmStepIndex] < 0
    // A very large tolerance rather than a separate "ignore" code path
    // through findCompatibleTracks() -- every real BPM falls inside it,
    // so the backend's own BPM filter never excludes anything, without it
    // needing to know about a distinct ignore mode.
    readonly property real bpmTolerancePct: root.bpmIgnored ? 100000 : root.bpmSteps[root.bpmStepIndex]

    // This Playlist's combo model: "All tracks" always first (the
    // obvious, always-present way back to searching the whole library),
    // then the anchor's own current playlists (quick picks -- jump
    // straight back to a playlist you're already looking at this track
    // from), then every playlist in their normal order. Deliberately
    // duplicates a name that appears in both the quick-pick and normal
    // sections rather than deduplicating; canonicalIndex is what
    // actually drives root.targetPlaylistIndex, so two rows sharing one
    // name is harmless.
    readonly property var playlistComboModel: {
        var full = ["All tracks"].concat(root.scanController.playlistNames);
        var items = [{ name: full[0], canonicalIndex: 0, count: root.scanController.totalTrackCount }];
        root.anchorPlaylistNames.forEach(function (name) {
            var canonicalIndex = full.indexOf(name);
            if (canonicalIndex > 0) {
                items.push({ name: name, canonicalIndex: canonicalIndex,
                    count: root.scanController.playlistTrackCounts[name] ?? 0 });
            }
        });
        for (var i = 1; i < full.length; i++) {
            items.push({ name: full[i], canonicalIndex: i, count: root.scanController.playlistTrackCounts[full[i]] ?? 0 });
        }
        return items;
    }

    // Mirrors ScanPage.qml's own formatDuration().
    function formatDuration(seconds) {
        var total = Math.round(seconds);
        var m = Math.floor(total / 60);
        var s = total % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    readonly property bool hasAnchor: root.anchorSourceId.length > 0
    readonly property bool hasTarget: root.targetPlaylistName.length > 0

    // One-decimal BPM, only when there actually is a fraction -- mirrors
    // ScanPage.qml's own formatBpm(); no shared utils module exists to
    // pull this from instead.
    function formatBpm(bpm) {
        if (bpm <= 0) {
            return "--";
        }
        var oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }

    function previewNotSaved(action, trackTitle) {
        root.previewStatus = "Preview: saving to the playlist isn't implemented yet. (" + action + " “"
            + trackTitle + "”)";
    }

    // Additive toggle for the key-tier row: clicking a tier that isn't
    // active yet adds it to the set (on top of whatever else is already
    // checked); clicking one that's already active instead solos it --
    // exclusively that tier, dropping every other one that was also
    // checked. There's no third click state to remove just one tier from
    // a multi-tier selection; "All" (clearKeyTiers()) is the way back to
    // no filtering at all.
    function toggleKeyTier(tier) {
        root.selectedKeyTiers = root.selectedKeyTiers.indexOf(tier) !== -1
            ? [tier]
            : root.selectedKeyTiers.concat([tier]);
    }

    function clearKeyTiers() {
        root.selectedKeyTiers = [];
    }

    // The one-stop reset for the empty-results state: every filter this
    // panel applies, back to its default, including This Playlist (the
    // exception among them since it's normally set independently of the
    // others -- but "clear everything" should mean everything).
    function clearAllFilters() {
        root.selectedKeyTiers = ["match", "relative", "harmonic"];
        root.minRating = 0;
        root.bpmStepIndex = 4;
        root.resultQuery = "";
        root.targetPlaylistIndex = 0;
    }

    // Recomputed whenever any filter input -- or the anchor/target
    // playlist -- changes; batched into one string so a single
    // onXChanged handles every trigger instead of six separate ones.
    // Debounced (see refreshDebounce below) rather than calling refresh()
    // directly, so dragging the BPM slider or typing in the result filter
    // doesn't re-run findCompatibleTracks() on every intermediate value.
    readonly property string refreshKeySignature: root.anchorSourceId + "|" + root.selectedKeyTiers.join(",")
        + "|" + root.minRating + "|" + root.bpmTolerancePct + "|" + root.resultQuery + "|" + root.targetPlaylistName
    onRefreshKeySignatureChanged: {
        // The wheel's hover-fade is only ever meant as an ephemeral cue
        // while actively pointing at the wheel -- clearing it here (not
        // just on mouse-exit) means a filter change never leaves stale
        // rows faded against a hover target from before the list
        // changed underneath it.
        root.wheelHoverCamelot = "";
        root.wheelHoverRelationLabel = "";
        refreshDebounce.restart();
    }
    Component.onCompleted: {
        root.targetPlaylistIndex = root.browseSelectedPlaylistIndex;
        root.refresh();
    }

    Timer {
        id: refreshDebounce
        interval: 200
        onTriggered: root.refresh()
    }

    function refresh() {
        root.candidates = root.hasAnchor
            ? root.scanController.findCompatibleTracks(root.anchorSourceId, root.selectedKeyTiers, root.minRating,
                  root.bpmTolerancePct, root.resultQuery, root.targetPlaylistName)
            : [];
    }

    // Contains every overflow risk this panel has (long playlist names,
    // three-way segmented labels, candidate rows) -- without this, content
    // that's briefly wider than the panel's own allocated width (dragging
    // the splitter narrow, a small window) painted straight through into
    // Browse or past the window edge instead of staying inside the
    // panel's own margins.
    clip: true

    ColumnLayout {
        Layout.fillWidth: true
        Layout.margins: 14 * Theme.iconScale
        spacing: 8 * Theme.iconScale

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                RowLayout {
                    spacing: 6
                    PageTitle { text: "Matching Tracks"; level: "section" }
                    Rectangle {
                        radius: 3
                        color: Theme.warnBg
                        border.color: Theme.warnBorder
                        implicitWidth: previewBadgeText.implicitWidth + 8
                        implicitHeight: previewBadgeText.implicitHeight + 4
                        Label {
                            id: previewBadgeText
                            anchors.centerIn: parent
                            text: "PREVIEW"
                            font.pointSize: Theme.fontTiny
                            font.bold: true
                            color: Theme.warnText
                        }
                        HoverHandler { id: previewBadgeHover }
                        ToolTip.visible: previewBadgeHover.hovered
                        ToolTip.text: "Preview: finds compatible tracks for real, but Before/After doesn't "
                            + "save to the playlist yet."
                        ToolTip.delay: 300
                    }
                }
                // The track this panel is actually matching against, right
                // under the title where it's immediately visible -- the
                // instructional placeholder only shows before one's picked.
                RowLayout {
                    visible: root.hasAnchor
                    Layout.fillWidth: true
                    spacing: 6
                    Rectangle {
                        Layout.preferredWidth: Theme.iconSizeSmall
                        Layout.preferredHeight: Theme.iconSizeSmall
                        color: Theme.groupBackground
                        Image {
                            anchors.fill: parent
                            visible: root.anchorArtworkPath.length > 0
                            source: root.anchorArtworkPath
                            fillMode: Image.PreserveAspectCrop
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: root.anchorTitle
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            visible: root.anchorArtist.length > 0
                            text: root.anchorArtist
                            color: Theme.textMuted
                            font.pointSize: Theme.fontSmall
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        text: root.formatBpm(root.anchorBpm)
                        font.family: Theme.dataFamily
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                    }
                    KeyBadge {
                        keyName: root.anchorKey
                        notation: root.keyNotation
                        onKeyHovered: (number, isMinor, hovering) =>
                            root.wheelHoverCamelot = hovering ? (number + (isMinor ? "A" : "B")) : ""
                        onRelationHovered: (relationLabel, hovering) =>
                            root.wheelHoverRelationLabel = hovering ? relationLabel : ""
                    }
                }
                Label {
                    visible: !root.hasAnchor
                    text: "Search This Playlist, then Before/After the track you're editing in Browse."
                    color: Theme.textMuted
                    font.pointSize: Theme.fontSmall
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
            ToolButton {
                Layout.alignment: Qt.AlignTop
                text: "✕"
                ToolTip.visible: hovered
                ToolTip.text: "Close"
                onClicked: root.closeRequested()
            }
        }

        // This Playlist gets its own row rather than competing with the
        // title for width -- a real playlist name plus the title/badge
        // above was exactly what was overflowing past the panel's own
        // edge before.
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "This Playlist:"
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }
            ComboBox {
                id: playlistCombo
                Layout.fillWidth: true
                Layout.minimumWidth: 70
                model: root.playlistComboModel
                textRole: "name"
                // The combo's own display index (into playlistComboModel,
                // which may list the real selection twice -- once as a
                // quick pick, once in its normal place) rather than
                // root.targetPlaylistIndex directly; picks the first
                // (quick-pick, when there is one) row whose canonicalIndex
                // matches.
                currentIndex: {
                    for (var i = 0; i < root.playlistComboModel.length; i++) {
                        if (root.playlistComboModel[i].canonicalIndex === root.targetPlaylistIndex) {
                            return i;
                        }
                    }
                    return -1;
                }
                delegate: PlaylistRowDelegate {
                    // index isn't redeclared here -- see
                    // PlaylistListView.qml's own delegate for why that
                    // silently breaks rendering.
                    required property var modelData
                    name: modelData.name
                    count: modelData.count
                    isCurrent: modelData.canonicalIndex === root.targetPlaylistIndex
                    // Sets this panel's own target only -- deliberately
                    // never touches Browse's own playlist filter (see
                    // root.targetPlaylistIndex's own doc comment above).
                    onPicked: {
                        root.targetPlaylistIndex = modelData.canonicalIndex;
                        playlistCombo.popup.close();
                    }
                }
                // The default popup doesn't size itself correctly against
                // a custom item delegate (PlaylistRowDelegate is a plain
                // Rectangle, not an ItemDelegate the style already knows
                // how to measure) -- came out oversized with the rows
                // barely visible inside it. Sizing it explicitly, the
                // documented way to customize a ComboBox popup, fixes
                // that: width matches the combo, height matches the real
                // row count up to a cap, with its own scrollbar past that.
                popup: Popup {
                    y: playlistCombo.height
                    width: playlistCombo.width
                    implicitHeight: Math.min(contentItem.implicitHeight, 320)
                    padding: 1

                    contentItem: ListView {
                        clip: true
                        implicitHeight: contentHeight
                        model: playlistCombo.popup.visible ? playlistCombo.delegateModel : null
                        // A custom popup replaces the default wiring that
                        // would otherwise apply ComboBox's own `delegate:`
                        // automatically -- without this, the popup sizes
                        // correctly (real rows, real height) but renders
                        // nothing into any of them.
                        delegate: playlistCombo.delegate
                        currentIndex: playlistCombo.highlightedIndex
                        ScrollBar.vertical: BigScrollBar {}
                    }
                    background: Rectangle {
                        color: Theme.surface
                        border.color: Theme.border
                        radius: 4
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: "Which playlist to search within. Changing it here does not change Browse."
            }
        }

        // Additive: any combination of the four real tiers may be active
        // at once, same colors as the Camelot Wheel popup's own legend
        // (see CamelotWheelPopup.qml) but no longer wired to it -- the
        // wheel is a pure visual aid now, this row is the only place that
        // actually sets the filter. Clicking an unchecked tier adds it;
        // clicking an already-checked one solos it (see toggleKeyTier()).
        // All clears every tier, showing every track regardless of key.
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Repeater {
                model: [
                    { value: "match", label: "Match", color: Theme.accent, tip: "Only the exact same key" },
                    { value: "relative", label: "Relative", color: Theme.good,
                        tip: "Relative major/minor of the anchor's key" },
                    { value: "harmonic", label: "Harmonic", color: Theme.warnIcon,
                        tip: "One step around the wheel, same mode: the classic harmonic-mixing move" },
                    { value: "energymix", label: "Energy Mix", color: Theme.danger,
                        tip: "One step around the wheel, opposite mode: a bigger energy shift" },
                    { value: "all", label: "All", color: Theme.textMuted,
                        tip: "Show every track regardless of key" },
                ]
                delegate: Button {
                    id: modeButton
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.minimumWidth: 56
                    text: modelData.label
                    checkable: true
                    checked: modelData.value === "all"
                        ? root.selectedKeyTiers.length === 0
                        : root.selectedKeyTiers.indexOf(modelData.value) !== -1
                    ToolTip.visible: hovered
                    ToolTip.text: modelData.tip
                    onClicked: modelData.value === "all"
                        ? root.clearKeyTiers()
                        : root.toggleKeyTier(modelData.value)

                    background: Rectangle {
                        color: modeButton.checked
                            ? Qt.rgba(modeButton.modelData.color.r, modeButton.modelData.color.g,
                                modeButton.modelData.color.b, 0.22)
                            : Theme.groupBackground
                        border.width: modeButton.checked ? 2 : 1
                        border.color: modeButton.checked ? modeButton.modelData.color : Theme.borderSubtle
                    }
                    contentItem: Label {
                        text: modeButton.text
                        color: modeButton.checked ? modeButton.modelData.color : Theme.textMuted
                        font.bold: modeButton.checked
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Button {
                Layout.fillWidth: true
                Layout.minimumWidth: 80
                text: "Genre: Ignore"
                enabled: false
                ToolTip.visible: hovered
                ToolTip.text: "Genre isn't read from your library yet"
            }
            Label {
                text: "Rating:"
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                ToolTip.visible: ratingLabelHover.hovered
                ToolTip.text: "Click to reset to Ignore"
                HoverHandler { id: ratingLabelHover }
                TapHandler { onTapped: root.minRating = 0 }
            }
            StarRating {
                value: root.minRating
                ToolTip.visible: hovered
                ToolTip.text: root.minRating > 0
                    ? "Only showing tracks rated " + root.minRating + "★ or higher; click again to ignore rating"
                    : "Rating: Ignore; click a star to only show tracks rated at least that high"
                onRatingPicked: (rating) => root.minRating = (rating === root.minRating ? 0 : rating)
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label { text: "BPM Range"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Slider {
                    id: bpmSlider
                    Layout.fillWidth: true
                    from: 0
                    to: root.bpmSteps.length - 1
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: root.bpmStepIndex
                    onMoved: root.bpmStepIndex = Math.round(value)
                }
                // Tick marks under the slider's own track, one per step in
                // bpmSteps -- an approximation of the handle's real groove
                // (ignores the small end-padding QQC2's default Slider
                // reserves for the handle radius), close enough to read as
                // "here's where each stop is" without reimplementing the
                // control's own geometry.
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    Repeater {
                        model: root.bpmSteps.length
                        delegate: Rectangle {
                            required property int index
                            width: 2
                            height: 4
                            x: (parent.width - width) * index / (root.bpmSteps.length - 1)
                            color: index === root.bpmStepIndex ? Theme.accent : Theme.borderSubtle
                        }
                    }
                }
            }
            Label {
                text: root.bpmIgnored ? "Ignore" : "± " + root.bpmSteps[root.bpmStepIndex] + "%"
                font.family: Theme.dataFamily
                Layout.preferredWidth: 44
                horizontalAlignment: Text.AlignRight
            }
        }

        TextField {
            Layout.fillWidth: true
            placeholderText: "Filter results by title or artist…"
            enabled: root.hasAnchor
            // Bound both ways (unlike a plain onTextChanged-only field)
            // so clearAllFilters() setting root.resultQuery back to ""
            // actually clears what's shown here too, not just the
            // property backing it.
            text: root.resultQuery
            onTextChanged: root.resultQuery = text
        }
    }

    Label {
        visible: root.previewStatus.length > 0
        Layout.fillWidth: true
        Layout.leftMargin: 14 * Theme.iconScale
        Layout.rightMargin: 14 * Theme.iconScale
        Layout.bottomMargin: 6 * Theme.iconScale
        text: root.previewStatus
        color: Theme.info
        wrapMode: Text.WordWrap
    }

    ListView {
        id: candidatesListView
        // Not draggable when everything already fits.
        interactive: contentHeight > height
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: root.candidates
        ScrollBar.vertical: BigScrollBar {}

        delegate: Rectangle {
            id: candidateDelegate
            required property int index
            required property var modelData
            width: ListView.view.width
            height: 40
            // Hovering a specific wedge highlights that exact key;
            // hovering a legend row highlights every candidate at that
            // exact relation tier instead. Only one is ever active at
            // once in practice (you're hovering one or the other), but
            // either can drive the fade below.
            readonly property bool wheelHoverActive: root.wheelHoverCamelot.length > 0
                || root.wheelHoverRelationLabel.length > 0
            readonly property bool wheelHoverMatch:
                (root.wheelHoverCamelot.length > 0 && candidateDelegate.modelData.camelotLabel === root.wheelHoverCamelot)
                || (root.wheelHoverRelationLabel.length > 0
                    && candidateDelegate.modelData.keyRelation === root.wheelHoverRelationLabel)
            color: candidateDelegate.index % 2 === 0 ? Theme.rowEven : Theme.rowOdd
            // Fade everything that *isn't* the hovered key/relation
            // rather than outlining what is -- a border around every
            // match in a longer list read as visual noise; dimming the
            // rest makes the matches stand out by contrast instead.
            opacity: candidateDelegate.wheelHoverActive && !candidateDelegate.wheelHoverMatch ? 0.35 : 1.0
            Behavior on opacity { NumberAnimation { duration: Theme.shortTransitionDuration } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 6

                Rectangle {
                    Layout.preferredWidth: Theme.iconSizeSmall
                    Layout.preferredHeight: Theme.iconSizeSmall
                    color: Theme.groupBackground
                    Image {
                        anchors.fill: parent
                        visible: candidateDelegate.modelData.artworkPath.length > 0
                        source: candidateDelegate.modelData.artworkPath
                        fillMode: Image.PreserveAspectCrop
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label {
                        text: candidateDelegate.modelData.title
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: candidateDelegate.modelData.artist
                            + (candidateDelegate.modelData.keyRelation.length > 0
                                ? "  ·  " + candidateDelegate.modelData.keyRelation : "")
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }
                Label {
                    text: root.formatBpm(candidateDelegate.modelData.bpm)
                    font.family: Theme.dataFamily
                    color: Theme.textMuted
                }
                KeyBadge {
                    keyName: candidateDelegate.modelData.key
                    notation: root.keyNotation
                    additionalText: candidateDelegate.modelData.keyRelation
                    onKeyHovered: (number, isMinor, hovering) =>
                        root.wheelHoverCamelot = hovering ? (number + (isMinor ? "A" : "B")) : ""
                    onRelationHovered: (relationLabel, hovering) =>
                        root.wheelHoverRelationLabel = hovering ? relationLabel : ""
                }
                ToolButton {
                    text: "ⓘ"
                    Layout.preferredWidth: Theme.iconSizeSmall
                    ToolTip.visible: hovered
                    ToolTip.text: "Track details"
                    onClicked: candidateInfoPopup.showFor(candidateDelegate.modelData)
                }
                ToolButton {
                    text: "↑"
                    enabled: root.hasTarget
                    ToolTip.visible: hovered
                    // Always a move, never an insert: every candidate is
                    // already a member of This Playlist by construction
                    // now (see findCompatibleTracks() -- This Playlist is
                    // a filter here, not just the write target).
                    ToolTip.text: !root.hasTarget ? "Pick a playlist first"
                        : "Move before “" + root.anchorTitle + "” in " + root.targetPlaylistName
                    onClicked: root.previewNotSaved("moved before", candidateDelegate.modelData.title)
                }
                ToolButton {
                    text: "↓"
                    enabled: root.hasTarget
                    ToolTip.visible: hovered
                    ToolTip.text: !root.hasTarget ? "Pick a playlist first"
                        : "Move after “" + root.anchorTitle + "” in " + root.targetPlaylistName
                    onClicked: root.previewNotSaved("moved after", candidateDelegate.modelData.title)
                }
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: parent.width - 40
            visible: root.hasAnchor && candidatesListView.count === 0
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: "No matching tracks. Try a looser key range, widen the BPM range, or pick All as This "
                    + "Playlist to search the whole library."
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "Clear all filters"
                color: Theme.accent
                font.underline: true

                TapHandler {
                    cursorShape: Qt.PointingHandCursor
                    onTapped: root.clearAllFilters()
                }
            }
        }
    }

    // Lightweight track-details popup for a candidate row's (i) button --
    // for now at least, just the fields findCompatibleTracks() already
    // returns (no waveform/cue editing the way Browse's own trackInfoPopup
    // has, since that's about editing cues on a track you're about to
    // play, not one you're just considering adding).
    Popup {
        id: candidateInfoPopup
        modal: true
        focus: true
        x: (root.width - width) / 2
        y: (root.height - height) / 2
        width: 340

        property string infoTitle: ""
        property string infoArtist: ""
        property string infoArtworkPath: ""
        property string infoKey: ""
        property double infoBpm: 0
        property int infoRating: -1
        property double infoDurationSeconds: 0
        property var infoPlaylistNames: []
        property string infoKeyRelation: ""

        function showFor(candidate) {
            candidateInfoPopup.infoTitle = candidate.title;
            candidateInfoPopup.infoArtist = candidate.artist;
            candidateInfoPopup.infoArtworkPath = candidate.artworkPath;
            candidateInfoPopup.infoKey = candidate.key;
            candidateInfoPopup.infoKeyRelation = candidate.keyRelation;
            candidateInfoPopup.infoBpm = candidate.bpm;
            candidateInfoPopup.infoRating = candidate.rating;
            candidateInfoPopup.infoDurationSeconds = candidate.durationSeconds;
            candidateInfoPopup.infoPlaylistNames = candidate.playlistNames;
            candidateInfoPopup.open();
        }

        ColumnLayout {
            width: parent.width
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: Theme.iconSizeLarge
                    Layout.preferredHeight: Theme.iconSizeLarge
                    color: Theme.groupBackground
                    Image {
                        anchors.fill: parent
                        visible: candidateInfoPopup.infoArtworkPath.length > 0
                        source: candidateInfoPopup.infoArtworkPath
                        fillMode: Image.PreserveAspectCrop
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: candidateInfoPopup.infoTitle
                        font.bold: true
                        font.pointSize: Theme.fontMedium
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: candidateInfoPopup.infoArtist
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    RowLayout {
                        spacing: 8
                        KeyBadge {
                            keyName: candidateInfoPopup.infoKey
                            notation: root.keyNotation
                            additionalText: candidateInfoPopup.infoKeyRelation
                        }
                        Label {
                            text: root.formatBpm(candidateInfoPopup.infoBpm) + " BPM"
                            font.family: Theme.dataFamily
                            color: Theme.textMuted
                        }
                        Label {
                            visible: candidateInfoPopup.infoKeyRelation.length > 0
                            text: "· " + candidateInfoPopup.infoKeyRelation
                            color: Theme.textMuted
                        }
                    }
                }
            }

            RowLayout {
                spacing: 8
                Label { text: "Rating:"; color: Theme.textMuted }
                StarRating {
                    value: candidateInfoPopup.infoRating > 0 ? candidateInfoPopup.infoRating : 0
                    editable: false
                }
            }
            Label {
                text: "Duration: " + root.formatDuration(candidateInfoPopup.infoDurationSeconds)
                color: Theme.textMuted
            }

            Label {
                text: "Playlists"
                font.bold: true
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: candidateInfoPopup.infoPlaylistNames.length > 0
                    ? candidateInfoPopup.infoPlaylistNames.join("\n") : "Not in any playlist"
            }

            Button {
                text: "Close"
                Layout.alignment: Qt.AlignRight
                onClicked: candidateInfoPopup.close()
            }
        }
    }
}
