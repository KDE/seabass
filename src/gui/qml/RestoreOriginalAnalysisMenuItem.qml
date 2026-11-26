// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A playlist menu's "Restore original analysis": enabled when at least
// one track of the playlist has the analysis rekordbox or the player made
// kept on the stick (OriginalAnalysisController::keptCount, asked again
// whenever the playlist changes and each time the menu opens). Triggered,
// the restores are prepared and staged, and the controller's
// restoreReady() opens RestoreOriginalAnalysisDialog.
MenuItem {
    id: item
    objectName: "restoreOriginalAnalysisItem"

    required property var controller
    required property string rekordboxPath
    required property string enginePath
    required property string playlist

    readonly property bool known: controller.keptCountPlaylist === playlist
    readonly property int kept: known ? controller.keptCount : 0

    function ask() {
        if (item.playlist.length > 0) {
            item.controller.refreshKeptCount(item.rekordboxPath, item.enginePath, item.playlist);
        }
    }
    onPlaylistChanged: ask()
    Component.onCompleted: ask()
    // Asked again each time the menu opens: a save since may have kept or
    // restored something.
    Connections {
        target: item.menu
        function onAboutToShow() { item.ask(); }
    }

    enabled: kept > 0 && controller.busy !== true
    text: kept > 0 ? qsTr("Restore original analysis (%n track(s))", "", kept) : qsTr("Restore original analysis")
    onTriggered: item.controller.restoreOriginalAnalysis(item.rekordboxPath, item.enginePath, item.playlist)
}
