// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "Beat grids" page: rekordbox's beat grids into the
// Engine library, the command line's copy-beatgrids with the same checks.
// The page opens on a preview (BeatGridsController): what a copy would do,
// counted, with nothing written. The button stages a copy of every grid
// the preview would write, and the floating Save writes them, backed up
// first, like every other fix in Library Health.
//
// Tracks the player has not analysed are left alone, and the page says
// why; the command line's Fill modes, which give such a row a grid of its
// own, are for testing on hardware and are not offered here.
HealthCheckPage {
    id: root

    checkTitle: "Beat Grids"

    // For a test to read what the page shows.
    readonly property var beatGridsController: beatGrids

    BeatGridsController {
        id: beatGrids
    }

    Component.onCompleted: beatGrids.analyze(root.rekordboxPath, root.enginePath)

    // "One beat grid" or "12 beat grids", for the sentences below.
    function counted(n, one, many) {
        return n === 1 ? one : n + " " + many;
    }

    readonly property bool showCounts: beatGrids.analysed && !beatGrids.busy && !beatGrids.engineTooOld
    readonly property bool staged: beatGrids.stagedCount > 0

    Label {
        objectName: "beatGridIntro"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: "Copies the beat grids from rekordbox into the Engine library, so a Denon player plays to the "
            + "same grid as a Pioneer one. Engine keeps its own analysis beside it, and the player can go back to it."
    }

    // The one bar of the check: reading the libraries, then each track.
    ProgressReport {
        objectName: "beatGridProgress"
        Layout.fillWidth: true
        visible: beatGrids.busy
        unitsDone: beatGrids.scanCurrent
        unitsTotal: beatGrids.scanTotal
        currentItem: beatGrids.scanLabel
    }

    Label {
        objectName: "beatGridError"
        Layout.fillWidth: true
        visible: beatGrids.errorMessage.length > 0
        wrapMode: Text.WordWrap
        color: Theme.danger
        text: beatGrids.errorMessage
    }

    Label {
        objectName: "beatGridEngineTooOld"
        Layout.fillWidth: true
        visible: beatGrids.engineTooOld
        wrapMode: Text.WordWrap
        color: Theme.warnText
        text: "This Engine library was made by Engine 1.x, whose beat grids Seabass does not write. Once Engine DJ "
            + "has updated the library, its grids can be copied here."
    }

    Label {
        objectName: "beatGridLedgerWarning"
        Layout.fillWidth: true
        visible: beatGrids.ledgerWarning.length > 0
        wrapMode: Text.WordWrap
        color: Theme.warnText
        text: beatGrids.ledgerWarning
    }

    // What a copy would do now, a line for each count that is not zero.
    ColumnLayout {
        objectName: "beatGridCounts"
        Layout.fillWidth: true
        visible: root.showCounts
        spacing: Theme.tightSpacing

        Label {
            objectName: "beatGridNothingMatched"
            Layout.fillWidth: true
            visible: beatGrids.matchedCount === 0
            wrapMode: Text.WordWrap
            text: "No track is in both libraries, so there is no beat grid to copy."
        }
        Label {
            objectName: "beatGridWouldCopy"
            Layout.fillWidth: true
            visible: beatGrids.wouldCopyCount > 0
            wrapMode: Text.WordWrap
            text: root.counted(beatGrids.wouldCopyCount, "One beat grid", "beat grids") + " can be copied."
        }
        Label {
            objectName: "beatGridAlreadySame"
            Layout.fillWidth: true
            visible: beatGrids.alreadySameCount > 0
            wrapMode: Text.WordWrap
            text: (beatGrids.alreadySameCount === 1 ? "One is" : beatGrids.alreadySameCount + " are")
                + " already the same in Engine."
        }
        Label {
            objectName: "beatGridNotAnalysed"
            Layout.fillWidth: true
            visible: beatGrids.notAnalysedCount > 0
            wrapMode: Text.WordWrap
            text: root.counted(beatGrids.notAnalysedCount, "One track has", "tracks have")
                + " not been analysed by the player yet. The player analyses a track the first time it is "
                + "loaded, and its grid can be copied after that."
        }
        Label {
            objectName: "beatGridLocked"
            Layout.fillWidth: true
            visible: beatGrids.lockedCount > 0
            wrapMode: Text.WordWrap
            text: root.counted(beatGrids.lockedCount, "One track has its grid", "tracks have their grid")
                + " locked on the player."
        }
        Label {
            objectName: "beatGridEdited"
            Layout.fillWidth: true
            visible: beatGrids.editedCount > 0
            wrapMode: Text.WordWrap
            text: root.counted(beatGrids.editedCount, "One track has its grid", "tracks have their grid")
                + " changed on the player."
        }
        Label {
            objectName: "beatGridNoRekordboxGrid"
            Layout.fillWidth: true
            visible: beatGrids.noRekordboxGridCount > 0
            wrapMode: Text.WordWrap
            text: root.counted(beatGrids.noRekordboxGridCount, "One track has", "tracks have")
                + " no beat grid in rekordbox."
        }
        Label {
            objectName: "beatGridUnreadable"
            Layout.fillWidth: true
            visible: beatGrids.unreadableCount > 0
            wrapMode: Text.WordWrap
            color: Theme.warnText
            text: beatGrids.unreadableCount === 1
                ? "One track could not be read to check it, and is left alone."
                : beatGrids.unreadableCount + " tracks could not be read to check them, and are left alone."
        }
    }

    // On the page's left line: the box itself, not its padding.
    SeabassCheckBox {
        objectName: "replaceEditsCheckBox"
        visible: !beatGrids.engineTooOld
        leftPadding: 0
        text: "Also replace grids locked or changed on the player"
        checked: beatGrids.replaceEngineEdits
        enabled: !beatGrids.busy && !beatGrids.writing && !root.staged
        ToolTip.visible: hovered && root.staged
        ToolTip.text: "Unstage the copy to change this"
        onToggled: beatGrids.replaceEngineEdits = checked
    }

    // The button first and the note after it, as Sample Rates has them;
    // the list of what it copies under them.
    RowLayout {
        Layout.fillWidth: true
        visible: !beatGrids.engineTooOld
        spacing: Theme.rowSpacing
        Button {
            objectName: "copyBeatGridsButton"
            text: root.staged ? "Unstage" : "Copy Beat Grids"
            enabled: !beatGrids.busy && !beatGrids.writing && !(consistencyController?.stickReadOnly ?? false)
                && (root.staged || beatGrids.wouldCopyCount > 0)
            ToolTip.visible: hovered
            ToolTip.text: (consistencyController?.stickReadOnly ?? false)
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : root.staged ? "Take this back out of the changes to save"
                : "Stage copying these beat grids into the Engine library. Save writes them."
            onClicked: root.staged ? beatGrids.unstage() : beatGrids.stage()
        }
        Label {
            objectName: "stagedBeatGridsNote"
            visible: root.staged
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    ListView {
        id: trackList
        objectName: "beatGridTrackList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: root.showCounts && count > 0
        clip: true
        model: beatGrids.tracks
        bottomMargin: root.saveClearance
        spacing: Theme.tightSpacing
        ScrollBar.vertical: BigScrollBar {}
        delegate: ColumnLayout {
            id: trackRow
            required property string title
            required property string artist
            width: ListView.view.width
            spacing: 0
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.bold: true
                text: trackRow.title
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                elide: Text.ElideRight
                color: Theme.textMuted
                text: trackRow.artist
            }
        }
    }

    Item {
        Layout.fillHeight: true
        visible: !trackList.visible
    }
}
