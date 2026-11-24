// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Sync Cue Points, filled with a fixed analysis (SyncPageFixture in
// qml_test_main.cpp) so its layout can be measured, and looked at, without
// a stick.
//
// What the redesign was for, held in numbers: every title in one column
// across both sections; an expanded panel lined up with that title rather
// than hanging off a label column; a decision's two waveforms the same
// size side by side, and stacked on a narrow window; Select All/None and
// Stage Selected confined to what the search shows, the way Clean Up is;
// and a list that can always scroll its last row clear of Save.
TestCase {
    id: testCase
    name: "SyncPage"
    width: 1100
    height: 720
    visible: true
    when: windowShown

    // Bound as Main.qml binds the app's one player: nothing is read
    // while a library is being written (see tst_CleanupPage).
    PlaybackController {
        id: realPlayback
        libraryBusy: EditSessionRegistry.anyWriting
    }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        SyncPage {
            stickLabel: "TESTSTICK"
            rekordboxPath: ""
            enginePath: ""
            playbackController: realPlayback
            appSettingsController: realAppSettings
        }
    }

    SignalSpy {
        id: junkSpy
        signalName: "junkCueCleanupRequested"
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/is not a function/);
        failOnWarning(/Unable to assign/);
        failOnWarning(/Cannot read property/);
    }

    function openPage(pageWidth, pageHeight) {
        var page = createTemporaryObject(pageComponent, testCase, {width: pageWidth, height: pageHeight});
        verify(page !== null, "page did not instantiate");
        var controller = findChild(page, "syncController");
        verify(controller !== null, "the page's controller was not found");
        // The page analyzes on open; with no catalogs that finishes at once
        // and empty. Filled only after, or it would wipe the fixture.
        tryCompare(controller, "busy", false, 5000);
        verify(syncPageFixture.fill(controller), "the fixture did not take");
        waitForRendering(page);
        return page;
    }

    // A page on a stick that exists on disk, so staging can take the edit
    // lock; the plans are still the fixture's.
    function openStagingPage(pageWidth, pageHeight) {
        var stick = syncPageFixture.scratchStick();
        verify(stick.length > 0, "no scratch stick");
        var page = createTemporaryObject(pageComponent, testCase, {
            width: pageWidth, height: pageHeight,
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        var controller = findChild(page, "syncController");
        tryCompare(controller, "busy", false, 5000);
        verify(syncPageFixture.fill(controller), "the fixture did not take");
        waitForRendering(page);
        return page;
    }

    function listOf(page) {
        var list = findChild(page, "plansList");
        verify(list !== null, "the list was not found");
        return list;
    }

    function collect(item, name, found) {
        found = found || [];
        if (item.objectName === name && item.visible) {
            found.push(item);
        }
        for (var i = 0; i < item.children.length; ++i) {
            collect(item.children[i], name, found);
        }
        return found;
    }

    function xIn(page, item) {
        return item.mapToItem(page, 0, 0).x;
    }

    function test_decisionsComeFirstAndEveryTitleLinesUp() {
        var page = openPage(1100, 720);
        var list = listOf(page);
        compare(list.count, 8, "two decisions and six tracks ready to sync");

        var titleX = -1;
        for (var r = 0; r < 4; ++r) {
            var row = list.itemAtIndex(r);
            verify(row !== null, "row " + r + " was not built");
            compare(row.needsDecision, r < 2, "decisions come first");
            var x = xIn(page, findChild(row, "rowTitle"));
            if (titleX < 0) {
                titleX = x;
            } else {
                compare(x, titleX, "row " + r + "'s title is out of line with the first row's");
            }
        }

        var decisionBox = findChild(list.itemAtIndex(0), "selectCheckBox");
        compare(decisionBox.enabled, false, "a decision cannot be ticked");
        compare(decisionBox.opacity, 0, "and shows no tick box, only its space");
        compare(findChild(list.itemAtIndex(2), "selectCheckBox").enabled, true);
    }

    function test_rightHandColumnsLineUp() {
        var page = openPage(1100, 720);
        var list = listOf(page);
        var names = ["directionColumn", "cueSummaryColumn", "statusColumn"];
        for (var n = 0; n < names.length; ++n) {
            var first = xIn(page, findChild(list.itemAtIndex(0), names[n]));
            for (var r = 1; r < 4; ++r) {
                compare(xIn(page, findChild(list.itemAtIndex(r), names[n])), first,
                        names[n] + " moves on row " + r);
            }
        }
    }

    function test_expandedSidesLineUpWithTheTitle_data() {
        return [
            {tag: "wide decision", pageWidth: 1100, row: 0, sideBySide: true},
            {tag: "wide ready track", pageWidth: 1100, row: 2, sideBySide: true},
            {tag: "narrow decision", pageWidth: 700, row: 0, sideBySide: false},
            {tag: "narrow ready track", pageWidth: 700, row: 2, sideBySide: false},
        ];
    }

    function test_expandedSidesLineUpWithTheTitle(data) {
        var page = openPage(data.pageWidth, 720);
        var list = listOf(page);
        var row = list.itemAtIndex(data.row);
        row.expanded = true;
        waitForRendering(page);

        var extra = findChild(row, "expandedExtra");
        verify(extra.visible, "the expanded content is not showing");
        compare(xIn(page, extra), xIn(page, findChild(row, "rowTitle")),
                "the panel must start where the title does");

        var cards = collect(row, "sideCard");
        compare(cards.length, 2, "both copies of the track are shown");
        compare(cards[0].width, cards[1].width, "the two sides are not the same width");
        var waves = collect(row, "sideWaveform");
        compare(waves.length, 2);
        compare(waves[0].width, waves[1].width, "the two waveforms are not the same width");
        compare(waves[0].height, waves[1].height, "the two waveforms are not the same height");
        if (data.sideBySide) {
            compare(cards[0].mapToItem(page, 0, 0).y, cards[1].mapToItem(page, 0, 0).y, "side by side");
            verify(xIn(page, cards[1]) > xIn(page, cards[0]));
        } else {
            compare(xIn(page, cards[0]), xIn(page, cards[1]), "stacked");
            verify(cards[1].mapToItem(page, 0, 0).y > cards[0].mapToItem(page, 0, 0).y);
        }
    }

    // Every decision says why it is one, under the track, in the row's
    // small type: the planner's own reason for the fixture's pair. A ready
    // track has nothing to explain and shows no such line.
    function test_aDecisionSaysWhy() {
        const page = openPage(1100, 720);
        const list = listOf(page);
        const decision = list.itemAtIndex(0);
        const note = findChild(decision, "rowNote");
        verify(note !== null, "the decision row has no reason line");
        verify(note.visible, "the reason line is hidden");
        compare(note.text, "Pad 2: rekordbox 2:00.000, Engine 2:30.000; Pad 4: rekordbox 4:20.000, Engine 5:00.000; "
                + "Pad 5: rekordbox 5:30.000, Engine empty");
        compare(note.font.pointSize, Theme.fontSmall);
        compare(note.maximumLineCount, 2);
        const title = findChild(decision, "rowTitle");
        verify(note.mapToItem(page, 0, 0).y > title.mapToItem(page, 0, 0).y, "the reason sits under the title");
        compare(xIn(page, note), xIn(page, title), "and on the title's line");
        compare(findChild(list.itemAtIndex(1), "rowNote").text, "Pad 2: Engine 2:20.000, rekordbox 2:12.000",
                "named in the pair's own order, as the row lists the two sides");
        compare(findChild(list.itemAtIndex(2), "rowNote").visible, false, "a ready track has no reason line");
    }

    function test_theKeptCueIsCountedAsKept() {
        var page = openPage(1100, 720);
        var row = listOf(page).itemAtIndex(2);
        compare(row.cueSummary, "+3 hot, keeps 1");
        row.expanded = true;
        waitForRendering(page);
        compare(findChild(row, "panelSentence").text,
                "Adds 3 hot cues from Engine and keeps the 1 cue DeviceLibrary already has. "
                + "Nothing is written until Save.");
    }

    function test_selectionAndStagingStayInsideTheSearch() {
        var page = openStagingPage(1100, 720);
        var controller = findChild(page, "syncController");
        compare(controller.selectedCount, 6, "every track ready to sync starts ticked");

        findChild(page, "searchField").text = "kollektiv";
        compare(controller.visibleConflictCount, 1);
        compare(controller.visiblePlanCount, 1);

        mouseClick(findChild(page, "selectNoneButton"));
        compare(controller.selectedVisibleCount, 0);
        compare(controller.selectedCount, 5, "Select None reached tracks the search hides");
        mouseClick(findChild(page, "selectAllButton"));
        compare(controller.selectedVisibleCount, 1);

        var stage = findChild(page, "stageSelectedButton");
        compare(stage.text, "Stage 1 Selected", "the button counts what the search shows");
        mouseClick(stage);
        // No confirmation: staging is a step short of writing anything.
        tryCompare(controller, "stagedCount", 1, 2000);
        compare(controller.selectedCount, 5, "the hidden ticked tracks stay ticked and unstaged");
        var notice = findChild(page, "cuesLeftOutDialog");
        verify(!notice.opened, "the shown track fits on Engine's pads, so nothing to say");
        controller.unstage(0);
    }

    function test_stagingSaysWhichTracksLeaveCuesOffEngine() {
        var page = openStagingPage(1100, 720);
        var controller = findChild(page, "syncController");
        controller.resetCuesLeftOutNotice();
        mouseClick(findChild(page, "stageSelectedButton"));
        tryCompare(controller, "stagedCount", 6, 2000);
        var notice = findChild(page, "cuesLeftOutDialog");
        tryCompare(notice, "opened", true, 2000);
        compare(notice.tracks.length, 1, "only the track whose pads were full");
        compare(notice.tracks[0].title, "Bloom");
        compare(notice.tracks[0].count, 1);

        // "Not again for these tracks": staging Bloom again says nothing.
        findChild(notice, "leftOutTheseTracksBox").checked = true;
        mouseClick(findChild(notice, "acceptButton"));
        tryCompare(notice, "opened", false, 2000);
        for (var i = 0; i < 6; ++i) {
            controller.unstage(i);
        }
        compare(controller.stagedCount, 0);
        // Through the controller: the dialog's closing overlay can still
        // swallow a click under the desktop style, and the button's path
        // is proven above.
        controller.stageSelected(false);
        tryCompare(controller, "stagedCount", 6, 2000);
        wait(200);
        verify(!notice.opened, "suppressed for Bloom");
        controller.resetCuesLeftOutNotice();
        for (var j = 0; j < 6; ++j) {
            controller.unstage(j);
        }
    }

    // One rekordbox memory cue becomes pad 1 and the cue point on Engine.
    // To the DJ that is one memory cue, and the row says so.
    function test_aMemoryCueOnAPadIsStillOneMemoryCue() {
        var page = openPage(1100, 720);
        var list = listOf(page);
        // The last row: a ListView only creates the delegates in view, and
        // on the CI runner's fonts the list is taller than the window, so
        // scroll there first rather than count on it being on screen.
        list.positionViewAtIndex(list.count - 1, ListView.Contain);
        tryVerify(() => list.itemAtIndex(list.count - 1) !== null, 2000, "the last row is on screen");
        var row = list.itemAtIndex(list.count - 1);
        row.expanded = true;
        waitForRendering(page);
        compare(findChild(row, "panelSentence").text,
                "Adds 1 memory cue from DeviceLibrary. Nothing is written until Save.");
    }

    // The player's import prompt is offered here, where a sync makes it
    // bite, and the mark is staged into the same session Save writes.
    function test_theImportPromptCanBeSettledFromTheSyncPage() {
        var page = openStagingPage(1100, 720);
        var controller = findChild(page, "syncController");
        var row = findChild(page, "importPromptRow");
        verify(!row.visible, "nothing known about the player: no offer");
        controller.setImportStateForTesting(true);
        tryCompare(row, "visible", true, 2000);
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/SyncPage-import-prompt.png");
        }
        var button = findChild(page, "markImportedButton");
        compare(button.text, "Mark As Already Imported");
        mouseClick(button);
        tryCompare(controller, "importMarkStaged", true, 2000);
        compare(button.text, "Unstage");
        mouseClick(button);
        tryCompare(controller, "importMarkStaged", false, 2000);
        controller.setImportStateForTesting(false);
        tryCompare(row, "visible", false, 2000);
    }

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function saveSession(session) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 300000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // A save that landed whole is said over the list with the countdown
    // toast, in the page's own words, and the catalogs are read again: the
    // written row is gone, what is left to sync stays, and no summary
    // dialog opens. On a copy of the fixture stick, through a real save.
    function test_aSaveIsSaidOverTheListAndTheListIsReadAgain() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 720, rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        const controller = findChild(page, "syncController");
        tryCompare(controller, "busy", true, 5000);
        tryCompare(controller, "busy", false, 300000);
        const list = listOf(page);
        verify(list.count > 2, "precondition: the fixture has tracks to sync, found " + list.count);
        const before = list.count;

        let row = null;
        for (let r = 0; r < list.count && row === null; ++r) {
            const candidate = list.itemAtIndex(r);
            if (candidate !== null && !candidate.needsDecision) {
                row = candidate;
            }
        }
        verify(row !== null, "precondition: a track ready to sync");
        const title = row.title;
        controller.setAllIncluded(false);
        controller.setIncluded(row.planIndex, true);
        controller.stageSelected(false);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        tryCompare(session, "pendingCount", 1, 5000);

        const summary = saveSession(session);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.written, 1, JSON.stringify(summary));
        // Counted as each change lands, so complete by the time the save is.
        const expectedCues = controller.syncedCueCount;
        verify(expectedCues > 0, "the save counts the cues that came across");

        const toast = findChild(page, "savedToast");
        tryCompare(toast, "showing", true, 5000);
        compare(findChild(toast, "confirmationToastLabel").text,
                expectedCues + (expectedCues === 1 ? " cue" : " cues") + " synced");
        const dialog = findChild(page, "summaryDialog");
        verify(dialog === null || !dialog.visible, "no summary dialog over a save that landed whole");

        // Read again from the stick: one row fewer, and not the written one.
        tryCompare(controller, "busy", false, 300000);
        tryCompare(list, "count", before - 1, 5000);
        for (let r = 0; r < list.count; ++r) {
            const left = list.itemAtIndex(r);
            if (left !== null) {
                verify(left.title !== title, "the written track is still listed: " + title);
            }
        }
        verify(!findChild(page, "nothingToSyncLabel").visible, "rows are left, so no empty state");
    }

    function findWhere(item, predicate) {
        if (predicate(item)) {
            return item;
        }
        const kids = item.children ? item.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findWhere(kids[i], predicate);
            if (found) {
                return found;
            }
        }
        return null;
    }

    function rectIn(page, item) {
        const p = item.mapToItem(page, 0, 0);
        const q = item.mapToItem(page, item.width, item.height);
        return {x: p.x, y: p.y, right: q.x, bottom: q.y};
    }

    function intersects(a, b) {
        return a.x < b.right && b.x < a.right && a.y < b.bottom && b.y < a.bottom;
    }

    function describe(r) {
        return "(" + Math.round(r.x) + "," + Math.round(r.y) + ")-(" + Math.round(r.right) + "," + Math.round(r.bottom) + ")";
    }

    // #122: the save's toast goes up in the same call that starts reading
    // the stick again, and both were centred on the page: the toast sat on
    // the very bar of the refresh it started, and for six seconds nobody
    // could read it. They never share a place now, with the page's own
    // words or a long text on a narrow window. A real save on a copy of the
    // fixture stick; the refresh is held showing while the two are measured,
    // since a small stick reads again faster than a test can look.
    function test_theSavedToastStaysClearOfTheRefreshBar_data() {
        return [
            {tag: "wide", pageWidth: 1100, text: ""},
            {tag: "narrow, long text", pageWidth: 480,
             text: "12 cues synced, and the import prompt marked as already answered on this stick"},
        ];
    }

    function test_theSavedToastStaysClearOfTheRefreshBar(row) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 720, rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        const controller = findChild(page, "syncController");
        tryCompare(controller, "busy", true, 5000);
        tryCompare(controller, "busy", false, 300000);
        const list = listOf(page);
        let ready = null;
        for (let r = 0; r < list.count && ready === null; ++r) {
            const candidate = list.itemAtIndex(r);
            if (candidate !== null && !candidate.needsDecision) {
                ready = candidate;
            }
        }
        verify(ready !== null, "precondition: a track ready to sync");
        controller.setAllIncluded(false);
        controller.setIncluded(ready.planIndex, true);
        controller.stageSelected(false);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        tryCompare(session, "pendingCount", 1, 5000);
        // Up for as long as the test needs, however slow the machine: the
        // refresh below may take longer than the toast's six seconds.
        const toast = findChild(page, "savedToast");
        toast.duration = 600000;
        compare(saveSession(session).error, "");

        tryCompare(toast, "showing", true, 5000);
        // The real refresh first, done, so it changes nothing under the
        // measurement (its end takes Cancel away and the column shrinks).
        tryCompare(controller, "busy", false, 300000);
        verify(toast.showing, "precondition: the toast outlasts the refresh");
        const overlay = findWhere(page, (item) => item.etaText !== undefined && item.cancelRequested !== undefined);
        verify(overlay !== null, "the page's progress overlay was not found");
        // Then held as the refresh looks a moment in: three of ten read,
        // and cancellable, which a read of the stick is.
        overlay.current = 3;
        overlay.total = 10;
        overlay.cancellable = true;
        overlay.busy = true;
        page.width = row.pageWidth;
        if (row.text.length > 0) {
            toast.show(row.text);
        }
        tryCompare(toast, "opacity", 1, 2000);
        waitForPolish(page);

        const panel = findChild(toast, "confirmationToastPanel");
        const bar = findChild(overlay, "progressTrack");
        const report = findChild(overlay, "progressReport");
        verify(bar !== null && bar.visible && report !== null, "the refresh's bar shows");
        // The toast may glide clear; give it the time to.
        tryVerify(() => !intersects(rectIn(page, panel), rectIn(page, bar))
                        && !intersects(rectIn(page, panel), rectIn(page, report)),
                  2000, "the toast " + describe(rectIn(page, panel)) + " covers the refresh bar "
                        + describe(rectIn(page, bar)) + " or its report " + describe(rectIn(page, report)));
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/SyncPage-toast-over-refresh-" + row.pageWidth + ".png");
        }
        // Clear of it by staying on the page, not by leaving it, and still
        // both up: nothing went away to make room.
        const t = rectIn(page, panel);
        verify(t.x >= -0.5 && t.y >= -0.5 && t.right <= page.width + 0.5 && t.bottom <= page.height + 0.5,
               "the toast " + describe(t) + " stays on the " + page.width + "x" + page.height + " page");
        verify(toast.showing && toast.opacity === 1 && overlay.visible && bar.visible, "both still showing");
    }

    // Undo Last Save finishes through the same signal as a save, and
    // synced no cues: the toast says what the undo did, not this page's
    // "Marked as already imported", and no summary dialog opens.
    function test_anUndoIsSaidInItsOwnWords() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 720, rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library"});
        verify(page !== null, "page did not instantiate");
        const controller = findChild(page, "syncController");
        tryCompare(controller, "busy", true, 5000);
        tryCompare(controller, "busy", false, 300000);
        const list = listOf(page);
        let row = null;
        for (let r = 0; r < list.count && row === null; ++r) {
            const candidate = list.itemAtIndex(r);
            if (candidate !== null && !candidate.needsDecision) {
                row = candidate;
            }
        }
        verify(row !== null, "precondition: a track ready to sync");
        controller.setAllIncluded(false);
        controller.setIncluded(row.planIndex, true);
        controller.stageSelected(false);
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        tryCompare(session, "pendingCount", 1, 5000);
        compare(saveSession(session).error, "");
        // The page reads the catalogs again after a save, but one turn of
        // the event loop later (SyncPage.onSaveFinished), so "not busy"
        // right after the save can be the moment before that read starts.
        // The undo then ran beside it, and on Windows could not replace the
        // Engine database the read had open: "could not write ... m.db".
        // The save's toast goes up in the same call that starts the read.
        const toast = findChild(page, "savedToast");
        tryCompare(toast, "showing", true, 5000);
        tryCompare(controller, "busy", false, 300000);
        verify(session.canUndo, "precondition: the save can be undone");

        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        session.undoLastSave();
        tryVerify(() => summary !== null, 300000, "the undo finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        compare(summary.undo, true, JSON.stringify(summary));
        // The toast below needs the summary whole (SyncPage.afterSave), and
        // its tryCompare can only print the two texts. Each condition on its
        // own, with the summary, so a failure names the one that broke
        // (#93: "5 cues synced" stayed up once and nothing said why).
        const said = JSON.stringify(summary);
        verify(summary.written > 0, "the undo wrote nothing: " + said);
        verify(!(summary.warning && summary.warning.length > 0), "the undo ended with a warning: " + said);
        verify(!(summary.skipped > 0), "the undo skipped steps: " + said);
        compare(summary.written, summary.total, "the undo wrote fewer steps than it had: " + said);
        verify(summary.cancelled !== true, "the undo was cancelled: " + said);

        // The save's own toast can still be up when the undo finishes, so
        // "showing" alone proves nothing: wait for the undo's words.
        tryCompare(findChild(toast, "confirmationToastLabel"), "text", "Changes undone.", 5000);
        verify(toast.showing, "the undo's toast is up");
        const dialog = findChild(page, "summaryDialog");
        verify(dialog === null || !dialog.visible, "no summary dialog over an undo that landed whole");
        tryCompare(controller, "busy", false, 300000);
    }

    // With a playlist picked, "nothing to sync" is about that playlist, and
    // says so: the picker is easy to forget, and the library may not be.
    function test_nothingToSyncNamesThePickedPlaylist() {
        var page = openPage(1100, 720);
        var controller = findChild(page, "syncController");
        verify(syncPageFixture.fillEmpty(controller));
        var nothing = findChild(page, "nothingToSyncLabel");
        tryCompare(nothing, "visible", true, 2000);
        compare(nothing.text, "Nothing to sync. Matched tracks' cues are already consistent.");
        page.selectedPlaylistName = "Warmup";
        compare(nothing.text, "Nothing to sync in playlist “Warmup”. Its matched tracks' cues are already consistent.");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/SyncPage-nothing-in-playlist.png");
        }
    }

    // With "Copy beat grids/waveforms" on, a track whose cues are in step
    // but that gets rekordbox's analysis is a synced track: a row of the
    // list like the others, ticked, counted, found by the search, saying
    // what it gets. Cues in step everywhere is then not "nothing to sync".
    function test_analysisOnlyTracksAreRows() {
        const page = openPage(1100, 720);
        const controller = findChild(page, "syncController");
        verify(syncPageFixture.fillEmpty(controller));
        const nothing = findChild(page, "nothingToSyncLabel");
        const figure = findChild(page, "analysisOnlyFigure");
        const stage = findChild(page, "stageSelectedButton");
        const list = listOf(page);
        tryCompare(nothing, "visible", true, 2000);
        compare(figure.visible, false, "no figure without such tracks");
        compare(stage.enabled, false);

        controller.setAnalysisOnlyForTesting(3, 2, 3, 1);
        verify(waitForPolish(page.Window.window));
        compare(list.count, 3, "each such track is a row");
        compare(nothing.visible, false, "rows, so never \"nothing to sync\"");
        compare(figure.visible, true);
        compare(figure.value, "3");
        compare(findChild(page, "needSyncFigure").value, "3", "counted with the tracks that need sync");
        compare(controller.selectedCount, 3, "ticked, as a cue row is");
        compare(stage.text, "Stage 3 Selected");
        compare(stage.enabled, true);

        const notes = [
            "Beat grid and waveform from rekordbox, then loads without analysis on the player",
            "Beat grid and waveform from rekordbox",
            "Waveform from rekordbox",
        ];
        for (let r = 0; r < 3; ++r) {
            const row = list.itemAtIndex(r);
            verify(row !== null, "row " + r + " was not built");
            compare(row.title, "Analysis Only " + (r + 1));
            verify(row.analysisOnly);
            compare(row.directionText, "DeviceLibrary → Engine");
            compare(row.cueSummary, "cues in step", "no cue counts invented");
            compare(findChild(row, "rowNote").text, notes[r]);
            compare(findChild(row, "rowNote").visible, true);
            compare(findChild(row, "selectCheckBox").enabled, true, "tickable");
            compare(findChild(row, "selectCheckBox").checked, true);
        }
        const first = list.itemAtIndex(0);
        first.expanded = true;
        waitForRendering(page);
        compare(findChild(first, "panelSentence").text, "The cues are already in step. Beat grid and waveform from "
                + "rekordbox, then loads without analysis on the player. Nothing is written until Save.");

        // Unticked: not counted, not on Stage.
        mouseClick(findChild(list.itemAtIndex(1), "selectCheckBox"));
        compare(controller.selectedCount, 2);
        compare(stage.text, "Stage 2 Selected");

        // The search narrows them as it narrows cue rows.
        findChild(page, "searchField").text = "Only 3";
        tryCompare(list, "count", 1, 2000);
        compare(controller.selectedVisibleCount, 1);
        compare(stage.text, "Stage 1 Selected");
        compare(list.itemAtIndex(0).title, "Analysis Only 3");
        findChild(page, "searchField").text = "";
        tryCompare(list, "count", 3, 2000);
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/SyncPage-analysis-only.png");
        }

        controller.setAnalysisOnlyForTesting(0, 0, 0, 0);
        tryCompare(nothing, "visible", true, 2000);
        compare(nothing.text, "Nothing to sync. Matched tracks' cues are already consistent.");
        compare(figure.visible, false);
    }

    // Beside cue rows they come after them, in the same section, and the
    // counts are the list's.
    function test_analysisOnlyRowsFollowTheCueRows() {
        const page = openPage(1100, 720);
        const controller = findChild(page, "syncController");
        const list = listOf(page);
        controller.setAnalysisOnlyForTesting(2, 2, 2, 0);
        verify(waitForPolish(page.Window.window));
        compare(list.count, 10, "two decisions, six cue rows, two analysis rows");
        compare(controller.planCount, 8);
        compare(controller.selectedCount, 8);
        compare(findChild(page, "stageSelectedButton").text, "Stage 8 Selected");
        list.positionViewAtIndex(list.count - 1, ListView.Contain);
        tryVerify(() => list.itemAtIndex(list.count - 1) !== null, 2000, "the last row is on screen");
        const last = list.itemAtIndex(list.count - 1);
        verify(last.analysisOnly, "the analysis rows come last");
        compare(last.section, "ready", "in the ready section, not one of their own");
        // Lined up with the cue rows. The last row's columns are read while
        // it is built: the list need not keep it once the third row is in
        // view, and with taller rows (macOS's fonts) it does not.
        const names = ["rowTitle", "directionColumn", "cueSummaryColumn", "statusColumn"];
        const lastX = names.map(name => xIn(page, findChild(last, name)));
        list.positionViewAtIndex(2, ListView.Contain);
        tryVerify(() => list.itemAtIndex(2) !== null, 2000, "the third row is on screen");
        const cueRow = list.itemAtIndex(2);
        verify(!cueRow.analysisOnly);
        compare(findChild(cueRow, "rowNote").visible, false, "a cue row carrying no analysis says none");
        for (let n = 0; n < names.length; ++n) {
            compare(lastX[n], xIn(page, findChild(cueRow, names[n])),
                    names[n] + " is out of line on an analysis row");
        }
        findChild(page, "searchField").text = "kollektiv";
        compare(findChild(page, "stageSelectedButton").text, "Stage " + controller.selectedVisibleCount + " Selected");
        compare(controller.selectedVisibleCount, 1, "the search hides the analysis rows that do not match");
    }

    // Beat grids and waveforms the analysis could not check are unsure:
    // said in a banner, and the list empty of rows is then not the green
    // "nothing to sync" check.
    function test_analysisNotCheckedIsSaidNotShownAsFine() {
        const page = openPage(1100, 720);
        const controller = findChild(page, "syncController");
        verify(syncPageFixture.fillEmpty(controller));
        const nothing = findChild(page, "nothingToSyncLabel");
        const banner = findChild(page, "analysisUncheckedBanner");
        tryCompare(nothing, "visible", true, 2000);
        compare(banner.visible, false, "everything checked: no banner");
        compare(nothing.tone, "good");

        controller.setAnalysisUncheckedForTesting(0, true);
        tryCompare(banner, "visible", true, 2000);
        compare(banner.text, "Beat grids and waveforms could not be checked.");
        compare(nothing.visible, true, "the cues are still in step");
        compare(nothing.tone, "info", "never the green check while unsure");
        verify(nothing.iconName !== "checkmark");

        controller.setAnalysisUncheckedForTesting(3, false);
        compare(banner.text, "Beat grids and waveforms could not be checked for 3 tracks.");
        controller.setAnalysisUncheckedForTesting(1, false);
        compare(banner.text, "Beat grids and waveforms could not be checked for 1 track.");
        verify(banner.text.indexOf("--") < 0 && banner.text.indexOf("\u2014") < 0);
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/SyncPage-analysis-unchecked.png");
        }

        controller.setAnalysisUncheckedForTesting(0, false);
        tryCompare(banner, "visible", false, 2000);
        compare(nothing.tone, "good");
    }

    // A save whose every change was skipped wrote nothing, but its rows
    // left the list with their changes: the page reads the stick again, so
    // what is still to do comes back rather than vanishing.
    function test_aSaveThatOnlySkippedReadsTheListAgain() {
        const page = openPage(1100, 720);
        const controller = findChild(page, "syncController");
        const list = listOf(page);
        verify(list.count > 0, "precondition: the fixture's rows");
        let analysed = 0;
        const count = () => { ++analysed; };
        controller.analysisChanged.connect(count);
        page.afterSave({written: 0, skipped: 2, total: 2, unit: "tracks", verb: "synchronised",
                        error: "", warning: "", notes: "", cancelled: false});
        // Busy from the call on: what the check below the next one relies on.
        verify(controller.busy || analysed > 0, "the read starts in the call");
        tryVerify(() => analysed > 0, 5000, "the list is read again after a save that only skipped");
        tryCompare(controller, "busy", false, 5000);
        controller.analysisChanged.disconnect(count);
        // And a save that did nothing at all is not a reason to read again:
        // an analysis would be running right after the call (busy is set as
        // it starts).
        page.afterSave({written: 0, skipped: 0, total: 0, unit: "tracks", verb: "synchronised"});
        compare(controller.busy, false, "nothing written or skipped: nothing read");
    }

    function test_theJunkCueNoteLinksToStrayCueCleanUp() {
        var page = openPage(1100, 720);
        var row = listOf(page).itemAtIndex(0);
        row.expanded = true;
        waitForRendering(page);
        var cards = collect(row, "sideCard");
        compare(collect(cards[0], "junkCueNote").length, 0, "the side without a 0:00 memory cue has no note");
        var notes = collect(cards[1], "junkCueLink");
        compare(notes.length, 1, "the side with one does");
        junkSpy.target = page;
        junkSpy.clear();
        notes[0].linkActivated("clean-up-stray-cues");
        tryCompare(junkSpy, "count", 1, 2000);
        compare(junkSpy.signalArguments[0][0], "TESTSTICK");
    }

    function test_theListScrollsClearOfSave() {
        var page = openPage(1100, 720);
        var overlay = findChild(page, "saveOverlay");
        verify(overlay !== null);
        verify(overlay.implicitHeight > 0);
        verify(listOf(page).bottomMargin >= overlay.implicitHeight + 2 * overlay.anchors.margins,
               "the last row could end up behind the Save button");
    }

    function test_rowsAndControlsFitANarrowWindow_data() {
        return [
            {tag: "700", pageWidth: 700},
            {tag: "560", pageWidth: 560},
        ];
    }

    function test_rowsAndControlsFitANarrowWindow(data) {
        var page = openPage(data.pageWidth, 720);
        var names = ["filterRow", "scopeRow", "introRow"];
        for (var n = 0; n < names.length; ++n) {
            var item = findChild(page, names[n]);
            var right = item.mapToItem(page, item.width, 0).x;
            verify(right <= page.width + 0.5, names[n] + " reaches " + right + " in a " + page.width + " page");
        }
        var row = listOf(page).itemAtIndex(2);
        var status = findChild(row, "statusColumn");
        verify(xIn(page, status) + status.width <= page.width + 0.5, "the status column runs off the page");
        verify(findChild(row, "rowTitle").width > 60, "the title has been squeezed out");
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = openPage(1100, 720);
        var list = listOf(page);
        list.itemAtIndex(0).expanded = true;
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/SyncPage.png");

        list.itemAtIndex(0).expanded = false;
        list.itemAtIndex(2).expanded = true;
        waitForRendering(page);
        wait(100);
        grabImage(page).save(screenshotDir + "/SyncPage-ready.png");

        var narrow = openPage(640, 720);
        listOf(narrow).itemAtIndex(0).expanded = true;
        waitForRendering(narrow);
        wait(100);
        grabImage(narrow).save(screenshotDir + "/SyncPage-narrow.png");
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from another
    // page on top of Home). The breadcrumb reads the stack's depth to
    // decide whether its middle segment is a link.
    Component {
        id: crumbStackComponent
        StackView { width: testCase.width; height: testCase.height }
    }
    Component {
        id: crumbFillerComponent
        Item {}
    }
    function pushOnStack(levelsBelow, props) {
        const stack = createTemporaryObject(crumbStackComponent, testCase);
        for (let i = 0; i < levelsBelow; ++i) {
            stack.push(crumbFillerComponent, {}, StackView.Immediate);
        }
        return stack.push(pageComponent, props, StackView.Immediate);
    }

    // From Home the stick is all that stands between the house and this
    // page; opened from Library Statistics it names that page too, as the way back,
    // rather than putting the stick's name on a link to it.
    function test_breadcrumb_data() {
        return [
            {tag: "home", below: 1, hubLabel: "", middle: "", link: false},
            {tag: "nested", below: 2, hubLabel: "Library Statistics", middle: "Library Statistics", link: true},
        ];
    }

    function test_breadcrumb(data) {
        const props = {};
        props.hubLabel = data.hubLabel;
        const page = pushOnStack(data.below, props);
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, data.middle);
        compare(crumb.middleIsLink, data.link);
        compare(crumb.title, "Sync Cue Points");
    }
}
