// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's front page: run every check this stick's library can be
// put through, then say what each one found in a sentence or two.
//
// The scan happens on arrival rather than behind a button. Checking is what
// this page is for, and a page whose first state is "press here to find
// out" makes the user ask for something they already asked for by opening
// it. It can take a while on a real stick -- reading three catalogs and
// stat-ing a few thousand files -- so each card says what it is waiting
// for, and the ones that finish early report early.
//
// Detail lives behind each card, not on it. The old single page put every
// broken row and every stray cue in one scroll, which answered "what
// exactly is wrong with row 412" well and "is my library alright" badly.
Page {
    id: root
    required property string stickLabel
    required property string rekordboxPath
    required property string enginePath
    required property var playbackController

    // Opens the detailed view, scrolled to the section that matters for
    // the card the user pressed.
    signal detailRequested(string section)

    LibraryConsistencyController {
        id: healthController
    }
    // Handed to the detail page so it shows this scan instead of running
    // its own. Named apart from the id: a property and an id of the same
    // name collide, and the binding would refer to itself.
    readonly property var consistencyController: healthController

    readonly property bool scanning: healthController.busy
    readonly property bool scanned: internal.hasScanned && !root.scanning

    QtObject {
        id: internal
        property bool hasScanned: false
    }

    Component.onCompleted: root.runChecks()

    function runChecks() {
        internal.hasScanned = false;
        healthController.scan(root.rekordboxPath, root.enginePath);
    }

    Connections {
        target: healthController
        function onBusyChanged() {
            if (!healthController.busy) {
                internal.hasScanned = true;
            }
        }
        // Both outcomes get said, in the same place, in so many words.
        // Someone who just watched a stick disappear and come back is in
        // no state to work out from a green line whether it worked.
        function onFilesystemRepairFinished(repaired, declined, message) {
            if (declined) {
                repairOutcomeDialog.severity = SeabassDialog.Info;
                repairOutcomeDialog.title = "Nothing Was Checked";
                repairOutcomeDialog.headline = "The permission prompt was declined, so the stick was left exactly "
                    + "as it was.";
                repairOutcomeDialog.detailText = "Press Check and Repair again when you are ready -- the system "
                    + "asks for your password because the check works on the whole drive.";
            } else if (repaired) {
                repairOutcomeDialog.severity = SeabassDialog.Success;
                repairOutcomeDialog.title = "The Stick Was Repaired";
                repairOutcomeDialog.headline = "The filesystem was checked and repaired, and the stick takes "
                    + "writes again.";
                repairOutcomeDialog.detailText = "The checks on this page have been run again on the repaired "
                    + "stick, so what they say now is what is really there. A check can find files it could not "
                    + "put back where they belong -- worth a look through your playlists before the next gig.";
            } else {
                repairOutcomeDialog.severity = SeabassDialog.Error;
                repairOutcomeDialog.title = "The Stick Was Not Repaired";
                repairOutcomeDialog.headline = message.length > 0 ? message
                    : "The check could not repair this stick's filesystem.";
                repairOutcomeDialog.detailText = "The stick still refuses writes, so nothing on this page can be "
                    + "fixed while it is like this. Copy what is on it first -- Backups can still read it -- and "
                    + "then a fresh format is the way back.";
            }
            repairOutcomeDialog.open();
        }
    }

    MessageDialog {
        id: repairOutcomeDialog
        objectName: "repairOutcomeDialog"
        showReject: false
        acceptText: "OK"
    }

    // --- what each check found, as a sentence -------------------------
    //
    // Written per check rather than from a shared template: "27 rows" and
    // "27 cues" need different sentences, and a template that fits both
    // fits neither well.

    readonly property int brokenCount: healthController.issues.count
    readonly property int repairableCount: healthController.repairableCount
    readonly property int junkCueCount: healthController.junkCues.count
    readonly property int artworkUnreadableCount: healthController.artworkUnreadableCount
    readonly property int artworkRepairableCount: healthController.artworkRepairableCount

    readonly property string brokenSummary: {
        if (root.scanning) {
            return "Checking every row in every catalog against the files on the stick"
                + (healthController.scanningFormat.length > 0
                    ? " (" + healthController.scanningFormat + ")..." : "...");
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (root.brokenCount === 0) {
            return "Every track in every catalog on this stick points at a file that is really there.";
        }
        let text = root.brokenCount === 1
            ? "One track points at a file that is no longer on the stick."
            : root.brokenCount + " tracks point at a file that is no longer on the stick.";
        if (root.repairableCount > 0) {
            text += " " + (root.repairableCount === root.brokenCount ? "All of them" : root.repairableCount + " of them")
                 + " have a healthy copy elsewhere in the same catalog and can be repaired automatically;"
                 + " the rest need a decision.";
        } else {
            text += " None of them has a healthy copy to repair from, so each needs a decision.";
        }
        return text;
    }

    // The stick before its library: a read-only filesystem makes every
    // other finding on this page unfixable, so it is said first and in
    // its own words.
    // Every other card's action writes to the stick, so a read-only one
    // disables them all rather than letting a press fail a thousand times
    // over. The offer stays on screen, greyed, with the reason on hover.
    // Read by Main while this page is in front: the repair below takes
    // the stick away and brings it back on purpose (unmount, check,
    // mount), so the window must not announce it as removed.
    readonly property bool stickAwayExpected: healthController.repairingFilesystem

    readonly property string blockedByReadOnly: healthController.stickReadOnly
        ? "This stick is read-only until its filesystem has been checked -- see the card above."
        : ""

    readonly property string filesystemSummary: {
        if (healthController.repairingFilesystem) {
            return "Checking and repairing this stick's filesystem. Its own permission prompt may ask first.";
        }
        if (healthController.stickReadOnly) {
            return "This stick is mounted read-only, so nothing can be written to it. That is what a stick pulled "
                + "out mid-write leaves behind: the filesystem is damaged and the system refuses to write to it "
                + "until it has been checked. Back it up before the check runs -- reading still works, and a "
                + "copy made now is the last one taken before anything moves.";
        }
        if (healthController.filesystemMessage.length > 0) {
            return healthController.filesystemMessage;
        }
        return "This stick takes writes normally.";
    }

    // Cover art is a property of the Engine library rather than of one
    // track's file, and it is the fault nobody finds by looking: the art
    // simply never appears on the player, with nothing to say why.
    readonly property string artworkSummary: {
        if (root.scanning) {
            return "Checking where each Engine track's cover art is stored...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (healthController.artworkError.length > 0) {
            return healthController.artworkError;
        }
        if (healthController.artworkTracksWithArt === 0) {
            return "No Engine library on this stick, or no track carries cover art.";
        }
        if (root.artworkUnreadableCount === 0) {
            return "Every Engine track's cover art is stored where a player can find it.";
        }
        let text = root.artworkUnreadableCount + " of " + healthController.artworkTracksWithArt
            + " Engine tracks have cover art no player can show.";
        if (root.artworkRepairableCount > 0) {
            text += " " + (root.artworkRepairableCount === root.artworkUnreadableCount ? "All of them"
                                                                                       : root.artworkRepairableCount + " of them")
                 + " can be fixed from the artwork already on this stick.";
        } else {
            text += " None of their images are on this stick, so re-importing in Engine DJ is what would rebuild them.";
        }
        return text;
    }

    readonly property string junkCueSummary: {
        if (root.scanning) {
            return "Looking for memory cues sitting at the very start of a track...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (root.junkCueCount === 0) {
            return "No memory cues are sitting at 0:00.";
        }
        return (root.junkCueCount === 1 ? "One memory cue sits" : root.junkCueCount + " memory cues sit")
             + " at 0:00. These are almost always accidental -- a stray press while the track was at the"
             + " start -- rather than something you placed on purpose.";
    }

    // The same header every other section page uses. This one had the
    // breadcrumb as the bare header, which is why it looked wrong in two
    // ways at once: no toolbar background or margin above the content,
    // and the crumbs spread across the full width, because a Page
    // stretches its header and a RowLayout with nothing to absorb the
    // slack hands it to the gaps between segments. The trailing filler
    // is what keeps them packed to the left.
    header: ToolBar {
        // Opaque background override: KDE's Breeze style bleeds the
        // window behind Seabass through an unstyled ToolBar.
        background: Rectangle { color: Theme.surface }
        // Every side zeroed so the header's inset is Theme.pageMargin
        // and nothing else. `padding` alone does not do it: styles set
        // horizontalPadding or leftPadding of their own on top of it.
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: Theme.headerBottomPadding
        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.pageMargin
            spacing: Theme.rowSpacing
            BackBreadcrumb {
                stack: root.StackView.view
                middleLabel: root.stickLabel
                title: "Library Health"
                onHomeRequested: root.StackView.view.pop(null)
                onBackRequested: root.StackView.view.pop()
            }
            Item { Layout.fillWidth: true }
            BusyIndicator {
                running: root.scanning
                visible: root.scanning
                implicitWidth: Theme.iconSizeSmall
                implicitHeight: Theme.iconSizeSmall
            }
        }
    }

    PageScrollView {
        objectName: "healthScroll"
        anchors.fill: parent
        // Every other page that uses PageScrollView insets its content
        // by this much. This one did not, so its cards ran to the window
        // edge while the breadcrumb above them kept its own spacing, and
        // the page read as broken rather than as tight.
        anchors.margins: Theme.pageMargin

        ColumnLayout {
            objectName: "healthColumn"
            width: parent.width
            spacing: 14

            Subtitle {
                Layout.fillWidth: true
                text: root.scanning
                    ? "Checking this library. This can take a minute on a full stick."
                    : "Everything Seabass can check about this library, and what it found."
            }

            HealthCheckCard {
                objectName: "stickFilesystemCard"
                title: "The stick itself"
                summary: root.filesystemSummary
                running: healthController.repairingFilesystem
                ok: !healthController.stickReadOnly
                failed: healthController.stickReadOnly
                actionLabel: healthController.stickReadOnly ? "Check and Repair" : ""
                actionEnabled: !healthController.repairingFilesystem && !root.scanning
                actionDisabledReason: root.scanning ? "Wait for the scan to finish" : ""
                onActionRequested: healthController.repairStickFilesystem()
            }

            HealthCheckCard {
                objectName: "brokenFilesCard"
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "Tracks and their files"
                summary: root.brokenSummary
                running: root.scanning
                ok: root.brokenCount === 0
                actionLabel: root.brokenCount > 0 ? "Review these tracks" : ""
                onActionRequested: root.detailRequested("broken")
            }

            HealthCheckCard {
                objectName: "junkCuesCard"
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "Memory cues at 0:00"
                summary: root.junkCueSummary
                running: root.scanning
                ok: root.junkCueCount === 0
                actionLabel: root.junkCueCount > 0 ? "Review these cues" : ""
                onActionRequested: root.detailRequested("junkcues")
            }

            HealthCheckCard {
                objectName: "coverArtCard"
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "Cover art"
                summary: root.artworkSummary
                running: root.scanning
                ok: root.artworkUnreadableCount === 0 && healthController.artworkError.length === 0
                failed: healthController.artworkError.length > 0
                actionLabel: root.artworkUnreadableCount > 0 ? "Review cover art" : ""
                onActionRequested: root.detailRequested("artwork")
            }

            // The rekordbox/OneLibrary comparison is specified in
            // docs/library-health-format-divergence.md and not built yet.
            // Deliberately not shown as a card until it can actually
            // report something: an empty check that always says "not
            // checked" teaches people to ignore the page.

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: root.scanned
                text: "Checked " + root.stickLabel + " just now."
                color: Theme.textMuted
                font.pointSize: Theme.fontSmall
            }

            Button {
                objectName: "recheckButton"
                Layout.alignment: Qt.AlignLeft
                visible: root.scanned
                text: "Check again"
                onClicked: root.runChecks()
            }

            Label {
                objectName: "errorLabel"
                Layout.fillWidth: true
                visible: healthController.errorMessage.length > 0
                text: healthController.errorMessage
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
        }
    }
}
