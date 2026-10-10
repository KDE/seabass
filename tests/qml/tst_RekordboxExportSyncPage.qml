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
        details: 275, hasTrack: 276, artworkPath: 277, fallbackArtworkPath: 278,
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

    // withCovers: every Engine track names one real cover on the copy
    // (StickFixture::stickCopyWithEngineCovers).
    function openOnAFreshCopy(withCovers) {
        const stick = withCovers ? stickFixture.stickCopyWithEngineCovers(testCase.fixtureRoot)
                                 : stickFixture.stickCopy(testCase.fixtureRoot);
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
    // The delegate of model row i, scrolled into view: the view's index
    // differs past a folded section (page.viewIndexOf).
    function rowItem(page, i) {
        const list = listOf(page);
        const v = page.viewIndexOf(i);
        verify(v >= 0, "row " + i + " is in the view, not folded away");
        list.positionViewAtIndex(v, ListView.Beginning);
        waitForRendering(list);
        const item = list.itemAtIndex(v);
        verify(item !== null, "row " + i + " is in view");
        return item;
    }
    // A section header, scrolled into view with its first row.
    function sectionHeader(page, name) {
        const first = firstRowWhere(page, (i) => rowData(page, i, "section") === name);
        verify(first >= 0, name + " has rows");
        const list = listOf(page);
        list.positionViewAtIndex(page.viewIndexOf(first), ListView.Center);
        waitForRendering(list);
        const header = findChildWhere(list, (o) => o.objectName === "sectionHeader_" + name && o.visible);
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
        compare(intro.text, "No earlier record of this stick. What rekordbox has and Engine lacks is taken as added "
                + "in rekordbox and is selected; what Engine has and rekordbox lacks is left for you to decide. "
                + "Anything you leave undecided counts as Engine's own from now on and is not asked again.");
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
        compare(legendTexts, ["Will write into the Engine library so it matches rekordbox",
                              "Cues or ratings Seabass had put on the rekordbox side that the export dropped will go "
                              + "back onto it",
                              "Engine's own, nothing will be written"]);
        // Engine's own carry no checkbox, nor does a conflict; a writable row does.
        const kept = firstRowWhere(page, (i) => rowData(page, i, "section") === "engineOwnKept");
        page.toggleSection("engineOwnKept");
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

    // The first line says what the page is for; the overview bar's legend
    // counts what the model holds, by category, and leaves out what is
    // empty; the conflicts are not in it.
    function test_explanationAndOverview() {
        const page = openOnAFreshCopy();
        const explanation = findChild(page.header, "explanationLabel");
        verify(explanation.visible);
        compare(explanation.text, "Rekordbox has changed the library on this stick. This page brings everything back "
                + "in step with Engine.");
        compare(explanation.color, Theme.text);
        verify(explanation.mapToItem(page, 0, 0).y < findChild(page.header, "introLabel").mapToItem(page, 0, 0).y,
               "it comes before the intro");
        // Under it, the one plain line of what the proposal does.
        const summary = findChild(page.header, "summaryLabel");
        verify(summary.visible);
        compare(summary.text, "779 tracks moved between playlists; 1 playlist created.");
        compare(summary.text, page.controller.summaryText);
        const below = (a, b) => a.mapToItem(page, 0, 0).y > b.mapToItem(page, 0, 0).y;
        verify(below(summary, explanation), "under the explanation");
        verify(below(findChild(page.header, "legendLine"), summary), "before the legend");
        // Counted here off the rows.
        let added = 0, created = 0, changed = 0, removed = 0, other = 0;
        for (let i = 0; i < rowCount(page); ++i) {
            const section = rowData(page, i, "section");
            const kind = rowData(page, i, "kind");
            if (section === "tracksToAdd") {
                ++added;
            } else if (section === "tracksToRemove" || kind === "removeMember" || kind.startsWith("delete")) {
                ++removed;
            } else if (kind.startsWith("create")) {
                ++created;
            } else if (kind === "renamePlaylist") {
                ++other;
            } else if (["membership", "metadataToEngine", "cuesToEngine", "restoresToRekordbox"].indexOf(section) >= 0) {
                ++changed;
            }
        }
        compare(changed, 952);
        compare(created, 1);
        compare(findChild(findChildWhere(page.header, (o) => o.objectName === "overviewLegend_newPlaylists"),
                          "overviewLegendText").text, "1 new playlist");
        const expected = {newTracks: added, newPlaylists: created, changed: changed, removed: removed, other: other};
        const labels = {newTracks: "new tracks", newPlaylists: "new playlists", changed: "changed", removed: "removed",
                        other: "other"};
        verify(findChild(page.header, "overview").visible);
        for (const key in expected) {
            compare(page.controller.categoryCounts[key], expected[key], key);
            const entry = findChildWhere(page.header, (o) => o.objectName === "overviewLegend_" + key);
            verify(entry !== null, key + " has a legend entry");
            compare(entry.visible, expected[key] > 0, key + " shows only with something in it");
            // One of a kind is said in the singular.
            const label = expected[key] === 1 ? labels[key].replace(/s$/, "") : labels[key];
            compare(findChild(entry, "overviewLegendText").text, expected[key] + " " + label);
            const segment = findChildWhere(page.header, (o) => o.objectName === "overviewSegment_" + key);
            compare(segment.visible, expected[key] > 0);
        }
        // To scale: changed is most of the bar.
        const bar = findChild(page.header, "overviewBar");
        const changedSegment = findChildWhere(page.header, (o) => o.objectName === "overviewSegment_changed");
        verify(changedSegment.width > bar.width * 0.9, changedSegment.width + " of " + bar.width);
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
        // A conflict's details list what each side writes, each list
        // headed by its button's words.
        const conflictLines = rowData(page, 0, "details");
        verify(conflictLines.indexOf(rowData(page, 0, "rekordboxChoiceLabel") + " (rekordbox's side):") >= 0,
               JSON.stringify(conflictLines));
        verify(conflictLines.indexOf(rowData(page, 0, "engineChoiceLabel") + " (Engine's side):") >= 0,
               JSON.stringify(conflictLines));
        if (screenshotDir && screenshotDir.length > 0) {
            mouseClick(findChild(rowItem(page, member), "rowTitle"));
            listOf(page).positionViewAtIndex(page.viewIndexOf(member), ListView.Center);
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
        // The overview stays, slim, as Clean Up Duplicates' space bar
        // does folded (SpaceReclaimBar: Theme.tightSpacing), without its
        // legend.
        const overview = findChild(page.header, "overview");
        const bar = findChild(page.header, "overviewBar");
        const legend = findChild(page.header, "overviewLegend");
        verify(overview.visible && bar.visible, "the bar stays when the header folds");
        compare(bar.height, Theme.tightSpacing);
        compare(legend.visible, false);
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
        compare(bar.height, 26);
        verify(legend.visible, "the legend is back");
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
        // Nothing left to write or decide but the conflicts: no bar, and
        // the first line still asks.
        compare(findChild(page.header, "overview").visible, false);
        compare(page.controller.proposalEmpty, false);
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
        page.toggleSection("engineOwnKept");
        compare(leftOf(page, findChild(rowItem(page, kept), "rowTitle")),
                leftOf(page, findChild(rowItem(page, create), "rowTitle")));
        compare(leftOf(page, findChild(rowItem(page, 0), "rowTitle")), leftOf(page, findChild(rowItem(page, create), "rowTitle")));
    }

    // A track's row shows its cover where the model names one, the
    // placeholder square where it names none; a playlist's row leaves the
    // slot empty; the titles stay in one column either way.
    function test_trackRowsShowTheirCovers() {
        const page = openOnAFreshCopy(true);
        // The conflicts at the top are Engine rows rekordbox no longer
        // lists: Engine's copy, so Engine's cover.
        compare(rowData(page, 0, "hasTrack"), true);
        const url = rowData(page, 0, "artworkPath");
        verify(url.indexOf("file:") === 0, url);
        verify(url.indexOf("seabass-test-cover.png") > 0, url);
        let item = rowItem(page, 0);
        const slot = findChild(item, "rowCoverSlot");
        verify(slot.visible);
        const art = findChild(item, "rowArtwork");
        compare(art.source.toString(), url);
        tryCompare(art, "showing", "source", 5000, "the cover loads");
        verify(findChild(art, "artworkImage").sourceSize.width > 0, "decoded at the size it is drawn");
        compare(slot.width, page.coverSide);
        // A membership's track is rekordbox's, whose cover the fixture
        // names but does not have (the anonymizer strips the images): the
        // load fails and the placeholder square shows, never a broken frame.
        const member = firstRowWhere(page, (i) => rowData(page, i, "kind") === "addMember");
        const memberUrl = rowData(page, member, "artworkPath");
        verify(memberUrl.indexOf("/PIONEER/Artwork/") > 0, memberUrl);
        item = rowItem(page, member);
        verify(findChild(item, "rowCoverSlot").visible, "the placeholder square");
        const memberArt = findChild(item, "rowArtwork");
        compare(memberArt.source.toString(), memberUrl);
        tryCompare(memberArt, "sourceFailed", true, 5000, "the named file is not there");
        compare(memberArt.showing, "");
        // A playlist's row: no slot drawn, its title in the same column.
        const create = createRow(page);
        compare(rowData(page, create, "hasTrack"), false);
        compare(rowData(page, create, "artworkPath"), "");
        const createItem = rowItem(page, create);
        compare(findChild(createItem, "rowCoverSlot").visible, false);
        compare(leftOf(page, findChild(createItem, "rowTitle")), leftOf(page, findChild(rowItem(page, member), "rowTitle")));
        if (screenshotDir && screenshotDir.length > 0) {
            listOf(page).positionViewAtBeginning();
            const headerDetails = findChild(page.header, "headerDetails");
            tryVerify(() => headerDetails.visible && headerDetails.height === headerDetails.implicitHeight, 2000);
            tryCompare(findChild(rowItem(page, 0), "rowArtwork"), "showing", "source", 5000);
            waitForRendering(page);
            wait(300);
            grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-covers.png");
        }
    }

    // Rows of each section the view shows (rowsModel's "shown" group),
    // and whether every one of them is drawn: by section name.
    function shownBySection(page) {
        const items = page.rowsModel.items;
        const out = {};
        for (let i = 0; i < items.count; ++i) {
            const item = items.get(i);
            // The model's own role: an item's model object is stale for a
            // row without a delegate.
            const name = rowData(page, i, "section");
            if (!(name in out)) {
                out[name] = 0;
            }
            if (item.inShown) {
                ++out[name];
            }
        }
        return out;
    }
    // Folded, a section shows its first row only, as an empty shell.
    function expectedShown(page, name, count) {
        return page.isCollapsed(name) ? Math.min(count, 1) : count;
    }
    // Every section folds and unfolds to exactly its own rows, and no
    // other section's rows change; the rows that come back are drawn.
    function foldEverySectionAndBack(page) {
        const counts = page.controller.sectionCounts;
        for (const name of page.sectionOrder) {
            const count = counts[name] || 0;
            if (count === 0) {
                continue;
            }
            const before = shownBySection(page);
            const startFolded = page.isCollapsed(name);
            page.toggleSection(name);
            page.toggleSection(name);
            compare(page.isCollapsed(name), startFolded);
            if (startFolded) {
                // Open it to look, then fold it back.
                page.toggleSection(name);
            }
            const after = shownBySection(page);
            compare(after[name], count, name + " shows all its " + count + " rows unfolded");
            for (const other of page.sectionOrder) {
                if (other !== name) {
                    compare(after[other] || 0, before[other] || 0, other + " is untouched by " + name + "'s fold");
                }
            }
            // The first few rows that came back are drawn.
            const first = firstRowWhere(page, (i) => rowData(page, i, "section") === name);
            for (let i = first; i < first + Math.min(count, 5); ++i) {
                const item = rowItem(page, i);
                verify(item.visible && item.height > 0, name + " row " + i + " is drawn");
                // Drawn as itself: the delegate's section and title are
                // the model's for that row.
                compare(item.section, name, "row " + i + "'s delegate says its section");
                compare(item.title, rowData(page, i, "title"), "row " + i + "'s delegate shows its title");
            }
            if (startFolded) {
                page.toggleSection(name);
                compare(shownBySection(page)[name], 1, name + " folds back to its shell");
            }
        }
    }

    // Each section by itself, on the fixture.
    function test_everySectionFoldsAndUnfoldsToItsRows() {
        const page = openOnAFreshCopy();
        const counts = page.controller.sectionCounts;
        const shown = shownBySection(page);
        for (const name of page.sectionOrder) {
            compare(shown[name] || 0, expectedShown(page, name, counts[name] || 0), name + " at the start");
        }
        foldEverySectionAndBack(page);
        // And with an answer's rows after the conflicts: every conflict
        // answered toward rekordbox adds removal rows to their section.
        page.controller.resolveAllConflicts(true);
        tryVerify(() => (page.controller.sectionCounts["tracksToRemove"] || 0) > 0, 2000);
        wait(50);
        const answered = page.controller.sectionCounts;
        const shownAnswered = shownBySection(page);
        for (const name of page.sectionOrder) {
            compare(shownAnswered[name] || 0, expectedShown(page, name, answered[name] || 0),
                    name + " after the answers");
        }
        foldEverySectionAndBack(page);
    }

    // The same on rows built by hand (StickFixture's), where the small
    // sections hold several rows each: four playlist rows, three cue
    // rows. A stand-in for the controller hands them to the page.
    Component {
        id: fakeControllerComponent
        QtObject {
            property var rows: null
            readonly property var sectionCounts: {
                const counts = {};
                for (let i = 0; i < rows.rowCount(); ++i) {
                    const name = rows.data(rows.index(i, 0), testCase.role.section);
                    counts[name] = (counts[name] || 0) + 1;
                }
                return counts;
            }
            readonly property var sectionCheckedCounts: ({})
            readonly property var categoryCounts: ({})
            property bool analyzed: true
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool onlyCues: false
            property bool proposalEmpty: false
            property bool hasBaseline: true
            property bool scanCancellable: false
            property int checkedCount: 0
            property int conflictCount: 1
            property int stagedCount: 0
            property int scanCurrent: 0
            property int scanTotal: 0
            property string scanLabel: ""
            property string errorMessage: ""
            property string statusMessage: ""
            property string introText: ""
            property string summaryText: ""
            signal scanCancelled()
            function analyze() {}
            function cancelScan() {}
            function setIncluded() {}
            function setSectionIncluded() {}
            function resolveConflict() {}
            function clearConflictResolution() {}
            function resolveAllConflicts() {}
            function clearAllConflictResolutions() {}
            function stageSelected() {}
            function unstageAll() {}
            function undoLastOperation() {}
        }
    }
    function test_everySectionFoldsAndUnfoldsOnHandBuiltRows() {
        const fake = createTemporaryObject(fakeControllerComponent, testCase,
                                           {rows: stickFixture.handBuiltExportSyncRows()});
        const page = createTemporaryObject(pageComponent, testCase, {controller: fake});
        verify(page !== null);
        waitForRendering(page);
        compare(fake.sectionCounts["playlists"], 4);
        compare(fake.sectionCounts["cuesToEngine"], 3);
        const shown = shownBySection(page);
        for (const name of page.sectionOrder) {
            compare(shown[name] || 0, expectedShown(page, name, fake.sectionCounts[name] || 0), name + " at the start");
        }
        foldEverySectionAndBack(page);
        // Through the header, as a person does it: fold Playlists, then
        // open it again from its chevron.
        mouseClick(findChild(sectionHeader(page, "playlists"), "sectionChevron"));
        compare(shownBySection(page)["playlists"], 1);
        mouseClick(findChild(sectionHeader(page, "playlists"), "sectionChevron"));
        compare(shownBySection(page)["playlists"], 4, "Playlists opens to its four rows");
        mouseClick(findChild(sectionHeader(page, "cuesToEngine"), "sectionTitle"));
        mouseClick(findChild(sectionHeader(page, "cuesToEngine"), "sectionTitle"));
        compare(shownBySection(page)["cuesToEngine"], 3, "Cues to Engine opens to its three rows");
        // Every section folded through its header, then each opened again.
        for (const name of page.sectionOrder) {
            if ((fake.sectionCounts[name] || 0) > 0 && !page.isCollapsed(name)) {
                mouseClick(findChild(sectionHeader(page, name), "sectionChevron"));
            }
        }
        for (const name of page.sectionOrder) {
            const count = fake.sectionCounts[name] || 0;
            if (count === 0) {
                continue;
            }
            compare(shownBySection(page)[name], 1, name + " folded");
            mouseClick(findChild(sectionHeader(page, name), "sectionChevron"));
            compare(page.isCollapsed(name), false, name + " opened");
            compare(shownBySection(page)[name], count, name + " opens to its rows");
            const first = firstRowWhere(page, (i) => rowData(page, i, "section") === name);
            for (let i = first; i < first + count; ++i) {
                const item = rowItem(page, i);
                verify(item.visible && item.height > 0, name + " row " + i + " is drawn");
                // Drawn as itself: the delegate's section and title are
                // the model's for that row.
                compare(item.section, name, "row " + i + "'s delegate says its section");
                compare(item.title, rowData(page, i, "title"), "row " + i + "'s delegate shows its title");
            }
        }
    }

    // A section folds from its chevron or its title: its rows are gone
    // and the next section moves up; the header stays with its count.
    // Engine's own and the refused adds start folded. A fold is kept by
    // name while the list is analysed again.
    function test_sectionsFold() {
        const page = openOnAFreshCopy();
        compare(page.isCollapsed("engineOwnKept"), true);
        compare(page.isCollapsed("notAdded"), true);
        compare(page.isCollapsed("conflicts"), false);
        compare(page.isCollapsed("membership"), false);
        compare(findChild(sectionHeader(page, "engineOwnKept"), "sectionChevron").iconName, "arrow-right");
        compare(findChild(sectionHeader(page, "engineOwnKept"), "sectionNote").text, "Kept, nothing is written");
        const kept = firstRowWhere(page, (i) => rowData(page, i, "section") === "engineOwnKept");
        let keptItem = rowItem(page, kept);
        compare(keptItem.visible, false, "a folded section's row is not shown");
        compare(keptItem.height, 0);

        const create = createRow(page);
        // From the playlists' header to the membership's: content
        // coordinates move as the list lays out, the distance does not.
        const gap = () => {
            const playlists = sectionHeader(page, "playlists");
            return findChildWhere(listOf(page), (o) => o.objectName === "sectionHeader_membership" && o.visible).y
                - playlists.y;
        };
        const membershipGap = gap();
        const playlistsHeader = sectionHeader(page, "playlists");
        const chevron = findChild(playlistsHeader, "sectionChevron");
        compare(chevron.iconName, "arrow-down");
        mouseClick(chevron);
        compare(page.isCollapsed("playlists"), true);
        let item = rowItem(page, create);
        compare(item.visible, false, "the create's row is gone");
        compare(item.height, 0);
        compare(findChild(item, "rowTitle"), null, "nothing is built inside a folded row");
        verify(gap() < membershipGap - 20, "the next header moved up: " + gap() + ", was " + membershipGap);
        compare(findChild(sectionHeader(page, "playlists"), "sectionCount").text, "1", "the count stays");
        verify(findChild(sectionHeader(page, "playlists"), "sectionCheck_playlists").visible, "the box stays");
        compare(page.controller.sectionCounts["playlists"], 1, "the model is not filtered");
        // A long section: only its first row stays in the view, as the
        // empty shell that carries the header.
        const members = firstRowWhere(page, (i) => rowData(page, i, "section") === "membership");
        const shownBefore = listOf(page).count;
        mouseClick(findChild(sectionHeader(page, "membership"), "sectionChevron"));
        compare(listOf(page).count, shownBefore - 924);
        compare(page.viewIndexOf(members + 1), -1);
        compare(rowItem(page, members).height, 0);
        compare(findChild(sectionHeader(page, "membership"), "sectionCount").text, "925");
        mouseClick(findChild(sectionHeader(page, "membership"), "sectionChevron"));
        compare(listOf(page).count, shownBefore);
        if (screenshotDir && screenshotDir.length > 0) {
            listOf(page).positionViewAtIndex(page.viewIndexOf(create), ListView.Center);
            waitForRendering(page);
            wait(300);
            grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-section-folded.png");
        }

        // Analysed again: the fold holds, by name.
        page.controller.analyze(page.stickLabel, page.rekordboxPath, page.enginePath);
        tryVerify(() => page.controller.busy, 5000, "the analysis starts");
        tryCompare(page.controller, "busy", false, 120000);
        compare(page.isCollapsed("playlists"), true);
        compare(rowItem(page, createRow(page)).visible, false, "still folded after the analysis");

        // The title opens it again.
        mouseClick(findChild(sectionHeader(page, "playlists"), "sectionTitle"));
        compare(page.isCollapsed("playlists"), false);
        item = rowItem(page, createRow(page));
        verify(item.visible && item.height > 0, "the row is back");
        verify(findChild(item, "rowTitle") !== null);
        compare(findChild(sectionHeader(page, "playlists"), "sectionChevron").iconName, "arrow-down");
        tryVerify(() => Math.abs(gap() - membershipGap) < 1, 2000, "the next header is back where it was");
    }

    // What picking a side means: a line under the conflicts' header, the
    // two sentences in its help and on the buttons that answer all.
    function test_conflictsSayWhatASideMeans() {
        const page = openOnAFreshCopy();
        const header = sectionHeader(page, "conflicts");
        const line = findChild(header, "conflictExplanation");
        verify(line.visible);
        compare(line.text, "Each button says what it writes. Nothing is written for a conflict you leave open.");
        verify(Qt.colorEqual(String(line.color), String(Theme.textMuted)), "muted");
        const title = findChild(header, "sectionTitle");
        verify(line.mapToItem(header, 0, 0).y > title.mapToItem(header, 0, 0).y + title.height - 1,
               "under the header's title");
        compare(page.conflictExplanation,
                "Rekordbox's side makes Engine match what rekordbox has now, for example by removing a track "
                + "rekordbox no longer lists. Engine's side keeps what Engine has, and for some cues and ratings "
                + "puts Engine's version back onto rekordbox, or for a playlist rekordbox may have renamed adds "
                + "rekordbox's beside Engine's.");
        const info = findChild(header, "conflictInfoButton");
        verify(info.visible);
        compare(info.summaryText, page.conflictExplanation);
        verify(findChild(header, "rekordboxSideForAllButton").toolTipText.indexOf(page.conflictExplanation) > 0);
        verify(findChild(header, "engineSideForAllButton").toolTipText.indexOf(page.conflictExplanation) > 0);
        compare(findChild(sectionHeader(page, "membership"), "conflictExplanation").visible, false);
        compare(findChild(sectionHeader(page, "membership"), "conflictInfoButton").visible, false);
        // A row's buttons are its own concrete answer.
        const item = rowItem(page, 0);
        compare(findChild(item, "rekordboxChoiceButton").text, "Remove this track from Engine");
        compare(findChild(item, "engineChoiceButton").text, "Keep it in Engine");
    }

    function test_screenshot() {
        if (!screenshotDir || screenshotDir.length === 0) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        const page = openOnAFreshCopy(true);
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page.png");
        // Past the conflicts: the header folded, the writable sections.
        listOf(page).positionViewAtIndex(page.viewIndexOf(createRow(page)), ListView.Beginning);
        waitForRendering(page);
        wait(400);
        grabImage(page).save(screenshotDir + "/rekordbox-export-sync-page-playlists.png");
    }
}
