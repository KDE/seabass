// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// Library Health's page for the Engine pads a Denon player does not show:
// hot cues and saved loops written without a colour by a build before
// 05d71bbd. The repair writes the same cues again with the player's own
// colour for each pad. See domain::HiddenEngineCues.
HealthCheckPage {
    id: root

    checkTitle: "Cues the Player Hides"

    readonly property bool checked: consistencyController?.hiddenCuesChecked ?? false
    readonly property int count: consistencyController?.hiddenCueCount ?? 0
    readonly property int trackCount: consistencyController?.hiddenCueTrackCount ?? 0

    Label {
        objectName: "hiddenCuesNotChecked"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        visible: !root.checked && !consistencyController?.busy
        color: Theme.textMuted
        text: "There is no Engine library on this stick, so there is nothing for this check to look at."
    }

    Label {
        objectName: "hiddenCuesSummary"
        visible: root.checked
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.count === 0
            ? "Every hot cue and saved loop in the Engine library has a colour, so the player shows them all."
            : root.count + (root.count === 1 ? " cue" : " cues") + " on " + root.trackCount
              + (root.trackCount === 1 ? " track" : " tracks") + " in the Engine library "
              + (root.count === 1 ? "has" : "have") + " no colour. A Denon player does not show a pad without "
              + "one: the cue is in the library and on no pad. These were written by an earlier Seabass, which "
              + "passed a cue that had no colour in rekordbox or OneLibrary through as none. Giving each the "
              + "player's own colour for its pad changes nothing else about it."
    }

    RowLayout {
        visible: root.checked
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Button {
            objectName: "recolourHiddenCuesButton"
            visible: root.count > 0 || (consistencyController?.hiddenCueFixStaged ?? false)
            text: consistencyController?.hiddenCueFixStaged ? "Unstage" : "Give Them a Colour"
            enabled: !consistencyController?.busy && !consistencyController?.writing
                && !consistencyController?.stickReadOnly
            ToolTip.visible: hovered
            ToolTip.text: consistencyController?.stickReadOnly
                ? "This stick is read-only until its filesystem has been checked. Library Health offers that."
                : consistencyController?.hiddenCueFixStaged
                ? "Take this back out of the changes to save"
                : "Stage writing these cues again with the player's colour for each pad. Nothing else about them "
                  + "changes. Save writes it."
            onClicked: consistencyController?.hiddenCueFixStaged
                ? consistencyController?.unstageHiddenCueFix()
                : consistencyController?.recolourHiddenCues()
        }
        Label {
            objectName: "stagedHiddenCuesNote"
            visible: consistencyController?.hiddenCueFixStaged ?? false
            text: "staged, not saved yet"
            color: Theme.warnText
        }
        Item { Layout.fillWidth: true }
    }

    Repeater {
        objectName: "hiddenCueTracks"
        model: consistencyController?.hiddenCueTracks
        delegate: Label {
            required property var modelData
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMuted
            text: "“" + modelData.title + "”" + (modelData.artist.length > 0 ? ", " + modelData.artist : "") + ": "
                + [modelData.hotCues > 0 ? modelData.hotCues + (modelData.hotCues === 1 ? " hot cue" : " hot cues") : "",
                   modelData.loops > 0 ? modelData.loops + (modelData.loops === 1 ? " loop" : " loops") : ""]
                      .filter((part) => part.length > 0).join(" and ") + " hidden"
        }
    }

    Item { Layout.fillHeight: true }
}
