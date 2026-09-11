import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController
    required property var appSettingsController

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property bool hasOneLibrary: root.hasRekordbox && duplicatesController.hasOneLibrary(root.rekordboxPath)
    // OneLibrary selection is deliberately page-local, not persisted into
    // appSettingsController.preferredFormat -- that setting is shared
    // with ScanPage/LocalCuePage (see FormatToggle.qml's own comment),
    // neither of which know what to do with "onelibrary" as a value.
    // Rekordbox/Engine still persist exactly as before.
    property string localFormatOverride: ""
    readonly property string format: {
        if (root.localFormatOverride.length > 0) return root.localFormatOverride;
        var pref = appSettingsController.preferredFormat;
        if (pref === "engine" && hasEngine) return "engine";
        if (pref === "rekordbox" && hasRekordbox) return "rekordbox";
        return hasEngine ? "engine" : "rekordbox";
    }
    // A plain function, not a cached property: QML evaluates onFormatChanged
    // before a *dependent* property like a cached "path" has re-settled, so
    // reading a cached path here could see the previous format's value.
    // A function call is always evaluated fresh against the current format.
    function currentPath() {
        // OneLibrary shares rekordbox's own PIONEER root -- exportLibrary.db
        // lives alongside export.pdb there.
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }
    function formatLabel(format) { return FormatLabels.label(format); }

    // "3 hot cue(s), 1 memory cue(s)" -- mirrors sync_controller.cpp's
    // own summarizeCueCounts() (no loop/non-loop split available here:
    // duplicates_controller.cpp's TracksRole cue map doesn't carry an
    // isLoop flag the way SyncController's does).
    function cueSummary(track) {
        var hot = 0, memory = 0;
        var cues = track.cues || [];
        for (var i = 0; i < cues.length; i++) {
            (cues[i].kind === "hot" ? hot++ : memory++);
        }
        var parts = [];
        if (hot > 0) parts.push(hot + " hot cue(s)");
        if (memory > 0) parts.push(memory + " memory cue(s)");
        return parts.length > 0 ? parts.join(", ") : "no cues";
    }

    // The actual conflicting options, one line per copy -- what the
    // Conflict badge's tooltip shows instead of just "these disagree,"
    // so the choice is legible from the badge alone.
    function conflictDetail(tracks) {
        var lines = [];
        for (var i = 0; i < tracks.length; i++) {
            var t = tracks[i];
            var label = t.filePath && t.filePath.length > 0 ? t.filePath.split("/").pop() : ("Copy " + (i + 1));
            lines.push(label + ": " + root.cueSummary(t));
        }
        return lines.join("\n");
    }

    DuplicatesController {
        id: duplicatesController
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "dup"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.hasRekordbox ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    Component.onCompleted: duplicatesController.scan(root.format, root.currentPath())
    onFormatChanged: duplicatesController.scan(root.format, root.currentPath())

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
        // Opaque background override -- see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        // See ScanPage.qml's header for why this is needed: a ColumnLayout
        // child sized via anchors.fill doesn't feed its implicit size back
        // up, so without this the second row renders past the ToolBar's
        // bottom edge instead of the ToolBar growing to fit it.
        implicitHeight: headerLayout.implicitHeight + 20

        ColumnLayout {
            id: headerLayout
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                BackBreadcrumb {
                    stack: root.StackView.view
                    middleLabel: "Housekeeping"
                    title: "Match Duplicate Cues"
                    backEnabled: !duplicatesController.writing
                    onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                    onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
                }
                Item { Layout.fillWidth: true }
                LibrarySourceToggle {
                    current: root.format
                    hasRekordbox: root.hasRekordbox
                    hasEngine: root.hasEngine
                    hasOneLibrary: root.hasOneLibrary
                    onSourceRequested: (value) => {
                        if (value === "onelibrary") {
                            root.localFormatOverride = "onelibrary";
                        } else {
                            root.localFormatOverride = "";
                            root.appSettingsController.preferredFormat = value;
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Label {
                    text: plansListView.count + " duplicate group(s) need attention"
                    color: Theme.textMuted
                }
                Label {
                    visible: plansListView.count > 0
                    text: "(" + duplicatesController.totalWastedBytesHuman + " could be freed if each were on the stick once)"
                    color: Theme.textMuted
                }
                Label {
                    visible: duplicatesController.stagedCount > 0
                    text: duplicatesController.stagedCount + " staged, not saved yet"
                    color: Theme.warnText
                }
                Item { Layout.fillWidth: true }
                Button {
                    text: "Stage All Fixable"
                    enabled: !duplicatesController.busy && !duplicatesController.writing
                    ToolTip.visible: hovered
                    ToolTip.text: "Stage copying cues onto every clear duplicate. Conflicts are left for you."
                    onClicked: duplicatesController.applyAllUnambiguous()
                }
                Button {
                    text: "Undo Last Save"
                    visible: duplicatesController.canUndo
                    enabled: !duplicatesController.busy && !duplicatesController.writing
                    ToolTip.visible: hovered
                    ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                    onClicked: duplicatesController.undoLastOperation()
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: duplicatesController.errorMessage.length > 0
            text: duplicatesController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: duplicatesController.statusMessage.length > 0
            text: duplicatesController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // Spelled out because this page sits between two neighbours it is
        // easy to mistake it for: Clean Up Duplicates (same duplicate
        // groups, but it consolidates them onto one copy and drops the
        // rest from the catalog -- the audio itself goes later, under
        // Delete Orphaned Files, see CleanupPage.qml's own explanation)
        // and Sync Cue Points (same cue copying, but between two catalogs
        // rather than within one).
        Label {
            text: "Copies of the same track inside this one library. Pick which copy's cues are right and "
                + "they are copied onto the other copies. Every copy stays in the catalog. Clean Up "
                + "Duplicates is where the redundant ones are consolidated away, and the stick's "
                + "other libraries are left alone, which is what Sync Cue Points does instead."
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            Layout.bottomMargin: 4
        }

        ListView {
            // Room to scroll the last row clear of the Save overlay (bottom right).
            bottomMargin: 80
            id: plansListView
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: duplicatesController.plans
            spacing: 4

            ScrollBar.vertical: BigScrollBar {}

            delegate: Column {
                id: delegateRoot
                width: ListView.view.width
                spacing: 4

                required property int index
                required property string kind
                required property string filename
                required property string description
                required property bool actionable
                required property var tracks
                required property string wastedBytesDescription
                required property bool staged
                required property string stagedDescription

                property bool expanded: false

                ItemDelegate {
                    width: parent.width
                    hoverEnabled: true
                    onClicked: delegateRoot.expanded = !delegateRoot.expanded

                    ToolTip.visible: hovered
                    ToolTip.text: delegateRoot.filename

                    contentItem: ColumnLayout {
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: delegateRoot.tracks.length > 0
                                    ? (delegateRoot.tracks[0].title + " - " + delegateRoot.tracks[0].artist)
                                    : delegateRoot.filename
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.preferredWidth: 320
                            }
                            StatusBadge {
                                label: delegateRoot.kind === "unambiguous" ? "Fixable" : "Conflict"
                                badgeColor: delegateRoot.kind === "unambiguous" ? Theme.good : Theme.conflictText
                                tooltipText: delegateRoot.actionable
                                    ? "Kept: cues, copied onto the copies missing them.\nUntouched: files, playlists, everything else."
                                    : "These copies disagree, so nothing is copied automatically; decide per-track with the "
                                      + "Copy buttons below.\n\n" + root.conflictDetail(delegateRoot.tracks)
                            }
                            StatusBadge {
                                visible: delegateRoot.staged
                                label: "Staged"
                                badgeColor: Theme.warnText
                                tooltipText: delegateRoot.stagedDescription + "\n\nNot on the stick yet: press Save."
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                text: delegateRoot.staged ? "Unstage" : "Copy Cues"
                                visible: delegateRoot.actionable || delegateRoot.staged
                                enabled: !duplicatesController.busy && !duplicatesController.writing
                                ToolTip.visible: hovered
                                ToolTip.text: delegateRoot.staged
                                    ? "Take this group back out of the changes to save"
                                    : "Stage copying the one copy's cues onto every other copy of this track; Save writes it"
                                onClicked: delegateRoot.staged
                                    ? duplicatesController.unstage(delegateRoot.index)
                                    : duplicatesController.applyOne(delegateRoot.index)
                            }
                            Label {
                                text: delegateRoot.expanded ? "▾" : "▸"
                                font.pointSize: Theme.fontHuge
                                font.bold: true
                                color: Theme.textMuted
                            }
                        }
                        Label {
                            text: delegateRoot.description
                            color: Theme.textMuted
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            text: delegateRoot.wastedBytesDescription
                            color: Theme.textMuted
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                // Groups this duplicate set's copies visually -- without a
                // shared border, several stacked expanded groups (each a
                // Repeater of per-track Frames) read as one long undifferentiated
                // list rather than distinct sets of the same track.
                Rectangle {
                    width: parent.width
                    visible: delegateRoot.expanded
                    height: delegateRoot.expanded ? groupColumn.implicitHeight + 16 : 0
                    color: Theme.groupBackground
                    border.color: Theme.borderSubtle
                    radius: 4

                    ColumnLayout {
                        id: groupColumn
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                    // The "meta track" this whole frame is about -- the
                    // logical song every copy below is a physical instance
                    // of. Without this, the frame just contains a bare list
                    // of copies with nothing tying them together as one
                    // group.
                    Label {
                        Layout.fillWidth: true
                        text: (delegateRoot.tracks.length > 0
                            ? delegateRoot.tracks[0].title + " - " + delegateRoot.tracks[0].artist
                            : delegateRoot.filename)
                            + "  (" + delegateRoot.tracks.length + " copies)"
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Repeater {
                        model: delegateRoot.tracks
                        delegate: TrackWaveformCard {
                            required property var modelData
                            track: modelData
                            formatLabelText: root.formatLabel(root.format)
                            showPlaylists: true
                            actionButtonText: delegateRoot.kind === "conflict" ? "Use These" : ""
                            actionButtonTooltip: delegateRoot.kind === "conflict"
                                ? "Stage copying this copy's cue points onto the other copies; Save writes it" : ""
                            actionButtonEnabled: modelData.cues.length > 0 && !duplicatesController.busy
                                && !duplicatesController.writing
                            onActionTriggered: duplicatesController.copyFromTrack(delegateRoot.index, modelData.sourceId)
                            playbackController: root.playbackController
                            playbackPath: root.currentPath()
                        }
                    }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: plansListView.count === 0 && !duplicatesController.busy
                text: "No duplicate tracks needing attention."
                color: Theme.textMuted
            }
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: duplicatesController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: duplicatesController.busy
        current: duplicatesController.scanCurrent
        total: duplicatesController.scanTotal
        unitName: "tracks"
        cancellable: duplicatesController.scanCancellable
        onCancelRequested: duplicatesController.cancelScan()
        // duplicatesController.scanLabel tracks the real current phase
        // ("Scanning rekordbox tracks", then "Finding duplicates..." for
        // the grouping pass that used to leave this stuck at a frozen
        // 100% with no explanation) -- falls back to a generic label only
        // for the brief window before the first phase has reported in.
        label: duplicatesController.scanLabel.length > 0 ? duplicatesController.scanLabel : "Scanning for duplicates..."
    }
}
