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

    // "Back up before you let anything near this stick." A filesystem
    // check moves and drops things; on a stick that is already damaged
    // the copy comes first, and the page offers it rather than telling
    // someone to go and find it.
    signal backupRequested(string mountPoint)

    // The stick's own root: the catalog paths sit one level inside it.
    readonly property string stickRoot: {
        var path = root.enginePath.length > 0 ? root.enginePath : root.rekordboxPath;
        var cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return cut > 0 ? path.substring(0, cut) : path;
    }

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
                repairOutcomeDialog.detailText = "Press Check and Repair again when you are ready. The system "
                    + "asks for your password because the check works on the whole drive.";
            } else if (repaired) {
                repairOutcomeDialog.severity = SeabassDialog.Success;
                repairOutcomeDialog.title = "The Stick Was Repaired";
                repairOutcomeDialog.headline = "The filesystem was checked and repaired, and the stick takes "
                    + "writes again.";
                repairOutcomeDialog.detailText = "The checks on this page have been run again on the repaired "
                    + "stick, so what they say now is what is really there. A check can find files it could not "
                    + "put back where they belong, so a look through your playlists before the next gig is worth it.";
            } else {
                repairOutcomeDialog.severity = SeabassDialog.Error;
                repairOutcomeDialog.title = "The Stick Was Not Repaired";
                repairOutcomeDialog.headline = message.length > 0 ? message
                    : "The check could not repair this stick's filesystem.";
                repairOutcomeDialog.detailText = "The stick still refuses writes, so nothing on this page can be "
                    + "fixed while it is like this. Copy what is on it first (Backups can still read it) and "
                    + "then a fresh format is the way back.";
            }
            repairOutcomeDialog.open();
        }
    }

    // Pressed Check and Repair: the copy is offered first, every time.
    // A check on a damaged filesystem is the last moment at which what is
    // still readable can be saved -- it moves files it cannot place into
    // lost+found and drops what it cannot make sense of.
    MessageDialog {
        id: repairAdviceDialog
        objectName: "repairAdviceDialog"
        severity: SeabassDialog.Warning
        title: "Back Up This Stick First?"
        headline: "The check repairs the filesystem in place: it moves files it cannot put back where they belong, "
            + "and drops what it cannot make sense of at all. Whatever is still readable is readable now."
        detailText: "A backup still works on a read-only stick, because it only reads, and Seabass marks one taken now "
            + "as an emergency copy, so a later restore says plainly what it holds. It takes as long as the stick "
            + "is big; the check will still be here afterwards."
        alternateText: "Back Up First"
        acceptText: "Check and Repair"
        rejectText: "Cancel"
        onAlternateRequested: root.backupRequested(root.stickRoot)
        onAccepted: healthController.repairStickFilesystem()
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
        ? "This stick is read-only until its filesystem has been checked. See the card above."
        : ""

    readonly property string filesystemSummary: {
        if (healthController.repairingFilesystem) {
            return "Checking and repairing this stick's filesystem. Its own permission prompt may ask first.";
        }
        if (healthController.stickReadOnly) {
            return "This stick is mounted read-only, so nothing can be written to it. That is what a stick pulled "
                + "out mid-write leaves behind: the filesystem is damaged and the system refuses to write to it "
                + "until it has been checked. Back it up before the check runs: reading still works, and a "
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
                 + " can be rebuilt, from the rekordbox art on this stick, the tracks' own tags, or a backup on "
                 + "this computer.";
        } else {
            text += " No copy of them was found on this stick, in the tracks themselves, or in a backup, so "
                 + "re-importing in Engine DJ is what would rebuild them.";
        }
        return text;
    }

    readonly property string importPromptSummary: {
        if (root.scanning) {
            return "Checking whether a player will offer to import the rekordbox library...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (!healthController.playerWillOfferImport) {
            return "A player will leave this stick's Engine library alone: it already knows the rekordbox "
                + "library beside it.";
        }
        return "On the next insert, a Denon player will ask whether to update the Engine library from the "
            + "rekordbox one on this stick, and say that existing playlist and track metadata will be "
            + "overwritten. That is what it means: accepting replaces the Engine side, cues, playlists and "
            + "cover art included, with whatever the rekordbox library holds. Seabass can tell Engine the "
            + "library is already imported, and the question stops being asked.";
    }

    readonly property int sampleRateMissingCount: healthController.sampleRateMissingCount
    readonly property int sampleRateFixableCount: healthController.sampleRateFixableCount

    readonly property string sampleRateSummary: {
        if (root.scanning) {
            return "Checking whether every Engine track says what sample rate it is...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (healthController.sampleRateError.length > 0) {
            return healthController.sampleRateError;
        }
        if (root.sampleRateMissingCount === 0) {
            return "Every Engine track says what sample rate it is, so its cues sit where they were put.";
        }
        let text = root.sampleRateMissingCount + " Engine track(s) do not say what sample rate they are. Engine "
            + "stores cue positions as a count of samples, so a player and Seabass both have to guess the rate to "
            + "turn those into times, and a guess of 44.1 kHz on a 48 kHz track puts a cue five minutes in "
            + "almost half a minute out of place.";
        if (root.sampleRateFixableCount > 0) {
            text += " " + (root.sampleRateFixableCount === root.sampleRateMissingCount ? "Every one of their files"
                                                                                       : root.sampleRateFixableCount
                                                                                         + " of their files")
                 + " says what the rate really is, and Seabass can write it into the library.";
        } else {
            text += " None of their files could be read to find out, so there is nothing to copy in.";
        }
        return text;
    }

    // #38. Deliberately has no action: see engine_analysis_state.hpp, and
    // the wording says so outright rather than leaving people waiting for
    // a button that is never coming.
    readonly property string analysisSummary: {
        if (root.scanning) {
            return "Counting the tracks an Engine player will have to analyse...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (healthController.analysisError.length > 0) {
            return healthController.analysisError;
        }
        if (!healthController.analysisKnown) {
            return "This Engine library does not record whether its tracks have been analysed, so there is "
                 + "nothing to report. Engine 1.x libraries predate that.";
        }
        if (healthController.analysisNotAnalyzedCount === 0) {
            return "Every Engine track has been analysed, so each one loads instantly with its waveform already "
                 + "drawn.";
        }
        return healthController.analysisNotAnalyzedCount + " of " + healthController.analysisTracksChecked
             + " tracks have not been analysed for Engine players. The player analyses each one the first time "
             + "it is loaded, which takes a moment, happens once, and is written back to the stick -- but that "
             + "moment is spent on the deck, and the waveform only appears when it finishes. Load them once "
             + "before the gig, or let Engine DJ analyse the library, and the first load at the gig is instant. "
             + "Seabass does not do this itself: the analysis is the player's own beatgrid, waveform and key "
             + "detection, and anything written here instead would be a worse guess that also stopped the "
             + "player ever doing it properly.";
    }

    readonly property string junkCueSummary: {
        if (root.scanning) {
            return "Looking for cues sitting at the very start of a track...";
        }
        if (!root.scanned) {
            return "Not checked yet.";
        }
        if (root.junkCueCount === 0) {
            return "No cues are sitting at 0:00.";
        }
        return (root.junkCueCount === 1 ? "One cue sits" : root.junkCueCount + " cues sit")
             + " at 0:00. These are almost always accidental: a stray press while the track was at the"
             + " start, rather than something you placed on purpose.";
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
                onActionRequested: repairAdviceDialog.open()
            }

            HealthCheckCard {
                objectName: "brokenFilesCard"
                fixableCount: root.repairableCount
                foundCount: root.brokenCount
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
                // Every stray cue can be taken out; the count is there to
                // say so, not to qualify it.
                fixableCount: root.junkCueCount
                foundCount: root.junkCueCount
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "Cues at 0:00"
                summary: root.junkCueSummary
                running: root.scanning
                ok: root.junkCueCount === 0
                actionLabel: root.junkCueCount > 0 ? "Review these cues" : ""
                onActionRequested: root.detailRequested("junkcues")
            }

            HealthCheckCard {
                objectName: "importPromptCard"
                fixableCount: healthController.playerWillOfferImport ? 1 : 0
                foundCount: healthController.playerWillOfferImport ? 1 : 0
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "The player's import prompt"
                summary: root.importPromptSummary
                running: root.scanning
                ok: !healthController.playerWillOfferImport
                actionLabel: healthController.playerWillOfferImport ? "Review this" : ""
                onActionRequested: root.detailRequested("import")
            }

            HealthCheckCard {
                objectName: "sampleRateCard"
                fixableCount: root.sampleRateFixableCount
                foundCount: root.sampleRateMissingCount
                actionEnabled: !healthController.stickReadOnly
                actionDisabledReason: root.blockedByReadOnly
                title: "Sample rates"
                summary: root.sampleRateSummary
                running: root.scanning
                ok: root.sampleRateMissingCount === 0 && healthController.sampleRateError.length === 0
                failed: healthController.sampleRateError.length > 0
                actionLabel: root.sampleRateMissingCount > 0 ? "Review sample rates" : ""
                onActionRequested: root.detailRequested("samplerates")
            }

            HealthCheckCard {
                objectName: "analysisStateCard"
                // No actionLabel, ever: the card hides its whole action
                // row when that is empty, which is what a check that only
                // reports should look like.
                //
                // And no tally either, deliberately. The tally reads
                // "<fixable> / <found>", so giving this one a count would
                // draw "0 / 1214" in red -- "Seabass can fix none of
                // these" -- when the truth is that fixing them is the
                // player's job and doing it here would be worse. The
                // number belongs in the sentence, where it comes with
                // that explanation attached.
                title: "Track analysis"
                summary: root.analysisSummary
                running: root.scanning
                ok: healthController.analysisError.length === 0
                    && (!healthController.analysisKnown || healthController.analysisNotAnalyzedCount === 0)
                failed: healthController.analysisError.length > 0
            }

            HealthCheckCard {
                objectName: "coverArtCard"
                fixableCount: root.artworkRepairableCount
                foundCount: root.artworkUnreadableCount
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
