// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Manage Backups: the full stick backups on this computer. Each one says
// which stick it came from, how old and how big it is and what library it
// holds; it can be browsed (without unpacking it) or deleted. Reached from
// a stick's Backups page -- that stick's backup is listed first -- and
// from Home's menu, with no stick at all.
Page {
    id: root
    // A FullBackupsController; a plain object in the tests.
    required property var controller
    // The stick this was opened for, if any.
    property string stickLabel: ""
    // The archive the stick advisor matched to that stick; listed first.
    property string currentArchivePath: ""
    // For which archives are open as browsed libraries right now.
    property var mediaController: null
    signal browseRequested(string archivePath)
    // What refreshOpenArchives last found, kept here as well as on the
    // controller: the rows bind to this, which changes exactly when the
    // page recomputes it.
    property var openArchives: []

    function refreshOpenArchives() {
        var open = [];
        var model = root.mediaController ? root.mediaController.sticks : null;
        if (model !== null && model !== undefined) {
            var count = model.length !== undefined ? model.length : model.rowCount();
            for (var i = 0; i < count; ++i) {
                var row = model.length !== undefined ? model[i] : model.get(i);
                if (row.isBrowsedBackup) {
                    var archive = root.controller.browsedArchiveFor(row.mountPoint);
                    if (archive.length > 0) {
                        open.push(archive);
                    }
                }
            }
        }
        root.openArchives = open;
        root.controller.openArchivePaths = open;
    }

    // The controller lists the folder as soon as it is given one, and again
    // when this stick's archive changes; listing here as well only queued a
    // second full pass. Coming back to the page (from Home's browse of a
    // backup, say) is when the list can be stale.
    property bool shownBefore: false
    Component.onCompleted: {
        root.controller.currentArchivePath = root.currentArchivePath;
        root.refreshOpenArchives();
    }
    StackView.onActivated: {
        root.refreshOpenArchives();
        if (root.shownBefore) {
            root.controller.refresh();
        }
        root.shownBefore = true;
    }

    readonly property var statusNames: ({
        "partial-cancelled": "Incomplete: cancelled and kept; the next backup continues from there",
        "partial-conflict": "Incomplete: the DJ software was running, so the databases were skipped",
        "partial-db-too-large": "Incomplete: the databases were too large to include",
        "partial-skipped": "Incomplete: some files could not be read",
    })

    // "today", "yesterday", "5 days ago", "3 weeks ago", "4 months ago".
    function age(iso) {
        if (!iso || iso.length === 0) {
            return "at an unknown time";
        }
        var then = new Date(iso);
        var now = new Date();
        var startOfToday = new Date(now.getFullYear(), now.getMonth(), now.getDate());
        var startOfThen = new Date(then.getFullYear(), then.getMonth(), then.getDate());
        var days = Math.round((startOfToday - startOfThen) / 86400000);
        if (days <= 0) return "today";
        if (days === 1) return "yesterday";
        if (days < 14) return days + " days ago";
        if (days < 60) return Math.round(days / 7) + " weeks ago";
        if (days < 730) return Math.round(days / 30) + " months ago";
        return Math.round(days / 365) + " years ago";
    }
    function exactDate(iso) {
        return iso && iso.length > 0 ? new Date(iso).toLocaleString(Qt.locale(), "d MMM yyyy, HH:mm") : "";
    }
    function details(backup) {
        var parts = ["Backed up " + root.age(backup.createdAt), Theme.humanBytes(backup.bytes)];
        if (backup.trackCount >= 0) {
            parts.push(backup.trackCount.toLocaleString(Qt.locale(), "f", 0) + " tracks");
        }
        if (backup.playlistCount >= 0) {
            parts.push(backup.playlistCount.toLocaleString(Qt.locale(), "f", 0) + " playlists");
        }
        return parts.join("  ·  ");
    }

    header: ToolBar {
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else -- see BackupsHubPage.qml's header.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        background: Rectangle { color: Theme.surface }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            BackBreadcrumb {
                stack: root.StackView.view
                // As before: the crumb names the Backups page it came from.
                middleLabel: root.stickLabel.length > 0 ? "Backups" : ""
                title: "Manage Backups"
                backEnabled: root.controller.deleting !== true
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                visible: root.controller.listing === true || root.controller.deleting === true
                running: visible
                implicitWidth: 24
                implicitHeight: 24
            }
        }
    }

    MessageDialog {
        id: confirmDeleteDialog
        objectName: "confirmDeleteDialog"
        property var backup: ({})
        severity: SeabassDialog.Warning
        destructive: true
        title: "Delete This Backup?"
        headline: "This permanently deletes the full backup of "
            + (confirmDeleteDialog.backup.label && confirmDeleteDialog.backup.label.length > 0
                ? confirmDeleteDialog.backup.label : confirmDeleteDialog.backup.fileName)
            + " (" + Theme.humanBytes(confirmDeleteDialog.backup.bytes) + ") from this computer."
        detailText: "The stick itself is not touched. Once deleted, the next backup of that stick copies the whole stick again."
        acceptText: "Delete"
        onAccepted: root.controller.deleteBackup(confirmDeleteDialog.backup.archivePath)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 8

        Label {
            objectName: "errorLabel"
            visible: text.length > 0
            text: root.controller.errorMessage || ""
            color: Theme.danger
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "statusLabel"
            visible: text.length > 0
            text: root.controller.statusMessage || ""
            color: Theme.good
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label {
            objectName: "summaryLabel"
            text: backupsList.count + (backupsList.count === 1 ? " backup, " : " backups, ")
                + Theme.humanBytes(root.controller.totalBytes) + " in " + root.controller.backupDirectory
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ListView {
            id: backupsList
            objectName: "backupsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // Not draggable when everything already fits.
            interactive: contentHeight > height
            clip: true
            spacing: 8
            model: root.controller.backups

            delegate: Rectangle {
                id: backupRow
                required property var modelData
                required property int index
                readonly property bool readable: modelData.error.length === 0
                readonly property bool open: root.openArchives.indexOf(modelData.archivePath) >= 0
                objectName: "backupRow" + index
                width: ListView.view.width
                implicitHeight: rowContent.implicitHeight + 24
                radius: 6
                color: Theme.surface
                border.width: 1
                border.color: modelData.isCurrentStick ? Theme.accent : Theme.border

                RowLayout {
                    id: rowContent
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 12

                    SeabassIcon {
                        Layout.alignment: Qt.AlignTop
                        iconName: backupRow.readable ? "archive-insert" : "dialog-warning"
                        size: Theme.iconSizeLarge
                        color: backupRow.readable ? Theme.text : Theme.warnIcon
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            spacing: 8
                            Label {
                                objectName: "backupTitle"
                                text: backupRow.readable && backupRow.modelData.label.length > 0
                                    ? backupRow.modelData.label : backupRow.modelData.fileName
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.maximumWidth: rowContent.width * 0.5
                            }
                            Label {
                                objectName: "currentStickBadge"
                                visible: backupRow.modelData.isCurrentStick
                                text: "This stick"
                                color: Theme.accent
                                font.pointSize: Theme.fontSmall
                            }
                            Label {
                                visible: backupRow.open
                                text: "Open on Home"
                                color: Theme.textMuted
                                font.pointSize: Theme.fontSmall
                            }
                        }
                        Label {
                            objectName: "backupDetails"
                            visible: backupRow.readable
                            text: root.details(backupRow.modelData)
                            color: Theme.textMuted
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            HoverHandler { id: detailsHover }
                            ToolTip.visible: detailsHover.hovered
                            ToolTip.text: root.exactDate(backupRow.modelData.createdAt) + "\n" + backupRow.modelData.archivePath
                        }
                        Label {
                            objectName: "backupStatus"
                            visible: backupRow.readable && backupRow.modelData.status !== "complete"
                            text: root.statusNames[backupRow.modelData.status] || "Incomplete"
                            color: Theme.warnIcon
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Label {
                            objectName: "backupError"
                            visible: !backupRow.readable
                            text: "Cannot be read: " + backupRow.modelData.error + "  ·  " + Theme.humanBytes(backupRow.modelData.bytes)
                            color: Theme.danger
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }

                    Button {
                        objectName: "browseButton"
                        Layout.alignment: Qt.AlignVCenter
                        text: "Browse"
                        enabled: backupRow.readable && root.controller.deleting !== true
                        ToolTip.visible: hovered
                        ToolTip.text: "Open this backup on the Home page like a stick, without unpacking it. Read-only."
                        onClicked: root.browseRequested(backupRow.modelData.archivePath)
                    }
                    Button {
                        objectName: "deleteButton"
                        Layout.alignment: Qt.AlignVCenter
                        text: "Delete…"
                        icon.source: Theme.iconUrl("edit-delete")
                        enabled: root.controller.deleting !== true && !backupRow.open
                        ToolTip.visible: hovered
                        ToolTip.text: backupRow.open ? "Open for browsing: close it on the Home page first"
                                                     : "Delete this backup from this computer"
                        onClicked: {
                            confirmDeleteDialog.backup = backupRow.modelData;
                            confirmDeleteDialog.open();
                        }
                    }
                }
            }

            Label {
                objectName: "emptyLabel"
                anchors.centerIn: parent
                width: parent.width * 0.8
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: backupsList.count === 0 && root.controller.listing !== true
                text: "No full stick backups yet. Make one from a stick's Backups page: Full Stick Backup."
                color: Theme.textMuted
            }
        }
    }
}
