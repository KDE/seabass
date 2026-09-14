// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Fans out the "Backups" top-level card into every backup and restore
// option that genuinely needs THIS stick present: the full stick backup
// into one archive on this computer (reads the stick), bringing it up to
// date from a newer copy of its library (writes the stick), and
// Manage Backups (BackupsPage: every full stick backup on this computer,
// this stick's first -- also reachable from Home's menu). Restoring a
// stick backup moved to a general block on the Home page instead: it is
// not actually about this specific stick -- Restore picks its own target
// drive -- so requiring a stick already be inserted and scanned just to
// reach them was the wrong gate. restoreStickBackupRequested stays here,
// used internally by Update Stick's disk-backup route.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var appSettingsController
    // The stick's mount point and device, and the stick list's backup
    // advisor: what the update card is decided from. Optional so the
    // page still works when pushed without them.
    property string mountPoint: ""
    property string devicePath: ""
    property var backupAdvisor: null
    // `currentArchivePath`: this stick's own full backup, when the advisor
    // matched one to it; listed first there.
    signal manageBackupsRequested(string stickLabel, string currentArchivePath)
    signal fullStickBackupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal restoreStickBackupRequested(string mountPoint, string devicePath, string archivePath)
    signal cloneStickRequested(string sourceLabel, string sourceRekordboxPath, string sourceEnginePath,
                               string targetMountPoint, string targetLabel, bool targetHasLibrary)

    // The edit-lock registry (EditSessionRegistry singleton; a fake in
    // tests): whether another instance is editing this stick's library.
    property var editRegistry: typeof EditSessionRegistry !== "undefined" ? EditSessionRegistry : null
    readonly property string libraryId: root.editRegistry !== null && root.editRegistry !== undefined
        ? root.editRegistry.libraryIdForPath(root.enginePath.length > 0 ? root.enginePath : root.rekordboxPath) : ""
    readonly property bool lockedByOther: root.libraryId.length > 0 && root.editRegistry !== null
        && root.editRegistry !== undefined && root.editRegistry.lockedByOther.indexOf(root.libraryId) >= 0
    function refreshLocks() {
        if (root.editRegistry !== null && root.editRegistry !== undefined) {
            root.editRegistry.refreshLocks();
        }
    }
    function explainLock() {
        lockedDialog.openFor(root.libraryId, root.editRegistry ? root.editRegistry.lockHolder(root.libraryId) : {});
    }
    Timer {
        interval: 2000
        repeat: true
        running: root.StackView.status === StackView.Active
        onTriggered: root.refreshLocks()
    }
    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: {
            if (root.editRegistry) {
                root.editRegistry.removeLock(lockedDialog.libraryId);
            }
        }
    }
    // Coming back from Manage Backups (or a backup): a deleted or new
    // archive makes the advice stale -- the Full Stick Backup card and the
    // backup handed on to Manage Backups both come from it. Not on the first
    // activation: Home reassessed just before pushing this page, and doing it
    // again would re-read every stick alongside whatever is clicked next.
    property bool shownBefore: false
    StackView.onActivated: {
        if (root.shownBefore && root.backupAdvisor !== null && typeof root.backupAdvisor.reassessAll === "function") {
            root.backupAdvisor.reassessAll();
        }
        root.shownBefore = true;
        root.refreshLocks();
    }

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property var advice: root.backupAdvisor !== null && root.mountPoint.length > 0
        ? (root.backupAdvisor.advice[root.mountPoint] || null) : null
    // A newer copy of this stick's library somewhere else -- see
    // BackupAdvisorController's advice map.
    // The full backup the advisor matched to this stick, by its library or
    // its hardware. Not "label": two sticks called NO NAME would put the
    // other one's backup first, marked "This stick", next to Delete. Not
    // "newest" either: that is only the most recent backup of anything.
    readonly property string currentArchivePath: root.advice && root.advice.backupPath
        && ["fingerprint", "identifier"].indexOf(root.advice.matchedBy) >= 0 ? root.advice.backupPath : ""
    readonly property var updateSource: root.advice && root.advice.updateSource && root.advice.updateSource.kind !== "none"
        ? root.advice.updateSource : null

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
                middleLabel: root.stickLabel
                title: "Backups"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            objectName: "fullStickBackupCard"
            cardTitle: "Full Stick Backup"
            cardSubtitle: root.advice && root.advice.state === "outdated"
                ? "Update the full stick backup: " + root.advice.detail
                : (root.advice && root.advice.state === "current"
                    ? "Full stick backup is up to date"
                    : "Back up the whole stick into one file on this computer")
            cardIcon: "archive-insert"
            // Experimental (see docs/experimental-features.md and
            // docs/stick-backup-plan.md): a new archive format and a
            // restore path that overwrites files on a stick.
            experimental: true
            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
            enabled: root.hasRekordbox || root.hasEngine
            onClicked: root.fullStickBackupRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            objectName: "updateStickCard"
            cardTitle: "Update Stick"
            cardSubtitle: root.updateSource !== null ? root.updateSource.detail : ""
            cardSubtitleIcon: root.updateSource !== null && root.advice.diverged === true ? "dialog-warning" : ""
            cardIcon: "view-refresh"
            experimental: true
            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
            // A newer copy of this stick's library exists: on another
            // mounted stick (copied via its backup) or as the disk backup
            // itself (restored).
            // An opened folder library has no device path; "update from
            // the stick" onto it would, in exact mode, delete whatever
            // that local folder holds that the stick does not.
            visible: root.updateSource !== null && root.devicePath.length > 0
                && (!experimental || experimentalFeaturesEnabled)
            enabled: root.updateSource !== null && root.updateSource.enoughSpace !== false
            onClicked: {
                if (root.updateSource.kind === "stick") {
                    root.cloneStickRequested(root.updateSource.label, root.updateSource.rekordboxPath,
                        root.updateSource.enginePath, root.mountPoint, root.stickLabel, true);
                } else {
                    root.restoreStickBackupRequested(root.mountPoint, root.devicePath, root.updateSource.backupPath);
                }
            }
        }
        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            objectName: "manageBackupsCard"
            cardTitle: "Manage Backups"
            cardSubtitle: "Browse and delete the full stick backups on this computer"
            cardIcon: "deep-history"
            // Not gated like Full Stick Backup: it only browses and deletes
            // files on this computer, and Home's menu offers it ungated too.
            onClicked: root.manageBackupsRequested(root.stickLabel, root.currentArchivePath)
        }
        Item { Layout.fillHeight: true }
    }
}
