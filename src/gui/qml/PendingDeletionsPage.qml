// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Moved out of CleanupPage.qml into its own page (was an embedded panel
// there, competing for space with the actual duplicate-group list it sat
// above) -- this is a distinct concern from finding/consolidating
// duplicates: reviewing and actually deleting the audio files earlier
// cleanups' database edits orphaned but never touched on disk.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    // The page works on the stick, not on one of its catalogs: a file is
    // listed whichever cleanup orphaned it, and the deletion reads every
    // catalog before it removes anything. The controller still wants a
    // catalog path to find the stick by; either will do.
    readonly property string format: hasEngine ? "engine" : "rekordbox"
    function currentPath() {
        return root.format === "engine" ? root.enginePath : root.rekordboxPath;
    }

    CleanupController {
        id: cleanupController
    }

    // loadPendingDeletionsOnly(), not scan() -- this page only ever shows
    // the pending-deletions list, so there's no reason to pay for a full
    // whole-library duplicate-group rescan just to get here.
    Component.onCompleted: cleanupController.loadPendingDeletionsOnly(root.format, root.currentPath())

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
                stickLabel: root.stickLabel
                                title: "Delete Orphaned Files"
                backEnabled: !cleanupController.writing
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
        }
    }

    MessageDialog {
        id: confirmDeletePendingDialog
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete " + cleanupController.pendingDeletionsIncludedCount + " File(s) From Disk?"
        headline: "This frees " + cleanupController.includedPendingBytesHuman + ". Each selected file is "
            + "re-verified against the current library right before deletion: if anything still "
            + "references it, it's left alone and reported instead of deleted."
        detailText: "This step is irreversible: a deleted file is gone. The library-database edit that "
            + "originally orphaned it was already backed up separately, when the duplicate was first "
            + "cleaned up; that backup restores the database entry, not this file."
        acceptText: "Delete"
        onAccepted: {
            root.refusedAction = "delete";
            cleanupController.deleteSelectedPendingFiles();
        }
    }

    MessageDialog {
        id: confirmPruneBackupsDialog
        objectName: "confirmPruneBackupsDialog"
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete " + cleanupController.staleBackupCount + " Backup(s) From Earlier Saves?"
        headline: "This frees " + cleanupController.staleBackupBytesHuman + ". Seabass made these backups "
            + "before earlier saves, and nothing in Seabass can restore from them any more."
        detailText: "Backups you asked for yourself are kept, and so are the ones Undo Last Save still needs. "
            + "The stick's library is not touched."
        acceptText: "Delete"
        onAccepted: {
            root.refusedAction = "prune";
            cleanupController.pruneStaleBackups();
        }
    }

    // Which write the lock refusal stopped, so Remove Lock retries that one.
    property string refusedAction: "delete"

    // A prune that removed every backup it set out to is said in a toast
    // that goes by itself; anything less opens the summary dialog.
    ConfirmationToast {
        id: prunedToast
        objectName: "prunedToast"
        parent: root
        z: 1100
        anchors.centerIn: parent
    }
    function afterPrune(summary) {
        const whole = summary.written > 0
            && !(summary.error && summary.error.length > 0)
            && !(summary.warning && summary.warning.length > 0) && summary.cancelled !== true
            && !(summary.skipped > 0) && summary.written === summary.total;
        if (whole) {
            prunedToast.show(summary.written === 1 ? "1 backup deleted" : summary.written + " backups deleted");
        } else {
            summaryDialog.show(summary);
        }
    }

    // Write mode: the lock refusal, the summary (OK goes back after a
    // cancelled run, stays after a completed one).
    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: {
            EditSessionRegistry.removeLock(lockedDialog.libraryId);
            if (root.refusedAction === "prune") {
                cleanupController.pruneStaleBackups();
            } else {
                cleanupController.deleteSelectedPendingFiles();
            }
        }
    }
    OperationSummaryDialog {
        id: summaryDialog
        objectName: "summaryDialog"
        onAccepted: {
            if (summaryDialog.cancelled) {
                root.StackView.view.pop();
            }
        }
    }
    Connections {
        target: cleanupController
        function onLockRefused(holder) {
            lockedDialog.openFor(EditSessionRegistry.libraryIdForPath(root.hasEngine ? root.enginePath : root.rekordboxPath), holder);
        }
        function onPendingDeletionsWriteFinished(summary) { summaryDialog.show(summary); }
        function onStaleBackupsPruneFinished(summary) { root.afterPrune(summary); }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: cleanupController.errorMessage.length > 0
            text: cleanupController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: cleanupController.statusMessage.length > 0
            text: cleanupController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                text: pendingListView.count + " file(s) from earlier cleanups are orphaned but still on disk"
                font.bold: true
            }
            Label {
                visible: pendingListView.count > 0
                text: "(" + cleanupController.totalPendingBytesHuman + " total)"
                color: Theme.textMuted
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: cleanupController.pendingDeletionsIncludedCount > 0
                text: cleanupController.pendingDeletionsIncludedCount + " selected ("
                    + cleanupController.includedPendingBytesHuman + ")"
                color: Theme.textMuted
            }
            Button {
                text: "Select All"
                enabled: !cleanupController.busy && pendingListView.count > 0
                onClicked: cleanupController.setAllPendingDeletionIncluded(true)
            }
            Button {
                text: "Deselect All"
                enabled: !cleanupController.busy && pendingListView.count > 0
                onClicked: cleanupController.setAllPendingDeletionIncluded(false)
            }
            Button {
                text: "Delete Selected Files"
                enabled: !cleanupController.busy && cleanupController.pendingDeletionsIncludedCount > 0
                onClicked: confirmDeletePendingDialog.open()
            }
        }

        ListView {
            id: pendingListView
            objectName: "pendingListView"
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: cleanupController.pendingDeletions
            spacing: 2

            ScrollBar.vertical: BigScrollBar {}

            delegate: ItemDelegate {
                id: pendingDelegate
                width: ListView.view.width
                hoverEnabled: false

                required property int index
                required property string title
                required property string artist
                required property string filePath
                required property string sizeHuman
                required property bool included
                required property string format

                contentItem: RowLayout {
                    spacing: 8
                    SeabassCheckBox {
                        checked: pendingDelegate.included
                        onToggled: cleanupController.setPendingDeletionIncluded(pendingDelegate.index, checked)
                        ToolTip.visible: hovered
                        ToolTip.text: "Include this file in the next deletion"
                    }
                    Label {
                        text: pendingDelegate.title + " - " + pendingDelegate.artist
                        elide: Text.ElideRight
                        Layout.preferredWidth: 260
                    }
                    Label {
                        text: pendingDelegate.filePath
                        elide: Text.ElideMiddle
                        color: Theme.textMuted
                        Layout.fillWidth: true
                    }
                    // Which catalog's cleanup orphaned it; the deletion
                    // checks every catalog regardless.
                    Label {
                        objectName: "orphanedByLabel"
                        text: pendingDelegate.format === "engine" ? "Engine cleanup" : "rekordbox cleanup"
                        color: Theme.textMuted
                        Layout.preferredWidth: 120
                    }
                    Label {
                        text: pendingDelegate.sizeHuman
                        color: Theme.textMuted
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            EmptyState {
                visible: pendingListView.count === 0 && !cleanupController.busy
                tone: "good"
                iconName: "checkmark"
                text: "Nothing orphaned."
                detail: "Every earlier cleanup's files are either still in use or already deleted."
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 12
            Label {
                objectName: "staleBackupCountLabel"
                text: cleanupController.staleBackupCount + " backup(s) from earlier saves are no longer needed"
                font.bold: true
            }
            Label {
                visible: cleanupController.staleBackupCount > 0
                text: "(" + cleanupController.staleBackupBytesHuman + " total)"
                color: Theme.textMuted
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "pruneBackupsButton"
                text: "Prune Backups"
                enabled: !cleanupController.busy && cleanupController.staleBackupCount > 0
                onClicked: confirmPruneBackupsDialog.open()
            }
        }
        Label {
            text: "Seabass backs up the files it is about to change before every save. Only the latest save "
                + "can be undone from inside Seabass, so earlier backups only take space. Backups you asked "
                + "for yourself are not listed here."
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        // A bounded height of its own, so a long list of orphaned files
        // above never squeezes it away, and a long list here never takes
        // the page from the files.
        ListView {
            id: staleBackupsListView
            objectName: "staleBackupsList"
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            Layout.minimumHeight: 100
            clip: true
            model: cleanupController.staleBackups
            spacing: 2

            ScrollBar.vertical: BigScrollBar {}

            delegate: ItemDelegate {
                id: backupDelegate
                width: ListView.view.width
                hoverEnabled: false

                required property var modelData

                contentItem: RowLayout {
                    spacing: 8
                    Label {
                        text: backupDelegate.modelData.when
                        elide: Text.ElideRight
                        Layout.preferredWidth: 260
                    }
                    Label {
                        text: backupDelegate.modelData.label
                        elide: Text.ElideMiddle
                        color: Theme.textMuted
                        Layout.fillWidth: true
                    }
                    Label {
                        text: backupDelegate.modelData.sizeHuman
                        color: Theme.textMuted
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }

            EmptyState {
                objectName: "staleBackupsEmptyState"
                visible: staleBackupsListView.count === 0 && !cleanupController.busy
                    && !cleanupController.listingStaleBackups
                tone: "good"
                iconName: "checkmark"
                text: "No old backups."
                detail: "Every backup on this stick is one Seabass can still undo from, or one you asked for."
            }
        }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: cleanupController.busy
        current: cleanupController.scanCurrent
        total: cleanupController.scanTotal
        label: cleanupController.pruningBackups ? "Deleting backups. Do not remove your USB stick."
            : cleanupController.writing ? "Deleting files. Do not remove your USB stick." : "Loading..."
        cancellable: cleanupController.writeCancellable
        onCancelRequested: cleanupController.cancelWrite()
    }
}
