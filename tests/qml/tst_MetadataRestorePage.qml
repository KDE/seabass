// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "Breadcrumb.js" as Breadcrumb

// Restore Metadata's page. This is the half of the feature that writes
// to a stick, so what is guarded here is that nothing reaches one by
// accident: staging is a separate act from saving, and the button that
// does the writing says so.
//
// Also saves a screenshot when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "MetadataRestorePage"
    width: 900
    height: 700
    visible: true
    when: windowShown

    Component {
        id: pageComponent
        MetadataRestorePage {
            width: 880
            height: 660
            stickLabel: "TESTSTICK"
            rekordboxPath: "/nonexistent/TESTSTICK/PIONEER"
            enginePath: "/nonexistent/TESTSTICK/Engine Library"
            libraryId: ""
        }
    }

    function make(extra) {
        var page = createTemporaryObject(pageComponent, testCase, extra || {});
        verify(page, "page did not instantiate");
        waitForRendering(page);
        return page;
    }

    function test_thereIsNoConflictQuestionToGetWrong() {
        // The pair of radio buttons that used to ask whether to keep the
        // stick's cues or replace them from the store is gone, in favour
        // of one rule applied per field: a blank is filled, more cues
        // wins, otherwise the later edit wins. This is the guard that
        // the question does not come back.
        var page = make();
        verify(!findChild(page, "keepStickRadio"), "the keep radio must be gone");
        verify(!findChild(page, "replaceFromStoreRadio"), "the replace radio must be gone");
    }

    function test_theSaveButtonSaysRestore() {
        // The page's one write. Everywhere else in Seabass this button
        // says "Save", which is right on a page you have been editing;
        // here the whole page is a single verb and the button is the
        // moment it happens.
        var page = make();
        var host = findChild(page, "saveOverlay");
        verify(host, "the floating save button must exist");
        compare(host.label, "Restore", "and must say what it does");
    }

    function test_thereIsOnlyTheStandardSaveButton() {
        // One button on this page, and it is the one every other editing
        // page has. There was briefly a second, under the list, that
        // turned a selection into staged changes -- two ways to say the
        // same thing, and a tooltip that had to explain which of them it
        // did. Ticking a row stages it; the standard button writes.
        var page = make();
        verify(!findChild(page, "stageSelectedButton"),
               "staging must not have a button of its own beside the save button");
        verify(findChild(page, "saveOverlay"), "the standard save button must be the one that writes");
    }

    function test_theListHasASearchAndMarking() {
        var page = make();
        var toolbar = findChild(page, "proposalToolbar");
        verify(toolbar, "the list toolbar must exist");
        verify(findChild(toolbar, "selectAllButton"), "Select All must exist");
        verify(findChild(toolbar, "selectNoneButton"), "Select None must exist");
        verify(findChild(toolbar, "searchField"), "the search field must exist");
        var clear = findChild(toolbar, "clearSearchButton");
        verify(clear, "the clear button must exist");
        verify(!clear.visible, "and must be hidden while the search is empty");
        toolbar.searchText = "anything";
        verify(clear.visible, "and shown once there is something to clear");
    }

    // ---- the stick and playlist pickers ---------------------------------
    //
    // The backup page's filters, on this page too: which stick's backup
    // and which playlist, in that order, above the list and its toolbar.
    // They bound the restore, not only the list, so the counts on the page
    // are counts of what they include and a narrowing takes off whatever
    // staged tracks it leaves out.

    // A page filled through the controller's scan-result door, with five
    // proposals: two from one NO NAME stick, one from another NO NAME
    // stick, two from RV2, across the playlists Warm Up, Closing and Peak
    // Time (see MetadataRestoreFixture::fill).
    function makeFilled(extra) {
        const page = make(extra);
        // The page has already tried the nonexistent stick it was handed
        // and failed; a scan result that does arrive clears that.
        tryVerify(() => !page.controller.busy, 5000);
        verify(metadataRestoreFixture.fill(page.controller), "the fixture must fill the controller");
        compare(page.controller.errorMessage, "", "a result that arrives supersedes an earlier failure");
        waitForRendering(page);
        compare(findChild(page, "proposalList").count, 5, "all five proposals listed");
        return page;
    }

    function pickerIndex(model, name) {
        for (let i = 0; i < model.length; i++) {
            if (model[i].name === name) {
                return i;
            }
        }
        return -1;
    }

    function test_theFiltersAreTheBackupPagesInTheSameOrder() {
        const page = make();
        const source = findChild(page, "sourcePicker");
        const playlist = findChild(page, "playlistPicker");
        const toolbar = findChild(page, "proposalToolbar");
        verify(source !== null, "the stick picker must exist");
        verify(playlist !== null, "the playlist picker must exist");
        verify(toolbar !== null && findChild(toolbar, "searchField") !== null, "and the search");
        const s = source.mapToItem(page, 0, 0);
        const p = playlist.mapToItem(page, 0, 0);
        const t = toolbar.mapToItem(page, 0, 0);
        compare(s.y, p.y, "the two pickers share a row");
        verify(s.x < p.x, "stick first, then playlist, as on the backup page");
        verify(p.y < t.y, "and both above the list's own toolbar");
        verify(findChild(page, "restoreInfoButton") !== null, "the help sits at the end of that row");
    }

    function test_theStickPickerNarrowsTheRestoreToOneSticksBackup() {
        const page = makeFilled();
        const list = findChild(page, "proposalList");
        const picker = findChild(page, "sourcePicker");
        const model = page.sourceModel;
        compare(model.length, 4, "every stick, then the three sticks the proposals came from");
        compare(model[0].name, "Every stick in the backup");
        // Two sticks called NO NAME are two entries, told apart by what
        // each offers; RV2 is unambiguous and says nothing extra.
        verify(pickerIndex(model, "USB Stick NO NAME (2 tracks)") > 0);
        verify(pickerIndex(model, "USB Stick NO NAME (1 track)") > 0);
        const rv2 = pickerIndex(model, "USB Stick RV2");
        verify(rv2 > 0);

        picker.activated(rv2);
        compare(list.count, 2, "only what was backed up from RV2");
        compare(page.controller.scopedProposalCount, 2);
        compare(picker.currentIndex, rv2, "and the picker shows it");
        compare(findChild(page, "listSummary").text, "2 tracks to restore",
                "the count is of the restore, not of the whole backup");

        picker.activated(pickerIndex(model, "USB Stick NO NAME (1 track)"));
        compare(list.count, 1, "the other NO NAME is a different stick");

        picker.activated(0);
        compare(list.count, 5, "and every stick again");
    }

    // A stick the store knows neither the id nor the label of is a stick
    // of its own: it has an entry, and picking it narrows to its tracks.
    // Its key used to be the empty one, which is "every stick", so the
    // picker showed "A stick with no name" while everything was in scope,
    // and picking it selected everything.
    // Rows backed up before the store recorded a stick's id carry only its
    // label. Under a label one recorded stick has, they are that stick:
    // one entry, and picking it restores all of it. Under a label two
    // recorded sticks share there is no telling which, and the entry says
    // so rather than passing for a third stick of that name.
    function test_rowsWithoutARecordedStickJoinTheirStick() {
        const page = make();
        tryVerify(() => !page.controller.busy, 5000);
        verify(metadataRestoreFixture.fillWithUnstampedRows(page.controller));
        waitForRendering(page);
        const model = page.sourceModel;
        compare(model.length, 5, "every stick, the two NO NAMEs, RV2, and the unrecorded NO NAME rows");
        const rv2 = pickerIndex(model, "USB Stick RV2");
        verify(rv2 > 0, "RV2 is still one entry: " + JSON.stringify(model));
        const unrecorded = pickerIndex(model, "USB Stick NO NAME (which stick not recorded, 1 track)");
        verify(unrecorded > 0, "the rows no stick can be told for say so: " + JSON.stringify(model));

        page.controller.setSourceStick(model[rv2].key);
        compare(page.controller.scopedProposalCount, 3, "Bloom, Sisters and the older RV2 row");
        page.controller.setSourceStick(model[unrecorded].key);
        compare(page.controller.scopedProposalCount, 1);
    }

    function test_aStickWithNoNameIsNotEveryStick() {
        const page = make();
        tryVerify(() => !page.controller.busy, 5000);
        verify(metadataRestoreFixture.fillWithAnUnnamedStick(page.controller));
        waitForRendering(page);
        const list = findChild(page, "proposalList");
        const picker = findChild(page, "sourcePicker");
        compare(list.count, 6);
        compare(picker.currentIndex, 0, "the page opens on every stick");
        compare(picker.displayText, "Every stick in the backup", "and says so");

        const unnamed = pickerIndex(page.sourceModel, "A stick with no name");
        verify(unnamed > 0, "the unidentified stick has an entry of its own");
        verify(page.sourceModel[unnamed].key.length > 0, "whose key is not every stick's");
        picker.activated(unnamed);
        compare(list.count, 1, "picking it narrows to its one track");
        compare(page.controller.scopedProposalCount, 1);
        compare(picker.currentIndex, unnamed, "and the picker shows it");

        picker.activated(0);
        compare(list.count, 6, "and every stick again");
        compare(picker.currentIndex, 0);
    }

    // The line under the pickers counts the stick and the store whole, and
    // the tracks left alone within the pickers' selection, which is what a
    // restore covers. Once a picker narrows, it says so: "1 track is left
    // alone" beside "Matched 5 tracks" read as one track of the five.
    function test_theMatchLineSaysWhichCountIsOfTheSelection() {
        const page = make();
        tryVerify(() => !page.controller.busy, 5000);
        verify(metadataRestoreFixture.fillWithConflictsLeftAlone(page.controller));
        waitForRendering(page);
        const line = findChild(page, "matchSummary");
        verify(line !== null);
        compare(line.text, "Matched 5 tracks on the stick against 5 in the store. 2 tracks have cues of their "
                + "own that the stored copy did not beat; they are left alone.");

        findChild(page, "sourcePicker").activated(pickerIndex(page.sourceModel, "USB Stick RV2"));
        compare(line.text, "Matched 5 tracks on the stick against 5 in the store. In this selection, 1 track "
                + "has cues of its own that the stored copy did not beat; it is left alone.");

        findChild(page, "sourcePicker").activated(0);
        const playlists = findChild(page, "playlistPicker");
        playlists.playlistPicked(pickerIndex(playlists.model, "Warm Up"), {name: "Warm Up", count: 3});
        compare(line.text, "Matched 5 tracks on the stick against 5 in the store.",
                "Warm Up has no track left alone, so nothing is said about any");
    }

    function test_thePlaylistPickerNarrowsTheRestoreToOnePlaylist() {
        const page = makeFilled();
        const list = findChild(page, "proposalList");
        const playlists = findChild(page, "playlistPicker");
        compare(playlists.model[0].name, "All tracks");
        compare(playlists.model[0].count, 5);
        const names = playlists.model.map(entry => entry.name + " " + entry.count);
        compare(names.join(", "), "All tracks 5, Closing 2, Peak Time 1, Warm Up 3");

        playlists.playlistPicked(pickerIndex(playlists.model, "Warm Up"), {name: "Warm Up", count: 3});
        compare(list.count, 3);
        compare(playlists.currentIndex, pickerIndex(playlists.model, "Warm Up"));

        // Both at once: RV2's tracks in Warm Up. The playlist list follows
        // the stick, and its counts are of that stick's proposals.
        findChild(page, "sourcePicker").activated(pickerIndex(page.sourceModel, "USB Stick RV2"));
        compare(list.count, 1, "RV2's one track in Warm Up");
        compare(page.controller.selectedPlaylist, "Warm Up", "the playlist survives a stick it is on");
        compare(findChild(page, "playlistPicker").model.map(entry => entry.name + " " + entry.count).join(", "),
                "All tracks 2, Closing 1, Warm Up 1");

        // A stick that has nothing in the playlist drops it rather than
        // narrowing to an empty list with nothing on screen to say why.
        findChild(page, "sourcePicker").activated(pickerIndex(page.sourceModel, "USB Stick NO NAME (1 track)"));
        compare(page.controller.selectedPlaylist, "", "Peak Time's stick has no Warm Up");
        compare(list.count, 1);
    }

    // The heart of it: a narrowing is a narrowing of the RESTORE. A staged
    // track the new scope leaves out comes off the save, and the page says
    // so, instead of being written out of sight by a save that the page
    // presents as covering one playlist.
    function test_narrowingUnstagesWhatFallsOutside() {
        const page = makeFilled();
        const controller = page.controller;
        // Flaschenpost (NO NAME, Warm Up) and Sisters (RV2, Closing).
        metadataRestoreFixture.markStaged(controller, 0);
        metadataRestoreFixture.markStaged(controller, 4);
        compare(controller.stagedCount, 2);
        compare(findChild(page, "listSummary").text, "2 of 5 staged");

        const feedback = [];
        const listen = (message, isError) => feedback.push(message);
        controller.actionFeedback.connect(listen);
        try {
            const playlists = findChild(page, "playlistPicker");
            playlists.playlistPicked(pickerIndex(playlists.model, "Warm Up"), {name: "Warm Up", count: 3});
        } finally {
            controller.actionFeedback.disconnect(listen);
        }
        compare(controller.stagedCount, 1, "Sisters is not in Warm Up, so it is not restored");
        compare(controller.stagedChangeCount, 1, "and nothing of it is left staged");
        compare(findChild(page, "listSummary").text, "1 of 3 staged", "counted over the playlist");
        compare(feedback.length, 1, "and the page is told");
        compare(feedback[0], "1 staged track is outside this selection and was unstaged.");
        compare(controller.allStaged, false);
    }

    // The search narrows what is shown within the pickers' selection, and
    // the count says which of the two it is showing.
    function test_theSearchNarrowsTheViewWithinTheSelection() {
        const page = makeFilled();
        const playlists = findChild(page, "playlistPicker");
        playlists.playlistPicked(pickerIndex(playlists.model, "Warm Up"), {name: "Warm Up", count: 3});
        findChild(page, "proposalToolbar").searchText = "Bloom";
        compare(findChild(page, "proposalList").count, 1);
        compare(page.controller.scopedProposalCount, 3, "the search does not change what is being restored");
        compare(findChild(page, "listSummary").text, "1 of 3 shown");
    }

    // ---- the waveform on an opened row -------------------------------------

    function openRow(page, row) {
        const list = findChild(page, "proposalList");
        const item = list.itemAtIndex(row);
        verify(item !== null, "row " + row + " must be built");
        item.expandToggled();
        waitForRendering(page);
        return item;
    }

    // Read from the stick for the row that is open, and for no other.
    // Opening the page reads nothing, however many rows it lists.
    function test_aRowReadsItsWaveformOnlyWhenOpened() {
        const asked = [];
        const player = {
            waveformFor: function (format, path, id) {
                asked.push(format + " " + path + " " + id);
                return [{low: 0.5, mid: 0.4, high: 0.3}, {low: 0.9, mid: 0.2, high: 0.1}];
            }
        };
        const page = makeFilled({playbackController: player});
        compare(asked.length, 0, "a list of closed rows reads no waveform");
        verify(findChild(page, "rowWaveform") === null, "and builds none");

        const row = openRow(page, 1);
        compare(asked.length, 1, "one read, for the one open row");
        compare(asked[0], "rekordbox /nonexistent/TESTSTICK/PIONEER 501",
                "from the stick's own catalog, at that format's path");
        const waveform = findChild(row, "rowWaveform");
        verify(waveform !== null);
        compare(waveform.waveformData.length, 2);
        compare(waveform.cueData.length, 2, "with the cues this restore would put on the track");
        compare(waveform.trackDurationMs, 418000);

        row.expandToggled();
        waitForRendering(page);
        verify(findChild(row, "rowWaveform") === null, "closed again, the waveform goes");
    }

    // Every row here is a track on the stick, and the waveform is read from
    // the stick: a missing one is the stick's (never analysed, listed only
    // by OneLibrary), not the backup's, and the placeholder says so.
    function test_aRowWithoutAWaveformSaysTheStickHasNone() {
        const page = makeFilled();  // no player: nothing to read a waveform with
        const row = openRow(page, 0);
        const waveform = findChild(row, "rowWaveform");
        verify(waveform !== null, "the placeholder is there all the same, with the cues on it");
        compare(waveform.hasWaveform, false);
        compare(waveform.missingText, "No waveform on the stick for this track");
        compare(waveform.cueData.length, 3);
    }

    // The real-scale case: every track of the committed anonymized library
    // offered back to a stick that lost its cues. The list must open
    // without a stall, because nothing about a waveform is paid for until
    // a row is opened; and opening one reads a real analysis file.
    function test_theRealScaleListOpensWithoutAStall() {
        const fixture = decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
        const prepared = metadataRestoreFixture.prepareFromLibrary(fixture);
        verify(prepared > 1000, "the fixture must offer the whole library back, got " + prepared);

        const page = make({playbackController: realPlayer});
        const list = findChild(page, "proposalList");
        const started = Date.now();
        verify(metadataRestoreFixture.applyPrepared(page.controller));
        waitForRendering(page);
        const opened = Date.now() - started;
        compare(list.count, prepared, "every proposal listed");
        console.log("  " + prepared + " proposals over " + metadataRestoreFixture.preparedStickTrackCount()
                    + " stick tracks: list opened in " + opened + " ms");
        // Generous on purpose: a loaded CI runner is slow. What it guards
        // is the shape, a waveform read per row would cost seconds here.
        verify(opened < 3000, "the list took " + opened + " ms to open");

        const openStarted = Date.now();
        const row = openRow(page, 0);
        const openedRow = Date.now() - openStarted;
        const waveform = findChild(row, "rowWaveform");
        verify(waveform !== null);
        console.log("  opening one row, with its waveform read from the stick: " + openedRow + " ms");
        verify(waveform.hasWaveform, "the fixture's analysis file has a waveform to show");
        if (screenshotDir) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/MetadataRestorePage-fixture-open.png");
        }
    }

    PlaybackController { id: realPlayer }

    // The real-scale bulk operations, on a writable copy of the committed
    // anonymized library: Select All, narrowing to one stick's backup with
    // everything staged, and a real save of what is left. Every property
    // the page shows hangs off analysisChanged, and each emission has the
    // page re-read them all and rebuild both pickers, so what is checked
    // is how often it fires per operation: once, not once per track. A
    // narrowing used to emit it per unstaged track and a save per landed
    // track, recounting the whole list each time, which is quadratic on
    // the UI thread. Timings are logged, never asserted.
    function test_bulkOperationsUpdateThePageOnce() {
        const fixture = decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
        const prepared = metadataRestoreFixture.prepareFromLibrary(fixture);
        verify(prepared > 1000, "the fixture must offer the whole library back, got " + prepared);
        const stick = metadataRestoreFixture.stickRoot();
        const rekordboxPath = stick + "/PIONEER";
        const libraryId = EditSessionRegistry.libraryIdForPath(rekordboxPath);
        verify(libraryId.length > 0, "the stick-shaped copy has a library id");
        const page = make({stickLabel: "FIXTURE", rekordboxPath: rekordboxPath,
                           enginePath: stick + "/Engine Library", libraryId: libraryId});
        const controller = page.controller;
        // The page scans the copy against the (empty) sandboxed store on
        // opening; that must be over before the prepared proposals go in.
        tryVerify(() => !controller.busy, 60000);
        verify(metadataRestoreFixture.applyPrepared(controller));
        waitForRendering(page);
        compare(controller.proposalCount, prepared);

        let emitted = 0;
        const count = () => emitted++;
        controller.analysisChanged.connect(count);
        try {
            // ---- Select All ----
            let started = Date.now();
            controller.stageAll();
            const stageMs = Date.now() - started;
            const staged = controller.stagedCount;
            // A proposal with no catalog row on the stick has nothing a
            // save could write, and is not staged.
            verify(staged > 1000, "Select All staged " + staged);
            console.log("  Select All: " + staged + " proposals, " + controller.stagedChangeCount + " changes, "
                        + emitted + " analysisChanged, " + stageMs + " ms");
            compare(emitted, 1, "Select All updates the page once");

            // ---- narrowing with everything staged ----
            const sources = controller.sourceSticks;
            compare(sources.length, 2, "the prepared proposals come from RV2 and A4");
            emitted = 0;
            started = Date.now();
            controller.setSourceStick("id:uuid-rv2");
            const narrowMs = Date.now() - started;
            const kept = controller.stagedCount;
            console.log("  narrowing to RV2 with " + staged + " staged: " + (staged - kept) + " unstaged, "
                        + emitted + " analysisChanged, " + narrowMs + " ms");
            verify(staged - kept > 800, "the narrowing unstaged " + (staged - kept));
            verify(kept > 800 && kept <= controller.scopedProposalCount, "what is left staged is in the scope");
            const scoped = controller.scopedProposalCount;
            compare(emitted, 1, "narrowing updates the page once, not once per unstaged track");

            // ---- a real save of what is left ----
            const session = EditSessionRegistry.sessionFor(libraryId, "FIXTURE");
            verify(session !== null);
            compare(session.pendingCount, controller.stagedChangeCount);
            const changes = session.pendingCount;
            let burstStarted = 0;
            let emittedInBurst = -1;
            let burstMs = -1;
            let summary = null;
            const onApplied = function() {
                if (burstStarted === 0) {
                    burstStarted = Date.now();
                    emitted = 0;
                }
            };
            const onFinished = function(result) {
                // Connected after the controller's own handler, so this
                // runs once the controller has taken the burst.
                burstMs = burstStarted > 0 ? Date.now() - burstStarted : -1;
                emittedInBurst = emitted;
                summary = result;
            };
            // The session says the save is over (pendingChanged, then
            // stateChanged, which is the page's writingChanged) before
            // saveFinished. The landed rows must be gone by then: the page
            // reads stagedCount on those signals, and used to find the
            // staged rows of a save that had already landed.
            let stagedWhenPendingChanged = -1;
            let stagedWhenWritingEnded = -1;
            const onPending = function() {
                if (burstStarted > 0 && summary === null) {
                    stagedWhenPendingChanged = controller.stagedCount;
                }
            };
            const onWriting = function() {
                if (!controller.writing && burstStarted > 0 && summary === null) {
                    stagedWhenWritingEnded = controller.stagedCount;
                }
            };
            session.changeApplied.connect(onApplied);
            session.saveFinished.connect(onFinished);
            session.pendingChanged.connect(onPending);
            controller.writingChanged.connect(onWriting);
            try {
                started = Date.now();
                session.save();
                tryVerify(() => summary !== null, 600000, "the save finishes");
            } finally {
                session.changeApplied.disconnect(onApplied);
                session.saveFinished.disconnect(onFinished);
                session.pendingChanged.disconnect(onPending);
                controller.writingChanged.disconnect(onWriting);
            }
            compare(summary.error, "", "the save worked");
            console.log("  save: " + kept + " proposals, " + changes + " changes, " + (Date.now() - started)
                        + " ms in all; the changeApplied burst took " + burstMs + " ms on the UI thread with "
                        + emittedInBurst + " analysisChanged");
            compare(emittedInBurst, 1, "a save updates the page once, not once per landed track");
            compare(controller.stagedCount, 0, "every staged proposal landed");
            compare(stagedWhenPendingChanged, 0, "already gone when the session's pending count changed");
            compare(stagedWhenWritingEnded, 0, "already gone when the page stopped writing");
            compare(controller.proposalCount, prepared - kept, "and is no longer offered");
            compare(findChild(page, "proposalList").count, scoped - kept,
                    "RV2 lists only what it could not restore");
        } finally {
            controller.analysisChanged.disconnect(count);
            EditSessionRegistry.closeSession(libraryId);
        }
    }

    function test_headerTextLinesUpWithTheBody() {
        var page = make();
        var crumbText = null;
        var bodyText = null;
        function walk(item) {
            for (var i = 0; i < item.children.length; ++i) {
                var child = item.children[i];
                // By objectName: the crumb draws Breeze's go-home icon
                // now, so it has no text to match against. Its content
                // item is what has to line up with the body.
                if (child.objectName === "homeCrumb") {
                    crumbText = child.contentItem;
                }
                if (child.objectName === "pageIntro") {
                    bodyText = child;
                }
                walk(child);
            }
        }
        walk(page);
        verify(crumbText, "the Home crumb's content item was not found");
        verify(bodyText, "the page's first body line was not found");
        compare(crumbText.mapToItem(page, 0, 0).x, bodyText.mapToItem(page, 0, 0).x,
                "breadcrumb text and body text must share a left edge");
    }

    // The artwork the store copied, drawn on the rows that are offering
    // it. Needs a real stick, because a proposal only exists where a
    // stick track matched a stored one -- and the cover comes from the
    // stored side, which is the whole point: the stick this page exists
    // for is the stick that lost its own.
    //
    // Read-only. Nothing reaches the stick before Restore is pressed,
    // and this never presses it.
    function test_screenshot_theProposalsDrawStoredArtwork() {
        if (!screenshotDir || !liveStickRoot) {
            skip("SEABASS_SCREENSHOT_DIR and SEABASS_LIVE_STICK not both set");
        }
        var page = make({
            stickLabel: "LIVE",
            rekordboxPath: liveStickRoot + "/PIONEER",
            enginePath: liveStickRoot + "/Engine Library",
        });
        var list = findChild(page, "proposalList");
        verify(list, "the proposal list must exist");
        // Waits for the SCAN to finish, not for proposals to exist. The
        // rig points this case at a stick to photograph whatever state
        // that stick is in -- its own comment says so, "fine for a
        // screenshot and would not be for an assertion" -- and the case
        // then asserted proposals anyway. It passed on a developer
        // machine, where the everyday profile holds a store somebody has
        // been filling for weeks, and failed in the rig's sandbox
        // profile, where the store is empty at S1 time because the
        // checks that fill it run later. A proposal only exists where
        // the store holds metadata a stick track is missing; two sticks
        // restored from the same backup have nothing to offer each
        // other, which is the normal state at the start of a round.
        //
        // So: the page must build, the scan must finish without error,
        // and the picture gets taken either way. How many rows are in it
        // is data, and it is logged rather than asserted.
        tryVerify(function () { return !page.controller.busy && page.controller.hasScanned; }, 120000,
                  "the scan over this stick must finish");
        compare(page.controller.errorMessage, "", "the scan must not error");
        console.log("  live stick offered " + list.count + " proposal(s)");
        waitForRendering(page);
        var image = grabImage(page);
        verify(image.width > 0 && image.height > 0, "the page grabbed nothing");
        image.save(screenshotDir + "/MetadataRestorePage-live.png");
    }

    function test_screenshot() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        var page = make();
        wait(100);
        var image = grabImage(page);
        image.save(screenshotDir + "/MetadataRestorePage.png");
    }

    // The pickers narrowed, and a row open with its cues on the placeholder
    // line: the states a default screenshot cannot show.
    function test_screenshot_narrowedAndOpen() {
        if (!screenshotDir) {
            skip("SEABASS_SCREENSHOT_DIR not set");
        }
        const page = makeFilled();
        const playlists = findChild(page, "playlistPicker");
        playlists.playlistPicked(pickerIndex(playlists.model, "Warm Up"), {name: "Warm Up", count: 3});
        metadataRestoreFixture.markStaged(page.controller, 0);
        openRow(page, 1);
        wait(100);
        grabImage(page).save(screenshotDir + "/MetadataRestorePage-narrowed-open.png");
    }

    // The page as the app shows it: inside a StackView, `levelsBelow`
    // pages up from the bottom (1 = pushed from Home, 2 = from a hub on
    // top of Home). The breadcrumb reads the stack's depth to decide
    // whether its middle segment is a link, so a page on its own cannot
    // show that.
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

    function saveCrumbShot(page, name) {
        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/crumb-" + name + ".png");
        }
    }

    // From the stick's row on Home, and from Metadata Backup's own
    // restore link one level further down.
    function test_breadcrumbFromHome() {
        const page = pushOnStack(1, {});
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, "");
        compare(crumb.title, "Restore Metadata");
    }

    function test_breadcrumbFromMetadataBackup() {
        const page = pushOnStack(2, {hubLabel: "Metadata Backup"});
        waitForRendering(page);
        const crumb = Breadcrumb.read(page);
        compare(crumb.stick, "TESTSTICK");
        compare(crumb.middle, "Metadata Backup");
        verify(crumb.middleIsLink);
        saveCrumbShot(page, "metadata-restore-from-backup");
    }
}
