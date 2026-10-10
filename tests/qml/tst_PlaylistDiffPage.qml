// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// PlaylistDiffPage.qml over the committed anonymized library, its real
// controller running: the pickers list the playlists, the page opens on
// the first one and its nearest relative, the diff rows render with
// their signs, a fold opens on click, the chips set B, and the page
// keeps one left line. With both catalogs, each side has its own catalog
// switch and lists that catalog's playlists. Saves a screenshot when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "PlaylistDiffPage"
    width: 1200
    height: 760
    visible: true
    when: windowShown

    AppSettingsController { id: realAppSettings }

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_JunkCuePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    Component {
        id: pageComponent
        PlaylistDiffPage {
            width: 1180
            height: 740
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            appSettingsController: realAppSettings
        }
    }

    function openOnAFreshCopy(withRekordbox) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase, {
            enginePath: stick + "/Engine Library",
            rekordboxPath: withRekordbox ? stick + "/PIONEER" : "",
        });
        verify(page !== null, "the page must instantiate");
        tryCompare(page.controller, "busy", false, 60000);
        compare(page.controller.errorMessage, "");
        waitForRendering(page);
        return page;
    }

    function rowsOf(page) {
        return findChild(page, "diffList");
    }

    function test_opensOnTheFirstPlaylistAndItsNearestRelative() {
        const page = openOnAFreshCopy();
        const controller = page.controller;
        compare(controller.playlistNamesA.length, 30, "the three empty playlists have no track to name them");
        verify(!findChild(page, "sourceToggleA").visible, "one catalog, no switch");
        compare(controller.playlistA, "Playlist 000");
        compare(controller.playlistB, "Playlist 019");
        const comboA = findChild(page, "playlistACombo");
        const comboB = findChild(page, "playlistBCombo");
        compare(comboA.count, 30);
        compare(comboA.currentText, "Playlist 000");
        compare(comboB.currentText, "Playlist 019");
        compare(findChild(page, "onlyACount").text, "−0");
        compare(findChild(page, "onlyBCount").text, "+10");
        compare(findChild(page, "sharedCount").text, "100");
        compare(findChild(page, "verdict").text, "Playlist 019 is Playlist 000 plus 10 tracks.");
    }

    // The hundred identical rows fold to their last two plus one fold
    // row; the ten added rows follow, green, with a plus.
    function test_identicalRunsFoldAndOpenOnClick() {
        const page = openOnAFreshCopy();
        const list = rowsOf(page);
        compare(list.count, 13);
        const fold = list.itemAtIndex(0);
        verify(fold !== null, "the fold row is up");
        compare(fold.kind, "fold");
        compare(fold.foldCount, 98);
        const added = list.itemAtIndex(3);
        compare(added.kind, "onlyB");
        compare(added.rightPosition, 101);
        compare(added.leftKind, "");
        mouseClick(fold);
        tryCompare(list, "count", 110);
        compare(list.itemAtIndex(0).kind, "same");
        compare(list.itemAtIndex(0).leftPosition, 1);
    }

    function test_theFoldCheckBoxFoldsAndUnfolds() {
        const page = openOnAFreshCopy();
        const check = findChild(page, "foldCheck");
        compare(check.checked, true);
        mouseClick(check);
        compare(page.controller.foldIdentical, false);
        tryCompare(rowsOf(page), "count", 110);
        mouseClick(check);
        tryCompare(rowsOf(page), "count", 13);
    }

    function test_swapTurnsThePlusIntoAMinus() {
        const page = openOnAFreshCopy();
        mouseClick(findChild(page, "swapButton"));
        compare(page.controller.playlistA, "Playlist 019");
        compare(findChild(page, "onlyACount").text, "−10");
        compare(findChild(page, "onlyBCount").text, "+0");
        compare(rowsOf(page).itemAtIndex(3).kind, "onlyA");
    }

    // The chips under the pickers: A's relatives, most alike first, and
    // a click sets B. Playlist 004 is the first thirty of Playlist 000.
    function test_aRelativeChipSetsB() {
        const page = openOnAFreshCopy();
        const chips = [];
        function collect(item) {
            if (item.objectName === "relativeChip") {
                chips.push(item);
            }
            for (let i = 0; i < item.children.length; ++i) {
                collect(item.children[i]);
            }
        }
        collect(findChild(page, "relatives"));
        verify(chips.length >= 2, "A has relatives: " + chips.length);
        compare(chips[0].modelData.name, "Playlist 019");
        compare(chips[0].modelData.relation, "superset");
        compare(chips[1].modelData.name, "Playlist 004");
        compare(chips[1].modelData.relation, "subset");
        mouseClick(chips[1]);
        compare(page.controller.playlistB, "Playlist 004");
        compare(findChild(page, "verdict").text, "Playlist 004 is Playlist 000 minus 70 tracks.");
    }

    // A reordered pair: moved rows on both sides, amber, each naming the
    // other's position, and the ribbons canvas over the list.
    function test_movedTracksShowOnBothSides() {
        const page = openOnAFreshCopy();
        page.controller.playlistA = "Playlist 003";
        page.controller.playlistB = "Playlist 025";
        page.controller.foldIdentical = false;
        const list = rowsOf(page);
        tryVerify(() => list.count > 90);
        compare(page.controller.movedCount, 36);
        let movedLeft = 0;
        for (let i = 0; i < list.count; ++i) {
            list.positionViewAtIndex(i, ListView.Visible);
            const row = list.itemAtIndex(i);
            if (row && row.leftKind === "moved") {
                ++movedLeft;
                verify(row.leftPartnerPosition > 0, "a moved row names where the track went");
            }
        }
        compare(movedLeft, 36);
        const ribbons = findChild(page, "ribbons");
        verify(ribbons !== null);
        compare(ribbons.width, list.width);
    }

    // One left line: the breadcrumb, the A picker's label and the list
    // all start at Theme.pageMargin.
    function test_oneLeftLine() {
        const page = openOnAFreshCopy();
        const labelA = findChild(page, "labelA");
        const list = rowsOf(page);
        const header = findChild(page, "columnHeader");
        compare(Math.round(labelA.mapToItem(page, 0, 0).x), Theme.pageMargin);
        compare(Math.round(list.mapToItem(page, 0, 0).x), Theme.pageMargin);
        compare(Math.round(header.mapToItem(page, 0, 0).x), Theme.pageMargin);
    }

    // Switching source rescans: nothing of the catalog shown so far stays
    // on the page, neither while the scan runs nor once it has failed.
    function test_aFailedRescanLeavesNothingBehind() {
        const page = openOnAFreshCopy();
        verify(rowsOf(page).count > 0);
        compare(findChild(page, "headerA").title, "Playlist 000");
        page.controller.scan("", testCase.fixtureRoot + "/no-such-stick/Engine Library");
        compare(rowsOf(page).count, 0);
        compare(findChild(page, "headerA").title, "");
        compare(findChild(page, "headerB").title, "");
        compare(findChild(page, "playlistACombo").count, 0);
        tryCompare(page.controller, "busy", false, 60000);
        verify(page.controller.errorMessage.length > 0);
        compare(rowsOf(page).count, 0);
        compare(findChild(page, "headerA").title, "");
        compare(findChild(page, "playlistACombo").count, 0);
    }

    // Both catalogs: a switch beside each picker, each picker listing its
    // own catalog's playlists. Engine on both sides to begin with, as the
    // page always opened; B to DeviceLibrary changes B's list alone.
    function test_eachSideHasItsOwnCatalog() {
        const page = openOnAFreshCopy(true);
        const controller = page.controller;
        const toggleA = findChild(page, "sourceToggleA");
        const toggleB = findChild(page, "sourceToggleB");
        verify(toggleA !== null && toggleB !== null);
        verify(toggleA.visible && toggleB.visible);
        compare(findChild(page, "librarySourceToggle"), null, "no third switch in the header");
        compare(toggleA.current, "engine");
        compare(toggleB.current, "engine");
        verify(toggleA.hasOneLibrary, "the fixture has a OneLibrary catalog");
        const comboA = findChild(page, "playlistACombo");
        const comboB = findChild(page, "playlistBCombo");
        compare(comboA.count, 30);
        compare(comboB.count, 30);
        compare(findChild(page, "headerACatalog").visible, false);

        toggleB.sourceRequested("rekordbox");
        compare(controller.formatB, "rekordbox");
        compare(toggleB.current, "rekordbox");
        compare(toggleA.current, "engine");
        compare(comboA.count, 30, "A's list is still Engine's");
        compare(comboB.count, controller.playlistNamesB.length);
        verify(comboB.count !== 30, "B lists DeviceLibrary's playlists: " + comboB.count);
        verify(controller.playlistNamesB.indexOf(comboB.currentText) >= 0);
        compare(findChild(page, "headerACatalog").text, "Engine");
        compare(findChild(page, "headerBCatalog").text, "DeviceLibrary");
        verify(findChild(page, "headerBCatalog").visible);

        // Swap takes the catalogs along.
        mouseClick(findChild(page, "swapButton"));
        compare(toggleA.current, "rekordbox");
        compare(toggleB.current, "engine");
        compare(comboB.count, 30);
    }

    // A chip for another catalog's playlist names that catalog and sets
    // B's catalog with B. Engine's Playlist 010 and DeviceLibrary's
    // Playlist 000 share 46 files (see playlist_diff_controller_test).
    function test_aChipForAnotherCatalogSetsItsCatalog() {
        const page = openOnAFreshCopy(true);
        page.controller.playlistA = "Playlist 010";
        // The chips' Flow places them on the next polish.
        waitForRendering(page);
        const chips = [];
        function collect(item) {
            if (item.objectName === "relativeChip") {
                chips.push(item);
            }
            for (let i = 0; i < item.children.length; ++i) {
                collect(item.children[i]);
            }
        }
        tryVerify(() => { chips.length = 0; collect(findChild(page, "relatives")); return chips.length > 0; });
        let chip = null;
        for (let i = 0; i < chips.length; ++i) {
            if (chips[i].modelData.label === "DeviceLibrary: Playlist 000") {
                chip = chips[i];
            }
        }
        verify(chip !== null, "a chip names DeviceLibrary's Playlist 000");
        mouseClick(chip);
        compare(page.controller.formatB, "rekordbox");
        compare(page.controller.playlistB, "Playlist 000");
        compare(findChild(page, "sharedCount").text, "46");
        compare(findChild(page, "onlyACount").text, "−4");
        compare(findChild(page, "onlyBCount").text, "+3");
        verify(chip.highlighted, "the chip for B is lit");
    }

    // One left line with the switches in the row: A's label still starts it.
    function test_oneLeftLineWithTwoCatalogs() {
        const page = openOnAFreshCopy(true);
        compare(Math.round(findChild(page, "labelA").mapToItem(page, 0, 0).x), Theme.pageMargin);
        compare(Math.round(rowsOf(page).mapToItem(page, 0, 0).x), Theme.pageMargin);
        compare(Math.round(findChild(page, "columnHeader").mapToItem(page, 0, 0).x), Theme.pageMargin);
        const toggleA = findChild(page, "sourceToggleA");
        const comboA = findChild(page, "playlistACombo");
        verify(toggleA.mapToItem(page, 0, 0).x > findChild(page, "labelA").mapToItem(page, 0, 0).x);
        verify(comboA.mapToItem(page, 0, 0).x > toggleA.mapToItem(page, 0, 0).x);
        verify(comboA.width > 100, "the picker keeps room: " + comboA.width);
    }

    function test_screenshot() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        const page = openOnAFreshCopy();
        grabImage(page).save(screenshotDir + "/playlist-diff-page.png");
        page.controller.playlistA = "Playlist 003";
        page.controller.playlistB = "Playlist 025";
        waitForRendering(page);
        grabImage(page).save(screenshotDir + "/playlist-diff-page-moved.png");
        const both = openOnAFreshCopy(true);
        both.controller.playlistA = "Playlist 010";
        both.controller.chooseB("rekordbox", "Playlist 000");
        waitForRendering(both);
        grabImage(both).save(screenshotDir + "/playlist-diff-page-catalogs.png");
    }
}
