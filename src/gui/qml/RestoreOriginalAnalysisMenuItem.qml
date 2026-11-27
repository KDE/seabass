// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// A playlist menu's "Restore original analysis": shown and enabled when at
// least one track of the playlist has the analysis rekordbox or the player
// made kept on the stick. Decided once each time the menu opens, from the
// controller's last answer for the playlist on this stick
// (OriginalAnalysisController::cachedKeptCount, which Browse warms when it
// loads the stick and every save keeps up to date), and never changed
// while the menu is open: the menu does not grow or shrink under the
// pointer. Each opening asks the controller once for a new answer, for
// the next opening; while no answer is known the item is absent. Triggered, the
// restores are prepared and staged, and the controller's restoreReady()
// opens RestoreOriginalAnalysisDialog.
MenuItem {
    id: item
    objectName: "restoreOriginalAnalysisItem"

    required property var controller
    required property string rekordboxPath
    required property string enginePath
    required property string playlist

    // The count this opening of the menu goes by; 0 until it opens.
    property int kept: 0

    function prepare() {
        if (item.playlist.length === 0) {
            item.kept = 0;
            return;
        }
        const cached = item.controller.cachedKeptCount(item.rekordboxPath, item.enginePath, item.playlist);
        item.kept = cached > 0 ? cached : 0;
        item.controller.refreshKeptCount(item.rekordboxPath, item.enginePath, item.playlist);
    }
    Connections {
        target: item.menu
        function onAboutToShow() { item.prepare(); }
    }

    visible: kept > 0
    height: visible ? implicitHeight : 0
    // Never reached by the keyboard while hidden.
    enabled: visible && kept > 0 && controller.busy !== true
    text: kept > 0 ? qsTr("Restore original analysis (%n track(s))", "", kept) : qsTr("Restore original analysis")
    onTriggered: item.controller.restoreOriginalAnalysis(item.rekordboxPath, item.enginePath, item.playlist)
}
