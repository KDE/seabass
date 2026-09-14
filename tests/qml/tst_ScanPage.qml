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

    // The classic layout, with its playlist column: Matching (experimental)
    // swaps that column for a sidebar pane, and the switch is a setting the
    // rest of the suite may have left on in this run's shared sandbox.
    property bool experimentalWas: false
    function initTestCase() {
        experimentalWas = realAppSettings.experimentalFeaturesEnabled;
        realAppSettings.experimentalFeaturesEnabled = false;
    }
    function cleanupTestCase() {
        realAppSettings.experimentalFeaturesEnabled = experimentalWas;
    }

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
    function test_searchFieldLinesUpWithTheList() {
        var page = makePage();
        var search = findChild(page, "searchField");
        var pane = findChild(page, "playlistPane");
        verify(search !== null && pane !== null);
        compare(pane.visible, true, "the playlist column is the list's left edge");
        compare(Math.round(search.mapToItem(page, 0, 0).x), Math.round(pane.mapToItem(page, 0, 0).x));
        compare(Math.round(pane.mapToItem(page, 0, 0).x), Theme.pageMargin);
    }
}
