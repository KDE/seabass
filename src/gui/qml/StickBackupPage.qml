// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Per-stick "Full Stick Backup" (see docs/stick-backup-plan.md; backing up
// and restoring both graduated from experimental on 2026-09-17). Backs the
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

    SeabassDialog {
        id: replaceCollidingDialog
        title: "Replace another stick's backup?"
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: root.controller.replaceCollidingBackup()

        ColumnLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: "\"" + root.controller.nameCollidedWith + "\" already has a backup under this name. "
                      + "Replacing it deletes that backup from this computer and starts a fresh one for "
                      + root.stickLabel + "."
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                // Said plainly because the alternative is already chosen
                // and working: nobody has to do this.
                text: "You do not need to. This stick is already being backed up under a name of its own, "
                      + "and both backups can exist side by side."
            }
        }
    }

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
        case "partial-conflict": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "The database was not captured: Engine DJ or rekordbox was running, or the database kept changing."};
        case "partial-db-too-large": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "The Engine database is too large for Seabass to back up safely; back it up by hand."};
        case "partial-skipped": return {label: "INCOMPLETE", color: Theme.warnIcon, tip: "Some files could not be read and are not in this backup; see the last run's warnings and back up again."};
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
        function onVerifyFailed(detail) {
            verifyFailedDialog.detail = detail;
            verifyFailedDialog.open();
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
                stack: root.StackView.view
                middleLabel: "Backups"
                title: "Full Stick Backup"
                backEnabled: !root.controller.busy
                backDisabledTooltip: "Wait for the backup to finish (or cancel it) before leaving this page"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
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
            + "here, or discard them"
            + (root.hasBackup ? ", which leaves the previous backup exactly as it was."
                              : " and remove the partial backup file.")
        detailText: "Nothing on the stick is affected either way."
        // Said out loud, because nothing else here gets it right.
        // SeabassDialog marks the LAST button when a dialog names no
        // default, on the assumption that the way out is declared last --
        // true of every other footer, and false of this one, where the
        // last button is the destructive answer.
        //
        // Reapplied on every open because selectFooterButton() assigns
        // `highlighted` on every footer button as focus moves, which
        // discards any binding a Button declared for itself. (Qt is not
        // the culprit: DialogButtonBox never touches `highlighted`.)
        // `focus: true` on the safe button is what makes the default hold
        // by construction rather than by being declared first, the way
        // MessageDialog does it.
        function applyDefaultButton() {
            keepPartialButton.highlighted = true;
            discardPartialButton.highlighted = false;
        }
        Component.onCompleted: cancelDecisionDialog.applyDefaultButton()
        // Both this and SeabassDialog's own onOpened run -- a derived
        // handler for the same signal does not replace the base file's --
        // so the focus call below is a deliberate duplicate. Qt.callLater
        // coalesces the two into one.
        onOpened: {
            cancelDecisionDialog.applyDefaultButton();
            Qt.callLater(cancelDecisionDialog.focusDefaultFooterButton);
        }
        footer: DialogButtonBox {
            Button {
                id: keepPartialButton
                objectName: "keepPartialButton"
                text: "Keep for Later"
                focus: true
                Keys.onReturnPressed: cancelDecisionDialog.activateFooterSelection()
                Keys.onEnterPressed: cancelDecisionDialog.activateFooterSelection()
                Keys.onLeftPressed: cancelDecisionDialog.moveFooterSelection(-1)
                Keys.onRightPressed: cancelDecisionDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) cancelDecisionDialog.selectFooterButton(keepPartialButton)
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                id: discardPartialButton
                objectName: "discardPartialButton"
                text: root.hasBackup ? "Discard This Update" : "Delete Partial Backup"
                Keys.onReturnPressed: cancelDecisionDialog.activateFooterSelection()
                Keys.onEnterPressed: cancelDecisionDialog.activateFooterSelection()
                Keys.onLeftPressed: cancelDecisionDialog.moveFooterSelection(-1)
                Keys.onRightPressed: cancelDecisionDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) cancelDecisionDialog.selectFooterButton(discardPartialButton)
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
                onClicked: {
                    cancelDecisionDialog.close();
                    root.controller.discardPartial();
                }
            }
        }
        onAccepted: root.controller.keepPartial()
    }

    // ---- A backup that failed verification ----
    //
    // A decision, not a message: a backup whose files do not match what was
    // written is worse than none, because the next restore would put them
    // back. Delete is the recommended answer. Keep exists because deleting
    // means the next backup copies the whole stick again, which for one
    // damaged file on a large stick is a lot to pay -- but keeping it must
    // not sound like a repair, because an incremental backup only
    // re-copies files that changed on the stick, and a damaged copy of an
    // unchanged file stays damaged.
    //
    // Delete is the keyboard default too, deliberately, and this is the
    // one dialog in the app where Return performs a destructive action.
    // MessageDialog moves the default to Cancel whenever `destructive`
    // is set; this dialog does not use it, and does not follow it.
    //
    // The reasoning is the headline's: a backup whose contents do not
    // match what was written is worse than no backup, because a restore
    // would put the damaged files back, and nothing records the failed
    // verification -- a kept archive goes on showing VERIFIED and is
    // offered as a good backup later. Deleting is expensive and cannot
    // be undone; keeping is quiet and misleading. The expensive answer
    // is the recoverable one.
    //
    // Worth revisiting if a failed verification is ever recorded on the
    // archive, because then keeping it stops being the quiet answer.
    SeabassDialog {
        id: verifyFailedDialog
        objectName: "verifyFailedDialog"
        property string detail: ""
        severity: SeabassDialog.Error
        closePolicy: Popup.NoAutoClose
        title: "This backup is likely corrupt"
        headline: verifyFailedDialog.detail + " Restoring from this backup could put damaged files back onto a stick."
        detailText: "Delete Backup removes it; the next backup copies the whole stick again. Keep Failed Backup "
            + "saves copying everything again, but the next backup only copies files that changed on the "
            + "stick, so damaged files in it may stay damaged. Run Verify again before relying on it."
        // Assigned rather than declared: selectFooterButton() writes
        // `highlighted` on every footer button as focus moves, so a
        // declared `highlighted: true` is discarded the first time anyone
        // tabs or arrows. Reapplied on every open for the same reason.
        function applyDefaultButton() {
            deleteFailedBackupButton.highlighted = true;
            keepFailedBackupButton.highlighted = false;
        }
        Component.onCompleted: verifyFailedDialog.applyDefaultButton()
        // Both this and SeabassDialog's own onOpened run -- a derived
        // handler for the same signal does not replace the base file's --
        // so the focus call below is a deliberate duplicate. Qt.callLater
        // coalesces the two into one.
        onOpened: {
            verifyFailedDialog.applyDefaultButton();
            Qt.callLater(verifyFailedDialog.focusDefaultFooterButton);
        }
        footer: DialogButtonBox {
            Button {
                id: deleteFailedBackupButton
                objectName: "deleteFailedBackupButton"
                text: "Delete Backup"
                focus: true
                Keys.onReturnPressed: verifyFailedDialog.activateFooterSelection()
                Keys.onEnterPressed: verifyFailedDialog.activateFooterSelection()
                Keys.onLeftPressed: verifyFailedDialog.moveFooterSelection(-1)
                Keys.onRightPressed: verifyFailedDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) verifyFailedDialog.selectFooterButton(deleteFailedBackupButton)
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                id: keepFailedBackupButton
                objectName: "keepFailedBackupButton"
                text: "Keep Failed Backup"
                Keys.onReturnPressed: verifyFailedDialog.activateFooterSelection()
                Keys.onEnterPressed: verifyFailedDialog.activateFooterSelection()
                Keys.onLeftPressed: verifyFailedDialog.moveFooterSelection(-1)
                Keys.onRightPressed: verifyFailedDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) verifyFailedDialog.selectFooterButton(keepFailedBackupButton)
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        onAccepted: root.controller.deleteBackup()
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
        // Named, not inferred. `focus: true` on the accept button was
        // enough on Linux and is not enough everywhere: measured on
        // Windows, the footer came back [Compact, Cancel] in that order
        // with focus AND highlight both on Cancel. Nothing had marked a
        // default by the time the dialog opened, so SeabassDialog's
        // fallback took the LAST button -- which is Cancel here -- and
        // Return then cancelled a compaction the user had just asked
        // for. The highlight followed focus correctly throughout; the
        // input to that contract was simply wrong.
        //
        // Only when it can be pressed: footerButtons() skips a disabled
        // button, so with no room to compact the default must fall to
        // Cancel, and marking a disabled Compact would fight that.
        //
        // Reapplied on every open because selectFooterButton() assigns
        // `highlighted` across the footer as focus moves, and because
        // `enabled` depends on the preflight this dialog is opened with.
        function applyDefaultButton() {
            compactAcceptButton.highlighted = compactAcceptButton.enabled;
            compactCancelButton.highlighted = !compactAcceptButton.enabled;
        }
        Component.onCompleted: compactDialog.applyDefaultButton()
        // Runs alongside SeabassDialog's own onOpened rather than
        // replacing it, so the focus call is repeated deliberately;
        // Qt.callLater coalesces the two.
        onOpened: {
            compactDialog.applyDefaultButton();
            Qt.callLater(compactDialog.focusDefaultFooterButton);
        }
        footer: DialogButtonBox {
            Button {
                id: compactAcceptButton
                objectName: "compactAcceptButton"
                text: "Compact"
                // Unlike the other two, this dialog only ever opens
                // because the user asked for it, so Return confirming is
                // what they came for. applyDefaultButton() above is what
                // makes that true; this only saves a frame of the wrong
                // button holding focus before it runs.
                focus: true
                enabled: compactDialog.preflight.enoughFreeSpace === true && compactDialog.preflight.reclaimableBytes > 0
                Keys.onReturnPressed: compactDialog.activateFooterSelection()
                Keys.onEnterPressed: compactDialog.activateFooterSelection()
                Keys.onLeftPressed: compactDialog.moveFooterSelection(-1)
                Keys.onRightPressed: compactDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) compactDialog.selectFooterButton(compactAcceptButton)
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                id: compactCancelButton
                objectName: "compactCancelButton"
                text: "Cancel"
                Keys.onReturnPressed: compactDialog.activateFooterSelection()
                Keys.onEnterPressed: compactDialog.activateFooterSelection()
                Keys.onLeftPressed: compactDialog.moveFooterSelection(-1)
                Keys.onRightPressed: compactDialog.moveFooterSelection(1)
                onActiveFocusChanged: if (activeFocus) compactDialog.selectFooterButton(compactCancelButton)
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
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
                    objectName: "compactReclaimsLabel"
                    font.family: Theme.dataFamily
                    // What compacting frees exactly, not the dead bytes: the
                    // rewritten central directory can be a little larger.
                    text: Theme.humanBytes(compactDialog.preflight.reclaimableBytes || 0) + "  ("
                        + Math.round((compactDialog.preflight.reclaimableBytes || 0)
                                     / Math.max(1, compactDialog.preflight.archiveBytes || 0) * 100) + "%)"
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
                        + "removing the old one, so it needs room for both. Free some space and try again. The "
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
            // The stick is damaged and the backup is still the right
            // move -- it only reads -- but what it captures is whatever
            // survived, and that has to be said before the press, not
            // discovered at restore time.
            Rectangle {
                Layout.fillWidth: true
                objectName: "emergencyCopyBanner"
                visible: root.controller.stickReadOnly === true
                implicitHeight: emergencyLabel.implicitHeight + 16
                radius: 4
                color: Theme.warnBg
                border.color: Theme.warnBorder
                Label {
                    id: emergencyLabel
                    anchors.fill: parent
                    anchors.margins: 8
                    wrapMode: Text.WordWrap
                    color: Theme.warnText
                    text: "This stick is mounted read-only, which means its filesystem is damaged. Backing it up "
                        + "still works and is worth doing now, since reading is all a backup does, but this will be "
                        + "an emergency copy: it holds whatever could still be read off a damaged stick, and it "
                        + "is marked as such. Restore it onto a working library only as a last resort."
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
                            visible: root.controller.nameCollidedWith.length > 0
                            label: "NAME TAKEN"
                            badgeColor: Theme.warnIcon
                            tooltipText: "\"" + root.controller.nameCollidedWith + "\" already has a backup under "
                                + "this name, so this stick is being backed up under a new one. That stick's "
                                + "backup is untouched."
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
                        Button {
                            objectName: "useTakenNameButton"
                            text: "Replace it…"
                            visible: root.controller.nameCollidedWith.length > 0
                            enabled: root.controller.busy !== true
                            ToolTip.visible: hovered
                            ToolTip.text: "Delete " + root.controller.nameCollidedWith
                                + "'s backup and use the plain name for this stick instead"
                            onClicked: replaceCollidingDialog.open()
                        }
                        Button {
                            objectName: "historyButton"
                            text: "History"
                            visible: root.hasBackup
                            enabled: root.controller.busy !== true
                            ToolTip.visible: hovered
                            ToolTip.text: "Open a text file listing every update this backup has had"
                            onClicked: root.controller.openChangelog()
                        }
                    }
                }
            }

            // ---- Back up (idle) / progress (running) ----
            Frame {
                objectName: "backUpFrame"
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
                            // Fills the column itself: a nested layout only
                            // grows as far as a visible child can, and while a
                            // backup runs the description below is hidden --
                            // the row then shrank to its content and Cancel
                            // stood in the middle of the card.
                            Label {
                                Layout.fillWidth: true
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

                            // Named here rather than afterwards, because
                            // there is no afterwards: the name is written
                            // into the archive's manifest as it is built,
                            // and a zip cannot have one entry rewritten
                            // without repacking the whole file. Editing it
                            // on a stick that already has a backup is fine
                            // -- the next update carries the change.
                            RowLayout {
                                visible: root.controller.backingUp !== true
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 8
                                Label {
                                    text: "Name"
                                    color: Theme.textMuted
                                }
                                TextField {
                                    objectName: "backupNameField"
                                    Layout.fillWidth: true
                                    enabled: root.controller.busy !== true
                                    // No placeholder: the field starts out
                                    // holding the stick's name, and this
                                    // style floats a placeholder above a
                                    // filled field as if it were its title
                                    // -- "Optional, e.g. before the Berlin
                                    // gig" sat over "SANDISK_1" and read
                                    // like the label of the box. The hint
                                    // it used to give is beside it.
                                    text: root.controller.backupName
                                    onEditingFinished: root.controller.backupName = text
                                }
                                Label {
                                    // Says the name may be changed or
                                    // cleared, which an already-filled box
                                    // no longer says for itself. Sized like
                                    // this page's other hints rather than
                                    // by a width of its own: a literal wide
                                    // enough here elides the hint on the
                                    // next font or screen.
                                    text: "Optional, e.g. before the Berlin gig"
                                    color: Theme.textMuted
                                    font.pointSize: Theme.fontSmall
                                }
                            }
                        }
                        // Everything in this column hugs the right edge: the
                        // hint under Cancel is wider than the button, and
                        // left-aligned the button drifted in from the side.
                        ColumnLayout {
                            spacing: 4
                            Layout.alignment: Qt.AlignTop
                            Button {
                                objectName: "backUpButton"
                                Layout.alignment: Qt.AlignRight
                                visible: root.controller.busy !== true
                                text: "Back Up Now"
                                highlighted: true
                                enabled: root.canBackUp
                                onClicked: root.controller.backUp()
                            }
                            Button {
                                objectName: "cancelButton"
                                Layout.alignment: Qt.AlignRight
                                visible: root.controller.busy === true && root.controller.activity !== "decide"
                                text: "Cancel"
                                onClicked: root.controller.cancel()
                            }
                            Label {
                                Layout.alignment: Qt.AlignRight
                                visible: root.controller.backingUp === true
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                                text: "Stops after the current file."
                            }
                            Label {
                                Layout.alignment: Qt.AlignRight
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
                        SeabassIcon { iconName: "go-next"; size: Theme.iconSizeSmall * 0.5; color: Theme.textMuted }
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
                            + "backup (a timezone or daylight-saving change), treated as unchanged, not re-read."
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
                        text: "Time estimated from this stick's last USB Stick Performance measurement."
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
                            // Dead space, not a promise: what compacting frees
                            // exactly is in the compact dialog, and can differ
                            // by a few bytes per entry that moves across 4 GiB.
                            text: root.dead.deadBytes > 0
                                ? Theme.humanBytes(root.dead.deadBytes) + " unused (" + Math.round((root.dead.ratio || 0) * 100)
                                  + "% of " + Theme.humanBytes(root.dead.archiveBytes) + ")"
                                  + (root.dead.suggested ? ", worth compacting." : ", not worth compacting yet.")
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
                objectName: "restoreSection"
                Layout.fillWidth: true
                opacity: root.hasBackup && root.controller.busy !== true ? 1.0 : 0.55
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            spacing: Theme.tightSpacing
                            Label { text: "Restore Onto " + root.stickLabel; font.bold: true }
                            ExperimentalBadge {}
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            text: root.hasBackup
                                ? "Puts this backup back onto the stick. Files on the stick that aren't in the backup are kept unless you choose an exact restore."
                                : "Nothing to restore yet. Make a backup first."
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
