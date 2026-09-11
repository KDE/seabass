import QtQuick
import QtQuick.Controls
import SeabassGui

// Everything an edit page needs from edit mode, in one item placed over
// the page's content: the floating Save button, the leave guard, write
// progress, the summary, and the lock refusal. The page keeps its own
// layout and only routes its Back/Home through requestLeave().
//
//   EditSessionHost {
//       id: editHost
//       anchors.fill: parent
//       libraryId: EditSessionRegistry.libraryIdForPath(root.rekordboxPath || root.enginePath)
//       stickLabel: root.stickLabel
//       rekordboxPath: root.rekordboxPath; enginePath: root.enginePath
//   }
//   BackBreadcrumb { onBackRequested: editHost.requestLeave(() => root.StackView.view.pop()) ... }
//
// Opens the session for the page's lifetime (no lock is taken by that;
// the first staged change takes it) and closes it on destruction.
// `registry` and the session are untyped so tests can pass fakes.
Item {
    id: host
    // The C++ singleton in the app; tests (whose QML module has no C++
    // types) pass a fake, hence the typeof guard.
    property var registry: typeof EditSessionRegistry !== "undefined" ? EditSessionRegistry : null
    required property string libraryId
    property string stickLabel: ""
    property string rekordboxPath: ""
    property string enginePath: ""
    // Which editing page this is, matching PendingChange::owner(). One
    // library is edited by one page at a time; while another page holds
    // unsaved changes this one is read-only, and its own staging attempts
    // are refused in C++ regardless of what the UI does.
    property string feature: ""
    // What the floating save button says. Defaults to "Save"; a page
    // whose save means something more specific than "write my edits"
    // overrides it.
    property string saveLabel: "Save"
    readonly property var session: internal.session
    readonly property string editorOwner: internal.session !== null && internal.session.editorOwner !== undefined
        ? internal.session.editorOwner : ""
    readonly property bool blockedByOtherPage: host.feature.length > 0 && host.editorOwner.length > 0
        && host.editorOwner !== host.feature
    readonly property bool dirty: internal.session !== null && internal.session.dirty === true
    readonly property bool writing: internal.session !== null && internal.session.writing === true

    // --- Where this session's backups will go -----------------------------
    // A cue backup is written before anything on the stick changes and it
    // stays there afterwards, so a stick that cannot hold one sends its
    // backups to local disk instead -- and undo stops being portable. That
    // is worth knowing before staging the first change rather than after
    // doing the work, so the question is asked here, on entering edit mode.
    //
    // The rule itself lives in C++ (infrastructure/backup/stick_space.hpp)
    // because the save has to make the same decision for real when it picks
    // where to write; this only displays it. The numbers are here for the
    // message, which has to name them -- "low on space" alone does not let
    // anyone decide.
    //
    // A session that has not measured (or no session at all) reports zeros
    // and false, which keeps every page silent.
    readonly property real stickBytesFree: internal.session && internal.session.stickBytesFree !== undefined
        ? internal.session.stickBytesFree : 0
    readonly property real stickBytesCapacity: internal.session && internal.session.stickBytesCapacity !== undefined
        ? internal.session.stickBytesCapacity : 0
    readonly property real backupBytesWorstCase: internal.session
        && internal.session.backupBytesWorstCase !== undefined ? internal.session.backupBytesWorstCase : 0
    readonly property bool backupWouldGoLocal: internal.session
        && internal.session.backupGoesLocal === true

    // Accepted: edit anyway, backups land on this computer. Declined: the
    // page takes the user back where they came from, as it does for Back.
    signal backupLocationAccepted()
    signal backupLocationDeclined()

    z: 900  // above page content, below BusyOverlay (1000): a scan's scrim covers the Save button

    QtObject {
        id: internal
        property var session: null
        property var pendingLeave: null
        property bool leaveAfterSave: false
        property bool lowSpaceAsked: false

        function askAboutLowSpace() {
            if (!host.backupWouldGoLocal || internal.lowSpaceAsked) {
                return;
            }
            internal.lowSpaceAsked = true;
            lowSpaceDialog.open();
        }

        function runPendingLeave() {
            var fn = internal.pendingLeave;
            internal.pendingLeave = null;
            internal.leaveAfterSave = false;
            if (fn) {
                fn();
            }
        }
    }

    Component.onCompleted: {
        if (host.libraryId.length > 0 && host.registry) {
            internal.session = host.registry.openSession(host.libraryId, host.stickLabel, host.rekordboxPath,
                                                         host.enginePath);
        }
        // Usually false at this instant now: the measurement behind it
        // runs on a worker thread (see LibraryEditSession::
        // setLibraryPaths for why) and lands a moment later, which is
        // what onBackupWouldGoLocalChanged below is for. Still checked
        // here because a remembered measurement answers immediately.
        internal.askAboutLowSpace();
    }

    // The measurement landing is what usually raises this dialog. Asked
    // once: the property can settle more than once (a re-measure on the
    // same session), and a question the user already answered must not
    // be put to them again.
    onBackupWouldGoLocalChanged: internal.askAboutLowSpace()

    // Bytes as the user reads them, for the one place that needs it.
    function humanSize(bytes) {
        if (bytes >= 1073741824) {
            return (bytes / 1073741824).toFixed(1) + " GB";
        }
        return Math.round(bytes / 1048576) + " MB";
    }
    Component.onDestruction: {
        if (internal.session !== null && host.registry) {
            host.registry.closeSession(host.libraryId);
        }
    }

    // Leave the page via leaveFn unless there is something to decide
    // first. Ignored while writing (the breadcrumb is disabled then too).
    function requestLeave(leaveFn) {
        if (host.writing) {
            return;
        }
        if (host.dirty) {
            internal.pendingLeave = leaveFn;
            unsavedDialog.open();
            return;
        }
        leaveFn();
    }

    SaveOverlayButton {
        objectName: "saveOverlay"
        session: host.session
        label: host.saveLabel
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 24
    }

    UnsavedChangesDialog {
        id: unsavedDialog
        objectName: "unsavedDialog"
        pendingCount: host.session ? host.session.pendingCount : 0
        onSaveRequested: {
            internal.leaveAfterSave = true;
            host.session.save();
        }
        onDiscardRequested: {
            host.session.discard();
            internal.runPendingLeave();
        }
    }

    WriteProgressDialog {
        objectName: "writeProgressDialog"
        session: host.session
    }

    OperationSummaryDialog {
        id: summaryDialog
        objectName: "summaryDialog"
        onAccepted: {
            // Leaving was the goal and everything landed: go. Otherwise
            // (a plain Save, or a cancelled/failed one) stay on the page
            // with whatever is still staged.
            if (internal.leaveAfterSave && host.session && host.session.dirty !== true) {
                internal.runPendingLeave();
            } else {
                internal.pendingLeave = null;
                internal.leaveAfterSave = false;
            }
        }
    }

    LockedLibraryDialog {
        id: lockedDialog
        objectName: "lockedDialog"
        onRemoveLockRequested: host.registry.removeLock(host.libraryId)
    }

    // The first user of MessageDialog. Nothing has been staged yet when
    // this opens, so Cancel costs the user nothing -- which is the whole
    // reason the question is asked here and not at save time.
    MessageDialog {
        id: lowSpaceDialog
        objectName: "lowSpaceDialog"
        severity: SeabassDialog.Warning
        title: host.stickLabel.length > 0 ? "Not enough room on " + host.stickLabel
                                          : "Not enough room on the stick"
        headline: "Editing this library needs to back up "
            + host.humanSize(host.backupBytesWorstCase) + " before anything changes, and "
            + (host.stickLabel.length > 0 ? host.stickLabel : "the stick") + " has "
            + host.humanSize(host.stickBytesFree) + " free."
        detailText: "The backup will be written to this computer instead. Undo will then work only "
            + "here, not from another machine with the stick."
        acceptText: "Back up here and edit"
        rejectText: "Cancel"
        onAccepted: host.backupLocationAccepted()
        onRejected: host.backupLocationDeclined()
    }

    // A second page tried to stage into a library another page is already
    // editing. Not a lock dialog: the other page is in this same window,
    // and the way out is to finish or discard there.
    MessageDialog {
        id: otherPageDialog
        objectName: "otherPageDialog"
        property string ownerName: ""
        severity: SeabassDialog.Warning
        closePolicy: Popup.NoAutoClose
        title: "Another page is editing this library"
        headline: "You have unsaved changes on " + otherPageDialog.ownerName + " for this stick. Save or "
            + "discard them there before editing the same library from here."
        detailText: "Nothing was changed."
        showReject: false
        acceptText: "Understood"
        acceptObjectName: "understoodButton"
    }

    Connections {
        target: host.session
        ignoreUnknownSignals: true
        function onSaveFinished(summary) {
            // On quit the window shows its own summary (see Main.qml).
            if (!(host.registry && host.registry.quitAfterSave === true)) {
                summaryDialog.show(summary);
            }
        }
        function onLockRefused(holder) {
            lockedDialog.openFor(host.libraryId, holder);
        }
        function onEditorConflict(owner, attempted) {
            otherPageDialog.ownerName = host.pageNameFor(owner);
            otherPageDialog.open();
        }
    }

    // PendingChange::owner() values, as the user knows the pages.
    function pageNameFor(owner) {
        switch (owner) {
        case "settings": return "Device Profile";
        case "sync": return "Sync Cue Points";
        case "library-health": return "Library Health";
        case "cleanup": return "Clean Up Duplicates";
        case "dup": return "Match Duplicate Cues";
        case "localcue": return "Local Cue Backup";
        case "addcue": return "Browse Library";
        default: return "another page";
        }
    }
}
