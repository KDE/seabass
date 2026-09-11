import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Fans out the "Backups" top-level card into every backup and restore
// option that genuinely needs THIS stick present: the full stick backup
// into one archive on this computer (reads the stick), bringing it up to
// date from a newer copy of its library (writes the stick), and
// BackupsPage (the automatic per-write backups kept on the stick itself
// under Seabass/backups -- deprecated pending a rework). Restoring a
// stick backup and LocalCuePage (cue backup/restore to/from this
// computer) moved to a general block on the Home page instead: neither
// is actually about this specific stick -- Restore picks its own target
// drive, and the local cue database spans every stick you've ever backed
// up -- so requiring a stick already be inserted and scanned just to
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
    signal manageBackupsRequested(string stickLabel, string rekordboxPath, string enginePath)
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
    StackView.onActivated: root.refreshLocks()

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property var advice: root.backupAdvisor !== null && root.mountPoint.length > 0
        ? (root.backupAdvisor.advice[root.mountPoint] || null) : null
    // A newer copy of this stick's library somewhere else -- see
    // BackupAdvisorController's advice map.
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
            cardIcon: "🗃"
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
            cardSubtitle: root.updateSource !== null
                ? (root.advice.diverged === true ? "⚠ " : "") + root.updateSource.detail
                : ""
            cardIcon: "⟳"
            cardIconFont: "Noto Sans Math"
            experimental: true
            experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
            // A newer copy of this stick's library exists: on another
            // mounted stick (copied via its backup) or as the disk backup
            // itself (restored).
            visible: root.updateSource !== null
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
            cardSubtitle: "List and clean up automatic write backups"
            cardIcon: "🗄"
            deprecated: true
            deprecatedNote: "Needs rework"
            enabled: root.hasRekordbox || root.hasEngine
            onClicked: root.manageBackupsRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        Item { Layout.fillHeight: true }
    }
}
