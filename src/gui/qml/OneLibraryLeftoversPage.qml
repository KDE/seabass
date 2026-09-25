// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "Duplicates left in OneLibrary" check (#8), on a page
// of its own: duplicates Clean Up removed from the rekordbox library that
// are still in OneLibrary, from before Clean Up wrote to both. The fix
// stages and unstages from one button, and every leftover it leaves alone
// is named with its reason.
HealthCheckPage {
    id: root

    checkTitle: "Duplicates Left in OneLibrary"

    // The check only runs on a stick with OneLibrary. The hub hides its
    // card until it has, so this is only ever false on a page opened
    // on its own, before or without that.
    readonly property bool checked: consistencyController?.cleanupLeftoversChecked
        || consistencyController?.cleanupLeftoverError.length > 0

    Label {
        objectName: "cleanupLeftoverNotChecked"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: !root.checked && !consistencyController?.busy
        color: Theme.textMuted
        text: "There is no OneLibrary on this stick, so there is nothing for this check to look at."
    }

    Label {
        objectName: "cleanupLeftoverSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: consistencyController?.cleanupLeftoverError.length > 0
            ? consistencyController?.cleanupLeftoverError
            : consistencyController?.cleanupLeftoverCount === 0
            ? "Every duplicate Clean Up removed from the rekordbox library is gone from OneLibrary too."
            : consistencyController?.cleanupLeftoverCount + " duplicate(s) Clean Up removed from the "
              + "rekordbox library are still in OneLibrary. "
              + (consistencyController?.cleanupLeftoverFixableCount > 0
                  ? consistencyController?.cleanupLeftoverFixableCount + " can be removed, their playlist "
                    + "entries moved onto the copy Clean Up kept."
                  : "None of them can be matched to the copy Clean Up kept.")
    }

    // The button first and the note after it, on the page's left line:
    // the same order Cover Art uses for its one action.
    RowLayout {
        visible: root.checked
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "finishCleanupButton"
            visible: consistencyController?.cleanupLeftoverFixableCount > 0
                || (consistencyController?.cleanupLeftoverFixStaged ?? false)
            text: consistencyController?.cleanupLeftoverFixStaged ? "Unstage" : "Finish The Clean Up"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.cleanupLeftoverFixStaged
                ? "Take this back out of the changes to save"
                : "Stage removing these from OneLibrary and moving their playlist entries onto the copy "
                  + "Clean Up kept. Save writes it."
            onClicked: consistencyController?.cleanupLeftoverFixStaged
                ? consistencyController?.unstageCleanupLeftoverFix()
                : consistencyController?.finishCleanupLeftovers()
        }
        Label {
            objectName: "stagedCleanupLeftoversNote"
            visible: consistencyController?.cleanupLeftoverFixStaged ?? false
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    // The few left alone, each with why: a decision the DJ may want
    // to make by hand, so it is named rather than counted.
    Repeater {
        objectName: "cleanupLeftoverHeldBack"
        model: consistencyController?.cleanupLeftoversHeldBack
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "“" + modelData.title + "”" + (modelData.artist.length > 0 ? ", " + modelData.artist : "")
                + ": left alone. " + modelData.reason
        }
    }

    Item { Layout.fillHeight: true }
}
