// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import SeabassGui

// The playlist list -- filter-by-name (see the shared search field's own
// comment in ScanPage.qml for why this reuses that field instead of a
// second one), delegated to PlaylistRowDelegate.qml (also used by the
// "This Playlist" ComboBox's own popup, so both pickers render
// identically). Used in two places in ScanPage.qml: always in the left
// Pane when the Matching panel (Experimental) is off, or inside
// a Drawer when it's on -- both instances share this one definition and
// both write to the same root.selectedPlaylistIndex, so switching
// playlists from either place stays in sync.
ListView {
    id: root
    // Not draggable when everything already fits.
    interactive: contentHeight > height
    required property var scanController
    property string searchQuery: ""
    property int selectedIndex: 0
    signal playlistPicked(int index, string name)

    clip: true
    ScrollBar.vertical: BigScrollBar {}
    property var allNames: ["All tracks"].concat(scanController.playlistNames)
    model: root.searchQuery.length === 0
        ? allNames
        : allNames.filter((n, i) => i === 0 || n.toLowerCase().includes(root.searchQuery.toLowerCase()))
    currentIndex: root.selectedIndex

    delegate: PlaylistRowDelegate {
        // index isn't redeclared here -- PlaylistRowDelegate's own root
        // already declares it as required, and the delegate model injects
        // it there directly; redeclaring it in this derived scope shadows
        // that slot instead of filling it, leaving the *base* class's own
        // required `index` permanently unsatisfied and the whole item
        // silently refusing to render (exactly what broke this list AND
        // the This Playlist combo's popup, which reuses this same
        // delegate the same broken way).
        required property string modelData
        name: modelData
        count: index === 0 ? root.scanController.totalTrackCount
            : (root.scanController.playlistTrackCounts[modelData] ?? 0)
        isCurrent: ListView.isCurrentItem
        onPicked: root.playlistPicked(index, modelData)
    }
}
