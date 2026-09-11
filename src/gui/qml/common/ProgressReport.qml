// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// What a long operation says about itself while it runs, for every
// operation in the app: the phase it is in, how far along it is, how
// fast, how much longer, what it is working on right now, and how to
// stop it.
//
// One component because the answer should not depend on which page you
// happen to be looking at. Before this, a full stick backup showed
// phases, bytes, a rate and an ETA; a duplicate scan showed a count and
// nothing else; and the two were drawn by different code that happened
// to look similar.
//
// Everything past the bar is optional and hides itself when it has
// nothing to say, so a caller that only knows "3 of 40" passes two
// numbers and gets a sensible strip, while one that also knows bytes and
// a rate gets the fuller version without a second component existing.
ColumnLayout {
    id: report

    // The phases this operation moves through, and which one it is in.
    // Empty means it has no phases worth naming, and the strip vanishes.
    property var phases: []
    property string phase: ""
    property var phaseLabel: function(id) { return id; }
    // Phases whose total is meaningful. In any other phase the bar
    // sweeps rather than claiming a position it does not know -- a bar
    // that reads 0% for a minute is worse than one that admits it is
    // counting.
    property var determinatePhases: []

    // Countable work: files, tracks, rows, whatever this operation does.
    property double unitsDone: 0
    property double unitsTotal: 0
    property string unitName: ""       // "files", "tracks"; blank prints just the numbers

    // Byte-denominated work, when the operation moves data.
    property double bytesDone: 0
    property double bytesTotal: 0
    property double bytesPerSecond: 0
    property int etaSeconds: -1

    // The one item being worked on, if naming it helps.
    property string currentItem: ""

    property bool cancellable: false
    property bool cancelEnabled: true
    property string cancelButtonObjectName: "cancelButton"
    signal cancelRequested()

    // Bytes win when both are known: they are the finer measure, and a
    // file count jumps in steps a person can see stalling.
    readonly property bool hasBytes: report.bytesTotal > 0
    readonly property bool hasUnits: report.unitsTotal > 0
    readonly property bool phaseIsDeterminate:
        report.phases.length === 0 || report.determinatePhases.length === 0
            || report.determinatePhases.indexOf(report.phase) >= 0
    readonly property bool indeterminate: !phaseIsDeterminate || (!report.hasBytes && !report.hasUnits)
    readonly property real fraction: report.hasBytes ? report.bytesDone / report.bytesTotal
        : report.hasUnits ? report.unitsDone / report.unitsTotal : 0

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: report.phases.length > 0
        Repeater {
            model: report.phases
            delegate: Rectangle {
                required property string modelData
                readonly property bool current: modelData === report.phase
                radius: 3
                color: current ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15) : "transparent"
                border.color: current ? Theme.accent : "transparent"
                implicitWidth: phaseText.implicitWidth + 16
                implicitHeight: phaseText.implicitHeight + 6
                Label {
                    id: phaseText
                    anchors.centerIn: parent
                    text: report.phaseLabel(parent.modelData)
                    color: parent.current ? Theme.accent : Theme.textMuted
                    font.bold: parent.current
                }
            }
        }
    }

    ProgressTrack {
        objectName: "progressTrack"
        Layout.fillWidth: true
        indeterminate: report.indeterminate
        value: report.indeterminate ? 0 : report.fraction
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        visible: report.hasUnits || report.hasBytes || report.bytesPerSecond > 0 || report.etaSeconds >= 0

        Label {
            objectName: "unitsLabel"
            visible: report.hasUnits
            font.family: Theme.dataFamily
            text: report.unitsDone + " / " + report.unitsTotal
                + (report.unitName.length > 0 ? " " + report.unitName : "")
        }
        Label { visible: report.hasUnits && report.hasBytes; text: "·"; color: Theme.textMuted }
        Label {
            objectName: "bytesLabel"
            visible: report.hasBytes
            font.family: Theme.dataFamily
            text: Theme.humanBytes(report.bytesDone) + " of " + Theme.humanBytes(report.bytesTotal)
        }
        Label { visible: report.bytesPerSecond > 0; text: "·"; color: Theme.textMuted }
        Label {
            objectName: "rateLabel"
            visible: report.bytesPerSecond > 0
            font.family: Theme.dataFamily
            text: (report.bytesPerSecond / (1024 * 1024)).toFixed(1) + " MiB/s"
        }
        Item { Layout.fillWidth: true }
        Label {
            objectName: "etaLabel"
            visible: report.etaSeconds >= 0
            color: Theme.textMuted
            text: Theme.humanDuration(report.etaSeconds) + " remaining"
        }
    }

    Label {
        objectName: "currentItemLabel"
        visible: report.currentItem.length > 0
        Layout.fillWidth: true
        elide: Text.ElideMiddle
        color: Theme.textMuted
        font.pointSize: Theme.fontSmall
        text: report.currentItem
    }

    Button {
        objectName: report.cancelButtonObjectName
        Layout.alignment: Qt.AlignRight
        visible: report.cancellable
        enabled: report.cancelEnabled
        text: "Cancel"
        onClicked: report.cancelRequested()
    }
}
