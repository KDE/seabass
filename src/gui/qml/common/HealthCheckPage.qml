// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// The frame every one of Library Health's check pages sits in: one check,
// its findings, its repair, and nothing else.
//
// These used to be one page. Every card on the hub opened the same
// scroll holding every check's repair UI at once -- missing files, stray
// cues, the import prompt, sample rates, OneLibrary leftovers -- so a
// press on "Review sample rates" landed on a page whose first thing was
// a list of missing files. Now each card opens a page of its own, and
// this is the part they share: the header, the edit session with its
// floating Save, the scan overlay, and the controller handed over from
// the hub.
//
// The controller stays one. The hub has usually just scanned this
// library, and a scan reads three catalogs and stats a few thousand files
// -- minutes on a full stick -- so the pages are views over the hub's
// controller rather than each running their own. Opened on its own (no
// hub), a page scans for itself.
//
// A page puts its own content in the body; it is laid out below the error
// and status lines, down the one left line the header shares.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath

    // What the breadcrumb calls this page: the check's own name.
    property string checkTitle: ""
    // What the floating save button says; see EditSessionHost.
    property alias saveLabel: editHost.saveLabel

    // The hub can be torn down before the page, taking its controller with
    // it (a stick pulled and its changes discarded does exactly that). So
    // this is an object-typed property, which turns null when that happens,
    // and consistencyController then falls back to ownController. Bindings
    // re-read in between, while it is null, so every read of it here and
    // on the pages goes through ?., with the type's own default where a
    // bare read lands in a typed property (?? false, ?? 0, ?? "").
    property QtObject sharedController: null
    readonly property QtObject consistencyController: root.sharedController !== null
        ? root.sharedController : ownController

    default property alias content: body.data

    LibraryConsistencyController {
        id: ownController
    }

    function formatLabel(format) {
        if (format === "engine") return "Engine OS";
        if (format === "onelibrary") return "OneLibrary";
        return "DeviceLibrary";
    }
    function pathForFormat(format) {
        return format === "engine" ? root.enginePath : root.rekordboxPath;
    }

    function rescan() {
        consistencyController?.scan(root.rekordboxPath, root.enginePath);
    }

    // Only when there is nothing to show yet: a shared controller arrives
    // already holding this library's results, and re-running the scan
    // would throw them away and make the user wait again.
    Component.onCompleted: {
        if (root.sharedController === null) {
            rescan();
        }
    }

    // Edit mode for this library: session, floating Save, leave guard.
    // One feature name for every check page, the same one the single page
    // they were split out of used: they stage into one session, and a
    // change staged on one and left unsaved is still that session's to
    // save or discard.
    EditSessionHost {
        id: editHost
        objectName: "healthCheckEditHost"
        // Cancel on the low-space question leaves, as Back does -- see
        // EditSessionHost's backupLocationDeclined for why it must.
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        feature: "library-health"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.rekordboxPath.length > 0 ? root.rekordboxPath : root.enginePath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it.
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
            spacing: Theme.rowSpacing
            // One level up is the hub, so the middle segment names it:
            // a click on it lands on Library Health, not on the stick.
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: "Library Health"
                title: root.checkTitle
                backEnabled: !consistencyController?.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
            RowLayout {
                visible: consistencyController?.busy ?? false
                spacing: Theme.tightSpacing
                BusyIndicator {
                    running: true
                    implicitWidth: Theme.iconSizeSmall
                    implicitHeight: Theme.iconSizeSmall
                }
                Label {
                    text: consistencyController?.scanningFormat.length > 0
                        ? "Scanning " + root.formatLabel(consistencyController?.scanningFormat) + "..."
                        : "Scanning..."
                    color: Theme.textMuted
                }
            }
            // The session's undo, so it reverts whatever the last save
            // wrote, from whichever check it was saved on. On every check
            // page for that reason: it used to sit beside the missing-file
            // repairs, which was the only place it could be found when
            // they all shared a page.
            Button {
                objectName: "undoLastSaveButton"
                text: "Undo Last Save"
                visible: consistencyController?.canUndo ?? false
                enabled: !consistencyController?.busy && !consistencyController?.writing
                onClicked: consistencyController?.undoLastOperation()
            }
        }
    }

    ColumnLayout {
        id: body
        objectName: "healthCheckBody"
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.rowSpacing

        Label {
            objectName: "healthCheckError"
            visible: consistencyController?.errorMessage.length > 0
            text: consistencyController?.errorMessage ?? ""
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "healthCheckStatus"
            visible: consistencyController?.statusMessage.length > 0
            text: consistencyController?.statusMessage ?? ""
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }

    // A cancelled scan takes the user back to where they came from.
    Connections {
        target: consistencyController
        function onScanCancelled() { root.StackView.view.pop(); }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: consistencyController?.busy ?? false
        current: consistencyController?.scanCurrent ?? 0
        total: consistencyController?.scanTotal ?? 0
        label: (consistencyController?.scanningFormat.length > 0
                ? "Scanning " + root.formatLabel(consistencyController?.scanningFormat) + "..." : "Scanning...")
        cancellable: consistencyController?.scanCancellable ?? false
        onCancelRequested: consistencyController?.cancelScan()
    }
}
