// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The cards of one tool group for one stick, as the home screen's pane
// shows them beside the rail: Explore, Sync, Backup or Maintain. Every
// card keeps the rule it had in the old home list (when it shows, when
// it is enabled, when it goes read-only and what it says then) and asks
// for the same page with the same arguments; only the grouping is new.
Item {
    id: root
    // The stick model's row (see StickHeaderRow for its roles).
    required property var row
    // "explore", "sync", "backup" or "maintain".
    required property string group
    required property var mediaController
    required property var appSettingsController
    required property var backupAdvisor
    // The edit-lock registry (EditSessionRegistry; a fake in tests), or
    // null where nothing is wired: no library is then ever locked.
    property var editRegistry: null
    property int columns: 2
    property bool large: true

    // Nothing to show for this row in this group: a stick with no
    // library, or a browsed backup, in a group whose cards all need one.
    readonly property bool empty: {
        switch (root.group) {
        case "explore":
            return !(root.showBrowse || root.showCompare || root.showStatistics || root.showDeviceProfile
                     || root.showPerformance);
        case "sync":
            return !(root.showSync || root.showMetadataBackup || root.showRestoreMetadata || root.showCreateEngine);
        case "backup":
            return !(root.showFullStickBackup || root.showUpdateStick || root.showRestoreBackup
                     || root.showManageBackups || root.showCreateBackupStick);
        case "maintain":
            return !(root.showCleanUp || root.showLibraryHealth || root.showFormat);
        default:
            return true;
        }
    }

    // How far a card's text sits from this item's left edge, so the pane
    // can put the stick's name and the group heading on the same line.
    // The cards sit at this item's left edge and are all alike, so any
    // one's inset is every one's; Browse Library's exists for every row,
    // shown or not.
    readonly property real textInset: browseLibraryCard.textInset

    signal browseRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal playlistDiffRequested(string stickLabel, string rekordboxPath, string enginePath)
    // Maintain's clean-up cards (they had a Housekeeping page of their own).
    signal duplicatesStatsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal cleanupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal pendingDeletionsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal junkCueCleanupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal recordingsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal libraryHealthRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickStatisticsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal stickPerformanceRequested(string stickLabel, string rekordboxPath, string enginePath, string mountPoint)
    signal engineLibraryCreatorRequested(string stickLabel, string rekordboxPath)
    signal settingsRequested(string stickLabel, string pioneerRoot)
    signal syncRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal fullStickBackupRequested(string stickLabel, string rekordboxPath, string enginePath)
    // `currentArchivePath`: this stick's own full backup, when the advisor
    // matched one to it; Manage Backups lists it first.
    signal manageBackupsRequested(string stickLabel, string currentArchivePath)
    signal formatUsbRequested()
    signal restoreStickBackupRequested(string mountPoint, string devicePath, string archivePath, string stickLabel)
    signal metadataBackupRequested(string stickLabel, string rekordboxPath, string enginePath, string libraryId)
    signal metadataRestoreRequested(string stickLabel, string rekordboxPath, string enginePath, string libraryId)
    signal cloneStickRequested(string sourceLabel, string sourceRekordboxPath, string sourceEnginePath,
                               string targetMountPoint, string targetLabel, bool targetHasLibrary)
    // A read-only card clicked while another instance holds the lock:
    // the page explains who holds it (StickListPage.explainLock).
    signal explainLockRequested(string libraryId)

    // ---- The row, read defensively: a fake row may lack a role.
    readonly property bool hasRow: root.row !== null && root.row !== undefined
    readonly property string label: root.hasRow ? String(root.row.label || "") : ""
    readonly property string mountPoint: root.hasRow ? String(root.row.mountPoint || "") : ""
    readonly property string devicePath: root.hasRow ? String(root.row.devicePath || "") : ""
    readonly property string rekordboxPath: root.hasRow ? String(root.row.rekordboxPath || "") : ""
    readonly property string enginePath: root.hasRow ? String(root.row.enginePath || "") : ""
    readonly property string libraryId: root.hasRow ? String(root.row.libraryId || "") : ""
    readonly property bool mounted: root.hasRow && root.row.mounted === true
    readonly property bool hasRekordbox: root.hasRow && root.row.hasRekordbox === true
    readonly property bool hasEngine: root.hasRow && root.row.hasEngine === true
    readonly property bool isFolder: root.hasRow && root.row.isFolder === true
    readonly property bool isBrowsedBackup: root.hasRow && root.row.isBrowsedBackup === true
    // The kernel mounted this stick read-only, which is what a damaged
    // filesystem looks like after an unclean unplug. Nothing can be
    // written until it has been checked, so every card that writes goes
    // read-only too and points at Library Health, which offers the repair.
    readonly property bool stickReadOnly: root.hasRow && root.row.readOnly === true

    readonly property bool hasKnownLibrary: root.hasRekordbox || root.hasEngine
    // Which cards a row may offer that write: a library, and not a stick
    // backup being browsed. Every writing card binds to this one line.
    readonly property bool writable: root.hasKnownLibrary && !root.isBrowsedBackup
    // Another instance is editing this stick's library: every card that
    // would change it goes read-only.
    readonly property bool lockedByOther: root.libraryId.length > 0 && root.editRegistry !== null
        && root.editRegistry !== undefined && root.editRegistry.lockedByOther.indexOf(root.libraryId) >= 0
    // What a card that writes says when the stick itself is the reason
    // it cannot: a click opens Library Health rather than only
    // explaining, since the repair lives there.
    readonly property string readOnlyNote:
        "This stick is mounted read-only: its filesystem needs checking. Library Health can do that."
    readonly property string lockNote: "Another Seabass instance is editing this library"
    // What the backup advisor found for this stick; null until it has looked.
    readonly property var advice: root.backupAdvisor && root.backupAdvisor.advice
        ? (root.backupAdvisor.advice[root.mountPoint] || null) : null
    readonly property string adviceState: root.advice ? root.advice.state : ""
    // The verdict stands unless the cues turn out different, and they are
    // still being read: said beside the verdict for the seconds that takes.
    readonly property bool cuesPending: root.advice !== null && root.advice.cuesPending === true
    // Another mounted stick whose library could be copied onto this empty
    // one / is a newer copy of this stick's library.
    readonly property var cloneSource: root.advice && root.advice.cloneSource && root.advice.cloneSource.kind === "stick"
        ? root.advice.cloneSource : null
    readonly property var updateSource: root.advice && root.advice.updateSource && root.advice.updateSource.kind !== "none"
        ? root.advice.updateSource : null
    // The full backup the advisor matched to this stick, by its library or
    // its hardware. Not "label": two sticks called NO NAME would put the
    // other one's backup first, marked "This stick", next to Delete. Not
    // "newest" either: that is only the most recent backup of anything.
    readonly property string currentArchivePath: root.advice && root.advice.backupPath
        && ["fingerprint", "identifier"].indexOf(root.advice.matchedBy) >= 0 ? String(root.advice.backupPath) : ""
    // The advisor is reading this stick's backups and has no verdict yet:
    // what the Full Stick Backup card would say is not known, so it says
    // it is scanning. Asked per stick, not of the advisor's busy, which
    // holds until the slowest of every queued stick has been read.
    readonly property bool scanningBackups: root.advice === null && root.mountPoint.length > 0
        && root.backupAdvisor && root.backupAdvisor.pending !== undefined && root.backupAdvisor.pending !== null
        && root.backupAdvisor.pending.indexOf(root.mountPoint) >= 0
    // In flight (mount, unmount, or an automatic mount) via this row's
    // own devicePath, not the app-wide busy flag.
    readonly property bool thisRowBusy: root.mediaController.busy
        && root.mediaController.busyDevicePath === root.devicePath

    // ---- Which card shows, one line each, the rules of the old list.
    // Every writing card is withheld for a browsed backup (`writable`):
    // its analysis files are in the archive, not on disk, and its
    // directory is replaced on the next open. Browse, Statistics and
    // Metadata Backup only read, and stay.
    readonly property bool showBrowse: root.hasKnownLibrary
    // Reads the same catalogs Browse does.
    readonly property bool showCompare: root.hasKnownLibrary
    readonly property bool showStatistics: root.hasKnownLibrary
    readonly property bool showDeviceProfile: root.writable
    // Needs no library, but a stick: a browsed backup or an opened
    // folder is on this computer, and measuring it says nothing.
    readonly property bool showPerformance: root.mounted && !root.isBrowsedBackup && !root.isFolder
    readonly property bool showSync: root.writable
    // Experimental (see docs/experimental-features.md), and hidden once
    // the stick has an Engine Library rather than shown disabled.
    readonly property bool showCreateEngine: root.writable && !root.hasEngine
        && root.appSettingsController.experimentalFeaturesEnabled === true
    // The four cards the stick's Backups page used to hold, with its
    // rules: offered wherever that page's card was (`writable`).
    readonly property bool showFullStickBackup: root.writable
    // A newer copy of this stick's library exists: on another mounted
    // stick (copied via its backup) or as the disk backup itself
    // (restored). An opened folder library has no device path; "update
    // from the stick" onto it would, in exact mode, delete whatever that
    // local folder holds that the stick does not.
    readonly property bool showUpdateStick: root.writable && root.updateSource !== null && root.devicePath.length > 0
    readonly property bool showManageBackups: root.writable
    readonly property bool showMetadataBackup: root.hasKnownLibrary
    readonly property bool showRestoreMetadata: root.writable
    // Only when another mounted stick has a library to copy onto this
    // empty one; restoring from this computer is Restore Backup's job.
    readonly property bool showCreateBackupStick: !root.hasKnownLibrary && !root.isFolder && root.cloneSource !== null
    // Restore Backup is one card for both kinds of stick: an empty one
    // (the disaster case, a blank replacement drive) and one with a
    // library (its own full backup put back). A folder row has no
    // devicePath to restore a whole stick through.
    readonly property bool showRestoreBackup: (!root.hasKnownLibrary && !root.isFolder)
        || (root.writable && root.devicePath.length > 0)
    readonly property bool showCleanUp: root.writable

    // What two of Maintain's cards say before they are opened: whether
    // earlier cleanups left files to delete, and what recordings the
    // players left. Both are cheap (a small text file and a stat per
    // entry; a directory listing), so they are taken when Maintain shows
    // and again on the way back home, never for a group not on screen.
    // Made on first use, one of each; tests set their own stand-ins.
    property var pendingProbe: null
    property var recordingsProbe: null
    Component { id: pendingProbeComponent; CleanupController {} }
    Component { id: recordingsProbeComponent; RecordingsController {} }
    property int pendingCount: 0
    property var recordingsSummary: ({count: 0, bytes: 0, sources: []})
    readonly property int recordingCount: root.recordingsSummary.count || 0
    property bool hasOneLibrary: false
    function refreshMaintain() {
        if (root.group !== "maintain" || !root.showCleanUp) {
            return;
        }
        if (root.pendingProbe === null) {
            root.pendingProbe = pendingProbeComponent.createObject(root);
        }
        if (root.recordingsProbe === null) {
            root.recordingsProbe = recordingsProbeComponent.createObject(root);
        }
        // One instance for both formats: right after loading, every entry
        // is included, so the included count is that format's total.
        let pending = 0;
        if (root.hasRekordbox) {
            root.pendingProbe.loadPendingDeletionsOnly("rekordbox", root.rekordboxPath);
            pending += root.pendingProbe.pendingDeletionsIncludedCount;
        }
        if (root.hasEngine) {
            root.pendingProbe.loadPendingDeletionsOnly("engine", root.enginePath);
            pending += root.pendingProbe.pendingDeletionsIncludedCount;
        }
        root.pendingCount = pending;
        root.hasOneLibrary = root.hasRekordbox && root.pendingProbe.hasOneLibrary(root.rekordboxPath);
        root.recordingsSummary = root.recordingsProbe.summarize(root.rekordboxPath, root.enginePath);
    }
    // Later, not at once: a handler on `row` runs before the bindings
    // read off it (showCleanUp, the paths) have caught up, so a stick
    // whose library was found after the row first came was asked about
    // with the row before it, found not writable, and its recordings
    // never counted.
    onGroupChanged: Qt.callLater(root.refreshMaintain)
    Component.onCompleted: root.refreshMaintain()
    onRowChanged: Qt.callLater(root.refreshMaintain)
    // "3 recordings, 5.8 GiB, from Engine OS and a Pioneer deck"
    readonly property string recordingsSubtitle: {
        const count = root.recordingCount;
        if (count <= 0) {
            return root.recordingsSummary.unreadable === true
                ? "Could not read the recording folders on this stick"
                : "No recordings on this stick";
        }
        const names = {engine: "Engine OS", pioneer: "a Pioneer deck", alphatheta: "an AlphaTheta deck"};
        const from = (root.recordingsSummary.sources || []).map(key => names[key] || key);
        const fromText = from.length === 0 ? ""
            : ", from " + (from.length === 1 ? from[0]
                : from.slice(0, from.length - 1).join(", ") + " and " + from[from.length - 1]);
        return count + (count === 1 ? " recording, " : " recordings, ")
            + Theme.humanBytes(root.recordingsSummary.bytes) + fromText;
    }
    readonly property bool showLibraryHealth: root.writable
    // There is no drive behind a folder row to erase.
    readonly property bool showFormat: !root.isFolder

    // A writing card clicked while it is read-only: a stick mounted
    // read-only goes to Library Health, which can repair it; a library
    // another instance holds has the lock explained.
    function explainWriteBlock() {
        if (root.stickReadOnly) {
            root.libraryHealthRequested(root.label, root.rekordboxPath, root.enginePath);
        } else {
            root.explainLockRequested(root.libraryId);
        }
    }

    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        // A group with nothing to offer this stick says so, under the
        // heading the pane shows, rather than leaving a blank.
        Label {
            objectName: "nothingHereLabel"
            Layout.fillWidth: true
            // On the pane's one text edge, with the cards' text and the
            // group heading above it.
            Layout.leftMargin: root.textInset
            visible: root.empty
            text: "Nothing here for this stick."
            color: Theme.textMuted
            wrapMode: Text.WordWrap
        }

        GridLayout {
            id: grid
            objectName: "actionGrid"
            Layout.fillWidth: true
            visible: !root.empty
            columns: Math.max(1, root.columns)
            columnSpacing: Theme.rowSpacing
            rowSpacing: Theme.rowSpacing

            // Every column the same width, whatever each card's text
            // would ask for on its own.
            readonly property real cellWidth: Math.max(0, (root.width - (grid.columns - 1) * grid.columnSpacing)
                                                          / grid.columns)

            // ---- Explore
            ActionCard {
                id: browseLibraryCard
                objectName: "browseLibraryCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Browse Library"
                cardSubtitle: "View tracks, playlists and cues"
                cardIcon: "view-media-track"
                visible: root.group === "explore" && root.showBrowse
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.browseRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "comparePlaylistsCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Compare Playlists"
                cardSubtitle: "Two playlists side by side: what one has that the other lacks"
                cardIcon: "vcs-diff"
                visible: root.group === "explore" && root.showCompare
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.playlistDiffRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "statisticsCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Library Statistics"
                cardSubtitle: "Filesystem, library stats, and disk usage"
                cardIcon: "office-chart-bar"
                visible: root.group === "explore" && root.showStatistics
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.stickStatisticsRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "deviceProfileCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Device Profile"
                readOnly: root.lockedByOther
                onReadOnlyClicked: root.explainLockRequested(root.libraryId)
                cardSubtitle: "View this stick's saved Rekordbox player settings"
                cardIcon: "view-media-equalizer"
                visible: root.group === "explore" && root.showDeviceProfile
                enabled: root.hasRekordbox
                onClicked: root.settingsRequested(root.label, root.rekordboxPath)
            }
            ActionCard {
                objectName: "stickPerformanceCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "USB Stick Performance"
                cardSubtitle: "Measure the stick the way a player reads it, per player generation"
                cardIcon: "speedometer"
                // The write test has nowhere to write on a read-only
                // stick, and half a benchmark is worse than none.
                readOnly: root.stickReadOnly
                readOnlyReason: root.readOnlyNote
                onReadOnlyClicked: root.libraryHealthRequested(root.label, root.rekordboxPath, root.enginePath)
                visible: root.group === "explore" && root.showPerformance
                onClicked: root.stickPerformanceRequested(root.label, root.rekordboxPath, root.enginePath,
                                                          root.mountPoint)
            }

            // ---- Sync
            ActionCard {
                objectName: "syncCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Sync Cue Points"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Syncs cue points between Pioneer and Denon libraries"
                cardIcon: "exchange-positions"
                visible: root.group === "sync" && root.showSync
                enabled: root.hasRekordbox && root.hasEngine
                onClicked: root.syncRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "metadataBackupCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                // In Sync, beside Sync Cue Points: it keeps the stick's cues,
                // ratings and comments in step with a copy on this
                // computer, and the Backup group is about whole sticks.
                cardTitle: "Metadata"
                cardSubtitle: "Keep a database of all your cues, ratings etc. on this computer"
                cardIcon: "document-save"
                // Not gated on the write lock: this only ever writes to
                // the local store, so another session editing the library
                // is no reason to refuse a copy of what is on it.
                visible: root.group === "sync" && root.showMetadataBackup
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.metadataBackupRequested(root.label, root.rekordboxPath, root.enginePath, root.libraryId)
            }
            ActionCard {
                objectName: "restoreMetadataCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Restore Metadata"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Put cues from this computer back on tracks that have lost them"
                cardIcon: "document-import"
                visible: root.group === "sync" && root.showRestoreMetadata
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.metadataRestoreRequested(root.label, root.rekordboxPath, root.enginePath, root.libraryId)
            }
            ActionCard {
                objectName: "createEngineLibraryCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Create Engine Library"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Build a new Engine Library from this stick's DeviceLibrary export"
                cardIcon: "server-database"
                // The first feature here that fabricates a whole new
                // database from scratch, and the one card still behind
                // the experimental setting. A visible binding of our own
                // replaces ActionCard's default, which is where the gate
                // lives, so showCreateEngine restates it.
                experimental: true
                experimentalFeaturesEnabled: root.appSettingsController.experimentalFeaturesEnabled
                visible: root.group === "sync" && root.showCreateEngine
                enabled: root.hasRekordbox
                onClicked: root.engineLibraryCreatorRequested(root.label, root.rekordboxPath)
            }

            // ---- Backup
            // The full stick backup: making it, bringing the stick up to
            // date from a newer copy, putting it back, and the list of
            // them. These four used to sit on a Backups page of their own
            // behind one card here; the advisor's verdict that card
            // carried is now the first card's line.
            ActionCard {
                objectName: "fullStickBackupCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Full Stick Backup"
                readOnly: root.lockedByOther
                onReadOnlyClicked: root.explainLockRequested(root.libraryId)
                cardSubtitle: {
                    const pending = root.cuesPending ? " (checking cues)" : "";
                    if (root.scanningBackups) {
                        return "Scanning existing backups...";
                    }
                    switch (root.adviceState) {
                    case "outdated": return "Update the full stick backup: " + root.advice.detail + pending;
                    case "current": return "Full stick backup is up to date" + pending;
                    default: return "Back up the whole stick into one file on this computer";
                    }
                }
                cardIcon: "archive-insert"
                // Graduated 2026-09-17 (docs/experimental-features.md), with
                // restoring and updating a stick from the backup.
                visible: root.group === "backup" && root.showFullStickBackup
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.fullStickBackupRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "updateStickCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Update Stick"
                readOnly: root.lockedByOther
                onReadOnlyClicked: root.explainLockRequested(root.libraryId)
                cardSubtitle: root.updateSource !== null ? root.updateSource.detail : ""
                cardSubtitleIcon: root.updateSource !== null && root.advice.diverged === true ? "dialog-warning" : ""
                cardIcon: "view-refresh"
                visible: root.group === "backup" && root.showUpdateStick
                enabled: root.updateSource !== null && root.updateSource.enoughSpace !== false
                onClicked: {
                    if (root.updateSource.kind === "stick") {
                        root.cloneStickRequested(root.updateSource.label, root.updateSource.rekordboxPath,
                            root.updateSource.enginePath, root.mountPoint, root.label, true);
                    } else {
                        root.restoreStickBackupRequested(root.mountPoint, root.devicePath,
                            root.updateSource.backupPath, root.label);
                    }
                }
            }
            // A stick with a library: its own full backup put back, or
            // another. An empty stick: the disaster case, a blank
            // replacement drive. Always offered there, whether or not a
            // backup is known, and worded so it does not presuppose one:
            // "no-backups" is a real, common state. Not gated on
            // `mounted`: a stick fresh out of Format USB Stick is not
            // remounted, and the restore page mounts it itself.
            ActionCard {
                objectName: "restoreBackupCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Restore Backup"
                // A stick mounted read-only is the empty stick's reason
                // too; a stick with a library keeps the rule its Backups
                // page gave this card, the lock alone.
                readOnly: root.lockedByOther || (!root.hasKnownLibrary && root.stickReadOnly)
                readOnlyReason: root.stickReadOnly && !root.hasKnownLibrary ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: root.hasKnownLibrary
                    ? (root.currentArchivePath.length > 0
                        ? "Put this stick's full backup back onto it, or another one"
                        : "Put a full stick backup from this computer onto this stick")
                    : (root.adviceState === "restore"
                        ? "Restore " + root.advice.backupLabel + "'s library onto this stick"
                        : "No known stick backups yet. Browse for a backup file to restore")
                cardIcon: "document-revert"
                visible: root.group === "backup" && root.showRestoreBackup
                enabled: root.hasKnownLibrary || !root.thisRowBusy
                onClicked: root.restoreStickBackupRequested(root.mountPoint, root.devicePath,
                    root.hasKnownLibrary ? root.currentArchivePath
                                         : (root.adviceState === "restore" ? root.advice.backupPath : ""),
                    root.label)
            }
            ActionCard {
                // Not "manageBackupsCard": that is the no-stick pane's card
                // on the home, which lists every stick's backups.
                objectName: "stickManageBackupsCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Manage Backups"
                readOnly: root.lockedByOther
                onReadOnlyClicked: root.explainLockRequested(root.libraryId)
                cardSubtitle: "Browse and delete the full stick backups on this computer"
                cardIcon: "deep-history"
                // Not gated, like Full Stick Backup: it only browses and
                // deletes files on this computer, and Home's menu offers
                // it ungated too.
                visible: root.group === "backup" && root.showManageBackups
                onClicked: root.manageBackupsRequested(root.label, root.currentArchivePath)
            }
            // Copying another mounted stick's live library onto this
            // empty one, through a fresh backup of it.
            ActionCard {
                objectName: "createBackupStickCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Create Backup USB Stick"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: root.cloneSource !== null ? root.cloneSource.detail : ""
                cardIcon: "edit-copy"
                visible: root.group === "backup" && root.showCreateBackupStick
                // The source stick has to be mounted, which cloneSource
                // being non-null already implies (peers are only ever
                // mounted sticks).
                enabled: !root.thisRowBusy && root.cloneSource !== null
                    && root.mounted && root.cloneSource.enoughSpace !== false
                onClicked: root.cloneStickRequested(root.cloneSource.label,
                    root.cloneSource.rekordboxPath, root.cloneSource.enginePath,
                    root.mountPoint, root.label, false)
            }

            // ---- Maintain
            // The clean-up cards first: Clean Up Duplicates, the one most
            // people come for, leads.
            ActionCard {
                objectName: "cleanupCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Clean Up Duplicates"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Remove redundant copies, keep the best one"
                    + (root.hasOneLibrary ? " (also updates OneLibrary)" : "")
                cardIcon: "edit-clear-all"
                visible: root.group === "maintain" && root.showCleanUp
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.cleanupRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "duplicateCuesCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Cues on Duplicate Copies"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Copies of a track whose cues differ: level the cues and keep every copy"
                cardIcon: "edit-duplicate"
                visible: root.group === "maintain" && root.showCleanUp
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.duplicatesStatsRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "strayCuesCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Clean Up Stray Cues"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: "Remove cues sitting at 0:00, almost always accidental"
                cardIcon: "draw-eraser"
                visible: root.group === "maintain" && root.showCleanUp
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.junkCueCleanupRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "recordingsCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Clean Up Recordings"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: root.recordingsSubtitle
                cardIcon: "media-record"
                visible: root.group === "maintain" && root.showCleanUp
                enabled: (root.hasRekordbox || root.hasEngine) && root.recordingCount > 0
                onClicked: root.recordingsRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "orphanedFilesCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Delete Orphaned Files"
                readOnly: root.lockedByOther || root.stickReadOnly
                readOnlyReason: root.stickReadOnly ? root.readOnlyNote : root.lockNote
                onReadOnlyClicked: root.explainWriteBlock()
                cardSubtitle: root.pendingCount > 0
                    ? "Free disk space: delete files earlier cleanups' database edits orphaned"
                    : "Nothing orphaned right now. Every earlier cleanup's files are accounted for"
                cardIcon: "edit-delete"
                visible: root.group === "maintain" && root.showCleanUp
                enabled: (root.hasRekordbox || root.hasEngine) && root.pendingCount > 0
                onClicked: root.pendingDeletionsRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "libraryHealthCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Library Health"
                // Not read-only on a read-only stick: this is where that
                // stick's repair lives.
                readOnly: root.lockedByOther
                onReadOnlyClicked: root.explainLockRequested(root.libraryId)
                cardSubtitle: "Find rows whose file is missing and repair or clean them up"
                cardIcon: "kt-check-data"
                visible: root.group === "maintain" && root.showLibraryHealth
                enabled: root.hasRekordbox || root.hasEngine
                onClicked: root.libraryHealthRequested(root.label, root.rekordboxPath, root.enginePath)
            }
            ActionCard {
                objectName: "formatUsbCard"
                large: root.large
                Layout.preferredWidth: grid.cellWidth
                Layout.maximumWidth: grid.cellWidth
                Layout.fillHeight: true
                cardTitle: "Format USB Stick"
                cardSubtitle: "Erase and prepare this drive for CDJs, XDJs, and Denon Engine players"
                cardIcon: "edit-delete-shred"
                // The one action here that can permanently erase a drive,
                // so it keeps its own warnings and its type-to-confirm
                // dialog, and it is offered whether or not the stick has
                // a library: it is the action for a stick with nothing
                // recognizable on it yet.
                visible: root.group === "maintain" && root.showFormat
                enabled: !root.mediaController.busy
                onClicked: root.formatUsbRequested()
            }
        }
    }
}
