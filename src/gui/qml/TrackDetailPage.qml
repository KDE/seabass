import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Rich per-track view -- pushed from ScanPage.qml's own row click (see
// its trackDetailRequested signal). Waveform with cues/loops (same
// WaveformView every other page uses, just given more room here than a
// row backdrop), plus how this track transitions to/from its neighbors in
// whichever list it was opened from: harmonic key relationship (via
// scanController.keyRelation(), domain::classifyKeyRelation() under the
// hood) and BPM difference either side.
Page {
    id: root
    required property var scanController
    required property int trackIndex
    required property string format
    required property string libraryPath
    required property var playbackController
    required property var appSettingsController

    // trackIndex is into scanController's *currently displayed*
    // (filtered/sorted) list -- the same instance ScanPage itself is
    // showing, passed through by reference rather than re-scanned, so
    // prev/next here match exactly what the user was looking at when
    // they clicked this row.
    property var track: root.scanController.trackAt(root.trackIndex)
    readonly property bool hasPrev: root.trackIndex > 0
    readonly property bool hasNext: root.trackIndex < root.scanController.trackCount() - 1
    property var prevTrack: root.hasPrev ? root.scanController.trackAt(root.trackIndex - 1) : null
    property var nextTrack: root.hasNext ? root.scanController.trackAt(root.trackIndex + 1) : null

    // Mirrors ScanPage.qml's own formatDuration()/formatBpm() --
    // established convention in this codebase (LibraryConsistencyPage
    // does the same) rather than a new shared utility for two three-line
    // functions.
    function formatDuration(seconds) {
        var total = Math.round(seconds);
        var m = Math.floor(total / 60);
        var s = total % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }
    function formatBpm(bpm) {
        if (bpm <= 0) {
            return "--";
        }
        var oneDecimal = bpm.toFixed(1);
        return oneDecimal.endsWith(".0") ? oneDecimal.slice(0, -2) : oneDecimal;
    }

    // BPM diff between this track and a neighbor, both directions read
    // naturally: "128 → 124 BPM (-3.1%)" is "what happens if I mix into
    // that track from this one."
    function bpmDiffText(fromBpm, toBpm) {
        if (!fromBpm || !toBpm || fromBpm <= 0 || toBpm <= 0) {
            return "";
        }
        var diffPct = ((toBpm - fromBpm) / fromBpm) * 100;
        var sign = diffPct >= 0 ? "+" : "";
        return root.formatBpm(fromBpm) + " → " + root.formatBpm(toBpm) + " BPM (" + sign + diffPct.toFixed(1) + "%)";
    }

    // Maps scanController.keyRelation()'s machine-readable tag to this
    // page's own wording/color -- "unrelated" reads as "Dissonant
    // transition" here specifically (a DJ-facing warning about *this*
    // pairing), not domain::keyRelationLabel()'s own neutral "Unrelated
    // key" wording that findCompatibleTracks()/MatchingPage use
    // for a plain descriptive list instead. Empty for "unknown" (either
    // key didn't parse) -- nothing honest to say, same "don't fabricate
    // it" stance the key-parsing code itself already takes.
    //
    // keyA/keyB must be passed in transition order (the key you'd be
    // mixing FROM, then the key you'd be mixing INTO) -- "adjacentup"/
    // "adjacentdown" depend on that order, same as
    // domain::classifyKeyRelation() itself; every other tag is symmetric.
    function relationDisplay(keyA, keyB) {
        if (!keyA || !keyB || keyA.length === 0 || keyB.length === 0) {
            return {label: "", color: Theme.textMuted};
        }
        var r = root.scanController.keyRelation(keyA, keyB);
        switch (r.relation) {
        case "same":
            return {label: "Same key", color: Theme.good};
        case "relative":
            return {label: "Relative major/minor", color: Theme.good};
        case "adjacentup":
            return {label: "Energy Boost ↑", color: Theme.good};
        case "adjacentdown":
            return {label: "Energy Drop ↓", color: Theme.good};
        case "energymix":
            return {label: "Energy mix", color: Theme.accent};
        case "unrelated":
            return {label: "Dissonant transition", color: Theme.danger};
        default:
            return {label: "", color: Theme.textMuted};
        }
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it,
        // 4px under Breeze and 6 under the default style, and that is
        // exactly how far right of the body the breadcrumb used to sit.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        // Opaque background override, see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Browse Library"
                title: root.track ? (root.track.title + " - " + root.track.artist) : "Track"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Rectangle {
                Layout.preferredWidth: Theme.iconSizeLarge * 2
                Layout.preferredHeight: Theme.iconSizeLarge * 2
                color: Theme.surface
                Image {
                    anchors.fill: parent
                    visible: root.track && root.track.artworkPath.length > 0
                    source: root.track ? root.track.artworkPath : ""
                    fillMode: Image.PreserveAspectCrop
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                PageTitle {
                    level: "page"
                    text: root.track ? root.track.title : ""
                    Layout.fillWidth: true
                }
                Label {
                    text: root.track ? root.track.artist : ""
                    color: Theme.textMuted
                }
                RowLayout {
                    spacing: 10
                    KeyBadge {
                        keyName: root.track ? root.track.key : ""
                        notation: root.appSettingsController.keyNotation
                    }
                    Label { text: root.track ? (root.formatBpm(root.track.bpm) + " BPM") : "" }
                    Label {
                        text: root.track ? root.formatDuration(root.track.durationSeconds) : ""
                        color: Theme.textMuted
                    }
                }
            }

            Button {
                text: "▶ Play"
                enabled: root.track && root.track.filePath.length > 0
                ToolTip.visible: hovered
                ToolTip.text: root.track && root.track.filePath.length === 0
                    ? "Streaming track - no local file, can't be played."
                    : "Play this track"
                onClicked: root.playbackController.load(root.format, root.libraryPath, root.track.sourceId,
                    root.track.filePath, root.track.title, root.track.artist, root.track.artworkPath, root.track.cues)
            }
        }

        WaveformView {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            waveformData: root.track
                ? root.playbackController.waveformFor(root.format, root.libraryPath, root.track.sourceId) : []
            format: root.format
            cueData: root.track ? root.track.cues : []
            trackDurationMs: root.track ? root.track.durationSeconds * 1000 : 0
        }
        CueFallbackNotice {
            cues: root.track ? root.track.cues : []
            durationMs: root.track ? root.track.durationSeconds * 1000 : 0
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            component TransitionPanel: Frame {
                id: panel
                Layout.fillWidth: true
                Layout.fillHeight: true

                property string headingText: ""
                property var neighborTrack: null
                // "before" (neighbor plays, then this track) or "after"
                // (this track plays, then neighbor) -- decides which way
                // round the BPM-diff arrow reads, and (since Energy
                // Boost/Drop is directional) which way the key relation
                // reads too.
                property string direction: "after"

                // Referenced by id (panel.xxx) throughout below, not
                // parent-chain lookups -- these children sit several
                // levels deep (ColumnLayout > RowLayout > Label), and a
                // parent.parent.xxx style reference silently breaks (or
                // worse, silently resolves to the wrong ancestor) the
                // moment nesting depth changes.
                readonly property var relation: panel.neighborTrack && root.track
                    ? (panel.direction === "before"
                        ? root.relationDisplay(panel.neighborTrack.key, root.track.key)
                        : root.relationDisplay(root.track.key, panel.neighborTrack.key))
                    : null
                readonly property string bpmDiff: panel.neighborTrack && root.track
                    ? (panel.direction === "before"
                        ? root.bpmDiffText(panel.neighborTrack.bpm, root.track.bpm)
                        : root.bpmDiffText(root.track.bpm, panel.neighborTrack.bpm))
                    : ""

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    visible: panel.neighborTrack !== null

                    Subtitle { text: panel.headingText }
                    RowLayout {
                        spacing: 10
                        KeyBadge {
                            keyName: panel.neighborTrack ? panel.neighborTrack.key : ""
                            notation: root.appSettingsController.keyNotation
                        }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: panel.neighborTrack
                                ? (panel.neighborTrack.title + " - " + panel.neighborTrack.artist) : ""
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: panel.relation && panel.relation.label.length > 0
                        text: panel.relation ? panel.relation.label : ""
                        color: panel.relation ? panel.relation.color : Theme.textMuted
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: panel.bpmDiff.length > 0
                        text: panel.bpmDiff
                        color: Theme.textMuted
                    }
                    Item { Layout.fillHeight: true }
                }

                Label {
                    anchors.centerIn: parent
                    visible: panel.neighborTrack === null
                    text: "(nothing this side)"
                    color: Theme.textMuted
                }
            }

            TransitionPanel {
                headingText: "◀ Previous track"
                neighborTrack: root.prevTrack
                direction: "before"
            }
            TransitionPanel {
                headingText: "Next track ▶"
                neighborTrack: root.nextTrack
                direction: "after"
            }
        }
    }
}
