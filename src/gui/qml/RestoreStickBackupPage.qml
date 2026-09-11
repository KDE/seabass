import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SeabassGui

// Top-level "Restore a Stick Backup" (experimental). Lives next to Format
// USB Stick rather than under a stick, because the disaster case is a
// lost or dead stick and a fresh blank one -- which the per-stick card
// grid structurally never shows. The per-stick backup page pushes this
// same page with the drive and archive preselected.
Page {
    id: root
    required property var controller
    // For the archive picker's starting folder. Passed in rather than
    // reached for through the controller: "file://" + path is malformed
    // on Windows, and AppSettingsController already exposes the one
    // converter (gui/local_file_url.hpp) every other page uses.
    required property var appSettingsController
    property string preselectedMountPoint: ""
    // A target that is not mounted yet (a stick fresh out of Format USB
    // Stick): mounted here on open, then selected via onDriveMounted.
    property string preselectedDevicePath: ""
    property string preselectedArchivePath: ""
    property string preselectedLabel: ""
    signal formatUsbRequested()
    // Same signature as StickListPage's own -- see TransferResultFrame's
    // repairLibraryRequested for why the result report offers this.
    signal libraryHealthRequested(string stickLabel, string rekordboxPath, string enginePath)

    readonly property var disks: controller.disks || []
    readonly property var info: controller.archiveInfo || ({})
    readonly property var preview: controller.preview || ({})
    readonly property var result: controller.result || ({})
    readonly property var knownBackups: controller.knownBackups || []
    // A file picked by hand (or preselected by the per-stick page) that is
    // not in the backup folder still needs a row that shows it as chosen.
    readonly property bool archiveIsCustom: (controller.archivePath || "").length > 0
        && !root.knownBackups.some(function(b) { return b.archivePath === controller.archivePath; })
    property int selectedIndex: -1
    readonly property var selectedDisk: (root.selectedIndex >= 0 && root.selectedIndex < root.disks.length)
        ? root.disks[root.selectedIndex] : null
    property bool exact: false
    readonly property bool archiveReady: root.info.label !== undefined && (!root.info.error || root.info.error.length === 0)
    readonly property bool needsTypedConfirmation: root.selectedDisk !== null
        && (root.selectedDisk.hasDjLibrary === true || root.exact || root.preview.extras > 0)
    readonly property bool canRestore: root.archiveReady && root.selectedDisk !== null && root.selectedDisk.usable === true
        && root.preview.filesToWrite !== undefined && root.preview.enoughFreeSpace !== false && root.controller.busy !== true

    function pickDefaultDrive() {
        if (root.preselectedMountPoint.length > 0) {
            for (var i = 0; i < root.disks.length; ++i) {
                if (root.disks[i].mountPoint === root.preselectedMountPoint) return i;
            }
        }
        for (var j = 0; j < root.disks.length; ++j) {
            if (root.disks[j].usable === true) return j;
        }
        return -1;
    }

    function applySelection(index) {
        root.selectedIndex = index;
        if (root.selectedDisk !== null && root.selectedDisk.usable === true && root.controller.analyze) {
            root.controller.analyze(root.selectedDisk.mountPoint);
        } else if (root.controller.analyze) {
            root.controller.analyze("");
        }
    }

    function chooseArchive(path) {
        root.controller.archivePath = path;
        root.applySelection(root.selectedIndex);
    }

    function phaseLabel(phase) {
        switch (phase) {
        case "analyzing": return "Comparing with the drive";
        case "writing": return "Restoring files";
        case "removing": return "Removing extras";
        case "checking": return "Checking the library";
        default: return "Working";
        }
    }

    function friendlyTimestamp(iso) {
        if (!iso) return "";
        var d = new Date(iso);
        return isNaN(d.getTime()) ? iso : d.toLocaleString(Qt.locale(), "d MMM yyyy, HH:mm");
    }

    // `disks` usually resolves (and onDisksChanged picks the default
    // drive) before this runs; only re-analyze here when the archive to
    // restore from was changed by a preselection.
    Component.onCompleted: {
        var archiveChanged = false;
        if (root.preselectedArchivePath.length > 0) {
            root.controller.archivePath = root.preselectedArchivePath;
            archiveChanged = true;
        } else if (root.preselectedLabel.length > 0 && root.controller.archivePathForLabel) {
            root.controller.archivePath = root.controller.archivePathForLabel(root.preselectedLabel);
            archiveChanged = true;
        } else if ((root.controller.archivePath || "").length === 0 && root.defaultArchivePath().length > 0) {
            root.controller.archivePath = root.defaultArchivePath();
            archiveChanged = true;
        }
        if (root.selectedIndex < 0) {
            root.applySelection(root.pickDefaultDrive());
        } else if (archiveChanged) {
            root.applySelection(root.selectedIndex);
        }
        root.mountPreselectedDrive();
    }

    // The newest readable backup, so a stick can be restored right away
    // without picking anything when nothing was preselected.
    function defaultArchivePath() {
        for (var i = 0; i < root.knownBackups.length; ++i) {
            if ((root.knownBackups[i].error || "").length === 0) {
                return root.knownBackups[i].archivePath;
            }
        }
        return "";
    }

    // The list arrives in the background after the page opened (and this
    // also fires once during creation, before Component.onCompleted picks
    // the drive: only analyze when there already is one).
    onKnownBackupsChanged: {
        if ((root.controller.archivePath || "").length === 0 && root.defaultArchivePath().length > 0) {
            root.controller.archivePath = root.defaultArchivePath();
            if (root.selectedIndex >= 0) {
                root.applySelection(root.selectedIndex);
            }
        }
    }

    function mountPreselectedDrive() {
        if (root.preselectedMountPoint.length > 0 || root.preselectedDevicePath.length === 0 || !root.controller.mount) {
            return;
        }
        for (var i = 0; i < root.disks.length; ++i) {
            var disk = root.disks[i];
            if (disk.devicePath !== root.preselectedDevicePath) {
                continue;
            }
            if (disk.mounted !== true && disk.hasNoFilesystem !== true) {
                root.controller.mount(disk.devicePath);
            } else if (disk.usable === true) {
                root.applySelection(i);
            }
            return;
        }
    }
    onDisksChanged: if (root.selectedIndex < 0) root.applySelection(root.pickDefaultDrive())

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
        function onDriveMounted(mountPoint) {
            for (var i = 0; i < root.disks.length; ++i) {
                if (root.disks[i].mountPoint === mountPoint && root.disks[i].usable === true) {
                    root.applySelection(i);
                    return;
                }
            }
        }
    }

    FileDialog {
        id: archiveDialog
        title: "Choose a Seabass stick backup"
        nameFilters: ["Stick backups (*.zip)", "All files (*)"]
        currentFolder: root.appSettingsController.toLocalFileUrl(root.controller.defaultBackupDirectory || "")
        onAccepted: {
            root.controller.archivePath = selectedFile.toString();
            root.applySelection(root.selectedIndex);
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
        background: Rectangle { color: Theme.surface }
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: 12
            BackBreadcrumb {
                title: "Restore a Stick Backup"
                backEnabled: !root.controller.busy
                backDisabledTooltip: "Wait for the restore to finish (or cancel it) before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            ExperimentalBadge {}
            Item { Layout.fillWidth: true }
            BusyIndicator { running: root.controller.analyzing === true; visible: running; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    TypedConfirmDialog {
        id: confirmDialog
        objectName: "confirmDialog"
        title: "Restore onto " + (root.selectedDisk ? root.selectedDisk.label : "") + "?"
        confirmTarget: root.selectedDisk ? (root.selectedDisk.label.length > 0 ? root.selectedDisk.label : "(untitled)") : ""
        needsTypedConfirmation: root.needsTypedConfirmation
        acceptText: "Restore Drive"
        acceptObjectName: "restoreAcceptButton"
        warningTitle: root.exact ? "This overwrites files and removes everything the backup doesn't contain."
                                 : "This overwrites files on a drive that already holds a DJ library."
        warningText: (root.preview.filesToWrite || 0) + " file(s) on " + confirmDialog.confirmTarget
            + " will be written from the backup"
            + (root.exact ? " and " + (root.preview.extras || 0) + " file(s) or folder(s) not in the backup removed." : ".")
            + " Audio files on the drive are not backed up first."
        onAccepted: root.controller.restore(root.selectedDisk.mountPoint, root.exact)

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 12
            rowSpacing: 4
            Label { text: "Drive"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { font.family: Theme.dataFamily; text: root.selectedDisk ? root.selectedDisk.mountPoint + "  ·  " + Theme.humanBytes(root.selectedDisk.capacityBytes) : "" }
            Label { text: "From"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; text: (root.info.label || "") + "  ·  " + root.friendlyTimestamp(root.info.createdAt) }
            Label { text: "Mode"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { text: root.exact ? "Exact restore" : "Overlay (keeps other files)" }
        }
    }

    PageScrollView {
        anchors.fill: parent
        anchors.margins: 16

        ColumnLayout {
            width: parent.width
            spacing: 16

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: "Puts a full stick backup onto a drive: a fresh stick after losing one, or the same stick after a bad "
                    + "night. Nothing is written until you confirm."
            }
            StickWriteWarning {
                visible: root.controller.restoring === true
                text: "Restoring. Do not remove the drive until this finishes."
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.errorMessage.length > 0
                wrapMode: Text.WordWrap
                color: Theme.danger
                text: root.controller.errorMessage
            }

            GroupBox {
                label: Subtitle { text: "1. Choose a backup" }
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        objectName: "knownBackupsLabel"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: root.knownBackups.length > 0
                            ? "Backups Seabass has made, newest first (" + root.controller.defaultBackupDirectory + ")."
                            : (root.controller.listingBackups === true
                               ? "Looking for backups…"
                               : "No backups found in " + root.controller.defaultBackupDirectory
                                 + ". Make one from a stick's Backups page, or choose a backup file below.")
                    }
                    ButtonGroup { id: backupGroup }
                    Repeater {
                        model: root.knownBackups
                        delegate: RadioButton {
                            id: backupRadio
                            objectName: "backupRadio"
                            required property var modelData
                            required property int index
                            readonly property bool unreadable: (modelData.error || "").length > 0
                            Layout.fillWidth: true
                            ButtonGroup.group: backupGroup
                            enabled: !unreadable && root.controller.busy !== true
                            checked: root.controller.archivePath === modelData.archivePath
                            // FluentWinUI3's RadioButton indicator is only
                            // pinned to the left when `text` is non-empty --
                            // left blank (as it was, since the visible
                            // label lives in contentItem below), the style
                            // centers the indicator in the middle of the
                            // row instead. Doubles as the accessible name,
                            // which a custom contentItem doesn't provide on
                            // its own.
                            text: (modelData.label || "").length > 0 ? modelData.label : modelData.fileName
                            onToggled: if (checked) root.chooseArchive(modelData.archivePath)
                            // Layout.leftMargin on the first child, not
                            // x/width on the RowLayout: see FormatUsbPage.qml's
                            // drive rows for why (Control overwrites the
                            // contentItem's x/width on every relayout).
                            contentItem: RowLayout {
                                spacing: 10
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: backupRadio.indicator.width + backupRadio.spacing
                                    spacing: 2
                                    Label {
                                        text: (backupRadio.modelData.label || "").length > 0 ? backupRadio.modelData.label : backupRadio.modelData.fileName
                                        color: Theme.text
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        elide: Text.ElideMiddle
                                        color: Theme.textMuted
                                        font.family: Theme.dataFamily
                                        font.pointSize: Theme.fontTiny
                                        text: backupRadio.unreadable
                                            ? backupRadio.modelData.fileName + " · " + backupRadio.modelData.error
                                            : backupRadio.modelData.fileName + " · " + root.friendlyTimestamp(backupRadio.modelData.createdAt)
                                              + " · " + backupRadio.modelData.entries + " entries"
                                    }
                                }
                                StatusBadge {
                                    label: backupRadio.unreadable ? "UNREADABLE"
                                        : (backupRadio.modelData.status === "complete" ? "VERIFIED" : "INCOMPLETE")
                                    badgeColor: backupRadio.unreadable ? Theme.danger
                                        : (backupRadio.modelData.status === "complete" ? Theme.good : Theme.warnIcon)
                                    tooltipText: backupRadio.unreadable ? backupRadio.modelData.error
                                        : (backupRadio.modelData.status === "complete"
                                           ? "Every entry was verified when this backup was written."
                                           : "This backup was interrupted or its database was not captured; restoring gives you what it holds.")
                                }
                                Label { font.family: Theme.dataFamily; color: Theme.textMuted; text: Theme.humanBytes(backupRadio.modelData.bytes) }
                            }
                        }
                    }
                    RadioButton {
                        id: customRadio
                        objectName: "customBackupRadio"
                        visible: root.archiveIsCustom
                        Layout.fillWidth: true
                        ButtonGroup.group: backupGroup
                        checked: root.archiveIsCustom
                        // See backupRadio's comment above: non-empty text
                        // is what keeps FluentWinUI3's indicator pinned
                        // left instead of centered.
                        text: root.archiveReady ? root.info.label : "Chosen file"
                        contentItem: RowLayout {
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: customRadio.indicator.width + customRadio.spacing
                                spacing: 2
                                Label { text: root.archiveReady ? root.info.label : "Chosen file"; color: Theme.text }
                                Label {
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    color: Theme.textMuted
                                    font.family: Theme.dataFamily
                                    font.pointSize: Theme.fontTiny
                                    text: root.controller.archivePath + (root.archiveReady
                                        ? " · " + root.friendlyTimestamp(root.info.createdAt) + " · " + root.info.entries + " entries" : "")
                                }
                            }
                            StatusBadge {
                                visible: root.archiveReady
                                label: root.info.status === "complete" ? "VERIFIED" : "INCOMPLETE"
                                badgeColor: root.info.status === "complete" ? Theme.good : Theme.warnIcon
                            }
                            Label { visible: root.archiveReady; font.family: Theme.dataFamily; color: Theme.textMuted; text: Theme.humanBytes(root.info.bytes) }
                        }
                    }
                    RowLayout {
                        Layout.topMargin: 4
                        spacing: 8
                        Button { text: "Choose a Backup File…"; flat: true; onClicked: archiveDialog.open() }
                        Button {
                            text: "Refresh"
                            flat: true
                            enabled: root.controller.listingBackups !== true
                            onClicked: if (root.controller.refreshKnownBackups) root.controller.refreshKnownBackups()
                        }
                    }
                }
            }

            GroupBox {
                label: Subtitle { text: "2. Choose a drive" }
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        text: root.disks.length > 0
                            ? "Every removable drive Seabass can see. A drive must be formatted and mounted to receive a restore."
                            : "No removable drives found. Plug one in and it will appear here."
                    }
                    ButtonGroup { id: driveGroup }
                    Repeater {
                        model: root.disks
                        delegate: RadioButton {
                            id: driveRadio
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            ButtonGroup.group: driveGroup
                            enabled: modelData.usable === true && root.controller.busy !== true
                            checked: root.selectedIndex === index
                            // See backupRadio's comment above: non-empty
                            // text is what keeps FluentWinUI3's indicator
                            // pinned left instead of centered.
                            text: modelData.label.length > 0 ? modelData.label : "(untitled)"
                            onToggled: if (checked) root.applySelection(index)
                            contentItem: RowLayout {
                                spacing: 10
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: driveRadio.indicator.width + driveRadio.spacing
                                    spacing: 2
                                    Label { text: driveRadio.modelData.label.length > 0 ? driveRadio.modelData.label : "(untitled)"; color: Theme.text }
                                    Label {
                                        Layout.fillWidth: true
                                        elide: Text.ElideRight
                                        color: Theme.textMuted
                                        font.family: Theme.dataFamily
                                        font.pointSize: Theme.fontTiny
                                        text: driveRadio.modelData.hasNoFilesystem
                                            ? driveRadio.modelData.wholeDiskPath + " · no filesystem"
                                            : (driveRadio.modelData.mounted
                                               ? driveRadio.modelData.mountPoint + (driveRadio.modelData.rootEntries.length > 0 ? " · " + driveRadio.modelData.rootEntries.join(", ") : " · empty")
                                               : driveRadio.modelData.devicePath + " · not mounted")
                                    }
                                }
                                StatusBadge {
                                    label: driveRadio.modelData.hasDjLibrary ? "Has a DJ library"
                                        : (driveRadio.modelData.hasNoFilesystem ? "Blank" : (driveRadio.modelData.mounted ? "Mounted" : "Not mounted"))
                                    badgeColor: driveRadio.modelData.hasDjLibrary ? Theme.warnIcon : (driveRadio.modelData.usable ? Theme.good : Theme.textMuted)
                                }
                                Button {
                                    visible: driveRadio.modelData.hasNoFilesystem === true
                                    text: "Format it first…"
                                    flat: true
                                    onClicked: root.formatUsbRequested()
                                }
                                Button {
                                    objectName: "mountDriveButton"
                                    visible: driveRadio.modelData.mounted !== true && driveRadio.modelData.hasNoFilesystem !== true
                                        && (driveRadio.modelData.devicePath || "").length > 0
                                    text: "Mount"
                                    flat: true
                                    enabled: root.controller.busy !== true
                                    onClicked: if (root.controller.mount) root.controller.mount(driveRadio.modelData.devicePath)
                                }
                                Label { font.family: Theme.dataFamily; color: Theme.textMuted; text: Theme.humanBytes(driveRadio.modelData.capacityBytes) }
                            }
                        }
                    }
                }
            }

            GroupBox {
                label: Subtitle { text: "3. What will happen" }
                Layout.fillWidth: true
                visible: root.archiveReady && root.selectedDisk !== null && root.selectedDisk.usable === true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 6
                        Label { text: "Restore"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Label {
                            objectName: "previewLabel"
                            font.family: Theme.dataFamily
                            text: root.preview.filesToWrite !== undefined
                                ? root.preview.filesToWrite + " file(s) to write (" + Theme.humanBytes(root.preview.bytesToWrite) + "), "
                                  + root.preview.filesUnchanged + " already up to date"
                                : "Analyzing…"
                        }
                        Label { text: "On the drive"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Label {
                            font.family: Theme.dataFamily
                            text: (root.preview.extras || 0) + " file(s) or folder(s) not in the backup"
                                + (root.exact ? ", will be removed" : ", kept")
                        }
                        Label { text: "Free space"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Label {
                            font.family: Theme.dataFamily
                            color: root.preview.enoughFreeSpace === false ? Theme.danger : Theme.good
                            text: Theme.humanBytes(root.preview.freeBytes) + " available, " + Theme.humanBytes(root.preview.bytesToWrite) + " needed"
                        }
                    }
                    // The central directory is metadata, written once, up
                    // front -- it can list a perfectly plausible file count
                    // and byte total while the entries themselves have no
                    // data behind them. This is the one signal available
                    // before actually restoring finds out file by file, so
                    // it gets its own unmissable line rather than folding
                    // into the ordinary "N problems" count restore reports
                    // afterward.
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: (root.info.unreadableEntries || 0) > 0
                        color: Theme.danger
                        text: "⚠ " + root.info.unreadableEntries + " of " + root.info.entries
                            + " entries in this backup have no readable data. The archive may be damaged"
                            + ((root.info.unreadableEntries || 0) * 2 > (root.info.entries || 1)
                               ? " and this restore will be refused." : "; the rest can still be restored.")
                    }
                    CheckBox {
                        objectName: "exactCheckBox"
                        text: "Exact restore: also remove files and folders that aren't in the backup"
                        checked: root.exact
                        onCheckedChanged: root.exact = checked
                    }
                    Label {
                        Layout.leftMargin: 28
                        color: Theme.textMuted
                        font.pointSize: Theme.fontSmall
                        text: "Asks you to type the drive's name before starting."
                    }
                }
            }

            // Progress while restoring: same shape as StickBackupPage's
            // backup progress (phase strip, bar, rate, ETA, current file).
            TransferProgressFrame {
                Layout.fillWidth: true
                visible: root.controller.restoring === true
                phases: root.exact ? ["analyzing", "writing", "removing", "checking"] : ["analyzing", "writing", "checking"]
                // Only the writing phase has a byte total; the others
                // (comparing, removing, checking) sweep instead.
                determinatePhases: ["writing"]
                phase: root.controller.phase
                phaseLabel: root.phaseLabel
                filesDone: root.controller.filesDone
                filesTotal: root.controller.filesTotal
                bytesDone: root.controller.bytesDone
                bytesTotal: root.controller.bytesTotal
                bytesPerSecond: root.controller.bytesPerSecond
                etaSeconds: root.controller.etaSeconds
                currentFile: root.controller.currentFile
                cancelButtonObjectName: "cancelRestoreButton"
                onCancelRequested: root.controller.cancel()
            }

            // Result report. Problems are counted, not listed, until asked
            // for: a stick yanked mid-restore used to produce one error per
            // remaining file, a wall of text with nothing to do about it.
            TransferResultFrame {
                Layout.fillWidth: true
                result: root.result
                errorMessage: root.controller.errorMessage
                statusMessage: root.controller.statusMessage
                busy: root.controller.busy === true
                startOverTooltip: "Clear this report and look for drives again. Files already restored are kept and skipped next time."
                onStartOverRequested: {
                    if (root.controller.clearResult) root.controller.clearResult();
                    root.controller.refresh();
                    root.selectedIndex = -1;
                    root.applySelection(root.pickDefaultDrive());
                }
                // The disk just restored onto, not whatever was selected
                // when the report was drawn -- selectedDisk can already
                // have moved on (Start Over resets it) by the time this
                // is clicked.
                onRepairLibraryRequested: root.libraryHealthRequested(
                    (root.selectedDisk && root.selectedDisk.label) || "",
                    (root.selectedDisk && root.selectedDisk.rekordboxPath) || "",
                    (root.selectedDisk && root.selectedDisk.enginePath) || "")
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "openConfirmButton"
                    text: "Restore Onto " + (root.selectedDisk ? (root.selectedDisk.label.length > 0 ? root.selectedDisk.label : "Drive") : "Drive") + "…"
                    highlighted: true
                    enabled: root.canRestore
                    onClicked: confirmDialog.open()
                }
            }
        }
    }
}
