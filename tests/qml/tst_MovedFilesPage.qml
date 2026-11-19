// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's "Moved files" page (#103), driven by a stand-in
// controller: one finding per shape the check can report, and what the
// page offers for each.
TestCase {
    id: testCase
    name: "MovedFilesPage"
    width: 1000
    height: 800
    visible: true
    when: windowShown

    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool writing: false
            property bool canUndo: false
            property bool stickReadOnly: false
            property bool movedFilesChecked: true
            property string movedFilesError: ""
            property var movedFiles: [
                // The one-click case: the same name and size elsewhere.
                {expectedPath: "/S/Contents/Old/Alpha.mp3", title: "Alpha", artist: "Someone",
                 libraries: "rekordbox, OneLibrary, Engine",
                 candidates: [{filePath: "/S/Contents/New/Alpha.mp3", confidence: "high",
                               reason: "the same name and the same size"}],
                 oneClick: true, oneToConfirm: false, relinkRefusal: "", staged: ""},
                // Probably: asked about before it is staged, and the
                // library cannot be pointed at it.
                {expectedPath: "/S/Contents/Old/Bravo.mp3", title: "Bravo", artist: "",
                 libraries: "rekordbox",
                 candidates: [{filePath: "/S/Contents/Longer folder name/Bravo.mp3", confidence: "confirm",
                               reason: "the same name, but 1001 bytes where the library recorded 1000"}],
                 oneClick: false, oneToConfirm: true,
                 relinkRefusal: "the new path is 40 characters, and the track's path field in export.pdb holds 24",
                 staged: ""},
                // Two files could be it: report only.
                {expectedPath: "/S/Contents/Old/Charlie.mp3", title: "Charlie", artist: "Else",
                 libraries: "Engine",
                 candidates: [{filePath: "/S/Contents/A/Charlie.mp3", confidence: "confirm", reason: "the same name"},
                              {filePath: "/S/Contents/B/Charlie.mp3", confidence: "confirm", reason: "the same name"}],
                 oneClick: false, oneToConfirm: false, relinkRefusal: "", staged: ""},
                // Nothing on the stick.
                {expectedPath: "/S/Contents/Old/Delta.mp3", title: "", artist: "",
                 libraries: "rekordbox", candidates: [],
                 oneClick: false, oneToConfirm: false, relinkRefusal: "", staged: ""}]
            property int movedFileCount: movedFiles.length
            property int movedFileFoundCount: movedFiles.filter(f => f.candidates.length > 0).length
            property int movedFileOneClickCount: movedFiles.filter(f => f.oneClick).length
            property int unstagedMovedFileOneClickCount: movedFiles.filter(f => f.oneClick && f.staged === "").length
            property int movedFileStagedCount: movedFiles.filter(f => f.staged !== "").length
            property var calls: []
            function restage(path, how) {
                movedFiles = movedFiles.map(f => f.expectedPath === path ? Object.assign({}, f, {staged: how}) : f);
            }
            function moveFileBack(path) {
                calls.push("move " + path);
                restage(path, "move");
            }
            function moveAllFilesBack() {
                calls.push("move all");
                movedFiles = movedFiles.map(f => f.oneClick ? Object.assign({}, f, {staged: "move"}) : f);
            }
            function relinkMovedFile(path) {
                calls.push("relink " + path);
                restage(path, "relink");
            }
            function unstageMovedFile(path) {
                calls.push("unstage " + path);
                restage(path, "");
            }
            function unstageMovedFiles() {
                calls.push("unstage all");
                movedFiles = movedFiles.map(f => Object.assign({}, f, {staged: ""}));
            }
            property string errorMessage: ""
            property string statusMessage: ""
            property string scanningFormat: ""
            property int scanCurrent: 0
            property int scanTotal: 0
            property bool scanCancellable: false
            signal scanCancelled()
            function scan(a, b, c) {}
            function cancelScan() {}
            function undoLastOperation() {}
        }
    }

    Component {
        id: pageComponent
        MovedFilesPage {
            width: 980
            height: 760
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
        }
    }

    // Built from character codes, so this file holds no dash literal.
    function noDashes(text) {
        return text.indexOf(String.fromCharCode(0x2014)) < 0 && text.indexOf("-".repeat(2)) < 0;
    }

    function makePage(props) {
        const controller = createTemporaryObject(controllerComponent, testCase, props || {});
        const page = createTemporaryObject(pageComponent, testCase, {sharedController: controller});
        verify(page !== null);
        waitForRendering(page);
        return {page: page, controller: controller};
    }

    function test_theSummaryCountsTracksAndFiles() {
        const t = makePage();
        const summary = findChild(t.page, "movedFilesSummary");
        compare(summary.text, "4 tracks point at a file that is not there. Found on the stick: 3.");
        verify(findChild(t.page, "movedFilesExplanation").visible);
        for (const label of [summary, findChild(t.page, "movedFilesExplanation")]) {
            verify(noDashes(label.text), "no dashes on screen");
        }
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(t.page);
            grabImage(t.page).save(screenshotDir + "/moved-files-page.png");
        }
    }

    // The sure match is staged with one press; the probable one is asked
    // about first, with the reason, and staged only on yes.
    function test_aSureMatchMovesAtOnceAndAProbableOneIsAskedAbout() {
        const t = makePage();
        const alpha = findChild(t.page, "move_/S/Contents/Old/Alpha.mp3");
        compare(alpha.text, "Move Back");
        compare(findChild(t.page, "state_/S/Contents/Old/Alpha.mp3").text,
                "Found: /S/Contents/New/Alpha.mp3 (the same name and the same size)");
        alpha.clicked();
        compare(t.controller.calls[0], "move /S/Contents/Old/Alpha.mp3", "staged without a question");
        tryCompare(findChild(t.page, "move_/S/Contents/Old/Alpha.mp3"), "text", "Unstage");
        compare(findChild(t.page, "state_/S/Contents/Old/Alpha.mp3").text,
                "Staged: /S/Contents/New/Alpha.mp3 moves back");

        const bravo = findChild(t.page, "move_/S/Contents/Old/Bravo.mp3");
        compare(findChild(t.page, "state_/S/Contents/Old/Bravo.mp3").text.indexOf("Probably: "), 0);
        bravo.clicked();
        const dialog = findChild(t.page, "confirmMoveDialog");
        tryCompare(dialog, "visible", true);
        verify(dialog.headline.indexOf("1001 bytes") >= 0, "the reason is in the question: " + dialog.headline);
        compare(t.controller.calls.length, 1, "nothing staged until the question is answered");
        findChild(dialog, "rejectButton").clicked();
        tryCompare(dialog, "visible", false);
        compare(t.controller.calls.length, 1, "no means no");
        bravo.clicked();
        tryCompare(dialog, "visible", true);
        findChild(dialog, "acceptButton").clicked();
        tryCompare(dialog, "visible", false);
        compare(t.controller.calls[1], "move /S/Contents/Old/Bravo.mp3");
        tryCompare(findChild(t.page, "move_/S/Contents/Old/Bravo.mp3"), "text", "Unstage");

        findChild(t.page, "move_/S/Contents/Old/Alpha.mp3").clicked();
        compare(t.controller.calls[2], "unstage /S/Contents/Old/Alpha.mp3");
        tryCompare(findChild(t.page, "move_/S/Contents/Old/Alpha.mp3"), "text", "Move Back");
    }

    // Pointing the library at the file is the other repair, offered only
    // where every catalog can take the path.
    function test_relinkIsOfferedOnlyWhereTheCatalogsCanTakeThePath() {
        const t = makePage();
        const alpha = findChild(t.page, "relink_/S/Contents/Old/Alpha.mp3");
        verify(alpha.enabled);
        compare(findChild(t.page, "relinkRefusal_/S/Contents/Old/Alpha.mp3").visible, false);
        alpha.clicked();
        compare(t.controller.calls[0], "relink /S/Contents/Old/Alpha.mp3");
        tryCompare(findChild(t.page, "state_/S/Contents/Old/Alpha.mp3"), "text",
                   "Staged: the library will point at /S/Contents/New/Alpha.mp3");
        compare(findChild(t.page, "relink_/S/Contents/Old/Alpha.mp3").visible, false, "one repair at a time");

        const bravo = findChild(t.page, "relink_/S/Contents/Old/Bravo.mp3");
        compare(bravo.enabled, false);
        const why = findChild(t.page, "relinkRefusal_/S/Contents/Old/Bravo.mp3");
        verify(why.visible);
        verify(why.text.indexOf("export.pdb holds 24") >= 0, why.text);
        verify(noDashes(why.text));
    }

    // Several files, or none: said, and nothing offered.
    function test_severalCandidatesOrNoneOfferNothing() {
        const t = makePage();
        compare(findChild(t.page, "move_/S/Contents/Old/Charlie.mp3").visible, false);
        const charlie = findChild(t.page, "state_/S/Contents/Old/Charlie.mp3");
        compare(charlie.text.indexOf("2 files could be this track, so nothing is offered: "), 0);
        verify(charlie.text.indexOf("/S/Contents/A/Charlie.mp3") >= 0 && charlie.text.indexOf("/S/Contents/B/Charlie.mp3") >= 0);
        compare(findChild(t.page, "move_/S/Contents/Old/Delta.mp3").visible, false);
        compare(findChild(t.page, "state_/S/Contents/Old/Delta.mp3").text, "No file on the stick matches this track.");
        compare(findChild(t.page, "title_/S/Contents/Old/Delta.mp3").text, "Old/Delta.mp3", "no title: the path");
    }

    // Move All Back takes the sure matches only, and becomes Unstage All
    // once nothing sure is left to stage.
    function test_moveAllBackTakesTheSureMatchesOnly() {
        const t = makePage();
        const all = findChild(t.page, "moveAllBackButton");
        compare(all.text, "Move 1 File Back");
        all.clicked();
        compare(t.controller.calls[0], "move all");
        tryCompare(all, "text", "Unstage All");
        compare(findChild(t.page, "move_/S/Contents/Old/Bravo.mp3").text, "Move Back", "the probable one is not taken");
        all.clicked();
        compare(t.controller.calls[1], "unstage all");
        tryCompare(all, "text", "Move 1 File Back");
    }

    function test_aReadOnlyStickOffersNoRepair() {
        const t = makePage({stickReadOnly: true});
        compare(findChild(t.page, "moveAllBackButton").enabled, false);
        compare(findChild(t.page, "move_/S/Contents/Old/Alpha.mp3").enabled, false);
        compare(findChild(t.page, "relink_/S/Contents/Old/Alpha.mp3").enabled, false);
    }

    function test_nothingListedSaysSo() {
        const t = makePage({movedFiles: []});
        compare(findChild(t.page, "movedFilesSummary").text, "Every track's file is where its library expects it.");
        compare(findChild(t.page, "movedFilesExplanation").visible, false);
        compare(findChild(t.page, "moveAllBackButton").visible, false);
    }

    function test_aCheckThatCouldNotRunSaysSo() {
        const t = makePage({movedFiles: [], movedFilesChecked: false, movedFilesError: "the stick went away"});
        compare(findChild(t.page, "movedFilesSummary").text, "The check could not run: the stick went away");
    }

    // Many findings do not fit the page: the list scrolls, with room under
    // the last row for the floating Save button.
    function test_aLongListScrolls() {
        const many = [];
        for (let i = 0; i < 60; ++i) {
            many.push({expectedPath: "/S/Contents/Old/t" + i + ".mp3", title: "Track " + i, artist: "Someone",
                       libraries: "rekordbox", candidates: [], oneClick: false, oneToConfirm: false,
                       relinkRefusal: "", staged: ""});
        }
        const t = makePage({movedFiles: many});
        const view = findChild(t.page, "movedFilesView");
        tryVerify(() => view.contentHeight > view.height, 5000, "more than fits");
        verify(view.interactive, "and so it scrolls");
        verify(view.bottomMargin >= t.page.saveClearance - 0.5, "room for the Save button under the last row");
        view.contentY = view.contentHeight + view.bottomMargin - view.height;
        const last = findChild(t.page, "movedFileRows").itemAt(59);
        verify(last !== null);
        const bottom = last.mapToItem(view, 0, last.height).y;
        verify(bottom <= view.height - t.page.saveClearance + 0.5 && bottom > 0, "the last row is on screen: " + bottom);
    }
}
