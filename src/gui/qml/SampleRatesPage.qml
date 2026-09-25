// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "Sample rates" check, on a page of its own. A track
// row without a sample rate means every cue on that track is placed by a
// guess, and the file itself can say what the rate really is. The button
// stages, like every other fix in Library Health.
HealthCheckPage {
    id: root

    checkTitle: "Sample Rates"

    Label {
        objectName: "sampleRateSummary"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: consistencyController?.sampleRateMissingCount === 0
            ? "Every Engine track says what sample rate it is."
            : consistencyController?.sampleRateMissingCount + " Engine track(s) do not say what sample rate "
              + "they are, so every cue on them is placed by a guess. "
              + (consistencyController?.sampleRateFixableCount > 0
                  ? consistencyController?.sampleRateFixableCount
                    + " of their files can say, and Seabass can write it in."
                  : "None of their files could be read to find out.")
    }

    // The button first and the note after it, on the page's left line:
    // the same order Cover Art uses for its one action.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "fillSampleRatesButton"
            visible: consistencyController?.sampleRateFixableCount > 0
                || (consistencyController?.sampleRateFillStaged ?? false)
            text: consistencyController?.sampleRateFillStaged ? "Unstage" : "Fill In From The Files"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.sampleRateFillStaged
                ? "Take this back out of the changes to save"
                : "Stage writing each track's real sample rate, read from the file itself. Save writes it."
            onClicked: consistencyController?.sampleRateFillStaged
                ? consistencyController?.unstageSampleRateFill()
                : consistencyController?.fillSampleRates()
        }
        Label {
            objectName: "stagedSampleRatesNote"
            visible: consistencyController?.sampleRateFillStaged ?? false
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    Item { Layout.fillHeight: true }
}
