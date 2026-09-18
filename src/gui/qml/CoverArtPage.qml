// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Cover art, from Library Health's own card: what a player can find, what
// it cannot, and the one action that fixes it.
//
// Its own page rather than a row among the missing-file findings, because
// it is a property of the Engine library as a whole and it is the fault
// nobody goes looking for: the art simply never appears on the player.
// The fix is one press -- stage, save, read the summary, back to the hub
// with its cards already re-counted.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath

    // Library Health hands over the controller that just scanned this
    // library, so this page reports that scan rather than repeating work
    // that reads three catalogs. Opened on its own, it scans for itself.
    property var sharedController: null
    readonly property var consistencyController: root.sharedController !== null
        && root.sharedController !== undefined ? root.sharedController : ownController

    LibraryConsistencyController {
        id: ownController
    }

    EditSessionHost {
        id: editHost
        objectName: "coverArtEditHost"
        onBackupLocationDeclined: editHost.requestLeave(() => root.StackView.view.pop())
        // Back to the hub once the save the user asked for has been read,
        // with its cards re-counted from what was just written. Only then:
        // a cancelled or failed save leaves the page as it is.
        onSummaryDismissed: {
            if (!consistencyController.artworkRepairStaged) {
                root.StackView.view.pop();
            }
        }
        feature: "library-health"
        anchors.fill: parent
        libraryId: typeof EditSessionRegistry !== "undefined"
            ? EditSessionRegistry.libraryIdForPath(root.enginePath.length > 0 ? root.enginePath : root.rekordboxPath) : ""
        stickLabel: root.stickLabel
        rekordboxPath: root.rekordboxPath
        enginePath: root.enginePath
        saveLabel: "Fix Cover Art"
    }

    Component.onCompleted: {
        if (root.sharedController === null || root.sharedController === undefined) {
            consistencyController.scan(root.rekordboxPath, root.enginePath);
        }
    }

    header: ToolBar {
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
                middleLabel: root.stickLabel
                title: "Cover Art"
                backEnabled: !consistencyController.busy && !consistencyController.writing
                onHomeRequested: editHost.requestLeave(() => root.StackView.view.pop(null))
                onBackRequested: editHost.requestLeave(() => root.StackView.view.pop())
            }
            Item { Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: 12

        Label {
            objectName: "coverArtHeadline"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.family: Theme.titleFamily
            font.pointSize: Theme.subtitleSize
            text: consistencyController.busy
                ? "Checking where each Engine track's cover art is stored..."
                : consistencyController.artworkError.length > 0
                    ? "Seabass could not check this library's cover art"
                    : consistencyController.artworkTracksWithArt === 0
                        ? "No Engine library on this stick, or no track carries cover art."
                        : consistencyController.artworkUnreadableCount === 0
                            ? "Every Engine track's cover art is stored where a player can find it."
                            : consistencyController.artworkUnreadableCount + " of "
                              + consistencyController.artworkTracksWithArt
                              + " Engine track(s) have cover art no player can show"
        }

        Label {
            objectName: "coverArtExplanation"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            visible: text.length > 0
            // One self-contained sentence per fault, each carrying its own
            // advice: an imported path is repairable from the rekordbox art
            // beside it, a missing cached file is a row that is already
            // right whose image was deleted, and a row with no hash has
            // nothing to look for at all.
            text: {
                if (consistencyController.busy) {
                    return "";
                }
                if (consistencyController.artworkError.length > 0) {
                    return consistencyController.artworkError;
                }
                var imported = consistencyController.artworkImportedCount;
                var missing = consistencyController.artworkMissingFileCount;
                var broken = consistencyController.artworkBrokenRowCount;
                var fixable = consistencyController.artworkRepairableCount;
                var parts = [];
                if (imported > 0) {
                    parts.push(imported + " point at a folder on the computer that ran Engine's "
                        + "\"import rekordbox library\", which a player does not have."
                        + (fixable > 0
                            ? " Seabass can copy " + fixable + " of them in from the rekordbox art on this stick, "
                              + "the way Engine stores its own."
                            : " None of their images are on this stick, so Seabass cannot copy them in; "
                              + "re-importing in Engine DJ would rebuild them."));
                }
                if (missing > 0) {
                    parts.push(missing + " are stored the way Engine stores its own, but the image file is gone "
                        + "from Engine Library/Artwork. Engine DJ writes those again the next time it analyses "
                        + "or re-imports them.");
                }
                var emptied = consistencyController.artworkEmptyFileCount;
                if (emptied > 0) {
                    parts.push(emptied + " have their image file sitting in Engine Library/Artwork with nothing in "
                        + "it: the name is right and the file is empty, which is what a stick pulled out "
                        + "mid-write leaves behind. A player draws its own grey placeholder for those, so they "
                        + "look like art that was never there. Engine DJ writes them again when it next analyses "
                        + "or re-imports those tracks.");
                }
                if (broken > 0) {
                    parts.push(broken + " point at an art row with nothing in it, so there is no name to look the "
                        + "image up by. The track still says which file it is, though, so a cover can come back from "
                        + "the file's own tags or from a backup that knows it.");
                }
                parts.push(fixable > 0
                    ? fixable + " of " + consistencyController.artworkUnreadableCount + " can be put back: Seabass "
                      + "takes the image from the rekordbox art on this stick, the track's own tags, or a stick "
                      + "backup on this computer, whichever still has a copy, in that order."
                    : "None of them has a copy left on this stick, in the tracks themselves, or in a backup on this "
                      + "computer. Re-importing or re-analysing in Engine DJ is what would rebuild them.");
                return parts.join(" ");
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 6
            spacing: Theme.rowSpacing

            Button {
                objectName: "fixCoverArtButton"
                text: consistencyController.artworkRepairStaged ? "Unstage" : "Fix Cover Art"
                visible: consistencyController.artworkRepairableCount > 0
                    || consistencyController.artworkRepairStaged
                // Staging a fix onto a stick that refuses writes only
                // moves the failure to the Save press.
                enabled: !consistencyController.busy && !consistencyController.writing
                    && !consistencyController.stickReadOnly
                ToolTip.visible: hovered
                ToolTip.text: consistencyController.stickReadOnly
                    ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                    : consistencyController.artworkRepairStaged
                    ? "Take this back out of the changes to save"
                    : "Stage copying each image into Engine Library/Artwork and pointing the track at it. "
                        + "Save writes it to the stick."
                // Staging only. Save is the press that writes, like
                // everywhere else in this app.
                onClicked: consistencyController.artworkRepairStaged
                    ? consistencyController.unstageArtworkRepair()
                    : consistencyController.repairArtwork()
            }
            Label {
                objectName: "coverArtStagedNote"
                visible: consistencyController.artworkRepairStaged
                color: Theme.warnText
                text: "staged, not saved yet. Press Save to write it to the stick"
            }
            Item { Layout.fillWidth: true }
        }

        Label {
            objectName: "coverArtNothingToDo"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            visible: !consistencyController.busy && consistencyController.artworkUnreadableCount > 0
                && consistencyController.artworkRepairableCount === 0
            text: "There is nothing for Seabass to copy in here, so this page has no action to offer."
        }

        Item { Layout.fillHeight: true }
    }

    BusyOverlay {
        anchors.fill: parent
        busy: consistencyController.busy
        current: consistencyController.scanCurrent
        total: consistencyController.scanTotal
        label: "Checking cover art..."
        cancellable: consistencyController.scanCancellable
        onCancelRequested: consistencyController.cancelScan()
    }
}
