import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property bool hasOneLibrary: root.hasRekordbox && localCueController.hasOneLibrary(root.rekordboxPath)
    // OneLibrary selection is page-local, not persisted into
    // appSettingsController.preferredFormat -- same reasoning as
    // DuplicatesPage.qml's own localFormatOverride (that setting is
    // shared with ScanPage, which doesn't know what to do with
    // "onelibrary" as a value).
    property string localFormatOverride: ""
    readonly property string format: {
        if (root.localFormatOverride.length > 0) return root.localFormatOverride;
        var pref = appSettingsController.preferredFormat;
        if (pref === "engine" && hasEngine) return "engine";
        if (pref === "rekordbox" && hasRekordbox) return "rekordbox";
        return hasEngine ? "engine" : "rekordbox";
    }
    function currentPath() {
        // OneLibrary shares rekordbox's own PIONEER root -- exportLibrary.db
        // lives alongside export.pdb there.
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }

    property var snapshots: []
    function refreshSnapshots() { root.snapshots = localCueController.listSnapshots(); }

    function friendlyTimestamp(iso) {
        // "2026-08-27T14:32:05Z" -> Date
        var d = new Date(iso);
        return isNaN(d.getTime()) ? iso : d.toLocaleString(Qt.locale(), "d MMM yyyy, HH:mm:ss");
    }

    LocalCueController {
        id: localCueController
    }

    // Edit mode for this library: session, floating Save, leave guard.
    EditSessionHost {
        id: editHost
        feature: "localcue"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.hasRekordbox ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    // Silent (reportFeedback defaults to false) -- only for automatic
    // calls (page load, format switch) that the user didn't directly ask
    // for. The "Re-Analyze Latest" button calls analyzeRestore() directly
    // with reportFeedback: true instead of this, precisely so it gets one.
    function refresh() { localCueController.analyzeRestore(root.format, root.currentPath()); }

    readonly property bool hasAnyStick: root.hasRekordbox || root.hasEngine

    Component.onCompleted: {
        // Opened generally (from the Backups block on Home, with no
        // stick's paths given): nothing to scan yet, and scanning an
        // empty path would just surface a confusing read error for a
        // page that's mainly here to show Backup History, which needs
        // no stick at all. Backup Now / Merge Cues below hide themselves
        // the same way.
        if (root.hasAnyStick) {
            refresh();
        }
        refreshSnapshots();
    }
    onFormatChanged: if (root.hasAnyStick) refresh()

    // Snapshot list/description/delete are synchronous calls with no
    // signal of their own, so refresh whenever a background operation
    // (backup, restore-analysis) that might have changed them finishes.
    Connections {
        target: localCueController
        function onBusyChanged() {
            if (!localCueController.busy) {
                root.refreshSnapshots();
            }
        }
    }

    // The only place a status/error message shows up on this page (see
    // the removed inline Label further down) -- stays up until you
    // dismiss it, since a "Restore From Here" click (up in Backup
    // History) or "Backup Now" result was easy to miss as quiet inline
    // text in a different section than whichever button was just
    // clicked. Wired to the dedicated actionFeedback signal, not a
    // Q_PROPERTY's change notification -- a property-change Connections
    // handler only fires when the new value differs from the old one, so
    // two outcomes in a row with identical text (e.g. "Restore From
    // Here" on two different snapshots that both turn out to offer
    // nothing new) would silently skip the second popup. actionFeedback
    // fires on every emission, no exceptions, which is the actual
    // guarantee behind "either succeed or don't do anything, but either
    // way give feedback."
    MessagePopup { id: messagePopup }
    Connections {
        target: localCueController
        function onActionFeedback(message, isError) {
            messagePopup.show(message, isError);
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
        // Opaque background override -- see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Backups"
                title: "Local Cue Backup"
                backEnabled: !localCueController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
        }
    }

    MessageDialog {
        id: confirmDialog
        property string sourceDescription: ""
        severity: SeabassDialog.Question
        title: "Stage merging cues onto the stick?"
        headline: "Add new cues from this computer's backup onto " + restoreListView.count + " track(s): "
            + "any cue already on the stick is kept exactly as it is, never overwritten."
            + (confirmDialog.sourceDescription.length > 0
                ? "\nSource: " + confirmDialog.sourceDescription : "")
        detailText: "Nothing is written yet: this stages the merges, and Save writes them. The stick is "
            + "backed up first; afterwards \"Undo Last Save\" reverts every file it touched."
        acceptText: "Stage Merge"
        onAccepted: localCueController.applyRestore()
    }

    MessageDialog {
        id: confirmDeleteSnapshotDialog
        property int targetId: -1
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete This Backup?"
        headline: "This permanently deletes this one backup snapshot from this computer."
        detailText: "It never touches the stick, and never touches any other backup."
        acceptText: "Delete"
        onAccepted: {
            localCueController.deleteSnapshot(targetId);
            root.refreshSnapshots();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 16

        Label {
            visible: !root.hasAnyStick
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "Backup History below covers every stick you've ever backed up here. "
                + "Open this from a specific stick's Backups page to back it up now or merge cues onto it."
        }

        // No inline error/status Label here -- the MessagePopup declared above
        // (fired from the same statusMessage/errorMessage changes) is the
        // only place either shows up now; having both said the same
        // thing twice on screen at once was the actual complaint.

        Frame {
            Layout.fillWidth: true
            visible: root.hasAnyStick
            ColumnLayout {
                anchors.fill: parent
                spacing: 8
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Label { text: "Back Up to This Computer"; font.bold: true }
                        Label {
                            text: {
                                var present = [];
                                if (root.hasRekordbox) present.push("DeviceLibrary");
                                if (root.hasEngine) present.push("Engine");
                                if (root.hasOneLibrary) present.push("OneLibrary");
                                var scope = present.length > 1
                                    ? "this stick's cues (" + present.join(", ") + ")"
                                    : "this stick's cues";
                                return "Copies " + scope + " to a local backup. Never touches the stick, "
                                    + "no confirmation needed.";
                            }
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                    TextField {
                        id: backupDescriptionField
                        Layout.preferredWidth: 220
                        placeholderText: "Note (e.g. \"before Berlin gig\")..."
                    }
                    Button {
                        text: "Backup Now"
                        enabled: !localCueController.busy
                        onClicked: {
                            localCueController.backupToComputer(root.stickLabel, backupDescriptionField.text,
                                root.rekordboxPath, root.enginePath,
                                root.hasOneLibrary ? root.rekordboxPath : "");
                            backupDescriptionField.text = "";
                        }
                    }
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: root.height * 0.32
            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label { text: "Backup History"; font.bold: true }
                Label {
                    text: "Each backup here is frozen at the moment it was made: restoring from one "
                        + "always uses exactly that snapshot, even if newer backups exist."
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                ListView {
                    id: snapshotListView
                    // Not draggable when everything already fits.
                    interactive: contentHeight > height
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.snapshots
                    spacing: 2

                    delegate: ItemDelegate {
                        id: snapshotDelegate
                        width: ListView.view.width
                        height: 40
                        hoverEnabled: true

                        required property var modelData

                        contentItem: RowLayout {
                            spacing: 8
                            Label {
                                text: FormatLabels.label(snapshotDelegate.modelData.sourceFormat)
                                color: Theme.accent
                                font.bold: true
                                Layout.preferredWidth: 104
                            }
                            Label {
                                // Just a label, kept for reference -- it plays
                                // no part in deciding what this snapshot can
                                // be restored onto (see the class comment on
                                // LocalCueStore: restoring across sticks is
                                // deliberate, not a bug).
                                text: root.friendlyTimestamp(snapshotDelegate.modelData.createdAt)
                                    + "  ·  " + snapshotDelegate.modelData.stickLabel
                                    + "  ·  " + snapshotDelegate.modelData.trackCount + " track(s), "
                                    + snapshotDelegate.modelData.cueCount + " cue(s)"
                                color: Theme.textMuted
                                Layout.preferredWidth: 420
                                elide: Text.ElideRight
                            }
                            TextField {
                                id: snapshotDescriptionField
                                Layout.fillWidth: true
                                placeholderText: "Click to add a note..."
                                text: snapshotDelegate.modelData.description
                                background: Rectangle {
                                    radius: 4
                                    color: snapshotDescriptionField.activeFocus ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.08) : "transparent"
                                    border.width: snapshotDescriptionField.activeFocus ? 1 : 0
                                    border.color: Theme.accent
                                }
                                onEditingFinished: {
                                    localCueController.setSnapshotDescription(snapshotDelegate.modelData.id, text);
                                    root.refreshSnapshots();
                                }
                            }
                            Button {
                                text: "Restore From Here"
                                enabled: !localCueController.busy
                                ToolTip.visible: hovered
                                ToolTip.text: "Match this backup against the " + FormatLabels.label(root.format)
                                    + " side of the stick."
                                onClicked: {
                                    confirmDialog.sourceDescription = snapshotDescriptionField.text.length > 0
                                        ? snapshotDescriptionField.text
                                        : root.friendlyTimestamp(snapshotDelegate.modelData.createdAt);
                                    localCueController.analyzeSnapshotRestore(snapshotDelegate.modelData.id, root.format, root.currentPath());
                                }
                            }
                            ToolButton {
                                text: "🗑"
                                font.family: "Noto Sans Symbols2"
                                opacity: 0.55
                                Layout.preferredWidth: Theme.iconSizeSmall
                                ToolTip.visible: hovered
                                ToolTip.text: "Delete this backup permanently"
                                onClicked: {
                                    confirmDeleteSnapshotDialog.targetId = snapshotDelegate.modelData.id;
                                    confirmDeleteSnapshotDialog.open();
                                }
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: snapshotListView.count === 0
                        text: "No backups yet. Click \"Backup Now\" above to create the first one."
                        color: Theme.textMuted
                    }
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.hasAnyStick
            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    ColumnLayout {
                        spacing: 2
                        Layout.fillWidth: true
                        Label { text: "Merge Cues from This Computer"; font.bold: true }
                        Label {
                            text: "Stick tracks: " + localCueController.stickTrackCount
                                + "   Backed up on this computer: " + localCueController.localTrackCount
                                + "   Have new cues to add: " + restoreListView.count
                            color: Theme.textMuted
                        }
                    }
                    // Scoped to just this section -- unlike every other
                    // page's header-level toggle, "Back Up to This
                    // Computer" above always backs up both formats
                    // together in one snapshot regardless of it, so a
                    // page-wide toggle up in the header implied a scope
                    // it didn't actually have. Merging is genuinely
                    // format-specific (one side of the stick at a time),
                    // so it lives right where it applies instead.
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
                    Button {
                        text: "Re-Analyze Latest"
                        enabled: !localCueController.busy
                        ToolTip.visible: hovered
                        ToolTip.text: "Match the stick against the current merged backup state (not one specific snapshot)"
                        onClicked: {
                            confirmDialog.sourceDescription = "";
                            // Not refresh(): this is a direct user click, so
                            // unlike refresh()'s own silent uses (page load,
                            // format switch), the outcome must be reported --
                            // see analyzeRestore()'s own doc comment.
                            localCueController.analyzeRestore(root.format, root.currentPath(), true);
                        }
                    }
                    Label {
                        visible: localCueController.stagedCount > 0
                        text: localCueController.stagedCount + " staged, not saved yet"
                        color: Theme.warnText
                    }
                    Button {
                        text: "Stage Merge Onto " + restoreListView.count + " Track(s)"
                        enabled: !localCueController.busy && !localCueController.writing
                            && restoreListView.count > localCueController.stagedCount
                        onClicked: confirmDialog.open()
                    }
                    Button {
                        text: "Undo Last Save"
                        visible: localCueController.canUndo
                        enabled: !localCueController.busy && !localCueController.writing
                        ToolTip.visible: hovered
                        ToolTip.text: "Revert the last save: restores every file it touched to what it was before"
                        onClicked: localCueController.undoLastOperation()
                    }
                }

                ListView {
                    id: restoreListView
                    interactive: contentHeight > height
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: localCueController.restoreCandidates
                    spacing: 2

                    delegate: ItemDelegate {
                        id: candidateRow
                        width: ListView.view.width
                        hoverEnabled: true

                        required property int index
                        required property string filename
                        required property string title
                        required property string artist
                        required property string description
                        required property bool staged

                        contentItem: RowLayout {
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    text: candidateRow.title.length > 0 ? (candidateRow.title + " - " + candidateRow.artist) : candidateRow.filename
                                    font.bold: true
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: candidateRow.description + " new cue(s) would be added"
                                    color: Theme.textMuted
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                }
                            }
                            StatusBadge {
                                visible: candidateRow.staged
                                label: "Staged"
                                badgeColor: Theme.warnText
                                tooltipText: "Not on the stick yet: press Save."
                            }
                            ToolButton {
                                visible: candidateRow.staged
                                text: "Unstage"
                                enabled: !localCueController.writing
                                onClicked: localCueController.unstage(candidateRow.index)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: restoreListView.count === 0 && !localCueController.busy
                        text: "Nothing to merge: either the stick already has every cue this backup offers,\nor none of its tracks match one backed up on this computer."
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textMuted
                    }
                }
            }
        }
    }

    // A cancelled analyze takes the user back to where they came from.
    Connections {
        target: localCueController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: localCueController.busy
        current: localCueController.scanCurrent
        total: localCueController.scanTotal
        label: "Analyzing backups..."
        cancellable: localCueController.scanCancellable
        onCancelRequested: localCueController.cancelScan()
    }
}
