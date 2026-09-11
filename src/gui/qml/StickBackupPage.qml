import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Per-stick "Full Stick Backup" (experimental, see
// docs/experimental-features.md and docs/stick-backup-plan.md). Backs the
// whole stick up into one browsable .zip on this computer and keeps it
// current incrementally; restore hands off to RestoreStickBackupPage
// with this stick preselected.
//
// `controller` is untyped and required, like FormatUsbPage's: a plain JS
// object stands in for the whole StickBackupController in
// tests/qml/tst_StickBackupPage.qml.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController
    required property var controller
    // "" or the DJ software the global guard currently sees; the page
    // shows the refusal banner from this so it reacts within the guard's
    // 3 s poll, not only when the controller last refreshed.
    property string conflictingSoftware: ""
    signal restoreRequested(string stickLabel, string stickRoot, string archivePath)

    readonly property var lastBackup: controller.lastBackup || ({})
    readonly property var since: controller.sinceLastBackup || ({})
    readonly property var dead: controller.deadSpace || ({})
    readonly property bool hasBackup: lastBackup.exists === true
    readonly property string blockedBy: root.conflictingSoftware.length > 0 ? root.conflictingSoftware : (controller.blockedBy || "")
    readonly property bool canBackUp: !controller.busy && !controller.pendingCancelDecision && root.blockedBy.length === 0
        && since.enoughFreeSpace !== false && !(lastBackup.error && lastBackup.error.length > 0)

    Component.onCompleted: {
        if (controller.configure) {
            controller.configure(root.stickLabel, root.rekordboxPath, root.enginePath,
                                 root.appSettingsController.stickBackupDirectory);
        }
    }

    function friendlyTimestamp(iso) {
        if (!iso) return "";
        var d = new Date(iso);
        return isNaN(d.getTime()) ? iso : d.toLocaleString(Qt.locale(), "d MMM yyyy, HH:mm");
    }

    function statusBadge(status) {
        switch (status) {
        case "complete": return {label: "VERIFIED", color: Theme.good, tip: "Every entry was checked against its checksum when this backup was written."};
        case "partial-cancelled": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "The last backup was cancelled and kept; the next run continues from there."};
        case "partial-conflict": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "The database was not captured -- Engine DJ or rekordbox was running, or the database kept changing."};
        case "partial-db-too-large": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "The Engine database is too large for Seabass to back up safely; back it up by hand."};
        default: return {label: "", color: Theme.textMuted, tip: ""};
        }
    }

    function phaseLabel(phase) {
        switch (phase) {
        case "scanning": return "Scanning stick";
        case "reading": return "Reading stick";
        case "database": return "Capturing database";
        case "writing": return "Writing index";
        case "verifying": return "Verifying";
        case "compacting": return "Compacting";
        default: return "Working";
        }
    }

    MessagePopup { id: messagePopup }
    // Another instance is editing one of the libraries this operation
    // would write; "Remove Lock" re-runs the refused action.
    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: {
            EditSessionRegistry.removeLock(lockedDialog.libraryId);
            root.controller.retryLockedAction();
        }
    }
    Connections {
        // A plain JS stand-in (tests) has no signals and is not a QObject.
        target: ("objectName" in root.controller) ? root.controller : null
        ignoreUnknownSignals: true
        function onLockRefused(holder, libraryId) {
            lockedDialog.openFor(libraryId, holder);
        }
        function onActionFeedback(message, isError) {
            messagePopup.show(message, isError);
        }
        function onPendingCancelDecisionChanged() {
            if (root.controller.pendingCancelDecision) {
                cancelDecisionDialog.open();
            }
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
        bottomPadding: 0
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Backups"
                title: "Full Stick Backup"
                backEnabled: !root.controller.busy
                backDisabledTooltip: "Wait for the backup to finish (or cancel it) before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            ExperimentalBadge {}
            Item { Layout.fillWidth: true }
            BusyIndicator { running: root.controller.previewing === true; visible: running; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    // ---- Cancel decision: keep for later or discard ----
    SeabassDialog {
        id: cancelDecisionDialog
        objectName: "cancelDecisionDialog"
        severity: SeabassDialog.Question
        closePolicy: Popup.NoAutoClose
        title: "Backup stopped"
        headline: "The files copied so far are complete. Keep them, and the next backup continues from "
            + "here -- or discard them"
            + (root.hasBackup ? ", which leaves the previous backup exactly as it was."
                              : " and remove the partial backup file.")
        detailText: "Nothing on the stick is affected either way."
        footer: DialogButtonBox {
            Button {
                objectName: "keepPartialButton"
                text: "Keep for Later"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                objectName: "discardPartialButton"
                text: root.hasBackup ? "Discard This Update" : "Delete Partial Backup"
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    cancelDecisionDialog.close();
                    root.controller.discardPartial();
                }
            }
        }
        onAccepted: root.controller.keepPartial()
    }

    // ---- Compaction ----
    SeabassDialog {
        id: compactDialog
        objectName: "compactDialog"
        property var preflight: ({})
        severity: SeabassDialog.Question
        title: "Compact " + root.stickLabel + ".zip?"
        headline: "Rewrites the backup without the space left behind by replaced and removed files."
        detailText: "Every file is checked against its checksum on the way."
        footer: DialogButtonBox {
            Button {
                objectName: "compactAcceptButton"
                text: "Compact"
                enabled: compactDialog.preflight.enoughFreeSpace === true && compactDialog.preflight.deadBytes > 0
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button { text: "Cancel"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        }
        onAccepted: root.controller.compact()
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 4
                Label { text: "Reclaims"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                Label {
                    font.family: Theme.dataFamily
                    text: Theme.humanBytes(compactDialog.preflight.deadBytes) + "  ("
                        + Math.round((compactDialog.preflight.ratio || 0) * 100) + "%)"
                }
                Label { text: "Needs free space"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                Label {
                    font.family: Theme.dataFamily
                    color: compactDialog.preflight.enoughFreeSpace === true ? Theme.text : Theme.danger
                    text: Theme.humanBytes(compactDialog.preflight.requiredFreeBytes) + "  ·  "
                        + Theme.humanBytes(compactDialog.preflight.availableFreeBytes) + " available"
                }
            }
            Rectangle {
                Layout.fillWidth: true
                visible: compactDialog.preflight.enoughFreeSpace === false
                implicitHeight: notEnoughLabel.implicitHeight + 16
                radius: 4
                color: Theme.warnBg
                border.color: Theme.warnBorder
                Label {
                    id: notEnoughLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: Text.WordWrap
                    color: Theme.warnText
                    text: "Not enough free space on this drive. Compacting writes a fresh copy of the backup before "
                        + "removing the old one, so it needs room for both. Free some space and try again -- the "
                        + "backup is complete and usable as it is."
                }
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
                text: "The current backup stays intact until the new one is complete. The stick isn't needed for this."
            }
        }
    }

    PageScrollView {
        anchors.fill: parent
        anchors.margins: 16

        ColumnLayout {
            width: parent.width
            spacing: 16

            // ---- Refusal / running banners ----
            Rectangle {
                Layout.fillWidth: true
                visible: root.blockedBy.length > 0
                implicitHeight: blockedLabel.implicitHeight + 16
                radius: 4
                color: Theme.dangerBg
                border.color: Theme.dangerBorder
                Label {
                    id: blockedLabel
                    objectName: "blockedBanner"
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: Text.WordWrap
                    font.bold: true
                    color: Theme.dangerText
                    text: root.blockedBy + " appears to be running: backups are refused until it's closed, so the "
                        + "database can't change while it's being read."
                }
            }
            StickWriteWarning {
                visible: root.controller.backingUp === true
                text: "Backing up. Do not remove the stick until this finishes."
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.errorMessage.length > 0
                wrapMode: Text.WordWrap
                color: Theme.danger
                text: root.controller.errorMessage
            }

            // ---- Status ----
            Frame {
                Layout.fillWidth: true
                GridLayout {
                    anchors.fill: parent
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 6
                    Label { text: "Last backup"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            objectName: "lastBackupLabel"
                            text: root.hasBackup ? root.friendlyTimestamp(root.lastBackup.createdAt) : "No backup of this stick yet."
                            color: root.hasBackup ? Theme.text : Theme.textMuted
                        }
                        Label { visible: root.hasBackup; text: "·"; color: Theme.textMuted }
                        Label { visible: root.hasBackup; font.family: Theme.dataFamily; text: Theme.humanBytes(root.lastBackup.archiveBytes) }
                        Label { visible: root.hasBackup; text: "·"; color: Theme.textMuted }
                        Label { visible: root.hasBackup; font.family: Theme.dataFamily; text: (root.lastBackup.entries || 0) + " files" }
                        StatusBadge {
                            visible: root.hasBackup && root.statusBadge(root.lastBackup.status).label.length > 0
                            label: root.statusBadge(root.lastBackup.status).label
                            badgeColor: root.statusBadge(root.lastBackup.status).color
                            tooltipText: root.statusBadge(root.lastBackup.status).tip
                        }
                        StatusBadge {
                            visible: root.lastBackup.identifierMismatch === true
                            label: "DIFFERENT STICK"
                            badgeColor: Theme.warnIcon
                            tooltipText: "This backup file was made from another stick that had the same name"
                                + (root.lastBackup.label ? " (" + root.lastBackup.label + ")" : "")
                                + ". Backing up continues it with this stick's contents."
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Label { text: "Archive"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            font.family: Theme.dataFamily
                            color: root.hasBackup ? Theme.text : Theme.textMuted
                            text: (root.hasBackup ? "" : "Will be created at ") + root.controller.archivePath
                        }
                        Button {
                            text: "Open Folder"
                            visible: root.hasBackup
                            onClicked: root.controller.openArchiveFolder()
                        }
                    }
                }
            }

            // ---- Back up (idle) / progress (running) ----
            Frame {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: root.controller.backingUp === true ? "Backing up " + root.stickLabel : "Back Up to This Computer"
                                font.bold: true
                            }
                            Label {
                                visible: root.controller.backingUp !== true
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textMuted
                                text: "Reads the whole stick into one file on this computer. Never writes to the stick."
                            }
                        }
                        ColumnLayout {
                            spacing: 4
                            Layout.alignment: Qt.AlignTop
                            Button {
                                objectName: "backUpButton"
                                visible: root.controller.busy !== true
                                text: "Back Up Now"
                                highlighted: true
                                enabled: root.canBackUp
                                onClicked: root.controller.backUp()
                            }
                            Button {
                                objectName: "cancelButton"
                                visible: root.controller.busy === true && root.controller.activity !== "decide"
                                text: "Cancel"
                                onClicked: root.controller.cancel()
                            }
                            Label {
                                visible: root.controller.backingUp === true
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: "Stops after the current file."
                            }
                            Label {
                                visible: root.controller.busy !== true && root.blockedBy.length > 0
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: "Close " + root.blockedBy + " to continue."
                            }
                        }
                    }

                    // Idle: what the next run would do.
                    Flow {
                        visible: root.controller.busy !== true && !root.controller.previewing && root.since.added !== undefined
                        Layout.fillWidth: true
                        spacing: 8
                        Label { text: root.hasBackup ? "Since last backup" : "First backup"; color: Theme.textMuted }
                        Label {
                            objectName: "sinceLabel"
                            font.family: Theme.dataFamily
                            text: root.hasBackup
                                ? (root.since.added + " added · " + root.since.changed + " changed · " + root.since.removed + " removed"
                                   + (root.since.databaseChanged ? " · database changed" : ""))
                                : ("reads everything: " + Theme.humanBytes(root.since.stickBytes) + ", " + root.since.entriesOnStick + " entries")
                        }
                        Label { text: "→"; color: Theme.textMuted }
                        Label {
                            font.family: Theme.dataFamily
                            text: "about " + Theme.humanBytes(root.since.bytesToRead) + " to read"
                                + (root.since.estimatedSeconds >= 0 ? ", " + Theme.humanDuration(root.since.estimatedSeconds) : "")
                        }
                    }
                    Label {
                        visible: root.controller.busy !== true && root.since.uniformShiftSeconds > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "The stick's timestamps all moved by " + (root.since.uniformShiftSeconds / 3600) + " h since the last "
                            + "backup (a timezone or daylight-saving change) -- treated as unchanged, not re-read."
                    }
                    RowLayout {
                        visible: root.controller.busy !== true && root.since.freeBytes !== undefined
                        spacing: 6
                        Label {
                            font.pointSize: Theme.fontSmall
                            color: root.since.enoughFreeSpace === false ? Theme.danger : Theme.textMuted
                            text: (root.since.enoughFreeSpace === false ? "Not enough free space on this computer: needs " : "Needs ")
                                + Theme.humanBytes(root.since.bytesToRead) + " free"
                        }
                        Label { text: "·"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Label {
                            font.pointSize: Theme.fontSmall
                            color: root.since.enoughFreeSpace === false ? Theme.danger : Theme.good
                            font.family: Theme.dataFamily
                            text: Theme.humanBytes(root.since.freeBytes) + " available"
                        }
                    }
                    Label {
                        visible: root.controller.busy !== true && root.since.estimatedSeconds >= 0
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "Time estimated from this stick's last read-speed benchmark."
                    }

                    // Running: progress.
                    //
                    // This block used to be ninety lines of bar, counts,
                    // rate and ETA written out by hand -- the same
                    // ninety lines as TransferProgressFrame, drifted
                    // apart in small ways nobody could have kept in
                    // step. It is the shared ProgressReport now.
                    ProgressReport {
                        objectName: "backupProgress"
                        visible: root.controller.busy === true && root.controller.activity !== "decide"
                        Layout.fillWidth: true
                        // Moved down here from beside the button: the
                        // phase a run is in belongs next to the bar that
                        // shows how far through it is, not across the
                        // card from it.
                        phases: root.controller.backingUp === true
                            ? ["scanning", "reading", "database", "writing", "verifying"]
                            : (root.controller.phase.length > 0 ? [root.controller.phase] : [])
                        phase: root.controller.phase
                        phaseLabel: root.phaseLabel
                        unitsDone: root.controller.filesDone
                        unitsTotal: root.controller.filesTotal
                        unitName: "files"
                        bytesDone: root.controller.bytesDone
                        bytesTotal: root.controller.bytesTotal
                        bytesPerSecond: root.controller.bytesPerSecond
                        etaSeconds: root.controller.etaSeconds
                        currentItem: root.controller.currentFile
                    }

                    Label {
                        visible: root.controller.busy !== true && root.controller.statusMessage.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.good
                        text: root.controller.statusMessage
                    }
                }
            }

            // ---- Archive health ----
            Frame {
                Layout.fillWidth: true
                visible: root.hasBackup
                opacity: root.controller.busy === true ? 0.5 : 1.0
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: "Archive Health"; font.bold: true }
                        Label {
                            objectName: "deadSpaceLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            text: root.dead.deadBytes > 0
                                ? Theme.humanBytes(root.dead.deadBytes) + " reclaimable (" + Math.round((root.dead.ratio || 0) * 100)
                                  + "% of " + Theme.humanBytes(root.dead.archiveBytes) + ")"
                                  + (root.dead.suggested ? " -- worth compacting." : " -- not worth compacting yet.")
                                : "No wasted space."
                        }
                    }
                    Button {
                        text: "Verify Backup"
                        enabled: root.controller.busy !== true && root.controller.pendingCancelDecision !== true
                        onClicked: root.controller.verify()
                    }
                    Button {
                        objectName: "compactButton"
                        text: "Compact…"
                        visible: root.dead.deadBytes > 0
                        enabled: root.controller.busy !== true && root.controller.pendingCancelDecision !== true
                        onClicked: {
                            compactDialog.preflight = root.controller.compactionPreflight();
                            compactDialog.open();
                        }
                    }
                }
            }

            // ---- Restore ----
            Frame {
                Layout.fillWidth: true
                opacity: root.hasBackup && root.controller.busy !== true ? 1.0 : 0.55
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: "Restore Onto " + root.stickLabel; font.bold: true }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            text: root.hasBackup
                                ? "Puts this backup back onto the stick. Files on the stick that aren't in the backup are kept unless you choose an exact restore."
                                : "Nothing to restore yet -- make a backup first."
                        }
                    }
                    Button {
                        objectName: "restoreButton"
                        text: "Restore Onto Stick…"
                        enabled: root.hasBackup && root.controller.busy !== true && root.controller.pendingCancelDecision !== true
                        onClicked: root.restoreRequested(root.stickLabel, root.controller.stickRoot, root.controller.archivePath)
                    }
                }
            }
        }
    }
}
