import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// "Create Backup USB Stick from X" (experimental): copies the library on
// one mounted stick onto another. Also "Update TARGET from X" when the
// target already holds an older copy of the same library. Two stages on
// one worker, both of them the trusted paths that already exist: an
// incremental full backup of the source into its own archive, then a
// restore of that archive onto the target. Pushed from the stick list,
// which knows both sticks and passes them in.
Page {
    id: root
    required property var controller
    property var appSettingsController: null
    // From the rekordbox guard: names the DJ software running on this
    // machine, or "". The controller's own preview reports the same.
    property string conflictingSoftware: ""
    property string sourceLabel: ""
    property string sourceRekordboxPath: ""
    property string sourceEnginePath: ""
    property string targetMountPoint: ""
    property string targetLabel: ""
    // The stick list's own knowledge, before the preview arrives: an
    // update (target holds a library) defaults to an exact copy so that
    // tracks removed on the source disappear here too; a fresh backup
    // stick keeps whatever unrelated files it had.
    property bool targetHasLibrary: false

    readonly property var preview: controller.preview || ({})
    readonly property var result: controller.result || ({})
    readonly property string blockedBy: root.conflictingSoftware.length > 0 ? root.conflictingSoftware : (controller.blockedBy || "")
    readonly property bool targetHoldsLibrary: root.preview.targetHasEngineLibrary === true || root.targetHasLibrary
    readonly property string targetName: root.targetLabel.length > 0 ? root.targetLabel : "(untitled)"
    readonly property bool isUpdate: root.targetHoldsLibrary
    readonly property bool needsTypedConfirmation: root.targetHoldsLibrary || exactCheckBox.checked || (root.preview.restoreExtras || 0) > 0
    readonly property bool canStart: root.preview.ready === true && root.controller.busy !== true && root.blockedBy.length === 0
        && root.preview.enoughTargetSpace !== false && root.preview.enoughBackupSpace !== false

    function phaseLabel(phase) {
        switch (phase) {
        case "scanning": return "Scanning " + root.sourceLabel;
        case "reading": return "Reading files";
        case "database": return "Capturing the database";
        case "writing": return root.controller.stage === "backup" ? "Writing the backup" : "Writing files";
        case "verifying": return "Verifying the backup";
        case "analyzing": return "Comparing with " + root.targetName;
        case "removing": return "Removing extras";
        case "checking": return "Checking the library";
        default: return "Working";
        }
    }

    Component.onCompleted: {
        exactCheckBox.checked = root.targetHasLibrary;
        root.controller.configure(root.sourceLabel, root.sourceRekordboxPath, root.sourceEnginePath,
            root.targetMountPoint, root.targetLabel,
            root.appSettingsController ? root.appSettingsController.stickBackupDirectory : "");
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
                title: root.isUpdate ? "Update " + root.targetName + " from " + root.sourceLabel
                                     : "Create Backup USB Stick from " + root.sourceLabel
                backEnabled: !root.controller.busy
                backDisabledTooltip: "Wait for the copy to finish (or cancel it) before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            ExperimentalBadge {}
            Item { Layout.fillWidth: true }
            BusyIndicator { running: root.controller.previewing === true; visible: running; implicitWidth: 20; implicitHeight: 20 }
        }
    }

    TypedConfirmDialog {
        id: confirmDialog
        objectName: "confirmDialog"
        title: (root.isUpdate ? "Update " : "Write onto ") + root.targetName + "?"
        confirmTarget: root.targetName
        needsTypedConfirmation: root.needsTypedConfirmation
        acceptText: root.isUpdate ? "Update Stick" : "Create Backup Stick"
        acceptObjectName: "cloneAcceptButton"
        warningTitle: exactCheckBox.checked
            ? "This overwrites files on " + root.targetName + " and removes everything " + root.sourceLabel + " doesn't have."
            : "This overwrites files on a drive that already holds a DJ library."
        warningText: (root.preview.restoreKnown === true
                ? (root.preview.restoreFilesToWrite || 0) + " file(s) will be written"
                : "Everything " + root.sourceLabel + " holds will be written")
            + (exactCheckBox.checked && (root.preview.restoreExtras || 0) > 0
                ? " and " + root.preview.restoreExtras + " file(s) or folder(s) not on " + root.sourceLabel + " removed." : ".")
            + " Files on " + root.targetName + " are not backed up first."
        onAccepted: root.controller.start(exactCheckBox.checked)

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 12
            rowSpacing: 4
            Label { text: "From"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; text: root.sourceLabel + "  ·  " + root.controller.sourceRoot }
            Label { text: "To"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; text: root.targetName + "  ·  " + root.controller.targetRoot }
            Label { text: "Via"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; text: root.controller.archivePath }
            Label { text: "Mode"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
            Label { text: exactCheckBox.checked ? "Exact copy" : "Overlay (keeps other files)" }
        }
    }

    PageScrollView {
        anchors.fill: parent
        anchors.margins: 16

        ColumnLayout {
            width: parent.width
            spacing: 14

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: "Seabass first brings " + root.sourceLabel + "'s full stick backup up to date on this computer, "
                    + "then writes that backup onto " + root.targetName + ". The backup stays on disk afterwards. "
                    + "Nothing on " + root.sourceLabel + " is changed."
            }
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
                    text: root.blockedBy + " appears to be running: copying is refused until it's closed, so the "
                        + "database can't change while it's being read."
                }
            }
            StickWriteWarning {
                visible: root.controller.cloning === true
                text: root.controller.stage === "restore"
                    ? "Writing " + root.targetName + ". Do not remove either stick until this finishes."
                    : "Reading " + root.sourceLabel + ". Do not remove either stick until this finishes."
            }
            Label {
                Layout.fillWidth: true
                visible: root.controller.errorMessage.length > 0 && root.result.status === undefined
                wrapMode: Text.WordWrap
                color: Theme.danger
                text: root.controller.errorMessage
            }

            // ---- From / To ----
            Frame {
                Layout.fillWidth: true
                RowLayout {
                    anchors.fill: parent
                    spacing: 16
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: "From"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Subtitle { text: root.sourceLabel; color: Theme.text }
                        Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; font.pointSize: Theme.fontSmall; color: Theme.textMuted; text: root.controller.sourceRoot || "" }
                        Label {
                            visible: root.preview.sourceBytes !== undefined
                            font.family: Theme.dataFamily
                            text: Theme.humanBytes(root.preview.sourceBytes || 0) + " on the stick"
                        }
                    }
                    Label { text: "→"; font.family: "Noto Sans Math"; font.pointSize: Theme.fontHuge; color: Theme.textMuted }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label { text: "To"; color: Theme.textMuted; font.pointSize: Theme.fontSmall }
                        Subtitle { text: root.targetName; color: Theme.text }
                        Label { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.dataFamily; font.pointSize: Theme.fontSmall; color: Theme.textMuted; text: root.controller.targetRoot || "" }
                        Label {
                            objectName: "targetStateLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: root.preview.enoughTargetSpace === false ? Theme.danger : Theme.text
                            text: (root.preview.targetFreeBytes !== undefined ? Theme.humanBytes(root.preview.targetFreeBytes) + " free · " : "")
                                + (root.targetHoldsLibrary ? "holds a library that will be updated" : "no library yet")
                        }
                    }
                }
            }

            // ---- Plan ----
            Frame {
                Layout.fillWidth: true
                visible: root.preview.ready === true
                GridLayout {
                    anchors.fill: parent
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 6
                    Label { text: "1. Backup step"; color: Theme.textMuted; font.pointSize: Theme.fontSmall; Layout.alignment: Qt.AlignTop }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            objectName: "backupStepLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.preview.archiveCurrent === true
                                ? root.sourceLabel + "'s backup is up to date: nothing to read."
                                : (root.preview.archiveExists === true
                                    ? "About " + Theme.humanBytes(root.preview.bytesToRead || 0) + " to read from " + root.sourceLabel
                                      + " (" + (root.preview.added || 0) + " added, " + (root.preview.changed || 0) + " changed, "
                                      + (root.preview.removed || 0) + " removed" + (root.preview.databaseChanged === true ? ", database changed" : "") + ")."
                                    : "First backup of " + root.sourceLabel + ": reads everything, " + Theme.humanBytes(root.preview.sourceBytes || 0) + ".")
                        }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            font.family: Theme.dataFamily
                            font.pointSize: Theme.fontSmall
                            color: root.preview.enoughBackupSpace === false ? Theme.danger : Theme.textMuted
                            text: (root.controller.archivePath || "") + "  ·  " + Theme.humanBytes(root.preview.backupFreeBytes || 0) + " free there"
                                + (root.preview.enoughBackupSpace === false ? ": not enough" : "")
                        }
                    }
                    Label { text: "2. Copy step"; color: Theme.textMuted; font.pointSize: Theme.fontSmall; Layout.alignment: Qt.AlignTop }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            objectName: "copyStepLabel"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: root.preview.enoughTargetSpace === false ? Theme.danger : Theme.text
                            text: (root.preview.restoreKnown === true
                                    ? (root.preview.restoreFilesToWrite || 0) + " file(s), about " + Theme.humanBytes(root.preview.bytesToTarget || 0)
                                    : "About " + Theme.humanBytes(root.preview.bytesToTarget || 0))
                                + " onto " + root.targetName + "; " + Theme.humanBytes(root.preview.targetFreeBytes || 0) + " free"
                                + (root.preview.enoughTargetSpace === false ? ": not enough space." : ".")
                        }
                        CheckBox {
                            id: exactCheckBox
                            objectName: "exactCheckBox"
                            text: "Exact copy: also remove files on " + root.targetName + " that " + root.sourceLabel + " doesn't have"
                                + (root.preview.restoreKnown === true && (root.preview.restoreExtras || 0) > 0
                                    ? " (" + root.preview.restoreExtras + " now)" : "")
                            enabled: root.controller.busy !== true
                        }
                    }
                }
            }

            // ---- Progress ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.controller.cloning === true
                spacing: 6
                RowLayout {
                    objectName: "stageStrip"
                    spacing: 8
                    Repeater {
                        model: [{id: "backup", text: "1  Back up " + root.sourceLabel}, {id: "restore", text: "2  Write " + root.targetName}]
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool current: modelData.id === root.controller.stage
                            readonly property bool done: modelData.id === "backup" && root.controller.stage === "restore"
                            objectName: "stageChip-" + modelData.id
                            radius: 3
                            color: current ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15) : "transparent"
                            border.color: current ? Theme.accent : Theme.borderSubtle
                            implicitWidth: stageText.implicitWidth + 16
                            implicitHeight: stageText.implicitHeight + 6
                            Label {
                                id: stageText
                                anchors.centerIn: parent
                                text: (parent.done ? "✓  " : "") + parent.modelData.text
                                color: parent.current ? Theme.accent : (parent.done ? Theme.good : Theme.textMuted)
                                font.bold: parent.current
                            }
                        }
                    }
                }
                TransferProgressFrame {
                    Layout.fillWidth: true
                    phases: root.controller.stage === "restore"
                        ? (exactCheckBox.checked ? ["analyzing", "writing", "removing", "checking"] : ["analyzing", "writing", "checking"])
                        : ["scanning", "reading", "database", "writing", "verifying"]
                    // Reading (backup) and writing (restore) have byte
                    // totals; the other phases sweep.
                    determinatePhases: root.controller.stage === "restore" ? ["writing"] : ["reading"]
                    phase: root.controller.phase
                    phaseLabel: root.phaseLabel
                    filesDone: root.controller.filesDone
                    filesTotal: root.controller.filesTotal
                    bytesDone: root.controller.bytesDone
                    bytesTotal: root.controller.bytesTotal
                    bytesPerSecond: root.controller.bytesPerSecond
                    etaSeconds: root.controller.etaSeconds
                    currentFile: root.controller.currentFile
                    cancelButtonObjectName: "cancelCloneButton"
                    onCancelRequested: root.controller.cancel()
                }
            }

            // ---- Result ----
            Label {
                objectName: "backupOutcomeLabel"
                Layout.fillWidth: true
                visible: root.result.status !== undefined
                wrapMode: Text.WordWrap
                color: root.result.restoreStarted === true ? Theme.textMuted : Theme.danger
                text: root.result.backupSkipped === true
                    ? "Backup step: " + root.sourceLabel + "'s backup was already up to date."
                    : "Backup step: " + root.result.backupStatus + ", " + Theme.humanBytes(root.result.backupBytesRead || 0) + " read from " + root.sourceLabel + "."
            }
            Label {
                Layout.fillWidth: true
                visible: root.result.status !== undefined && root.result.restoreStarted !== true
                wrapMode: Text.WordWrap
                color: root.result.status === "cancelled" ? Theme.textMuted : Theme.danger
                text: root.controller.errorMessage.length > 0 ? root.controller.errorMessage : (root.result.message || "")
            }
            TransferResultFrame {
                Layout.fillWidth: true
                result: root.result
                errorMessage: root.controller.errorMessage
                statusMessage: root.controller.statusMessage
                busy: root.controller.busy === true
                startOverTooltip: "Clear this report. Files already on " + root.targetName + " are kept and skipped next time."
                onStartOverRequested: {
                    if (root.controller.clearResult) root.controller.clearResult();
                    root.controller.refresh();
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "openConfirmButton"
                    text: (root.isUpdate ? "Update " + root.targetName : "Create Backup Stick") + "…"
                    highlighted: true
                    enabled: root.canStart
                    onClicked: confirmDialog.open()
                }
            }
        }
    }
}
