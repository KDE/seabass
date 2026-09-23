// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// "27 cues removed", or "5 of 31 tracks synchronised" when some of it
// did not happen: what the user asked for and how much of it took
// effect, shown after it finished, was cancelled, or failed. OK is the
// only way out; the page decides what happens after (stay, or go back
// to where the user came from).
//
// summary: {written, total, unit, cancelled, error} plus an optional
// verb ("written" by default; "deleted" for a prune), an optional
// detail line that replaces the default cancel explanation, and an
// optional skipped count (part of total, not of written).
SeabassDialog {
    id: dialog
    property var summary: ({})

    readonly property int written: dialog.summary.written !== undefined ? dialog.summary.written : 0
    readonly property int total: dialog.summary.total !== undefined ? dialog.summary.total : 0
    readonly property string unit: dialog.summary.unit !== undefined ? dialog.summary.unit : "items"
    readonly property string verb: dialog.summary.verb !== undefined ? dialog.summary.verb : "written"
    readonly property bool cancelled: dialog.summary.cancelled === true
    readonly property string error: dialog.summary.error !== undefined ? dialog.summary.error : ""
    // Everything was written, but something after it was not finished --
    // a write-ahead log that would not fold. Never "Done" in silence:
    // before this existed the same state was reported as an error, and a
    // warning nobody renders is worse than the error it replaced.
    readonly property string warning: dialog.summary.warning !== undefined ? dialog.summary.warning : ""
    readonly property string detail: dialog.summary.detail !== undefined ? dialog.summary.detail : ""
    // Reached by the save and gone by then: a track deleted, or an image
    // that stopped reading, since the page scanned. Counted in total and
    // not in written, so the count sentence already shows a shortfall;
    // this says where it went, which the count alone cannot.
    readonly property int skipped: dialog.summary.skipped !== undefined ? dialog.summary.skipped : 0
    readonly property string skippedSentence: {
        if (dialog.skipped <= 0) {
            return "";
        }
        const noun = dialog.skipped === 1 ? dialog.singularUnit : dialog.unit;
        return dialog.skipped + " " + noun + " skipped, because "
            + (dialog.skipped === 1 ? "it" : "they")
            + " changed on the stick after the scan. The stick's log names "
            + (dialog.skipped === 1 ? "it." : "each one.");
    }

    // "27 cues removed." when everything staged went through, and
    // "5 of 31 cues removed." only when it did not.
    //
    // "27 of 27" is a report of a loop rather than an answer: the reader
    // has to compare the two numbers to learn that nothing was left, and
    // the interesting case -- some of it did not happen -- is the one
    // that looks identical at a glance. Naming the shortfall only when
    // there is one makes the two outcomes different shapes.
    readonly property string countSentence: {
        // The noun agrees with the number it follows, which is the
        // total in both shapes: "1 cue added", but "1 of 9 groups
        // cleaned up" -- there are nine groups, one of which was done.
        // Singularising on the count instead gives "1 of 9 group", which
        // a test caught.
        const noun = dialog.total === 1 ? dialog.singularUnit : dialog.unit;
        const count = dialog.written === dialog.total
            ? String(dialog.written)
            : dialog.written + " of " + dialog.total;
        return count + " " + noun + " " + dialog.verb + ".";
    }

    // "1 cues added" is the same kind of wrongness one word smaller.
    // Enough English for the nouns this app actually counts -- cues,
    // tracks, entries, groups, settings, files -- rather than a general
    // pluraliser it has no use for.
    readonly property string singularUnit: {
        if (dialog.unit.endsWith("ies")) {
            return dialog.unit.substring(0, dialog.unit.length - 3) + "y";
        }
        if (dialog.unit.endsWith("s")) {
            return dialog.unit.substring(0, dialog.unit.length - 1);
        }
        return dialog.unit;
    }

    function show(newSummary) {
        dialog.summary = newSummary;
        dialog.open();
    }

    severity: dialog.error.length > 0 ? SeabassDialog.Error
        : ((dialog.warning.length > 0 || dialog.skipped > 0) && !dialog.cancelled) ? SeabassDialog.Warning
        : dialog.cancelled ? SeabassDialog.Warning
        : SeabassDialog.Info
    closePolicy: Popup.NoAutoClose
    title: dialog.error.length > 0 ? "Stopped with an error"
        : (dialog.warning.length > 0 && !dialog.cancelled) ? "Done, with something left over"
        : dialog.cancelled ? "Cancelled"
        : dialog.skipped > 0 ? "Done, with some skipped"
        : "Done"

    footer: DialogButtonBox {
        Button {
            Keys.onReturnPressed: root.activateFooterSelection()
            Keys.onEnterPressed: root.activateFooterSelection()
            Keys.onLeftPressed: root.moveFooterSelection(-1)
            Keys.onRightPressed: root.moveFooterSelection(1)
            objectName: "okButton"
            text: "OK"
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            objectName: "countLabel"
            color: Theme.text
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.family: Theme.titleFamily
            font.weight: Theme.cardTitleWeight
            font.pointSize: Theme.fontMedium
            text: dialog.countSentence
        }
        // Selectable, because when this carries an error it is the text
        // someone will be asked to paste into a bug report.
        SelectableText {
            objectName: "detailLabel"
            visible: text.length > 0
            color: dialog.error.length > 0 ? Theme.danger : Theme.textMuted
            text: [dialog.skippedSentence,
                   dialog.error.length > 0 ? "Then: " + dialog.error
                   : (dialog.warning.length > 0 && dialog.cancelled)
                       ? "Stopped at your request. " + dialog.warning
                   : dialog.warning.length > 0 ? dialog.warning
                   : dialog.detail.length > 0 ? dialog.detail
                   : dialog.cancelled ? "Stopped at your request. Everything up to here is complete; the rest was not touched."
                   : ""].filter(part => part.length > 0).join(" ")
        }
    }
}
