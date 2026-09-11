import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Fans out the "Duplicate Tracks" top-level card into its sub-features,
// non-destructive stats/metadata-sync (DuplicatesPage), the destructive
// survivor-select+delete flow (CleanupPage), and reviewing/deleting the
// actual audio files those cleanups orphaned but never touched on disk
// (PendingDeletionsPage). Three separate pages, not merged into one,
// specifically so each destructive step always requires its own
// deliberate navigation rather than sitting next to read-only stats
// where a stray click could reach it.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    signal duplicatesStatsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal cleanupRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal pendingDeletionsRequested(string stickLabel, string rekordboxPath, string enginePath)
    signal junkCueCleanupRequested(string stickLabel, string rekordboxPath, string enginePath)

    readonly property bool hasRekordbox: rekordboxPath.length > 0
    readonly property bool hasEngine: enginePath.length > 0
    readonly property bool hasOneLibrary: root.hasRekordbox && pendingCheckController.hasOneLibrary(root.rekordboxPath)

    // Cheap (a small text file + a stat() per entry, no library scan,
    // see loadPendingDeletionsOnly()'s own doc comment), so checked
    // synchronously right when this hub opens, purely to decide whether
    // "Delete Orphaned Files" has anything to do. Reused sequentially for
    // both formats rather than one instance each, right after
    // loadPendingDeletionsOnly() populates the model, every entry starts
    // included, so pendingDeletionsIncludedCount is exactly the total
    // count at that moment, before either format's next call overwrites it.
    CleanupController {
        id: pendingCheckController
    }
    property int rekordboxPendingCount: 0
    property int enginePendingCount: 0
    readonly property bool hasPendingDeletions: rekordboxPendingCount > 0 || enginePendingCount > 0

    function refreshPendingCounts() {
        if (root.hasRekordbox) {
            pendingCheckController.loadPendingDeletionsOnly("rekordbox", root.rekordboxPath);
            root.rekordboxPendingCount = pendingCheckController.pendingDeletionsIncludedCount;
        }
        if (root.hasEngine) {
            pendingCheckController.loadPendingDeletionsOnly("engine", root.enginePath);
            root.enginePendingCount = pendingCheckController.pendingDeletionsIncludedCount;
        }
    }

    // StackView.onActivated, not Component.onCompleted, fires both when
    // this hub is first pushed AND every time navigation returns to it
    // (from Clean Up Duplicates, which can create new orphaned files, or
    // from Delete Orphaned Files itself, which clears them), so the card
    // below never goes stale after either happens.
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
    StackView.onActivated: {
        root.refreshPendingCounts();
        root.refreshLocks();
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
        // Opaque background override, see AppSettingsPage.qml's header
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
                title: "Housekeeping"
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
            cardTitle: "Match Duplicate Cues"
            cardSubtitle: "Give every copy of a track the same cues, and see the space they waste"
            cardIcon: "▣"
            enabled: root.hasRekordbox || root.hasEngine
            onClicked: root.duplicatesStatsRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            cardTitle: "Clean Up Duplicates"
            cardSubtitle: "Remove redundant copies, keep the best one"
                + (root.hasOneLibrary ? " (also updates OneLibrary)" : "")
            cardIcon: "🧹"
            enabled: root.hasRekordbox || root.hasEngine
            onClicked: root.cleanupRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            cardTitle: "Delete Orphaned Files"
            cardSubtitle: root.hasPendingDeletions
                ? "Free disk space: delete files earlier cleanups' database edits orphaned"
                : "Nothing orphaned right now. Every earlier cleanup's files are accounted for"
            cardIcon: "🗑"
            enabled: (root.hasRekordbox || root.hasEngine) && root.hasPendingDeletions
            onClicked: root.pendingDeletionsRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        ActionCard {
            readOnly: root.lockedByOther
            onReadOnlyClicked: root.explainLock()
            cardTitle: "Clean Up Stray Cues"
            cardSubtitle: "Remove memory cues sitting at 0:00, almost always accidental"
            cardIcon: "🧽"
            enabled: root.hasRekordbox || root.hasEngine
            onClicked: root.junkCueCleanupRequested(root.stickLabel, root.rekordboxPath, root.enginePath)
        }
        Item { Layout.fillHeight: true }
    }
}
