// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Health's "OneLibrary cue tables" page (#57), driven by a
// stand-in controller.
TestCase {
    id: testCase
    name: "CueTablePage"
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
            property bool cueTablesChecked: true
            property string cueTablesError: ""
            property var cueTableRows: [
                {contentId: "392", title: "Alpha", artist: "Someone", filePath: "/S/Contents/a.mp3",
                 extra: "pad E at 2:15.251, pad B at 1:07.751",
                 fileCues: "memory cue at 0:04.399, pad D at 0:01.657, pad A at 0:00.247, pad C at 0:01.188", staged: false},
                {contentId: "17", title: "", artist: "", filePath: "/S/Contents/b.mp3",
                 extra: "pad A at 0:30.000", fileCues: "none", staged: false}]
            property int cueTableRowCount: cueTableRows.length
            property int cueTableStagedCount: 0
            property var calls: []
            function restage(ids, on) {
                cueTableRows = cueTableRows.map(r => Object.assign({}, r, {staged: ids.indexOf(r.contentId) >= 0 ? on : r.staged}));
                cueTableStagedCount = cueTableRows.filter(r => r.staged).length;
            }
            function repairCueTables(ids) {
                calls.push("repair " + ids.join(","));
                restage(ids.length === 0 ? cueTableRows.map(r => r.contentId) : ids, true);
            }
            function unstageCueTable(id) {
                calls.push("unstage " + id);
                restage([id], false);
            }
            function unstageCueTables() {
                calls.push("unstage all");
                restage(cueTableRows.map(r => r.contentId), false);
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
        CueTablePage {
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

    function test_oneRowStagesThatRowAndUnstagesIt() {
        const t = makePage();
        const summary = findChild(t.page, "cueTableSummary");
        compare(summary.text, "2 tracks' OneLibrary cue table holds cues its analysis file does not.");
        verify(findChild(t.page, "cueTableExplanation").visible);
        compare(findChild(t.page, "extra_392").text, "Only in the table: pad E at 2:15.251, pad B at 1:07.751");
        // Looked up again after each press: a new list of rows makes new
        // delegates.
        compare(findChild(t.page, "repair_392").text, "Repair");
        findChild(t.page, "repair_392").clicked();
        compare(t.controller.calls[0], "repair 392", "only the row pressed is staged");
        tryCompare(findChild(t.page, "repair_392"), "text", "Unstage");
        compare(findChild(t.page, "repair_17").text, "Repair", "the other row stays as it was");
        compare(findChild(t.page, "repairAllButton").text, "Repair All");
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(t.page);
            grabImage(t.page).save(screenshotDir + "/cue-table-page.png");
        }
        findChild(t.page, "repair_392").clicked();
        compare(t.controller.calls[1], "unstage 392");
        tryCompare(findChild(t.page, "repair_392"), "text", "Repair");
        for (const label of [summary, findChild(t.page, "cueTableExplanation"), findChild(t.page, "extra_392")]) {
            verify(noDashes(label.text), "no dashes on screen");
        }
    }

    function test_repairAllStagesEveryRowAndUnstageAllTakesThemBack() {
        const t = makePage();
        const all = findChild(t.page, "repairAllButton");
        all.clicked();
        compare(t.controller.calls[0], "repair ", "an empty list means every listed row");
        compare(t.controller.cueTableStagedCount, 2);
        compare(all.text, "Unstage All");
        tryCompare(findChild(t.page, "repair_17"), "text", "Unstage");
        all.clicked();
        compare(t.controller.calls[1], "unstage all");
        compare(t.controller.cueTableStagedCount, 0);
    }

    function test_aReadOnlyStickOffersNoRepair() {
        const t = makePage({stickReadOnly: true});
        compare(findChild(t.page, "repairAllButton").enabled, false);
        compare(findChild(t.page, "repair_392").enabled, false);
    }

    function test_nothingListedSaysSo() {
        const t = makePage({cueTableRows: []});
        compare(findChild(t.page, "cueTableSummary").text,
                "Every OneLibrary cue table on this stick agrees with its analysis file.");
        compare(findChild(t.page, "cueTableExplanation").visible, false);
        compare(findChild(t.page, "repairAllButton").visible, false);
    }
}
