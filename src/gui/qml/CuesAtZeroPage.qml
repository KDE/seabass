// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's "Cues at 0:00" check, on a page of its own: every cue
// that looks accidental rather than placed, each with why it is here, its
// waveform with that cue picked out, and Remove or Ignore beside it.
//
// Not Clean Up Stray Cues (JunkCuePage), although it lists the same
// thing. That page is reached from Housekeeping, scans for itself and can
// be narrowed to one playlist; this one shows the hub's own scan of the
// whole library, so the number on the card and the number here are the
// same number.
HealthCheckPage {
    id: root
    required property var playbackController

    checkTitle: "Cues at 0:00"

    MessageDialog {
        id: confirmRemoveJunkCueDialog
        property int pendingIndex: -1
        severity: SeabassDialog.Question
        title: "Stage removing this cue?"
        headline: "Removes this cue sitting at 0:00 from the track."
        detailText: "Backed up first."
        acceptText: "Stage Removal"
        onAccepted: if (pendingIndex >= 0) consistencyController?.removeJunkCue(pendingIndex)
    }

    MessageDialog {
        id: confirmRemoveAllJunkCuesDialog
        severity: SeabassDialog.Warning
        destructive: true
        // The model's count, the same number the card on the hub says.
        title: "Stage removing all " + consistencyController?.junkCues.count + " cue(s) that look accidental?"
        headline: "This stages removing every cue at 0:00 currently listed, across every catalog on "
            + "this stick. Once you press Save that is a real write, not just dismissing them from view."
        detailText: "Everything is backed up first, but make sure this is really what you want."
        acceptText: "Stage Removal"
        onAccepted: consistencyController?.removeAllJunkCues()
    }

    MessageDialog {
        id: confirmIgnoreAllJunkCuesDialog
        severity: SeabassDialog.Question
        title: "Ignore all cues at 0:00"
        headline: "Dismisses every cue at 0:00 currently listed, just for this view."
        detailText: "Nothing is written, they'll show up again the next time you scan."
        acceptText: "Ignore All"
        onAccepted: consistencyController?.ignoreAllJunkCues()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Label {
            objectName: "cuesAtZeroSummary"
            // Two checks feed this list now: a cue at the very
            // start of a track, and one of a crowd of hot cues in
            // its first two seconds (#41). Naming only the first
            // would describe a row at 1.2 s as being at 0:00.
            text: consistencyController?.junkCues.count === 0
                ? "No cues look accidental."
                : "I found " + consistencyController?.junkCues.count
                  + " cue(s) that look accidental rather than placed"
        }
        Item { Layout.fillWidth: true }
        Label {
            objectName: "stagedJunkCuesNote"
            visible: consistencyController?.stagedJunkCueCount > 0
            text: consistencyController?.stagedJunkCueCount + " staged, not saved yet"
            color: Theme.warnText
        }
        Button {
            visible: consistencyController?.junkCues.count > 0
            text: "Remove All"
            enabled: !consistencyController?.busy && !consistencyController?.stickReadOnly
                && consistencyController?.unstagedJunkCueCount > 0
            ToolTip.visible: hovered
            // It stages; it does not remove. The row buttons
            // beside it and the confirmation this opens both
            // said so already -- this one promised an
            // immediate permanent delete, which is the wrong
            // thing to tell someone in both directions: they
            // either avoid a reversible action thinking it is
            // final, or click it and believe the cues are
            // already gone.
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.unstagedJunkCueCount === 0
                ? "Every one of them is staged already. Press Save to write it."
                : "Stage removing every cue at 0:00 listed, in all catalogs. Save writes it."
            onClicked: confirmRemoveAllJunkCuesDialog.open()
        }
        Button {
            visible: consistencyController?.junkCues.count > 0
            text: "Ignore All"
            enabled: !consistencyController?.busy
            ToolTip.visible: hovered
            ToolTip.text: "Hide these from this view only. Nothing on the stick changes."
            onClicked: confirmIgnoreAllJunkCuesDialog.open()
        }
    }

    // Room to scroll the last row clear of the Save overlay (bottom right),
    // and the one BigScrollBar every other list in this app has.
    ListView {
        id: junkCueListView
        objectName: "junkCueList"
        bottomMargin: 80
        // Not draggable when everything already fits.
        interactive: contentHeight > height
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: consistencyController?.junkCues
        spacing: 10
        ScrollBar.vertical: BigScrollBar {}

        // Was a bare format-badge + title/artist line with no
        // way to see or hear the cue in question at all --
        // the same shared track delegate the missing-file
        // detail view and Sync/Duplicates use elsewhere, so
        // this row shows a real waveform with the 0:00 memory
        // cue about to be removed, and a Play button that
        // simply didn't exist here before.
        delegate: ColumnLayout {
            id: junkDelegate
            width: ListView.view.width
            spacing: 4

            required property int index
            required property var track
            required property bool staged
            required property string reason
            required property real positionMs

            // Why this row is here, in its own words. One
            // sentence for the whole section cannot cover
            // both a cue at 0:00 and one of three pads
            // inside two seconds, and every row here is an
            // offer to delete somebody's cue.
            Label {
                objectName: "junkCueReason"
                visible: junkDelegate.reason.length > 0
                text: junkDelegate.reason
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                font.pointSize: Theme.fontSmall
                color: Theme.textMuted
            }

            TrackWaveformCard {
                Layout.fillWidth: true
                track: junkDelegate.track
                formatLabelText: root.formatLabel(junkDelegate.track.side)
                // Highlights the memory cue at 0:00 -- the one
                // this row is actually about -- and dims every
                // other cue the track happens to have, so it's
                // unambiguous which one Remove kills. See
                // WaveformView's own doc comment on this
                // property.
                // The cue this row is about, which is not
                // always 0. A clustered hot cue sits a
                // second or so in, and highlighting 0:00
                // while Remove takes away a cue at 1.188 s
                // is the opposite of unambiguous.
                highlightCuePositionMs: junkDelegate.positionMs
                actionButtonText: junkDelegate.staged ? "Unstage" : "Remove"
                actionButtonTooltip: junkDelegate.staged
                    ? "Staged for removal, not on the stick yet: press Save. Click to take it back out."
                    : "Stage removing this cue at 0:00 from the track; Save writes it. Backed up first."
                actionButtonEnabled: !consistencyController?.busy && !consistencyController?.writing
                onActionTriggered: {
                    if (junkDelegate.staged) {
                        consistencyController?.unstageJunkCue(junkDelegate.index);
                    } else {
                        confirmRemoveJunkCueDialog.pendingIndex = junkDelegate.index;
                        confirmRemoveJunkCueDialog.open();
                    }
                }
                playbackController: root.playbackController
                playbackPath: root.pathForFormat(junkDelegate.track.side)
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: "Ignore"
                    enabled: !consistencyController?.busy
                    ToolTip.visible: hovered
                    ToolTip.text: "Dismiss this one, just for this view. Nothing on the stick changes"
                    onClicked: consistencyController?.ignoreJunkCue(junkDelegate.index)
                }
            }
        }
    }
}
