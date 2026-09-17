// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse Library's header, against the real controllers the page builds
// itself. No stick: nothing is scanned.
TestCase {
    id: testCase
    name: "ScanPage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        ScanPage {}
    }

    // Matching graduated on 2026-09-17, so its sidebar pane is the layout
    // every run gets, whatever the experimental setting says.

    function makePage() {
        var page = createTemporaryObject(pageComponent, testCase, {
            width: 880, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER", enginePath: "/nonexistent/TESTSTICK/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
        verify(page !== null);
        waitForRendering(page);
        return page;
    }

    // An arrow, not the words: the direction is an icon, named in its
    // tooltip and to an assistive reader.
    function test_sortDirectionIsAnIcon() {
        var page = makePage();
        var button = findChild(page, "sortDirectionButton");
        verify(button !== null, "the sort direction button must exist");
        compare(button.display, AbstractButton.IconOnly);
        compare(button.icon.source.toString(), Theme.iconUrl("view-sort-ascending"));
        compare(button.text, "Ascending");
        button.checked = false;
        compare(button.icon.source.toString(), Theme.iconUrl("view-sort-descending"));
        compare(button.text, "Descending");
    }

    // One left line: the search field and the list below it -- the
    // playlist column, which is the list's left edge -- start at the same
    // x. The list used to run to the window edge, 16 px left of the search.
    // One left line for the page. Since Matching graduated (2026-09-17) the
    // header row starts with the sidebar's own pill button rather than the
    // search field, and the playlists sidebar -- open by default, where the
    // classic playlist column used to be -- is the list's left edge.
    function test_theHeaderAndTheListShareOneLeftLine() {
        var page = makePage();
        var pill = findChild(page, "playlistSidebarButton");
        var search = findChild(page, "searchField");
        var sidebar = findChild(page, "playlistSidebar");
        verify(pill !== null && search !== null && sidebar !== null);
        compare(sidebar.visible, true, "the playlists sidebar is the list's left edge");
        compare(Math.round(pill.mapToItem(page, 0, 0).x), Math.round(sidebar.mapToItem(page, 0, 0).x));
        compare(Math.round(sidebar.mapToItem(page, 0, 0).x), Theme.pageMargin);
        verify(search.mapToItem(page, 0, 0).x > pill.mapToItem(page, 0, 0).x,
               "the search field follows the pill in the same row");
    }
}
