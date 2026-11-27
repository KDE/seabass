// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SeabassGui

// What Restore original analysis came to, opened by the controller's
// restoreReady(): how many tracks wait for Save to get their original
// analysis back, which were left as they are and why, and for each track
// whose cue points Analyse again moved, a switch that moves them back with
// the grid. A switch starts on only where every move is under the
// threshold set in Preferences, as for Analyse again. Nothing is written
// until the save.
SeabassDialog {
    id: dialog
    objectName: "restoreOriginalAnalysisDialog"

    required property var controller

    contentHeightFollowsContent: true
    readonly property var outcome: controller.lastRestore ?? ({})
    readonly property int staged: outcome.staged ?? 0
    readonly property var leftAlone: outcome.leftAlone ?? []
    readonly property string playlist: outcome.playlist ?? ""
    readonly property int autoCueTracks: outcome.autoCueTracks ?? 0
    readonly property int askedCueTracks: outcome.askedCueTracks ?? 0
    readonly property int thresholdMs: outcome.cueFollowThresholdMs ?? 0
    readonly property int blockedCueTracks: outcome.blockedCueTracks ?? 0
    readonly property int unknownOriginTracks: outcome.unknownOriginTracks ?? 0
    readonly property var tracks: controller.cueReview ?? []

    Connections {
        target: dialog.controller
        function onRestoreReady() { dialog.open(); }
    }

    severity: staged > 0 ? SeabassDialog.Info : SeabassDialog.Warning
    title: qsTr("Restore original analysis")
    headline: staged > 0 ? qsTr("%n track(s) will get back the analysis rekordbox or the player made.", "", staged)
        : qsTr("No track of \"%1\" can get its original analysis back.").arg(playlist)
    detailText: (staged > 0 ? qsTr("Their beat grid, waveforms and tempo go back to what they were before Seabass "
                                   + "analysed them again. Cue points you set since stay. Nothing is written until "
                                   + "you save.") : "")
        + (autoCueTracks > 0
            ? "\n\n" + qsTr("On %n track(s) the cue points Analyse again moved go back with the grid on their own, "
                            + "as they move by less than %1 ms.", "", autoCueTracks).arg(thresholdMs)
            : "")
        + (askedCueTracks > 0
            ? "\n\n" + qsTr("On %n track(s) cue points sit on a beat of Seabass's grid that the original grid has "
                            + "elsewhere. They stay where they are unless you switch them on below.", "",
                            askedCueTracks)
            : "")
        + (unknownOriginTracks > 0
            ? "\n\n" + qsTr("On %n track(s) what goes back is the analysis that was on the stick before Seabass "
                            + "analysed them again: Seabass had lost its record of its own analyses, so it may not be "
                            + "rekordbox's.", "", unknownOriginTracks)
            : "")
        + (blockedCueTracks > 0
            ? "\n\n" + qsTr("On %n track(s) a cue point stays where it is: another cue point is already at the "
                            + "place it would go back to.", "", blockedCueTracks)
            : "")
    width: Math.min(parent ? parent.width - 2 * Theme.pageMargin : Theme.scaled(700), Theme.scaled(700))

    Label {
        objectName: "restoreLeftAloneLabel"
        visible: dialog.leftAlone.length > 0
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        text: qsTr("Left as they are:") + "\n" + dialog.leftAlone.map(left => left.name + ": " + left.why).join("\n")
    }

    RowLayout {
        visible: dialog.tracks.length > 0
        Layout.fillWidth: true
        spacing: Theme.rowSpacing
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.bold: true
            text: qsTr("Move cue points back onto the original grid?")
        }
        Button {
            objectName: "restoreCuesAllOn"
            text: qsTr("Move all")
            onClicked: dialog.controller.setMoveCuesBackForAll(true)
        }
        Button {
            objectName: "restoreCuesAllOff"
            text: qsTr("Move none")
            onClicked: dialog.controller.setMoveCuesBackForAll(false)
        }
    }

    ListView {
        objectName: "restoreCueTracks"
        visible: dialog.tracks.length > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.scaled(240))
        clip: true
        interactive: contentHeight > height
        model: dialog.tracks
        delegate: RowLayout {
            id: track
            required property var modelData
            width: ListView.view.width
            spacing: Theme.rowSpacing
            SeabassCheckBox {
                objectName: "restoreCueSwitch"
                checked: track.modelData.moveCues
                onToggled: dialog.controller.setMoveCuesBack(track.modelData.changeId, checked)
            }
            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: track.modelData.subject
            }
            Label {
                color: Theme.textMuted
                text: qsTr("%n cue point(s), up to %1 ms", "", track.modelData.moves).arg(track.modelData.largestMoveMs)
            }
        }
    }

    footer: DialogButtonBox {
        Button {
            objectName: "restoreTakeBackButton"
            visible: dialog.staged > 0
            text: qsTr("Take back")
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                dialog.controller.dropStaged();
                dialog.close();
            }
        }
        Button {
            objectName: "restoreOkButton"
            text: qsTr("OK")
            highlighted: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }
}
