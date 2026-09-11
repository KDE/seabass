// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// A big, centered "this is busy" overlay for a page's content area --
// used in place of the small progress bar that used to live tucked into
// the header's toolbar. Every action that drives `busy` on these pages
// already disables the header's own buttons while busy, so the rest of
// the page underneath is genuinely unusable for the duration anyway; a
// small corner indicator was easy to miss for exactly the state where the
// user is most likely to be waiting and wondering if anything's happening.
// Deliberately does not cover the header itself (only anchors.fill's the
// content area it's placed in) -- the header's own disabled-but-visible
// controls (search, format toggle, "Back") already communicate that
// state, and covering them too would hide the one thing (Back, if
// enabled) still meaningful mid-write.
Item {
    id: root
    required property bool busy
    property int current: 0
    property int total: 0
    property string label: "Working..."
    // A read-only scan can be stopped at any time (nothing to keep
    // consistent); the page then pops back to where the user came from.
    // Off for writes -- those are cancelled from WriteProgressDialog,
    // which stops at the next consistent point instead.
    property bool cancellable: false
    signal cancelRequested()

    // Passed straight through to ProgressReport. A caller that knows
    // more than "n of m" -- which phase it is in, which file it is on --
    // can now say so without the overlay needing to grow a second body.
    property var phases: []
    property string phase: ""
    property var phaseLabel: function(id) { return id; }
    property string unitName: ""
    property string currentItem: ""

    visible: root.busy
    z: 1000

    // Estimated-remaining-time support. Deliberately withheld for the
    // first 10 seconds -- an ETA computed from only a couple of ticks'
    // worth of progress is noise, not a useful number, and would just
    // flicker/jump around for the many scans that finish in a couple of
    // seconds anyway.
    property real _startTimeMs: 0
    property real _nowMs: 0
    readonly property real _elapsedSeconds: root.busy ? (root._nowMs - root._startTimeMs) / 1000 : 0
    readonly property string etaText: {
        if (root._elapsedSeconds < 10 || root.total <= 0 || root.current <= 0 || root.current >= root.total) {
            return "";
        }
        var rate = root.current / root._elapsedSeconds;
        if (rate <= 0) return "";
        var remainingSeconds = Math.max(0, (root.total - root.current) / rate);
        if (remainingSeconds < 60) {
            return "~" + Math.round(remainingSeconds) + "s remaining";
        }
        var minutes = Math.floor(remainingSeconds / 60);
        var seconds = Math.round(remainingSeconds % 60);
        return "~" + minutes + "m " + seconds + "s remaining";
    }

    onBusyChanged: {
        if (root.busy) {
            root._startTimeMs = Date.now();
            root._nowMs = root._startTimeMs;
            etaTimer.restart();
        } else {
            etaTimer.stop();
        }
    }

    Timer {
        id: etaTimer
        interval: 1000
        repeat: true
        onTriggered: root._nowMs = Date.now()
    }

    // Dims rather than fully hides -- makes clear the same content is
    // still there and will return, not that the page has been replaced.
    Rectangle {
        anchors.fill: parent
        color: Theme.background
        opacity: 0.72
    }

    MouseArea {
        // Absorbs clicks/hover so nothing scrollable/clickable underneath
        // reacts while this is up, even for controls that don't otherwise
        // bind their own `enabled` to busy.
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        onClicked: {}
    }

    // The body is ProgressReport, so this overlay and the inline
    // progress on the backup/clone/restore pages are the same thing
    // drawn in the same way -- the bar, the counts, the ETA and the
    // Cancel button all come from one component now rather than three
    // copies that had drifted into three slightly different bars.
    //
    // The overlay's own job is what is left: dimming the page, holding
    // the label above the report, and keeping it centred.
    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12
        width: Math.min(parent.width - 64, 420)

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.label
            font.bold: true
            font.pointSize: Theme.fontLarge
        }

        ProgressReport {
            objectName: "progressReport"
            Layout.fillWidth: true
            phases: root.phases
            phase: root.phase
            phaseLabel: root.phaseLabel
            unitsDone: root.current
            unitsTotal: root.total
            unitName: root.unitName
            currentItem: root.currentItem
            // The overlay computes its own ETA from elapsed time, since
            // most of its callers count items rather than bytes and have
            // no rate to report. Handed over as a formatted string so
            // ProgressReport does not need two ways to say the same
            // thing; see etaText above.
            etaSeconds: -1
            cancellable: root.cancellable
            onCancelRequested: root.cancelRequested()
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: root.etaText.length > 0
            text: root.etaText
            color: Theme.textMuted
            font.pointSize: Theme.fontSmall
        }
    }
}