// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "The player's import prompt" check, on a page of its
// own: the one check about what a Denon player will do the next time the
// stick is in it, and the only one whose fix is telling another program
// something rather than changing what is on the stick.
HealthCheckPage {
    id: root

    checkTitle: "The Player's Import Prompt"

    Label {
        objectName: "importPromptSummary"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: consistencyController?.playerWillOfferImport
            ? "A player will ask whether to update the Engine library from the rekordbox library on this "
              + "stick, warning that existing playlist and track metadata will be overwritten. Accepting "
              + "replaces the Engine side, cues and cover art included."
            : "A player will leave the Engine library alone: it already knows the rekordbox library "
              + "beside it."
    }

    // The button first and the note after it, on the page's left line:
    // the same order Cover Art uses for its one action.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "markImportedButton"
            visible: consistencyController?.playerWillOfferImport || (consistencyController?.importMarkStaged ?? false)
            text: consistencyController?.importMarkStaged ? "Unstage" : "Mark As Already Imported"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.importMarkStaged
                ? "Take this back out of the changes to save"
                : "Writes the rekordbox library's own sequence number into the Engine library, which is what "
                  + "the player compares. Nothing else changes, and importing stays available on the player "
                  + "if you ever do want it."
            onClicked: consistencyController?.importMarkStaged
                ? consistencyController?.unstageRekordboxImportMark()
                : consistencyController?.markRekordboxImported()
        }
        Label {
            objectName: "stagedImportMarkNote"
            visible: consistencyController?.importMarkStaged ?? false
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    Item { Layout.fillHeight: true }
}
