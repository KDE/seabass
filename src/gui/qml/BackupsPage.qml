import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath

    BackupsController {
        id: backupsController
    }

    Component.onCompleted: backupsController.load(root.rekordboxPath, root.enginePath, root.stickLabel)

    // The mutating action the user last asked for, re-run after "Remove
    // Lock" in the locked-library dialog.
    property var pendingAction: null
    function runWrite(action) {
        root.pendingAction = action;
        action();
    }

    readonly property var reasonNames: ({
        "duplicate-cue-consolidation": "Duplicate cue consolidation",
        "sync": "Cross-format sync",
        "local-restore": "Local cue restore",
        "device-settings": "Device settings change",
        "pre-restore": "Pre-restore snapshot",
    })

    // Backup ids are "<timestamp>-<reason>[-N]"; label is everything after
    // the first dash (see FilesystemBackupStore::list()). Presented as a
    // readable date/reason pair, with the raw id/label kept as a tooltip
    // for anyone who needs to find the directory on disk by hand.
    function friendlyTimestamp(id) {
        var m = id.match(/^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})/);
        if (!m) {
            return id;
        }
        var d = new Date(parseInt(m[1]), parseInt(m[2]) - 1, parseInt(m[3]), parseInt(m[4]), parseInt(m[5]), parseInt(m[6]));
        return d.toLocaleString(Qt.locale(), "d MMM yyyy, HH:mm:ss");
    }

    function friendlyReason(label) {
        var stripped = label.replace(/-\d+$/, "");
        if (root.reasonNames[stripped] !== undefined) {
            return root.reasonNames[stripped];
        }
        return stripped.split("-").map((w) => w.length > 0 ? w[0].toUpperCase() + w.slice(1) : w).join(" ");
    }

    // "What's actually in this backup" -- export.pdb/m.db give no hint by
    // themselves that OneLibrary's exportLibrary.db was (or wasn't)
    // included alongside them, which is exactly what was invisible here
    // before this list existed at all.
    readonly property var fileDisplayNames: ({
        "exportLibrary.db": "OneLibrary",
        "export.pdb": "export.pdb (DeviceLibrary)",
        "m.db": "m.db (Engine library)",
    })
    function friendlyFileNames(names) {
        return names.map((n) => root.fileDisplayNames[n] !== undefined ? root.fileDisplayNames[n] : n).join(", ");
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
        // Opaque background override -- see AppSettingsPage.qml's header
        // for why (KDE's Breeze style bleeds the window behind Seabass
        // through an unstyled ToolBar).
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Backups"
                title: "Manage Backups"
                backEnabled: !backupsController.busy
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            Label { text: "Keep:" }
            SpinBox {
                id: keepSpinBox
                from: 0
                to: 1000
                value: 10
                ToolTip.visible: hovered
                ToolTip.text: "How many of the most recent backups to keep"
            }
            Button {
                text: "Clean Up"
                enabled: !backupsController.busy && backupsListView.count > 0
                ToolTip.visible: hovered
                ToolTip.text: "Permanently delete older backup copies; never touches the stick's live data"
                onClicked: confirmCleanDialog.open()
            }
            BusyIndicator {
                visible: backupsController.busy
                running: backupsController.busy
                implicitWidth: 24
                implicitHeight: 24
            }
        }
    }

    MessageDialog {
        id: confirmCleanDialog
        severity: SeabassDialog.Warning
        destructive: true
        title: "Clean Up Backups?"
        headline: "This permanently deletes the "
            + Math.max(0, backupsListView.count - keepSpinBox.value)
            + " oldest backup(s) under " + backupsController.backupDir + "."
        detailText: "It never touches the stick's live DeviceLibrary/Engine data."
        acceptText: "Clean Up"
        onAccepted: root.runWrite(() => backupsController.clean(keepSpinBox.value))
    }

    MessageDialog {
        id: confirmRestoreDialog
        property string targetId: ""
        severity: SeabassDialog.Warning
        title: "Restore This Backup?"
        headline: "This overwrites the current files on the stick with this backup's copies."
        detailText: "The files being overwritten are themselves backed up first, so this can be undone."
        acceptText: "Restore"
        onAccepted: {
            const id = targetId;
            root.runWrite(() => backupsController.restoreBackup(id));
        }
    }

    MessageDialog {
        id: confirmDeleteDialog
        property string targetId: ""
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete This Backup?"
        headline: "This permanently deletes this one backup copy."
        detailText: "It never touches the stick's live data."
        acceptText: "Delete"
        onAccepted: {
            const id = targetId;
            root.runWrite(() => backupsController.deleteBackup(id));
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            visible: backupsController.errorMessage.length > 0
            text: backupsController.errorMessage
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            visible: backupsController.statusMessage.length > 0
            text: backupsController.statusMessage
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            text: backupsListView.count + " backup(s), " + backupsController.totalSizeHuman + " total: " + backupsController.backupDir
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            id: backupsListView
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: backupsController.backups
            spacing: 2

            delegate: ItemDelegate {
                id: backupDelegate
                width: ListView.view.width
                // Sized to its own content, not a fixed pixel height: the
                // contentItem below is two rows (the main row, then the
                // "Files:" summary), and a fixed height didn't reliably
                // fit both -- the second line's text overflowed its own
                // boundary, worse at larger system font sizes.
                height: backupContent.implicitHeight + 16
                hoverEnabled: true

                required property string id
                required property string label
                required property string description
                required property string sizeHuman
                required property var fileNames

                ToolTip.visible: hovered
                ToolTip.text: "ID: " + backupDelegate.id + "\nReason: " + backupDelegate.label
                    + "\nFiles: " + (backupDelegate.fileNames.length > 0
                        ? root.friendlyFileNames(backupDelegate.fileNames) : "(unknown, predates file tracking)")

                contentItem: ColumnLayout {
                    id: backupContent
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: root.friendlyTimestamp(backupDelegate.id) + "  ·  " + root.friendlyReason(backupDelegate.label)
                            Layout.preferredWidth: 320
                            elide: Text.ElideRight
                        }
                        // Reads as plain text until clicked -- a border and
                        // background only appear while actually editing, so the
                        // row doesn't look like a form when you're just scanning
                        // the list for a backup.
                        TextField {
                            id: descriptionField
                            Layout.fillWidth: true
                            placeholderText: "Click to add a note (e.g. \"before Berlin gig\")..."
                            text: backupDelegate.description
                            background: Rectangle {
                                radius: 4
                                color: descriptionField.activeFocus ? Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.08) : "transparent"
                                border.width: descriptionField.activeFocus ? 1 : 0
                                border.color: Theme.accent
                            }
                            onEditingFinished: backupsController.setDescription(backupDelegate.id, text)
                        }
                        Label { text: backupDelegate.sizeHuman; color: Theme.textMuted; Layout.preferredWidth: 70 }
                        Button {
                            text: "Restore"
                            enabled: !backupsController.busy
                            ToolTip.visible: hovered
                            ToolTip.text: "Copy this backup's files back to where they came from"
                            onClicked: {
                                confirmRestoreDialog.targetId = backupDelegate.id;
                                confirmRestoreDialog.open();
                            }
                        }
                        // Deliberately understated (flat, dim, icon-only) --
                        // deleting a single backup is rarer and less reversible
                        // than "Clean Up," so it shouldn't compete visually
                        // with Restore.
                        ToolButton {
                            text: "🗑"
                            font.family: "Noto Sans Symbols2"
                            opacity: 0.55
                            enabled: !backupsController.busy
                            Layout.preferredWidth: Theme.iconSizeSmall
                            ToolTip.visible: hovered
                            ToolTip.text: "Delete this backup permanently"
                            onClicked: {
                                confirmDeleteDialog.targetId = backupDelegate.id;
                                confirmDeleteDialog.open();
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 2
                        text: "Files: " + (backupDelegate.fileNames.length > 0
                            ? root.friendlyFileNames(backupDelegate.fileNames) : "unknown (predates file tracking)")
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        elide: Text.ElideRight
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: backupsListView.count === 0
                text: "No backups found for this stick."
                color: Theme.textMuted
            }
        }
    }

    // Write mode for the direct writes on this page: lock refusal, the
    // cancellable progress of a Clean Up, and the summary afterwards. OK
    // on the summary stays here after a completed action (the list is
    // fresh) and goes back after a cancelled one.
    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: {
            EditSessionRegistry.removeLock(backupsController.libraryId);
            if (root.pendingAction) {
                root.pendingAction();
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
        target: backupsController
        function onLockRefused(holder) { lockedDialog.openFor(backupsController.libraryId, holder); }
        function onWriteFinished(summary) { summaryDialog.show(summary); }
    }
    BusyOverlay {
        anchors.fill: parent
        busy: backupsController.writing
        cancellable: backupsController.writeCancellable
        current: backupsController.writeCurrent
        total: backupsController.writeTotal
        label: backupsController.writeTotal > 0 ? "Deleting old backups. Do not remove your USB stick."
            : "Writing to the stick. Do not remove your USB stick."
        onCancelRequested: backupsController.cancelWrite()
    }
}
