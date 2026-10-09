// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// RekordboxExportSyncPage.qml over a copy of the committed anonymized
// stick (never the fixture), its real controller running. The fixture is
// in "Sync Needed" state with no record of an earlier save, so the page
// shows the numbers rekordbox_export_sync_controller_test pins: 1 playlist
// to create, 925 membership rows, 1245 conflicts, 1177 of Engine's own.
//
// Through the page: the conflicts head the list, the section headers carry
// those counts, the first-run sentence and the legend of the three
// directions are said, a section checkbox selects its rows, unticking the
// new playlist's create unticks its members, a conflict answered toward
// rekordbox shows its side and adds the removal, every conflict answered
// and cleared from the section's header, a row opened to its details,
// the header folded while the list scrolls, and a small selection staged
// and saved through the host's "Sync Engine" lands, the page analyses
// again and offers Undo. One left line. Screenshots when
// SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "RekordboxExportSyncPage"
    width: 1200
    height: 800
    visible: true
    when: windowShown

    AppSettingsController { id: realAppSettings }

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_JunkCuePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // RekordboxExportSyncListModel's roles (Qt::UserRole + 1 onwards), for
    // reading rows that are not in view.
    readonly property var role: ({
        section: 257, sectionIndex: 258, kind: 259, key: 260, title: 261, artist: 262, detail: 263,
        reason: 264, isConflict: 265, rekordboxChoiceLabel: 266, engineChoiceLabel: 267, resolvedSide: 268,
        included: 269, dependsOn: 270, staged: 271, stagedDescription: 272, direction: 273, fromConflict: 274,
        details: 275,
    })

    Component {
        id: pageComponent
        RekordboxExportSyncPage {
            width: 1180
            height: 780
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            appSettingsController: realAppSettings
        }
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/is not a function/);
        failOnWarning(/Unable to assign/);
        failOnWarning(/Cannot read property/);
    }

    function openOnAFreshCopy() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase,
            {rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "the page must instantiate");
        tryVerify(() => page.controller.analyzed || page.controller.errorMessage.length > 0, 120000,
                  "the analysis lands");
        tryCompare(page.controller, "busy", false, 120000);
        compare(page.controller.errorMessage, "");
        waitForRendering(page);
        return page;
    }

    function listOf(page) {
        return findChild(page, "rowsList");
    }
    function rowData(page, i, name) {
        const rows = page.controller.rows;
        return rows.data(rows.index(i, 0), testCase.role[name]);
    }
    function rowCount(page) {
        return page.controller.rows.rowCount();
    }
    function firstRowWhere(page, predicate) {
        for (let i = 0; i < rowCount(page); ++i) {
            if (predicate(i)) {
                return i;
            }
        }
        return -1;
    }
    function rowsDependingOn(page, key) {
        const out = [];
        for (let i = 0; i < rowCount(page); ++i) {
            if (rowData(page, i, "dependsOn").indexOf(key) >= 0) {
                out.push(i);
            }
        }
        return out;
    }
    // The delegate of row i, scrolled into view.
    function rowItem(page, i) {
        const list = listOf(page);
        list.positionViewAtIndex(i, ListView.Beginning);
        waitForRendering(list);
        const item = list.itemAtIndex(i);
        verify(item !== null, "row " + i + " is in view");
        return item;
    }
    // A section header, scrolled into view with its first row.
    function sectionHeader(page, name) {
        const first = firstRowWhere(page, (i) => rowData(page, i, "section") === name);
        verify(first >= 0, name + " has rows");
        const list = listOf(page);
        list.positionViewAtIndex(first, ListView.Center);
        waitForRendering(list);
        const header = findChild(list, "sectionHeader_" + name);
        verify(header !== null, "the " + name + " header is up");
        return header;
    }
    function sectionCountText(page, name) {
        return findChild(sectionHeader(page, name), "sectionCount").text;
    }
    function leftOf(page, item) {
        return Math.round(item.mapToItem(page, 0, 0).x);
    }
    // The new playlist's create: the first row of the playlists section.
    function createRow(page) {
        const create = firstRowWhere(page, (i) => rowData(page, i, "kind") === "createPlaylist");
        verify(create >= 0, "the fixture proposes a playlist");
        return create;
    }

    function test_analyzesOnOpenAndShowsTheCountsAndTheFirstRunRule() {
        const page = openOnAFreshCopy();
        compare(page.controller.hasBaseline, false);
        // The questions first: the only rows that need an answer.
        compare(rowData(page, 0, "section"), "conflicts");
        compare(rowData(page, 0, "sectionIndex"), 0);
        compare(page.firstSection, "conflicts");
        compare(sectionCountText(page, "playlists"), "1");
        compare(sectionCountText(page, "membership"), "925");
        compare(sectionCountText(page, "conflicts"), "1245");
        compare(sectionCountText(page, "engineOwnKept"), "1177");
        compare(findChild(sectionHeader(page, "membership"), "sectionTitle").text, "Playlist membership");
        const intro = findChild(page.header, "introLabel");
        compare(intro.text, "No earlier record of this stick: additions are assumed, removals are left to you. "
                + "Items you leave undecided here count as Engine's own from now on.");
        verify(intro.visible);
        compare(findChild(page.header, "conflictFigure").value, "1245");
        compare(findChild(page.header, "checkedFigure").value, "953");
        compare(findChild(page.header, "checkedFigure").label, "selected");
        compare(findChild(page.header, "stageButton").text, "Stage Selected");
        // What the three directions mean, one line each, the chip as rows
        // show it.
        const chips = [];
        const texts = [];
        for (const name of ["to Engine", "back to rekordbox", "kept"]) {
            const chip = findChildWhere(page.header, (o) => o.objectName === "legendChip" && o.label === name);
            verify(chip !== null && chip.visible, "the legend shows the " + name + " chip");
            chips.push(chip);
        }
        compare(chips[0].badgeColor, Theme.accent);
        compare(chips[1].badgeColor, Theme.warnText);
        compare(chips[2].badgeColor, Theme.textMuted);
        const legendTexts = findChildrenWhere(page.header, (o) => o.objectName === "legendText").map((o) => o.text);
        compare(legendTexts, ["Written into the Engine library so it matches rekordbox",
                              "Cues or ratings Seabass had put on the rekordbox side that the export dropped, going back",
                              "Engine's own, nothing is written"]);
        // Engine's own carry no checkbox, nor does a conflict; a writable row does.
        const kept = firstRowWhere(page, (i) => rowData(page, i, "section") === "engineOwnKept");
        compare(findChild(rowItem(page, kept), "rowCheck").visible, false);
        compare(findChild(rowItem(page, 0), "rowCheck").visible, false);
        compare(findChild(rowItem(page, createRow(page)), "rowCheck").visible, true);
    }

    // Depth first over children, in order.
    function findChildrenWhere(item, predicate) {
        let out = [];
        if (predicate(item)) {
            out.push(item);
        }
        for (let i = 0; i < item.children.length; ++i) {
            out = out.concat(findChildrenWhere(item.children[i], predicate));
        }
        return out;
    }
    function findChildWhere(item, predicate) {
        const all = findChildrenWhere(item, predicate);
        return all.length > 0 ? all[0] : null;
    }

    function test_aSectionCheckboxTicksItsRows() {
        const page = openOnAFreshCopy();
        const check = findChild(sectionHeader(page, "membership"), "sectionCheck_membership");
        verify(check.visible);
        compare(check.checked, true, "every membership row starts ticked");
        mouseClick(check);
        compare(page.controller.sectionCheckedCounts["membership"], 0);
        compare(page.controller.checkedCount, 953 - 925);
        compare(findChild(page.header, "checkedFigure").value, String(953 - 925));
        compare(check.checked, false);
        mouseClick(check);
        compare(page.controller.sectionCheckedCounts["membership"], 925);
        compare(page.controller.checkedCount, 953);
        compare(check.checked, true);
        // No section-wide box where a tick writes nothing.
        compare(findChild(sectionHeader(page, "conflicts"), "sectionCheck_conflicts").visible, false);
    }

    function test_uncheckingTheNewPlaylistUnticksItsMembers() {
        const page = openOnAFreshCopy();
        const create = createRow(page);
        compare(rowData(page, create, "section"), "playlists");
        compare(rowData(page, create, "title"), "Playlist 01");
        const members = rowsDependingOn(page, rowData(page, create, "key"));
        compare(members.length, 14);
        for (const m of members) {
            compare(rowData(page, m, "included"), true);
        }
        const check = findChild(rowItem(page, create), "rowCheck");
        mouseClick(check);
        compare(rowData(page, create, "included"), false);
        compare(page.expandedIndex, -1, "the box takes its own click; the row stays closed");
        for (const m of members) {
            compare(rowData(page, m, "included"), false, "member " + m + " goes with its playlist");
        }
        const member = rowItem(page, members[0]);
        compare(findChild(member, "rowCheck").checked, false, "the member's box shows it");
    }

    function test_aConflictAnsweredTowardRekordboxShowsItsSideAndAddsTheRemoval() {
        const page = openOnAFreshCopy();
        const conflict = firstRowWhere(page, (i) => rowData(page, i, "isConflict")
            && rowData(page, i, "rekordboxChoiceLabel").length > 0 && rowData(page, i, "engineChoiceLabel").length > 0);
        verify(conflict >= 0);
        const key = rowData(page, conflict, "key");
        const removalsBefore = page.controller.sectionCounts["tracksToRemove"];
        let item = rowItem(page, conflict);
        const rekordboxButton = findChild(item, "rekordboxChoiceButton");
        const engineButton = findChild(item, "engineChoiceButton");
        verify(rekordboxButton.visible && engineButton.visible, "both sides are offered");
        compare(rekordboxButton.text, rowData(page, conflict, "rekordboxChoiceLabel"));
        compare(engineButton.text, rowData(page, conflict, "engineChoiceLabel"));
        compare(findChild(item, "rowCheck").visible, false, "a conflict is answered, not ticked");
        compare(findChild(item, "chosenSideLabel").visible, false);
        mouseClick(rekordboxButton);
        compare(page.controller.sectionCounts["tracksToRemove"], removalsBefore + 1);
        compare(page.controller.conflictCount, 1244);
        // The answer's row goes after the conflicts: the conflict stays.
        const answered = firstRowWhere(page, (i) => rowData(page, i, "isConflict") && rowData(page, i, "key") === key);
        compare(rowData(page, answered, "resolvedSide"), "rekordbox");
        item = rowItem(page, answered);
        compare(findChild(item, "chosenSideLabel").visible, true);
        compare(findChild(item, "chosenSideLabel").text, "Rekordbox's side chosen");
        compare(findChild(item, "rekordboxChoiceButton").highlighted, true);
        const removal = firstRowWhere(page, (i) => rowData(page, i, "section") === "tracksToRemove"
            && rowData(page, i, "fromConflict"));
        verify(removal >= 0);
        compare(rowData(page, removal, "key"), key);
        compare(rowData(page, removal, "included"), true);
        compare(findChild(rowItem(page, removal), "directionChip").label, "to Engine");
        // Undo choice takes it back.
        mouseClick(findChild(rowItem(page, answered), "undoChoiceButton"));
        compare(page.controller.sectionCounts["tracksToRemove"], removalsBefore);
        compare(page.controller.conflictCount, 1245);
        compare(answered, conflict);
    }

    // The conflicts' header answers every one of them, and takes every
    // answer back.
    function test_everyConflictAnsweredFromTheSectionHeader() {
        const page = openOnAFreshCopy();
        const header = sectionHeader(page, "conflicts");
        const rekordboxAll = findChild(header, "rekordboxSideForAllButton");
        const engineAll = findChild(header, "engineSideForAllButton");
        const clearAll = findChild(header, "clearAllChoicesButton");
        verify(rekordboxAll.visible && engineAll.visible && clearAll.visible);
        compare(rekordboxAll.text, "Rekordbox's side for all");
        compare(engineAll.text, "Engine's side for all");
        compare(clearAll.text, "Clear all choices");
        compare(clearAll.enabled, false, "nothing to clear yet");
        compare(findChild(sectionHeader(page, "membership"), "rekordboxSideForAllButton").visible, false,
                "only the conflicts have them");
        const removalsBefore = page.controller.sectionCounts["tracksToRemove"];
        mouseClick(findChild(sectionHeader(page, "conflicts"), "rekordboxSideForAllButton"));
        compare(page.controller.conflictCount, 0);
        verify(page.controller.sectionCounts["tracksToRemove"] > removalsBefore, "the import conflicts' removals");
        compare(rowData(page, 0, "resolvedSide"), "rekordbox");
        mouseClick(findChild(sectionHeader(page, "conflicts"), "engineSideForAllButton"));
        compare(page.controller.sectionCounts["tracksToRemove"], removalsBefore);
        compare(rowData(page, 0, "resolvedSide"), "engine");
        const clear = findChild(sectionHeader(page, "conflicts"), "clearAllChoicesButton");
        verify(clear.enabled);
        mouseClick(clear);
        compare(page.controller.conflictCount, 1245);
        compare(page.controller.checkedCount, 953);
        compare(rowData(page, 0, "resolvedSide"), "");
    }

    // A click on a row's text opens it to what the save does with it; the
    // chevron and a second click close it; one row is open at a time.
    function test_aRowOpensToItsDetails() {
        const page = openOnAFreshCopy();
        const members = rowsDependingOn(page, rowData(page, createRow(page), "key"));
        const member = members[1];
        const lines = rowData(page, member, "details");
        verify(lines.length >= 4, "a membership says what it does: " + JSON.stringify(lines));
        compare(lines[0], "Puts " + rowData(page, member, "title") + " into \"Playlist 01\" on Engine");
        verify(lines.some((line) => line.startsWith("Goes after ")), JSON.stringify(lines));
        let item = rowItem(page, member);
        const details = findChild(item, "rowDetails");
        compare(details.visible, false);
        mouseClick(findChild(item, "rowTitle"));
        compare(page.expandedIndex, member);
        item = rowItem(page, member);
        verify(findChild(item, "rowDetails").visible);
        compare(findChild(item, "rowDetails").text, lines.join("\n"));
        compare(findChild(item, "rowDetails").font.family, Theme.dataFamily);
        compare(findChild(item, "expandButton").iconName, "arrow-down");
        verify(findChild(item, "rowDetails").height > findChild(item, "rowTitle").height * 3, "every line shows");
        // Another row: this one closes.
        const create = createRow(page);
        mouseClick(findChild(rowItem(page, create), "rowTitle"));
        compare(page.expandedIndex, create);
        verify(findChild(rowItem(page, create), "rowDetails").text.indexOf("Creates playlist \"Playlist 01\" on Engine") === 0);
        compare(findChild(rowItem(page, member), "rowDetails").visible, false);
        // The chevron closes it again.
        mouseClick(findChild(rowItem(page, create), "expandButton"));
        compare(page.expandedIndex, -1);
        // A conflict's details name both sides.
        const conflictLines = rowData(page, 0, "details");
        verify(conflictLines.some((line) => line.startsWith("Rekordbox's side")), JSON.stringify(conflictLines));
        verify(conflictLines.some((line) => line.startsWith("Engine's side")), JSON.stringify(conflictLines));
        if (screenshotDir && screenshotDir.length > 0) {
            mouseClick(findChild(rowItem(page, member), "rowTitle"));
            listOf(page).positionViewAtIndex(member, ListView.Center);
            waitForRendering(page);
            wait(300);
            grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-row-open.png");
        }
    }

    // Scrolling the list folds the intro and the legend away and keeps the
    // counts and the buttons; back at the top it unfolds (ScrollCollapse,
    // as Clean Up Duplicates does).
    function test_scrollingFoldsTheHeader() {
        const page = openOnAFreshCopy();
        const list = listOf(page);
        const details = findChild(page.header, "headerDetails");
        verify(details.visible, "the intro shows before any scroll");
        const fullHeader = page.header.height;
        for (let i = 0; i < 3; ++i) {
            mouseWheel(list, list.width / 2, list.height / 2, 0, -120);
            wait(30);
        }
        tryVerify(() => !details.visible, 2000, "scrolling never folded the intro");
        waitForRendering(page);
        verify(page.header.height < fullHeader - 40, "folded " + page.header.height + ", was " + fullHeader);
        for (const name of ["checkedFigure", "conflictFigure", "stageButton", "breadcrumb"]) {
            const control = findChild(page.header, name);
            verify(control.visible, name + " stays");
            const top = control.mapToItem(page, 0, 0).y;
            verify(top >= 0 && top + control.height <= page.header.height + 0.5, name + " is inside the folded header");
        }
        if (screenshotDir && screenshotDir.length > 0) {
            wait(300);
            grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-folded.png");
        }
        list.positionViewAtBeginning();
        tryCompare(page.header, "height", fullHeader, 2000, "the header did not unfold at the top");
        verify(details.visible);
    }

    // The new playlist and its members, staged on the page and saved
    // through the host's floating button, which says "Sync Engine".
    function test_aSmallSelectionSavedThroughTheHost() {
        const page = openOnAFreshCopy();
        for (const name of ["playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                            "cuesToEngine", "restoresToRekordbox"]) {
            page.controller.setSectionIncluded(name, false);
        }
        compare(page.controller.checkedCount, 0);
        const create = createRow(page);
        const members = rowsDependingOn(page, rowData(page, create, "key"));
        for (const m of members) {
            mouseClick(findChild(rowItem(page, m), "rowCheck"));
        }
        compare(rowData(page, create, "included"), true, "a member brings its playlist");
        compare(page.controller.checkedCount, 15);
        compare(findChild(page.header, "checkedFigure").value, "15");

        mouseClick(findChild(page.header, "stageButton"));
        compare(page.controller.errorMessage, "");
        compare(page.controller.stagedCount, 15);
        compare(findChild(page.header, "stageButton").text, "Stage Selected Again");
        compare(findChild(rowItem(page, create), "stagedLabel").visible, true);
        compare(findChild(page.header, "unstageAllButton").visible, true);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(page.rekordboxPath));
        verify(session !== null);
        compare(session.editorOwner, "rekordbox-export-sync");
        const save = findChild(page, "saveButton");
        verify(save !== null && save.visible, "the host offers its save");
        tryVerify(() => save.enabled);
        compare(findChild(page, "saveOverlay").label, "Sync Engine");

        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        mouseClick(save);
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.warning, "", JSON.stringify(summary));
        compare(summary.written, summary.total);

        // Analysed again: the record is there, and what is left is what
        // nobody decided, now Engine's own, and the conflicts that remain.
        tryVerify(() => page.controller.hasBaseline && !page.controller.busy, 120000, "the page analyses again");
        compare(page.controller.stagedCount, 0);
        compare(page.controller.sectionCounts["conflicts"], 3);
        compare(page.controller.sectionCounts["engineOwnKept"], 2439);
        compare(page.controller.sectionCounts["playlists"], 0);
        compare(page.controller.sectionCounts["membership"], 0);
        compare(findChild(page.header, "introLabel").text,
                "Compared with how this stick looked when Seabass last saved it, export 15132.");
        compare(page.firstSection, "conflicts");
        const undo = findChild(page.header, "undoButton");
        verify(undo.visible, "Undo is offered");
        verify(undo.enabled);
        const dialog = findChild(page, "summaryDialog");
        if (dialog && dialog.visible) {
            dialog.close();
        }
        if (screenshotDir && screenshotDir.length > 0) {
            listOf(page).positionViewAtBeginning();
            // At the top the header unfolds; the picture waits for all of it.
            const headerDetails = findChild(page.header, "headerDetails");
            tryVerify(() => headerDetails.visible && headerDetails.height === headerDetails.implicitHeight, 2000,
                      "the header unfolds at the top");
            waitForRendering(page);
            wait(100);
            grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-after-save.png");
        }
    }

    // One left line: the breadcrumb, the intro, a section header's box
    // and a row's box all start at the page margin.
    function test_oneLeftLine() {
        const page = openOnAFreshCopy();
        const crumb = findChild(page.header, "breadcrumb");
        compare(leftOf(page, crumb), Theme.pageMargin);
        compare(leftOf(page, findChild(page.header, "introLabel")), Theme.pageMargin);
        compare(leftOf(page, findChild(page.header, "legendChip")), Theme.pageMargin);
        compare(leftOf(page, listOf(page)), Theme.pageMargin);
        const header = sectionHeader(page, "membership");
        const sectionBox = findChild(header, "sectionCheck_membership");
        compare(leftOf(page, sectionBox.indicator), Theme.pageMargin);
        const create = createRow(page);
        compare(leftOf(page, findChild(rowItem(page, create), "rowCheck").indicator), Theme.pageMargin);
        // A row gap between the box and the chip, and the section's title
        // over its rows' chips.
        const chip = findChild(rowItem(page, create), "directionChip");
        const box = findChild(rowItem(page, create), "rowCheck");
        verify(leftOf(page, chip) - (leftOf(page, box.indicator) + box.indicator.width) >= Theme.rowSpacing,
               "chip at " + leftOf(page, chip) + ", box ends at " + (leftOf(page, box.indicator) + box.indicator.width));
        compare(leftOf(page, findChild(sectionHeader(page, "membership"), "sectionTitle")),
                leftOf(page, findChild(rowItem(page, firstRowWhere(page, (i) => rowData(page, i, "section") === "membership")),
                                       "directionChip")));
        // A section without a box puts its title on the line.
        compare(leftOf(page, findChild(sectionHeader(page, "conflicts"), "sectionTitle")), Theme.pageMargin);
        // Every title in one column, whatever the row carries.
        const kept = firstRowWhere(page, (i) => rowData(page, i, "section") === "engineOwnKept");
        compare(leftOf(page, findChild(rowItem(page, kept), "rowTitle")),
                leftOf(page, findChild(rowItem(page, create), "rowTitle")));
        compare(leftOf(page, findChild(rowItem(page, 0), "rowTitle")), leftOf(page, findChild(rowItem(page, create), "rowTitle")));
    }

    function test_screenshot() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        const page = openOnAFreshCopy();
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page.png");
        // Past the conflicts: the header folded, the writable sections.
        listOf(page).positionViewAtIndex(createRow(page), ListView.Beginning);
        waitForRendering(page);
        wait(400);
        grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-playlists.png");
    }
}
