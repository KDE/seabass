// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse's fixed rows: "All tracks", and Own Recordings (Experimental)
// while the stick holds a recording a player left on it. A stick laid out
// in scratch with WAVs the test writes (silence, no music), a library the
// held catalog stands in for (browseFixture.holdCues), and the real
// ScanController the page makes, which lists the recordings as the
// Recordings page does.
TestCase {
    id: testCase
    name: "OwnRecordings"
    width: 1100
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        ScanPage {}
    }

    // 2026-11-20 10:00, and a day apart each.
    readonly property double day: 24 * 3600
    readonly property double base: 1795255200

    function cleanup() {
        EditSessionRegistry.discardAll();
        realPlayback.stop();
        realAppSettings.experimentalFeaturesEnabled = false;
        realAppSettings.lastPlaylistName = "";
        catalogGate.restore();
        browseFixture.restore();
    }

    // A stick in scratch with `recordings` ([{folder, name, seconds,
    // unfinished, at}]) in its recording folders; the library has three
    // tracks with files, in "Held Playlist" (and `extraPlaylist`).
    function makeStick(recordings) {
        const root = stickFixture.scratchDirectory();
        verify(root.length > 0);
        for (const r of recordings) {
            verify(browseFixture.writeRecording(root + "/" + r.folder + "/" + r.name, r.seconds, r.unfinished === true,
                                                r.at), r.name);
        }
        return root;
    }

    function makePage(root, experimental, extraPlaylist, keep) {
        realAppSettings.experimentalFeaturesEnabled = experimental;
        browseFixture.holdCues(3, false, true, "", extraPlaylist || "");
        browseFixture.releaseCues();
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: root + "/PIONEER", enginePath: root + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings,
            keepController: keep === undefined ? null : keep});
        verify(page !== null);
        const scan = findChild(page, "scanController");
        tryVerify(() => scan.playlistNames.indexOf("Held Playlist") >= 0 && !scan.busy && !scan.cuesPending, 10000);
        tryCompare(findChild(page, "trackListView"), "count", 3);
        return page;
    }

    // The listing has come back: every read the page started has returned
    // and been delivered.
    function settle(page) {
        verify(browseFixture.waitForScans());
        wait(50);
    }

    function sidebar(page) {
        return findChild(page, "playlistList");
    }

    // The sidebar's rows, in their order.
    function sidebarRows(page) {
        const list = sidebar(page);
        const rows = [];
        (function walk(item) {
            if (item.fixedKind !== undefined && item.acceptsFileDrop !== undefined) {
                rows.push(item);
            }
            for (let i = 0; i < item.children.length; ++i) {
                walk(item.children[i]);
            }
        })(list.contentItem);
        rows.sort((a, b) => a.index - b.index);
        return rows;
    }

    function sidebarNames(page) {
        return sidebarRows(page).map(r => r.name + (r.isFixed ? "(" + r.fixedKind + ")" : "")).join("|");
    }

    function recordingsRow(page) {
        return sidebarRows(page).find(r => r.fixedKind === "recordings") || null;
    }

    function titles(page) {
        const tracks = findChild(page, "scanController").tracks;
        const shown = [];
        for (let i = 0; i < tracks.trackCount(); ++i) {
            shown.push(tracks.trackAt(i).title);
        }
        return shown.join("|");
    }

    function trackRows(page) {
        const list = findChild(page, "trackListView");
        const rows = [];
        for (let i = 0; i < list.count; ++i) {
            const item = list.itemAtIndex(i);
            if (item !== null) {
                rows.push(item);
            }
        }
        return rows;
    }

    readonly property var threeRecordings: [
        {folder: "Sessions", name: "Session-0001.wav", seconds: 2, at: testCase.base},
        {folder: "Sessions", name: "Session-0002.wav", seconds: 3, at: testCase.base + 2 * testCase.day},
        {folder: "PIONEER REC", name: "REC001.WAV", seconds: 1, at: testCase.base + testCase.day},
    ]

    // A stick without recordings has no Own Recordings row: "All tracks"
    // alone above the playlists.
    function test_aStickWithoutRecordingsHasNoRow() {
        const page = makePage(makeStick([]), true);
        const scan = findChild(page, "scanController");
        // The listing ran and found none: not a row missing for want of it.
        tryCompare(scan, "recordingsListed", true, 10000);
        settle(page);
        compare(scan.recordingCount, 0);
        compare(sidebar(page).fixedRowCount, 1);
        compare(sidebarNames(page), "All tracks(all)|Held Playlist");
    }

    // Without Experimental nothing is listed and the sidebar is as before;
    // switched on, the row comes, and switched off while it is shown, it
    // goes and Browse is on "All tracks" again.
    function test_theRowIsBehindExperimental() {
        const page = makePage(makeStick(testCase.threeRecordings), false);
        settle(page);
        const scan = findChild(page, "scanController");
        compare(scan.recordingsEnabled, false);
        compare(scan.recordingCount, 0, "nothing listed");
        compare(sidebarNames(page), "All tracks(all)|Held Playlist");
        compare(findChild(page, "keepProgress").visible, false, "no Save as FLAC strip");

        realAppSettings.experimentalFeaturesEnabled = true;
        tryCompare(scan, "recordingCount", 3);
        tryCompare(sidebar(page), "fixedRowCount", 2);
        compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist");

        mouseClick(recordingsRow(page));
        tryCompare(page, "showingRecordings", true);
        tryCompare(findChild(page, "trackListView"), "count", 3);

        realAppSettings.experimentalFeaturesEnabled = false;
        tryCompare(page, "showingRecordings", false);
        compare(scan.recordingCount, 0);
        compare(sidebarNames(page), "All tracks(all)|Held Playlist");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002", "the library again");
        compare(sidebarRows(page)[0].isCurrent, true, "on All tracks");
    }

    // The row lists every recording, newest first, with its name, date,
    // length, size and "Not in the library"; its count is theirs. "All
    // tracks" lists none of them and counts none.
    function test_theRowListsTheRecordingsNewestFirst() {
        const page = makePage(makeStick(testCase.threeRecordings), true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        const fixed = recordingsRow(page);
        verify(fixed !== null);
        compare(fixed.count, 3, "the row's count");
        compare(fixed.iconName, "media-record");
        compare(sidebarRows(page)[0].count, 3, "All tracks counts the library's tracks only");

        mouseClick(fixed);
        tryCompare(page, "showingRecordings", true);
        compare(titles(page), "Session-0002.wav|REC001.WAV|Session-0001.wav");
        compare(fixed.isCurrent, true);
        compare(sidebarRows(page)[0].isCurrent, false, "All tracks is not the one shown");
        compare(page.currentPlaylistLabel, "Own Recordings");

        const first = scan.tracks.trackAt(0);
        compare(first.recording, true);
        compare(first.durationSeconds, 3, "the length from the header");
        compare(first.recordingSizeBytes, 44 + 3 * 44100 * 2);
        compare(first.recordedAt.getTime(), (testCase.base + 2 * testCase.day) * 1000);
        compare(first.recordingUnfinished, false);
        verify(first.sourceId.endsWith("/Sessions/Session-0002.wav"), first.sourceId);
        verify(first.sourceId.startsWith("recording:"), first.sourceId);

        waitForRendering(page);
        const rows = trackRows(page);
        compare(rows.length, 3);
        verify(findChild(rows[0], "recordingBadge").visible, "marked as not in the library");
        const subtitle = findChild(rows[0], "rowSubtitle").text;
        verify(subtitle.indexOf(Theme.humanBytes(44 + 3 * 44100 * 2)) >= 0, subtitle);
        verify(subtitle.indexOf("not finished") < 0, subtitle);
        compare(findChild(rows[0], "cueCountLabel").text, "", "no cue count for a file without cues");

        page.selectPlaylist(0, "");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002", "All tracks lists none of them");
        compare(page.showingRecordings, false);
        compare(fixed.isCurrent, false);
    }

    // A WAV the player did not finish says so, and its length is the
    // audio the file holds (its header says none).
    function test_anUnfinishedRecordingSaysSo() {
        const page = makePage(makeStick([
            {folder: "Sessions", name: "Session-0007.wav", seconds: 4, unfinished: true, at: testCase.base},
        ]), true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 1);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 1);
        const row = scan.tracks.trackAt(0);
        compare(row.recordingUnfinished, true);
        compare(row.durationSeconds, 4, "what the file holds");
        waitForRendering(page);
        const subtitle = findChild(trackRows(page)[0], "rowSubtitle").text;
        verify(subtitle.indexOf("not finished by the player") >= 0, subtitle);
    }

    // A recording deleted meanwhile is gone once the stick's recordings
    // are read again (the Recordings page's delete tells the page so);
    // with the last one gone the row goes and Browse is on "All tracks".
    function test_aRecordingDeletedMeanwhile() {
        const root = makeStick(testCase.threeRecordings.slice(0, 2));
        const page = makePage(root, true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 2);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 2);

        verify(browseFixture.removeFile(root + "/Sessions/Session-0002.wav"));
        scan.refreshRecordings();
        tryCompare(scan, "recordingCount", 1);
        tryCompare(findChild(page, "trackListView"), "count", 1);
        compare(titles(page), "Session-0001.wav");
        compare(recordingsRow(page).count, 1);

        verify(browseFixture.removeFile(root + "/Sessions/Session-0001.wav"));
        scan.refreshRecordings();
        tryCompare(scan, "recordingCount", 0);
        tryCompare(page, "showingRecordings", false);
        compare(sidebarNames(page), "All tracks(all)|Held Playlist");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(sidebarRows(page)[0].isCurrent, true);
    }

    Component {
        id: recordingsControllerComponent
        RecordingsController {}
    }

    // A delete on the Recordings page, of a page of its own, is heard:
    // Browse reads the stick's recordings again.
    function test_aDeleteOnTheRecordingsPageIsHeard() {
        const root = makeStick(testCase.threeRecordings);
        const page = makePage(root, true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);

        const recordings = createTemporaryObject(recordingsControllerComponent, testCase);
        recordings.load("TESTSTICK", root + "/PIONEER", root + "/Engine Library");
        tryVerify(() => recordings.listed && !recordings.busy, 10000);
        compare(recordings.recordingCount, 3);
        recordings.setIncluded(0, true);
        const finished = signalSpy.createObject(testCase, {target: recordings, signalName: "finished"});
        recordings.deleteSelected();
        tryCompare(finished, "count", 1, 10000);
        compare(finished.signalArguments[0][0].written, 1, JSON.stringify(finished.signalArguments[0][0]));
        tryCompare(scan, "recordingCount", 2);
        finished.destroy();
    }

    Component {
        id: signalSpy
        SignalSpy {}
    }

    // A delete that ends after its Recordings page has gone is heard too:
    // the page's own end never runs, the one its write is watched by does.
    function test_aDeleteThatOutlivesItsPageIsHeard() {
        const root = makeStick(testCase.threeRecordings);
        const page = makePage(root, true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);

        browseFixture.holdRecordingDeletes();
        const recordings = recordingsControllerComponent.createObject(testCase);
        recordings.load("TESTSTICK", root + "/PIONEER", root + "/Engine Library");
        tryVerify(() => recordings.listed && !recordings.busy, 10000);
        recordings.setIncluded(0, true);
        recordings.deleteSelected();
        // In its file: leaving the page stops a delete after the file in
        // hand, and this one is in hand.
        tryVerify(() => browseFixture.recordingDeletesWaiting() === 1, 10000, "the delete waits at its first file");
        recordings.destroy();
        wait(50);
        compare(scan.recordingCount, 3, "nothing deleted yet");

        browseFixture.releaseRecordingDeletes();
        tryCompare(scan, "recordingCount", 2, 10000);
        compare(scan.tracks.trackCount(), 3, "the library's rows are as they were");
    }

    // A listing of the stick read before comes back after a read of
    // another one began: it is not this stick's, and what was listed of
    // the one before went with it.
    function test_aListingOfTheStickBeforeIsNotThisOnes() {
        const page = makePage(makeStick(testCase.threeRecordings), true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        compare(scan.recordingsListed, true);
        const other = makeStick([]);

        // The other stick's read waits before its tracks, so nothing of
        // it lists its recordings while the first stick's listing ends:
        // the app's catalog cache, gated, instead of the held one.
        browseFixture.restore();
        catalogGate.hold(3, true);
        scan.refreshRecordings();
        verify(scan.recordingsListing);
        scan.scan("rekordbox", other + "/PIONEER", "");
        compare(scan.recordingCount, 0, "the stick before's recordings went with it");
        compare(scan.recordingsListed, false);
        tryVerify(() => catalogGate.waiting() === 1, 10000, "the other stick's read waits");
        tryCompare(scan, "recordingsListing", false, 10000);
        compare(scan.recordingCount, 0, "a listing of the stick before is not this one's");
        compare(scan.recordingsListed, false);
        compare(sidebar(page).fixedRowCount, 1);

        catalogGate.release();
        tryCompare(scan, "recordingsListed", true, 10000);
        compare(scan.recordingCount, 0);
    }

    // The Recordings page's rows, as its delegate reads them.
    Component {
        id: recordingRowsComponent
        Instantiator {
            delegate: QtObject {
                required property string path
                required property string fileName
                required property bool unfinished
                required property bool holdsNoAudio
                required property double heldSeconds
                required property double declaredSeconds
                required property bool declaredUnusable
                required property double missingBytes
            }
        }
    }

    // Browse and the Recordings page say the same of one file: a WAV the
    // player did not finish, one that holds no audio, one that is whole.
    function test_bothPagesSayTheSameOfAFile() {
        const root = makeStick([
            {folder: "Sessions", name: "Session-0003.wav", seconds: 4, unfinished: true, at: testCase.base},
            {folder: "Sessions", name: "Session-0004.wav", seconds: 0, at: testCase.base + testCase.day},
            {folder: "Sessions", name: "Session-0005.wav", seconds: 2, at: testCase.base + 2 * testCase.day},
        ]);
        const page = makePage(root, true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);

        const recordings = createTemporaryObject(recordingsControllerComponent, testCase,
                                                 {keepEnabled: realAppSettings.experimentalFeaturesEnabled});
        recordings.load("TESTSTICK", root + "/PIONEER", root + "/Engine Library");
        tryVerify(() => recordings.listed && !recordings.busy, 10000);
        const rows = createTemporaryObject(recordingRowsComponent, testCase, {model: recordings.recordings});
        compare(rows.count, 3);
        const said = {};
        for (let i = 0; i < rows.count; ++i) {
            const row = rows.objectAt(i);
            said[row.path] = {unfinished: row.unfinished, holdsNoAudio: row.holdsNoAudio, row: row};
        }
        let unfinished = 0;
        let noAudio = 0;
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            const row = scan.tracks.trackAt(i);
            const there = said[row.filePath];
            verify(there !== undefined, row.filePath);
            compare(row.recordingUnfinished, there.unfinished, row.title + " unfinished");
            compare(row.recordingHoldsNoAudio, there.holdsNoAudio, row.title + " holds no audio");
            // What the form is opened with is the page's row, field for field.
            const asked = scan.recordingAt(row.filePath);
            for (const field of ["path", "fileName", "unfinished", "holdsNoAudio", "heldSeconds", "declaredSeconds",
                                 "declaredUnusable", "missingBytes"]) {
                compare(asked[field], there.row[field], row.title + " " + field);
            }
            unfinished += row.recordingUnfinished ? 1 : 0;
            noAudio += row.recordingHoldsNoAudio ? 1 : 0;
        }
        compare(unfinished, 1, "the unfinished one is told on both");
        compare(noAudio, 1, "the empty one is told on both");
        waitForRendering(page);
        const empty = trackRows(page).find(r => r.title === "Session-0004.wav");
        const subtitle = findChild(empty, "rowSubtitle").text;
        verify(subtitle.indexOf("holds no audio") >= 0, subtitle);
    }

    // A recording plays as the Recordings page plays one (loadFile), with
    // no queue: the next row would be read as a library's track. Back in
    // the library, its rows are the queue again.
    function test_aRecordingPlaysThroughLoadFile() {
        const root = makeStick(testCase.threeRecordings);
        const page = makePage(root, true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.selectPlaylist(0, "");
        page.playRow(scan.tracks.trackAt(0));
        compare(realPlayback.currentQueueRow(), 0, "a library track queues");
        // Its details opened with it.
        page.trackPanelOpen = false;

        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(realPlayback.currentQueueRow(), -1, "no queue while the recordings are shown");
        page.selectPlaylist(0, "");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(realPlayback.currentQueueRow(), 0, "the library's rows queue again");

        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        waitForRendering(page);
        const row = trackRows(page)[1];
        compare(row.title, "REC001.WAV");
        page.playRow(row);
        compare(realPlayback.currentSourceId, "recording:" + row.filePath);
        compare(realPlayback.hasNext, false, "no queue");
        compare(realPlayback.hasPrevious, false, "no queue");
        tryCompare(row, "isPlaying", true);
        compare(page.trackPanelOpen, false, "no details pane for a file no library lists");

        // Read again with the recording playing: still no queue of them.
        page.rescan();
        tryVerify(() => !scan.busy && !scan.cuesPending, 10000);
        settle(page);
        compare(page.showingRecordings, true);
        compare(realPlayback.currentQueueRow(), -1, "the recordings are no queue after a read either");
        compare(realPlayback.hasNext, false);

        page.selectPlaylist(0, "");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(realPlayback.currentQueueRow(), -1, "the recording is none of the library's rows");
    }

    // What does not apply to a recording is not offered: no details pane
    // (cues, tags), no row menu (Analyse again, delete, add to a
    // playlist), no drag, no files dropped on the list; the row says why.
    // The fixed rows take no dropped track and have no menu.
    function test_editsThatDoNotApplyAreNotOffered() {
        const page = makePage(makeStick(testCase.threeRecordings), true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        waitForRendering(page);
        const row = trackRows(page)[0];

        compare(page.openDetails(row), false);
        compare(page.trackPanelOpen, false);
        page.openTrackMenu(row, row);
        const menu = findChild(page, "trackMenu");
        wait(50);
        compare(menu.opened, false, "no track menu");
        const own = findChild(page, "recordingMenu");
        tryCompare(own, "opened", true);
        own.close();
        tryCompare(own, "visible", false);
        compare(page.trackDragCard.canDrag(row), false, "not dragged into a playlist");
        compare(findChild(page, "trackFileDropArea").enabled, page.trackImportEnabled,
                "files dropped on Own Recordings go into its playlist");
        compare(row.ToolTip.text, page.recordingRowText);
        mouseMove(row, row.width / 2, row.height / 2);
        tryCompare(row.ToolTip, "visible", true);

        // A library track still offers all of it.
        page.selectPlaylist(0, "");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        waitForRendering(page);
        const track = trackRows(page)[0];
        compare(page.trackDragCard.canDrag(track), true);
        compare(findChild(track, "trackRowMenuButton").enabled, true);

        const menus = signalSpy.createObject(testCase, {target: sidebar(page), signalName: "playlistContextRequested"});
        for (const fixed of sidebarRows(page).filter(r => r.isFixed)) {
            // Own Recordings is its playlist, made at the first track.
            compare(fixed.acceptsTrackDrop, fixed.isRecordings, fixed.name + " takes a track as its playlist");
            fixed.contextRequested();
        }
        compare(menus.count, 0, "no menu on a fixed row");
        compare(recordingsRow(page).acceptsFileDrop, page.trackImportEnabled, "files dropped on Own Recordings too");
        compare(sidebarRows(page)[0].acceptsFileDrop, page.trackImportEnabled, "All tracks as before");
        const playlistRow = sidebarRows(page).find(r => r.name === "Held Playlist");
        playlistRow.contextRequested();
        compare(menus.count, 1, "a playlist has its menu");
        menus.destroy();
        findChild(page, "playlistMenu").close();
    }

    function typeInto(field, text) {
        mouseClick(field);
        tryCompare(field, "activeFocus", true);
        for (const ch of text) {
            keyClick(ch);
        }
        compare(field.text, text);
    }

    // The filter narrows the playlists and never the fixed rows, with one
    // fixed row and with two; the row shown stays the current one.
    function test_theFilterKeepsTheFixedRows() {
        for (const experimental of [false, true]) {
            const page = makePage(makeStick(testCase.threeRecordings), experimental);
            const scan = findChild(page, "scanController");
            if (experimental) {
                tryCompare(scan, "recordingCount", 3);
                page.showOwnRecordings();
            } else {
                settle(page);
            }
            const list = sidebar(page);
            const fixed = experimental ? 2 : 1;
            compare(list.fixedRowCount, fixed);
            const filter = findChild(page, "playlistFilterField");
            typeInto(filter, "zzz");
            tryCompare(list, "count", fixed);
            verify(findChild(page, "playlistFilterEmptyState").visible, "no playlist matches");
            compare(list.currentIndex, experimental ? 1 : 0, "the row shown is current");
            filter.text = "own";
            tryCompare(list, "count", fixed);
            compare(list.model.indexOf("Held Playlist"), -1);
            filter.text = "held";
            tryCompare(list, "count", fixed + 1);
            verify(!findChild(page, "playlistFilterEmptyState").visible);
            compare(list.model[fixed], "Held Playlist");
            // A playlist shown, found below the fixed rows, filtered or not.
            page.selectPlaylist(1, "Held Playlist");
            compare(list.currentIndex, fixed);
            filter.text = "";
            tryCompare(list, "count", fixed + 1);
            compare(list.currentIndex, fixed);
            page.destroy();
            wait(0);
            browseFixture.restore();
        }
    }

    // A stick playlist named "Own Recordings" is the fixed row: listed
    // once, it shows the playlist's tracks and after them the recordings,
    // and is the playlist shown for everything that asks (the header, the
    // count, Matching's target). The remembered playlist of that name is
    // the row when the stick is read again. With Experimental off it is an
    // ordinary playlist again, as before.
    function test_aStickPlaylistOfThatNameIsTheRow() {
        // "Zed Playlist" sorts after the playlist the row stands for, so
        // its row is found by name, not by its place among the stick's.
        const page = makePage(makeStick(testCase.threeRecordings), true, "Own Recordings|Zed Playlist");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist|Zed Playlist",
                "one row, not two");
        const fixed = recordingsRow(page);
        compare(fixed.count, 6, "its rows: three tracks and three recordings");
        realAppSettings.lastPlaylistName = "Held Playlist";

        mouseClick(fixed);
        tryCompare(page, "showingRecordings", true);
        compare(page.shownPlaylist, "Own Recordings");
        tryCompare(findChild(page, "trackListView"), "count", 6);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002|Session-0002.wav|REC001.WAV|Session-0001.wav",
                "the playlist's tracks, then the recordings newest first");
        compare(scan.recordingRowsShown, 3);
        compare(scan.tracks.trackAt(0).recording, false);
        compare(scan.tracks.trackAt(3).recording, true);
        compare(fixed.isCurrent, true);
        compare(sidebar(page).currentIndex, 1);
        compare(page.currentPlaylistLabel, "Own Recordings");
        compare(page.selectedPlaylistIndex, scan.playlistNames.indexOf("Own Recordings") + 1, "Matching's target");
        compare(findChild(page, "matchingPage").browseSelectedPlaylistIndex, page.selectedPlaylistIndex);
        compare(realAppSettings.lastPlaylistName, "Held Playlist", "the fixed row is not remembered as a playlist");
        // A search narrows both kinds.
        findChild(page, "searchField").text = "1001";
        tryCompare(findChild(page, "trackListView"), "count", 1);
        compare(scan.recordingRowsShown, 0);
        findChild(page, "searchField").text = "REC";
        tryCompare(findChild(page, "trackListView"), "count", 1);
        compare(scan.recordingRowsShown, 1);
        findChild(page, "searchField").text = "";
        tryCompare(findChild(page, "trackListView"), "count", 6);

        // Filtered by its name, only the fixed row is left.
        const filter = findChild(page, "playlistFilterField");
        filter.text = "own";
        tryCompare(sidebar(page), "count", 2);
        compare(sidebar(page).currentIndex, 1);
        filter.text = "";
        tryCompare(sidebar(page), "count", 4);

        // Other playlists are found by name: one before it among the
        // stick's playlists, and one after it, whose place there is one
        // past its row.
        page.selectPlaylist(1, "Held Playlist");
        compare(sidebar(page).currentIndex, 2);
        compare(page.showingRecordings, false);
        verify(scan.playlistNames.indexOf("Zed Playlist") > scan.playlistNames.indexOf("Own Recordings"),
               "precondition: it sorts after the row's playlist");
        page.selectPlaylist(scan.playlistNames.indexOf("Zed Playlist") + 1, "Zed Playlist");
        compare(sidebar(page).currentIndex, 3);
        compare(sidebarRows(page)[3].name, "Zed Playlist");
        verify(sidebarRows(page)[3].isCurrent);
        page.selectPlaylist(1, "Held Playlist");

        // Read again with the playlist of that name picked last: the row.
        realAppSettings.lastPlaylistName = "Own Recordings";
        page.rescan();
        tryVerify(() => !scan.busy && !scan.cuesPending, 10000);
        tryCompare(page, "showingRecordings", true);
        compare(page.shownPlaylist, "Own Recordings");
        tryCompare(findChild(page, "trackListView"), "count", 6);

        // Off: an ordinary playlist below the others, shown as one.
        realAppSettings.experimentalFeaturesEnabled = false;
        tryCompare(page, "showingRecordings", false);
        compare(sidebarNames(page), "All tracks(all)|Held Playlist|Own Recordings|Zed Playlist");
        compare(page.shownPlaylist, "Own Recordings");
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002");
        compare(sidebarRows(page)[2].isCurrent, true);
        // On again, the playlist shown is the row.
        realAppSettings.experimentalFeaturesEnabled = true;
        tryCompare(page, "showingRecordings", true);
        tryCompare(findChild(page, "trackListView"), "count", 6);
    }

    // The playlist alone, without a recording on the stick, makes the row.
    function test_thePlaylistAloneMakesTheRow() {
        const page = makePage(makeStick([]), true, "Own Recordings");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingsListed", true, 10000);
        settle(page);
        compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist");
        compare(recordingsRow(page).count, 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(scan.recordingRowsShown, 0);
    }

    // Own Recordings shown stays shown when the stick is read again (a
    // save, the other library picked), and Matching's target stays the
    // whole library.
    function test_theRowShownIsRestoredAfterARead() {
        const page = makePage(makeStick(testCase.threeRecordings), true);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.selectPlaylist(1, "Held Playlist");
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(page.selectedPlaylistIndex, 0);
        page.rescan();
        tryVerify(() => !scan.busy && !scan.cuesPending, 10000);
        settle(page);
        compare(page.showingRecordings, true);
        compare(scan.showingRecordings, true);
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Session-0002.wav|REC001.WAV|Session-0001.wav");
        compare(recordingsRow(page).isCurrent, true);
        compare(realAppSettings.lastPlaylistName, "Held Playlist", "what is remembered is the playlist");
    }

    // The sidebar outside Browse never has the row, whatever the stick
    // holds: its fixed rows are "All tracks" unless the host adds one.
    Component {
        id: fakeScanComponent
        QtObject {
            property var playlistNames: ["Peak"]
            property var playlistTrackCounts: ({"Peak": 2})
            property var playlistThumbnails: ({})
            property int totalTrackCount: 4
            property int importAwaitingReadCount: 0
            property int recordingCount: 5
            function playlistSummary(name) { return {}; }
        }
    }
    Component {
        id: listComponent
        PlaylistListView { width: 240; height: 200 }
    }
    function test_otherHostsGetOnlyAllTracks() {
        const fake = createTemporaryObject(fakeScanComponent, testCase);
        const list = createTemporaryObject(listComponent, testCase, {scanController: fake, checkable: true});
        waitForRendering(list);
        compare(list.fixedRowCount, 1);
        compare(list.count, 2);
        compare(JSON.stringify(list.model), JSON.stringify(["All tracks", "Peak"]));
        compare(list.itemAtIndex(0).fixedKind, "all");
        compare(list.itemAtIndex(1).isFixed, false);
        // No row here has an icon: none is made, let alone loaded.
        for (let i = 0; i < list.count; ++i) {
            const icon = findChild(list.itemAtIndex(i), "playlistRowIcon");
            verify(icon !== null);
            compare(icon.item, null, "no icon made for row " + i);
        }
    }

    // In Browse only Own Recordings has one.
    function test_onlyOwnRecordingsMakesAnIcon() {
        const page = makePage(makeStick(testCase.threeRecordings), true);
        tryCompare(findChild(page, "scanController"), "recordingCount", 3);
        waitForRendering(page);
        for (const row of sidebarRows(page)) {
            const icon = findChild(row, "playlistRowIcon");
            if (row.isRecordings) {
                verify(icon.item !== null, "Own Recordings shows its icon");
            } else {
                compare(icon.item, null, row.name + " makes none");
            }
        }
    }

    // ---- Save as FLAC and the Recordings page, from Browse ----

    Component {
        id: realKeepComponent
        KeepRecordingController {}
    }

    function menuTexts(menu) {
        const texts = [];
        for (let i = 0; i < menu.count; ++i) {
            texts.push(menu.itemAt(i).text);
        }
        return texts;
    }

    function openRecordingMenu(page, title) {
        waitForRendering(page);
        const row = trackRows(page).find(r => r.title === title);
        verify(row !== undefined, title);
        const button = findChild(row, "trackRowMenuButton");
        verify(button.enabled, "a recording's menu button is enabled");
        // As a right click does: the button's column may be squeezed out
        // at this width under another style.
        page.openTrackMenu(row, row);
        const menu = findChild(page, "recordingMenu");
        tryCompare(menu, "opened", true);
        return menu;
    }

    // A recording's menu offers exactly saving it as FLAC, adding that
    // FLAC to the library, and showing it on the Recordings page; no
    // delete. Saving is not offered for a WAV
    // without audio, as the Recordings page does not offer it.
    function test_aRecordingsMenu() {
        const keep = createTemporaryObject(realKeepComponent, testCase);
        verify(keep.available, "this build saves FLAC");
        const root = makeStick([
            {folder: "Sessions", name: "Session-0005.wav", seconds: 2, at: testCase.base + testCase.day},
            {folder: "Sessions", name: "Session-0004.wav", seconds: 0, at: testCase.base},
        ]);
        const page = makePage(root, true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 2);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 2);

        let menu = openRecordingMenu(page, "Session-0005.wav");
        compare(JSON.stringify(menuTexts(menu)), JSON.stringify(["Save as FLAC...", "Add to the library (save it as FLAC first)",
                                                                 "Show on the Recordings page"]));
        compare(findChild(menu, "addToLibraryItem").enabled, false, "nothing to add before the FLAC is saved");
        verify(findChild(menu, "saveAsFlacItem").enabled);
        const asked = signalSpy.createObject(testCase, {target: page, signalName: "recordingsPageRequested"});
        findChild(menu, "showOnRecordingsPageItem").triggered();
        compare(asked.count, 1);
        compare(asked.signalArguments[0][0], scan.tracks.trackAt(0).filePath);
        verify(asked.signalArguments[0][0].endsWith("/Sessions/Session-0005.wav"));
        asked.destroy();
        menu.close();
        tryCompare(menu, "visible", false);

        menu = openRecordingMenu(page, "Session-0004.wav");
        compare(findChild(menu, "saveAsFlacItem").enabled, false, "nothing to save in a WAV without audio");
        menu.close();
        tryCompare(menu, "visible", false);
    }

    // "Save as FLAC..." opens the Recordings page's form, on this stick:
    // the controller is told which stick it is for, so its rules hold
    // (a folder on the stick is refused), and an unfinished recording is
    // saved only with its box ticked.
    function test_saveAsFlacOpensTheRecordingsPagesForm() {
        const keep = createTemporaryObject(realKeepComponent, testCase);
        const root = makeStick([
            {folder: "Sessions", name: "Late_Set.wav", seconds: 2, at: testCase.base + testCase.day},
            {folder: "Sessions", name: "Cut.wav", seconds: 3, unfinished: true, at: testCase.base},
        ]);
        const page = makePage(root, true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 2);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 2);

        let menu = openRecordingMenu(page, "Late_Set.wav");
        findChild(menu, "saveAsFlacItem").triggered();
        const dialog = findChild(page, "keepDialog");
        tryCompare(dialog, "opened", true);
        compare(dialog.title, "Save Recording as FLAC");
        compare(dialog.headline, "Late_Set.wav becomes a FLAC file with tags and a cover on this computer.");
        compare(findChild(dialog, "keepStartButton").text, "Save as FLAC");
        // One ellipsis spelling, Browse's and the Recordings page's: "...".
        compare(findChild(dialog, "keepCoverButton").text, "Choose Picture...");
        compare(findChild(dialog, "keepFolderButton").text, "Choose...");
        compare(keep.stickRoot, scan.recordingsRoot, "the form is for this stick");
        verify(findChild(dialog, "keepTitleField").text.indexOf("Late") >= 0, findChild(dialog, "keepTitleField").text);
        compare(findChild(dialog, "keepUnfinishedNotice").visible, false);

        const folder = findChild(dialog, "keepFolderField");
        folder.text = root + "/Kept";
        const problems = findChild(dialog, "keepProblemsLabel");
        tryCompare(problems, "visible", true);
        compare(problems.text, "This folder is on the stick. The FLAC file goes onto this computer; the stick keeps "
                + "the recording.");
        compare(findChild(dialog, "keepStartButton").enabled, false, "nothing is saved onto the stick");
        folder.text = stickFixture.scratchDirectory() + "/Kept";
        tryCompare(problems, "visible", false);
        verify(findChild(dialog, "keepStartButton").enabled);
        dialog.close();
        tryCompare(dialog, "visible", false);

        menu = openRecordingMenu(page, "Cut.wav");
        findChild(menu, "saveAsFlacItem").triggered();
        tryCompare(dialog, "opened", true);
        verify(findChild(dialog, "keepUnfinishedNotice").visible, "an unfinished recording says so");
        findChild(dialog, "keepFolderField").text = stickFixture.scratchDirectory() + "/Kept";
        compare(findChild(dialog, "keepStartButton").enabled, false, "only with the box ticked");
        findChild(dialog, "keepUnfinishedBox").checked = true;
        verify(findChild(dialog, "keepStartButton").enabled);
        dialog.close();
        tryCompare(dialog, "visible", false);
    }

    // The window's KeepRecordingController's shape, as far as Browse asks
    // it: what a save of a recording is or came to, and the stick's work.
    Component {
        id: fakeKeepComponent
        QtObject {
            id: fakeKeep
            property bool available: true
            property string stickRoot: ""
            property int revision: 0
            property bool busy: false
            property bool cancelRequested: false
            property string currentStickRoot: ""
            property string currentRecording: ""
            property string currentFileName: ""
            property string currentFolder: ""
            property string progressStep: ""
            property double progress: 0
            // path -> outcome
            property var outcomes: ({})
            property var finished: []
            // What outcomeFor() was asked, without a change signal (a
            // binding asks it).
            property var asked: []
            function localPath(path) { return path; }
            function folderUrl(folder) { return "file://" + folder; }
            property string errorMessage: ""
            property string coverPath: ""
            property string coverPreview: ""
            property string coverError: ""
            property bool coverPreparing: false
            property int coverWidth: 0
            property int coverHeight: 0
            function fileNameFor(artist, title, folder) { return title + ".flac"; }
            function suggestionsFor(name, recorded) {
                return {artist: "", title: name, album: "", genre: "", year: 0, folder: "/home/dj/Kept"};
            }
            function pickCover(path) {}
            function textProblem(field, text) { return ""; }
            function yearProblem(year) { return ""; }
            function folderProblem(folder) { return ""; }
            function targetProblem(folder, artist, title) { return ""; }
            function canKeep(path) { return path.toLowerCase().endsWith(".wav"); }
            function outcomeFor(stick, path) {
                fakeKeep.asked.push(stick + "|" + path);
                return fakeKeep.outcomes[path] ?? ({});
            }
            // path -> the FLAC it was saved as, while there
            property var flacs: ({})
            function keptFlacFor(stick, path) { return fakeKeep.flacs[path] ?? ""; }
            // path -> true: a file is at its FLAC's path that is not the one saved
            property var changed: ({})
            function keptFlacChanged(stick, path) { return fakeKeep.changed[path] === true; }
            function runsFor(stick) { return fakeKeep.busy && fakeKeep.currentStickRoot === stick; }
            function runsElsewhere(stick) { return fakeKeep.busy && fakeKeep.currentStickRoot !== stick; }
            function queuedFor(stick) { return []; }
            function finishedJobsFor(stick) { return fakeKeep.finished; }
            function hasWorkFor(stick) { return fakeKeep.runsFor(stick); }
            function cancelFor(stick) {}
            function dismissFinishedFor(stick) { fakeKeep.finished = []; fakeKeep.revision++; }
            function showInFolder(path) {}
        }
    }

    // A recording's row says what its save is or came to, as its row on
    // the Recordings page does, from the controller's own outcome; the
    // strip of the stick's work is the Recordings page's. Nothing of it
    // is made on a library track's row.
    function test_theRowAndTheStripShowTheSave() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        // A library track asks nothing of the controller.
        waitForRendering(page);
        compare(findChild(trackRows(page)[0], "recordingSaveStateLoader").item, null);
        compare(keep.asked.length, 0, "no library row asks after a save");

        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        const path = scan.tracks.trackAt(0).filePath;
        const state = () => {
            waitForRendering(page);
            const item = findChild(trackRows(page)[0], "recordingSaveStateLoader").item;
            return item === null ? "" : item.text;
        };
        compare(state(), "");
        verify(keep.asked.indexOf(scan.recordingsRoot + "|" + path) >= 0, "asked of this stick and this file");

        keep.outcomes = {[path]: {state: "queued", fileName: "DJ Kelp - Late Set.flac"}};
        keep.revision++;
        compare(state(), "Waits to be saved as DJ Kelp - Late Set.flac");
        const menu = openRecordingMenu(page, "Session-0002.wav");
        compare(findChild(menu, "saveAsFlacItem").enabled, false, "a save waiting is not asked for again");
        menu.close();
        tryCompare(menu, "visible", false);

        keep.outcomes = {[path]: {state: "running", fileName: "DJ Kelp - Late Set.flac"}};
        keep.busy = true;
        keep.currentStickRoot = scan.recordingsRoot;
        keep.currentRecording = "Session-0002.wav";
        keep.progressStep = "encoding";
        keep.progress = 0.4;
        keep.revision++;
        compare(state(), "Being saved as DJ Kelp - Late Set.flac: encoding");
        const strip = findChild(page, "keepProgress");
        tryCompare(strip, "visible", true);
        compare(findChild(strip, "passiveProgressSummary").text, "Saving Session-0002.wav as FLAC: encoding, 40%");

        keep.busy = false;
        keep.outcomes = {[path]: {state: "kept", fileName: "DJ Kelp - Late Set.flac", folder: "/home/dj/Kept"}};
        keep.finished = [{state: "kept", recording: "Session-0002.wav", fileName: "DJ Kelp - Late Set.flac",
                          folder: "/home/dj/Kept", flacPath: "/home/dj/Kept/DJ Kelp - Late Set.flac", leftBehind: ""}];
        keep.revision++;
        compare(state(), "Saved as DJ Kelp - Late Set.flac in /home/dj/Kept");
        compare(findChild(strip, "passiveProgressSummary").text, "Saved Session-0002.wav as DJ Kelp - Late Set.flac");

        keep.outcomes = {[path]: {state: "failed", fileName: "x.flac", message: "There is not enough room in /f."}};
        keep.finished = [];
        keep.revision++;
        compare(state(), "Not saved: There is not enough room in /f.");
        tryCompare(strip, "visible", false);
    }

    // "Show on the Recordings page" opens Clean Up Recordings at that
    // recording: scrolled to and marked, the others not.
    Component {
        id: recordingsPageComponent
        RecordingsPage {}
    }
    Component {
        id: stackComponent
        StackView {}
    }
    function test_theRecordingsPageMarksTheRecordingAsked() {
        const recordings = [];
        for (let i = 0; i < 30; ++i) {
            recordings.push({folder: "Sessions", name: "Session-" + (1000 + i) + ".wav", seconds: 0.1,
                             at: testCase.base + i * 60});
        }
        const root = makeStick(recordings);
        const marked = root + "/Sessions/Session-1027.wav";
        const stack = createTemporaryObject(stackComponent, testCase, {width: 900, height: 500});
        const page = stack.push(recordingsPageComponent, {
            stickLabel: "TESTSTICK", rekordboxPath: root + "/PIONEER", enginePath: root + "/Engine Library",
            controller: recordingsControllerComponent.createObject(stack), playbackController: realPlayback,
            markedPath: marked}, StackView.Immediate);
        tryVerify(() => page.controller.listed && !page.controller.busy, 10000);
        const list = findChild(page, "recordingsList");
        const index = page.controller.rowOf(marked);
        verify(index >= 20, "far enough down to need scrolling: " + index);
        tryVerify(() => {
            const item = list.itemAtIndex(index);
            return item !== null && item.y + item.height > list.contentY && item.y < list.contentY + list.height;
        }, 3000, "the recording asked for is in view");
        const item = list.itemAtIndex(index);
        verify(findChild(item, "markedFrame").visible, "and marked");
        compare(item.marked, true);
        const first = list.itemAtIndex(index - 1);
        compare(first.marked, false);

        // Listed again (as after a delete): the view stays where it is put.
        list.positionViewAtBeginning();
        // From the list's start: a style with a header moves its origin
        // when the rows are made again.
        const top = list.contentY - list.originY;
        const listedAgain = signalSpy.createObject(testCase, {target: page.controller, signalName: "listingChanged"});
        page.controller.load("TESTSTICK", root + "/PIONEER", root + "/Engine Library");
        tryVerify(() => listedAgain.count > 0 && page.controller.listed && !page.controller.busy, 10000);
        listedAgain.destroy();
        // The page answers a listing with Qt.callLater(): once a call made
        // later has run, so has the page's, and the view is laid out.
        let answered = false;
        Qt.callLater(() => { answered = true; });
        tryVerify(() => answered, 2000, "the page's answer to the listing has run");
        verify(waitForPolish(list));
        compare(list.contentY - list.originY, top, "no jump back to the marked row");
        // The mark stays: on its row, brought into view to be read.
        list.positionViewAtIndex(index, ListView.Center);
        verify(waitForPolish(list));
        const markedAgain = list.itemAtIndex(index);
        verify(markedAgain !== null, "the marked row is made");
        compare(markedAgain.marked, true, "the mark stays");
        verify(findChild(markedAgain, "markedFrame").visible);
        compare(list.itemAtIndex(index - 1).marked, false);
    }

    // ---- The stick playlist "Own Recordings" ----

    // What the page asks of the import: the calls, and nothing staged.
    Component {
        id: fakeImportComponent
        QtObject {
            property var calls: []
            property bool busy: false
            property string progressStep: ""
            property int progressDone: 0
            property int progressTotal: 0
            property string progressFile: ""
            property var progressFiles: []
            property real progressFraction: 0
            property bool progressPlanned: false
            property string errorMessage: ""
            property string errorSource: "import"
            property int pendingRevision: 0
            property var stagedImport: ({})
            property int stagedCount: 0
            property string stagedPlaylist: ""
            property bool stagedPlaylistIsNew: false
            property var lastResult: ({})
            property string reanalysingPlaylist: ""
            property var lastReanalysis: ({})
            property var cueReview: []
            property var cueReviewTooDifferent: []
            property int cueFollowThresholdMs: 10
            property bool saveWriting: false
            signal resultReady()
            signal tracksImported()
            signal reanalysisReady()
            signal moveCuesChanged(string changeId, bool move)
            function importFiles(rekordboxPath, enginePath, files, playlist, create, atIndex) {
                calls.push([files.map(f => f.toString()), playlist, create, atIndex]);
                return true;
            }
            function importMakingPlaylistWhereMissing(rekordboxPath, enginePath, files, playlist, atIndex) {
                calls.push([files.map(f => f.toString()), playlist, "where missing", atIndex]);
                return true;
            }
            function cancel() {}
            function releaseLanded() {}
            function dropStaged() {}
            function trackCountChange(playlist) { return 0; }
            function reanalysisCount(playlist) { return 0; }
            function fileDialogFilter() { return "*.wav"; }
            function suggestedPlaylistName(files) { return ""; }
        }
    }

    // A saved FLAC added to the library goes into the stick playlist "Own
    // Recordings", at its top (newest first): made by that import while
    // the stick has none, put into it once it has.
    function test_addToTheLibraryGoesIntoThePlaylist() {
        for (const hasPlaylist of [false, true]) {
            const keep = createTemporaryObject(fakeKeepComponent, testCase);
            const page = makePage(makeStick(testCase.threeRecordings), true, hasPlaylist ? "Own Recordings" : "", keep);
            const scan = findChild(page, "scanController");
            tryCompare(scan, "recordingCount", 3);
            const fake = createTemporaryObject(fakeImportComponent, testCase);
            page.trackImportController = fake;

            page.showOwnRecordings();
            tryCompare(findChild(page, "trackListView"), "count", hasPlaylist ? 6 : 3);

            const path = scan.tracks.trackAt(hasPlaylist ? 3 : 0).filePath;
            keep.outcomes = {[path]: {state: "kept", fileName: "Late Set.flac", folder: "/home/dj/Kept"}};
            keep.flacs = {[path]: "/home/dj/Kept/Late Set.flac"};
            keep.revision++;
            const menu = openRecordingMenu(page, "Session-0002.wav");
            const add = findChild(menu, "addToLibraryItem");
            compare(add.text, "Add to the library");
            verify(add.enabled);
            add.triggered();
            compare(JSON.stringify(fake.calls[0]), JSON.stringify([["/home/dj/Kept/Late Set.flac"], "Own Recordings",
                                                                   "where missing", 0]), "the FLAC, at the top");
            menu.close();
            tryCompare(menu, "visible", false);
            page.destroy();
            wait(0);
            browseFixture.restore();
        }
    }

    // Files dropped on the row or on its list go into the stick playlist
    // "Own Recordings" as on any playlist (at its end): made by that
    // import while the stick has none, put into it once it has.
    function test_filesDroppedGoIntoThePlaylist() {
        for (const hasPlaylist of [false, true]) {
            const page = makePage(makeStick(testCase.threeRecordings), true, hasPlaylist ? "Own Recordings" : "");
            const scan = findChild(page, "scanController");
            tryCompare(scan, "recordingCount", 3);
            const fake = createTemporaryObject(fakeImportComponent, testCase);
            page.trackImportController = fake;

            verify(recordingsRow(page).acceptsFileDrop);
            recordingsRow(page).filesDropped(["file:///home/dj/a.wav"]);
            compare(JSON.stringify(fake.calls[0]), JSON.stringify([["file:///home/dj/a.wav"], "Own Recordings",
                                                                   "where missing", -1]), "a drop on the row");
            page.showOwnRecordings();
            tryCompare(findChild(page, "trackListView"), "count", hasPlaylist ? 6 : 3);
            verify(findChild(page, "trackFileDropArea").enabled, "the list takes files too");
            page.destroy();
            wait(0);
            browseFixture.restore();
        }
    }

    // Add to the library is offered only for the FLAC Save as FLAC made
    // and while it is there; never for the WAV. Otherwise shown, off,
    // with the reason.
    function test_addToTheLibraryNeedsTheFlac() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        const path = scan.tracks.trackAt(0).filePath;
        const said = () => {
            const menu = openRecordingMenu(page, "Session-0002.wav");
            const add = findChild(menu, "addToLibraryItem");
            const out = [add.enabled, add.text];
            menu.close();
            tryCompare(menu, "visible", false);
            return JSON.stringify(out);
        };
        compare(said(), JSON.stringify([false, "Add to the library (save it as FLAC first)"]));
        keep.outcomes = {[path]: {state: "running", fileName: "Late Set.flac"}};
        keep.revision++;
        compare(said(), JSON.stringify([false, "Add to the library (once saved as FLAC)"]));
        keep.outcomes = {[path]: {state: "kept", fileName: "Late Set.flac", folder: "/home/dj/Kept"}};
        keep.revision++;
        compare(said(), JSON.stringify([false, "Add to the library (its FLAC is no longer where it was saved)"]),
                "saved, and the FLAC moved away since");
        keep.changed = {[path]: true};
        keep.revision++;
        compare(said(), JSON.stringify([false, "Add to the library (its FLAC is no longer the one that was saved)"]),
                "saved, and another file is under its name since");
        keep.changed = {};
        keep.flacs = {[path]: "/home/dj/Kept/Late Set.flac"};
        keep.revision++;
        compare(said(), JSON.stringify([true, "Add to the library"]));
        // That FLAC waits in the import for Save already: not a second time.
        const importer = createTemporaryObject(fakeImportComponent, testCase);
        page.trackImportController = importer;
        importer.stagedImport = {playlist: "Own Recordings", isNew: true, atIndex: 0, revision: 1, landed: [],
                                 rows: [{changeId: "addcue:import:x", existing: false, filePath: scan.recordingsRoot + "/Contents/Late Set.flac",
                                         sourceFile: "/home/dj/Kept/Late Set.flac", title: "Late Set", artist: "",
                                         album: "", durationSeconds: 3, bpm: 0, key: "", bitrate: 0, artworkPath: ""}]};
        importer.stagedPlaylist = "Own Recordings";
        importer.stagedPlaylistIsNew = true;
        importer.pendingRevision++;
        compare(said(), JSON.stringify([false, "Add to the library (waits for Save)"]));
    }

    // Just enough of the playlist edits to see what a dragged track asks.
    Component {
        id: fakePlaylistEditComponent
        QtObject {
            property var calls: []
            property int pendingRevision: 0
            property string errorMessage: ""
            property var newPlaylists: []
            property var pendingMoves: ({})
            property var pendingAdditions: ({})
            property string lastAddPlaylist: ""
            // The playlists staged to be deleted.
            property var deleted: []
            // Whether a library lacks the playlist a where-missing create
            // is asked for, so that it stages one.
            property bool lacking: true
            signal pendingChanged()
            function createPlaylist(rekordboxPath, enginePath, name) {
                calls.push(["create", name]);
                newPlaylists = newPlaylists.concat([name]);
                return true;
            }
            function createPlaylistWhereMissing(rekordboxPath, enginePath, name) {
                calls.push(["create where missing", name]);
                if (lacking) {
                    newPlaylists = newPlaylists.concat([name]);
                    pendingRevision++;
                }
                return true;
            }
            function dropNewPlaylist(name) {
                calls.push(["don't create", name]);
                newPlaylists = newPlaylists.filter(n => n !== name);
                pendingRevision++;
            }
            function keepPlaylist(name) {
                calls.push(["keep", name]);
                deleted = deleted.filter(n => n !== name);
                pendingRevision++;
            }
            function addToPlaylist(rekordboxPath, enginePath, playlist, filePath, title, toIndex, moveFrom) {
                calls.push(["add", playlist, filePath, toIndex]);
                return true;
            }
            function isNewPlaylistStaged(name) { return newPlaylists.indexOf(name) >= 0; }
            function isPlaylistStaged(name) { return deleted.indexOf(name) >= 0; }
            function isRemovalStaged() { return false; }
            function isAdditionStaged() { return false; }
            function isRenameStaged() { return false; }
            function isPlaylistEdited() { return false; }
            function trackCountChange() { return 0; }
        }
    }

    // A track dragged onto the row goes into its playlist: made first in
    // every library that lacks it (staged as "Create new playlist" stages
    // one), whichever library is browsed, then the add, which then goes
    // into every library.
    function test_aDraggedTrackGoesIntoThePlaylist() {
        for (const hasPlaylist of [false, true]) {
            const page = makePage(makeStick(testCase.threeRecordings), true, hasPlaylist ? "Own Recordings" : "");
            tryCompare(findChild(page, "scanController"), "recordingCount", 3);
            const edits = createTemporaryObject(fakePlaylistEditComponent, testCase);
            edits.lacking = !hasPlaylist;
            page.playlistEditController = edits;
            verify(recordingsRow(page).acceptsTrackDrop);
            const drag = findChild(page, "trackDragItem");
            drag.filePath = "/nonexistent/HELD/Contents/held-1001.mp3";
            drag.title = "Held Track 1001";
            drag.addChosen("Own Recordings");
            compare(JSON.stringify(edits.calls), JSON.stringify(
                [["create where missing", "Own Recordings"],
                 ["add", "Own Recordings", "/nonexistent/HELD/Contents/held-1001.mp3", -1]]));
            // Staged to be made: still the one row.
            if (!hasPlaylist) {
                compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist");
            }
            page.destroy();
            wait(0);
            browseFixture.restore();
        }
    }

    // The row's summary is its playlist's, once the stick has it; the
    // recordings no library lists are left out of it.
    function test_theSummaryIsThePlaylists() {
        let page = makePage(makeStick(testCase.threeRecordings), true);
        tryCompare(findChild(page, "scanController"), "recordingCount", 3);
        compare(recordingsRow(page).hasSummary, false, "no playlist, no summary");
        page.destroy();
        wait(0);
        browseFixture.restore();
        page = makePage(makeStick(testCase.threeRecordings), true, "Own Recordings");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        const row = recordingsRow(page);
        verify(row.hasSummary);
        compare(JSON.stringify(row.summaryFor()), JSON.stringify(scan.playlistSummary("Own Recordings")));
    }

    // The row's menu while something waits for its playlist: no rename,
    // no delete, only the taking back of what waits.
    function ownRecordingsMenuTexts(page) {
        recordingsRow(page).contextRequested();
        const menu = findChild(page, "ownRecordingsMenu");
        if (!menu.visible) {
            return [];
        }
        tryCompare(menu, "opened", true);
        const texts = [];
        for (let i = 0; i < menu.count; ++i) {
            if (menu.itemAt(i).visible) {
                texts.push(menu.itemAt(i).text);
            }
        }
        return texts;
    }
    function closeOwnRecordingsMenu(page) {
        const menu = findChild(page, "ownRecordingsMenu");
        menu.close();
        tryCompare(menu, "visible", false);
    }
    function closeMessage(page) {
        const message = findChild(page, "playlistEditMessage");
        const said = message.headline;
        message.close();
        tryCompare(message, "visible", false);
        return said;
    }

    // A playlist made by a track dragged onto the row is hidden behind it,
    // so the row's menu takes the create back, in a staged new playlist's
    // words. Until Save an import cannot join it (it is checked against
    // the stick as it is): Add to the library and a drop of files say so.
    function test_theRowTakesBackTheCreateADragStaged() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        const edits = createTemporaryObject(fakePlaylistEditComponent, testCase);
        page.playlistEditController = edits;
        const importer = createTemporaryObject(fakeImportComponent, testCase);
        page.trackImportController = importer;
        compare(JSON.stringify(ownRecordingsMenuTexts(page)), "[]", "nothing waits: no menu");

        const drag = findChild(page, "trackDragItem");
        drag.filePath = "/nonexistent/HELD/Contents/held-1001.mp3";
        drag.title = "Held Track 1001";
        drag.addChosen("Own Recordings");
        compare(edits.newPlaylists.indexOf("Own Recordings") >= 0, true, "precondition: the create is staged");
        compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist", "still the one row");

        // Add to the library and files dropped wait for that Save.
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        const path = scan.tracks.trackAt(0).filePath;
        keep.outcomes = {[path]: {state: "kept", fileName: "Late Set.flac", folder: "/home/dj/Kept"}};
        keep.flacs = {[path]: "/home/dj/Kept/Late Set.flac"};
        keep.revision++;
        const menu = openRecordingMenu(page, "Session-0002.wav");
        const add = findChild(menu, "addToLibraryItem");
        compare(add.enabled, false);
        compare(add.text, "Add to the library (once Save has made \"Own Recordings\")");
        menu.close();
        tryCompare(menu, "visible", false);
        recordingsRow(page).filesDropped(["file:///home/dj/a.wav"]);
        compare(importer.calls.length, 0, "nothing offered to the import");
        compare(closeMessage(page), "\"Own Recordings\" waits to be made by Save, with the tracks dragged onto it. "
                + "Save first, then add to it.");

        compare(JSON.stringify(ownRecordingsMenuTexts(page)), JSON.stringify(["Don't create this playlist"]));
        findChild(page, "ownRecordingsDontCreateItem").triggered();
        closeOwnRecordingsMenu(page);
        compare(JSON.stringify(edits.calls[edits.calls.length - 1]), JSON.stringify(["don't create", "Own Recordings"]));
        compare(JSON.stringify(ownRecordingsMenuTexts(page)), "[]", "taken back: nothing waits");
    }

    // A delete of the stick playlist staged while Experimental was off is
    // the row's once it goes on: shown as a staged delete, nothing taken
    // in (tracks, files, Add to the library, each saying why), and the
    // row's menu keeps the playlist.
    function test_aStagedDeleteIsTheRows() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), false, "Own Recordings", keep);
        const scan = findChild(page, "scanController");
        const edits = createTemporaryObject(fakePlaylistEditComponent, testCase);
        page.playlistEditController = edits;
        const importer = createTemporaryObject(fakeImportComponent, testCase);
        page.trackImportController = importer;
        edits.deleted = ["Own Recordings"];
        edits.pendingRevision++;
        verify(sidebarRows(page).find(r => r.name === "Own Recordings").pendingDelete, "precondition: the ordinary row");

        realAppSettings.experimentalFeaturesEnabled = true;
        tryCompare(scan, "recordingCount", 3);
        const row = recordingsRow(page);
        verify(row !== null);
        compare(row.pendingDelete, true, "shown as a staged delete");
        compare(row.acceptsTrackDrop, false);
        compare(row.acceptsFileDrop, false);

        const drag = findChild(page, "trackDragItem");
        drag.filePath = "/nonexistent/HELD/Contents/held-1001.mp3";
        drag.title = "Held Track 1001";
        drag.addChosen("Own Recordings");
        compare(edits.calls.length, 0, "no add into a playlist about to be deleted");
        compare(closeMessage(page), "\"Own Recordings\" is to be deleted when you Save. Keep it first (the row's menu), "
                + "then add to it.");
        page.importFiles(["file:///home/dj/a.wav"], "Own Recordings", false, -1);
        compare(importer.calls.length, 0, "no import into it");
        verify(closeMessage(page).indexOf("is to be deleted when you Save") >= 0);

        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 6);
        const path = scan.tracks.trackAt(3).filePath;
        keep.outcomes = {[path]: {state: "kept", fileName: "Late Set.flac", folder: "/home/dj/Kept"}};
        keep.flacs = {[path]: "/home/dj/Kept/Late Set.flac"};
        keep.revision++;
        const menu = openRecordingMenu(page, "Session-0002.wav");
        const add = findChild(menu, "addToLibraryItem");
        compare(add.enabled, false);
        compare(add.text, "Add to the library (\"Own Recordings\" is to be deleted when you Save)");
        menu.close();
        tryCompare(menu, "visible", false);

        compare(JSON.stringify(ownRecordingsMenuTexts(page)), JSON.stringify(["Keep this playlist"]));
        findChild(page, "ownRecordingsKeepItem").triggered();
        closeOwnRecordingsMenu(page);
        compare(JSON.stringify(edits.calls[edits.calls.length - 1]), JSON.stringify(["keep", "Own Recordings"]));
        tryCompare(recordingsRow(page), "pendingDelete", false);
        verify(recordingsRow(page).acceptsFileDrop, "kept: it takes files again");
    }

    // The tracks of a model, as it is at one moment.
    function modelState(scan) {
        let recordings = 0;
        const count = scan.tracks.trackCount();
        for (let i = 0; i < count; ++i) {
            recordings += scan.tracks.trackAt(i).recording ? 1 : 0;
        }
        return {count: count, recordings: recordings, shown: scan.recordingRowsShown};
    }
    // Every state the list's model passes through, as its signals tell it.
    function watchModel(scan) {
        const seen = [];
        const note = () => seen.push(modelState(scan));
        scan.tracks.modelReset.connect(note);
        scan.tracks.rowsInserted.connect(note);
        scan.tracks.rowsRemoved.connect(note);
        return {seen: seen, stop: () => {
            scan.tracks.modelReset.disconnect(note);
            scan.tracks.rowsInserted.disconnect(note);
            scan.tracks.rowsRemoved.disconnect(note);
        }};
    }

    // The count of recordings shown is the rows' own whenever the rows
    // change: a reader woken by the model (the drop gap's rowCount) never
    // sees new rows with the old count.
    function test_theRecordingCountComesWithItsRows() {
        const page = makePage(makeStick(testCase.threeRecordings), true, "Own Recordings");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 6);
        const watch = watchModel(scan);
        const search = findChild(page, "searchField");
        for (const [text, rows] of [["REC", 1], ["1001", 1], ["", 6], ["Session", 2], ["", 6]]) {
            search.text = text;
            tryCompare(findChild(page, "trackListView"), "count", rows, 5000, text);
        }
        watch.stop();
        verify(watch.seen.length >= 4, "the rows changed: " + watch.seen.length);
        const mixed = watch.seen.filter(state => state.recordings !== state.shown);
        compare(JSON.stringify(mixed), "[]", "every state with its own count");
        compare(modelState(scan).shown, 3);
    }

    // Experimental off while the row is shown: the playlist shown as an
    // ordinary one, never the recordings alone (nor nothing) on the way.
    function test_experimentalOffLeavesTheRecordingsAtOnce() {
        const page = makePage(makeStick(testCase.threeRecordings), true, "Own Recordings");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 6);
        const watch = watchModel(scan);
        realAppSettings.experimentalFeaturesEnabled = false;
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002");
        watch.stop();
        verify(watch.seen.length > 0, "the rows changed");
        const wrong = watch.seen.filter(state => state.count - state.recordings === 0);
        compare(JSON.stringify(wrong), "[]", "the playlist's tracks in every state on the way");

        // Whichever the page's bindings hear first: the controller told
        // there is no playlist to show the recordings with, while they are
        // still listed, leaves their view in that step.
        realAppSettings.experimentalFeaturesEnabled = true;
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 6);
        const direct = watchModel(scan);
        scan.recordingsPlaylist = "";
        direct.stop();
        compare(scan.showingRecordings, false, "left in the same step");
        compare(JSON.stringify(direct.seen.filter(state => state.count - state.recordings === 0)), "[]",
                "never the recordings alone");
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002",
                "the playlist it showed them with");
    }

    // ---- The whole way, on a copy of a real stick ----

    // tests/qml/ -> tests/fixtures/anonymized_library.
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }
    Component {
        id: scanComponent
        ScanController {}
    }
    // The tracks of `playlist` as `format`'s library on the stick lists
    // them, read afresh.
    function playlistFiles(stick, format, playlist) {
        const scan = createTemporaryObject(scanComponent, testCase);
        scan.scan(format, format === "engine" ? stick + "/Engine Library" : stick + "/PIONEER",
                  format === "engine" ? stick + "/PIONEER" : "");
        tryVerify(() => !scan.busy && scan.tracks.trackCount() > 0, 60000, format + " is read");
        scan.filterByPlaylist(playlist);
        const files = [];
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            files.push(scan.tracks.trackAt(i).filePath);
        }
        return files;
    }

    // A recording on a copy of a real stick, saved as FLAC with the real
    // KeepRecordingController, added to the library from its menu: staged
    // into a new "Own Recordings" (shown in the row before Save), saved,
    // and read back from every library. The row then lists the FLAC, and
    // the WAV marked as in the library as FLAC; playing the FLAC, nothing
    // comes next (the WAV after it is no row of the queue).
    function test_aSavedFlacAddedThroughSave() {
        realAppSettings.experimentalFeaturesEnabled = true;
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture stick copies");
        verify(browseFixture.writeRecording(stick + "/Sessions/Session-0009.wav", 3, false, testCase.base));
        const keep = createTemporaryObject(realKeepComponent, testCase);
        verify(keep.available);
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings, keepController: keep});
        const scan = findChild(page, "scanController");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 60000, "the stick is read");
        tryCompare(scan, "recordingCount", 1, 10000);
        verify(scan.playlistNames.indexOf("Own Recordings") < 0, "precondition: no such playlist yet");
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 1);
        const wav = scan.tracks.trackAt(0).filePath;

        // Saved as FLAC on "this computer", as the form starts it.
        keep.stickRoot = scan.recordingsRoot;
        const computer = stickFixture.scratchDirectory();
        const finished = signalSpy.createObject(testCase, {target: keep, signalName: "finished"});
        verify(keep.keep({wavPath: wav, recording: "Session-0009.wav", artist: "DJ Kelp", title: "Late Set",
                          album: "", genre: "", year: "", folder: computer, keepUnfinished: false}), keep.errorMessage);
        tryCompare(finished, "count", 1, 60000);
        compare(finished.signalArguments[0][0].state, "kept", JSON.stringify(finished.signalArguments[0][0]));
        finished.destroy();
        const flac = keep.keptFlacFor(scan.recordingsRoot, wav);
        verify(flac.endsWith(".flac"), flac);

        let menu = openRecordingMenu(page, "Session-0009.wav");
        const add = findChild(menu, "addToLibraryItem");
        verify(add.enabled, add.text);
        add.triggered();
        menu.close();
        const importer = page.trackImportController;
        tryCompare(importer, "busy", false, 120000);
        compare(importer.errorMessage, "");
        const dialog = findChild(page, "importResultDialog");
        tryCompare(dialog, "opened", true);
        dialog.close();
        compare(importer.stagedCount, 1);
        compare(importer.stagedPlaylist, "Own Recordings");
        compare(importer.stagedPlaylistIsNew, true);
        // Before Save: the staged track in the row, at the top, then the
        // recording; the row once in the sidebar, counting the new track.
        tryCompare(findChild(page, "trackListView"), "count", 2);
        compare(scan.tracks.trackAt(0).pendingImport, true, "the staged FLAC");
        compare(scan.tracks.trackAt(1).recording, true);
        compare(sidebar(page).model.filter(n => n === "Own Recordings").length, 1, "one row");
        compare(recordingsRow(page).countChange, 1);

        // Save, as the page's button does.
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        let summary = null;
        const done = result => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));

        // Read again (the import asks for it): the playlist is on the
        // stick, the row its, with the FLAC and the WAV marked.
        tryVerify(() => !scan.busy && scan.playlistNames.indexOf("Own Recordings") >= 0, 60000, "read again");
        tryCompare(page, "showingRecordings", true);
        tryVerify(() => scan.tracks.trackCount() === 2 && scan.tracks.trackAt(1).recording
                  && scan.tracks.trackAt(1).recordingInLibraryAs.length > 0, 20000, "the WAV marked as in the library");
        const track = scan.tracks.trackAt(0);
        compare(track.recording, false);
        verify(track.filePath.endsWith(".flac"), track.filePath);
        compare(scan.tracks.trackAt(1).recordingInLibraryAs, track.title);
        compare(sidebarNames(page).split("|").filter(n => n.startsWith("Own Recordings")).length, 1, "still one row");
        compare(recordingsRow(page).count, 2);
        waitForRendering(page);
        const wavRow = trackRows(page)[1];
        compare(findChild(wavRow, "recordingBadge").children[0].text, "In the library as FLAC");
        compare(wavRow.ToolTip.text, page.recordingInLibraryText(track.title));
        menu = openRecordingMenu(page, "Session-0009.wav");
        compare(findChild(menu, "addToLibraryItem").enabled, false, "not a second time");
        compare(findChild(menu, "addToLibraryItem").text, "Add to the library (it is there as \"" + track.title + "\")");
        menu.close();
        tryCompare(menu, "visible", false);

        // The queue: the FLAC plays, and the WAV after it is not next.
        page.playRow(scan.tracks.trackAt(0));
        compare(realPlayback.currentQueueRow(), 0);
        compare(realPlayback.hasNext, false, "a recording is no row of the queue");
        page.trackPanelOpen = false;

        for (const format of ["rekordbox", "onelibrary", "engine"]) {
            verify(stickFixture.playlistsIn(stick, format).indexOf("Own Recordings") >= 0, format + " has the playlist");
            const files = playlistFiles(stick, format, "Own Recordings");
            compare(files.length, 1, format + ": " + files.join(", "));
            verify(files[0].endsWith(".flac"), format + ": " + files[0]);
        }
    }

    // Save, as the page's button does; the summary.
    function saveStick(stick) {
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        let summary = null;
        const done = result => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }
    // "Add to the library" on the recording `title`, and the import staged.
    function addToLibrary(page, title) {
        const menu = openRecordingMenu(page, title);
        const add = findChild(menu, "addToLibraryItem");
        verify(add.enabled, add.text);
        add.triggered();
        menu.close();
        tryCompare(menu, "visible", false);
        const importer = page.trackImportController;
        tryCompare(importer, "busy", false, 120000);
        compare(importer.errorMessage, "");
        const dialog = findChild(page, "importResultDialog");
        tryCompare(dialog, "opened", true);
        dialog.close();
        tryCompare(dialog, "visible", false);
    }

    // The stick playlist "Own Recordings" in some libraries only (a player
    // or rekordbox left the others without it), whichever is browsed: Add
    // to the library makes it in every library that lacks it, in the same
    // save, and the FLAC goes into it in all of them. A second add before
    // Save joins the first, at the top: the newest added first. An
    // unfinished recording saved with the audio it holds is known in the
    // library as a finished one is.
    function test_addedRecordingsMakeThePlaylistWhereMissing_data() {
        return [
            {tag: "the library browsed has it", planted: ["rekordbox", "onelibrary"]},
            {tag: "only Engine has it", planted: ["engine"]},
        ];
    }
    function test_addedRecordingsMakeThePlaylistWhereMissing(data) {
        realAppSettings.experimentalFeaturesEnabled = true;
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture stick copies");
        verify(browseFixture.writeRecording(stick + "/Sessions/Session-0011.wav", 2, false, testCase.base));
        verify(browseFixture.writeRecording(stick + "/Sessions/Session-0012.wav", 3, true, testCase.base + testCase.day));
        stickFixture.plantPlaylist(stick, "Own Recordings", data.planted, 0);
        for (const format of ["rekordbox", "onelibrary", "engine"]) {
            compare(stickFixture.playlistsIn(stick, format).indexOf("Own Recordings") >= 0,
                    data.planted.indexOf(format) >= 0, "precondition, " + format);
        }
        const keep = createTemporaryObject(realKeepComponent, testCase);
        verify(keep.available);
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings, keepController: keep});
        const scan = findChild(page, "scanController");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 60000, "the stick is read");
        tryCompare(scan, "recordingCount", 2, 10000);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 2);
        compare(scan.tracks.trackAt(0).title, "Session-0012.wav");
        const unfinished = scan.tracks.trackAt(0).filePath;
        const finishedWav = scan.tracks.trackAt(1).filePath;

        // Both saved as FLAC, the unfinished one with the audio it holds.
        keep.stickRoot = scan.recordingsRoot;
        const computer = stickFixture.scratchDirectory();
        const ended = signalSpy.createObject(testCase, {target: keep, signalName: "finished"});
        verify(keep.keep({wavPath: finishedWav, recording: "Session-0011.wav", artist: "DJ Kelp", title: "First Set",
                          album: "", genre: "", year: "", folder: computer, keepUnfinished: false}), keep.errorMessage);
        verify(keep.keep({wavPath: unfinished, recording: "Session-0012.wav", artist: "DJ Kelp", title: "Second Set",
                          album: "", genre: "", year: "", folder: computer, keepUnfinished: true}), keep.errorMessage);
        tryCompare(ended, "count", 2, 60000);
        for (let i = 0; i < 2; ++i) {
            compare(ended.signalArguments[i][0].state, "kept", JSON.stringify(ended.signalArguments[i][0]));
        }
        ended.destroy();

        // The older first, then the newer: it goes on top of the waiting one.
        addToLibrary(page, "Session-0011.wav");
        const importer = page.trackImportController;
        compare(importer.stagedCount, 1);
        let menu = openRecordingMenu(page, "Session-0011.wav");
        compare(findChild(menu, "addToLibraryItem").enabled, false, "the FLAC waits already");
        compare(findChild(menu, "addToLibraryItem").text, "Add to the library (waits for Save)");
        menu.close();
        tryCompare(menu, "visible", false);
        addToLibrary(page, "Session-0012.wav");
        compare(importer.stagedCount, 2, "one batch");
        compare(importer.stagedPlaylist, "Own Recordings");
        const rows = importer.stagedImport.rows;
        compare(rows.length, 2);
        compare(rows[0].sourceFile, keep.keptFlacFor(scan.recordingsRoot, unfinished), "the one added last on top");
        compare(rows[1].sourceFile, keep.keptFlacFor(scan.recordingsRoot, finishedWav));

        const summary = saveStick(stick);
        compare(summary.error, "", JSON.stringify(summary));

        // Every library has the playlist, with both, the newest first.
        for (const format of ["rekordbox", "onelibrary", "engine"]) {
            verify(stickFixture.playlistsIn(stick, format).indexOf("Own Recordings") >= 0, format + " has the playlist");
            const files = playlistFiles(stick, format, "Own Recordings");
            compare(files.length, 2, format + ": " + files.join(", "));
            verify(files[0].indexOf("Second Set") >= 0 && files[0].endsWith(".flac"), format + ": " + files.join(", "));
            verify(files[1].indexOf("First Set") >= 0 && files[1].endsWith(".flac"), format + ": " + files.join(", "));
        }

        // Read again: both recordings are known as in the library, the
        // unfinished one too, and offer no second add.
        tryVerify(() => !scan.busy && scan.playlistNames.indexOf("Own Recordings") >= 0, 60000, "read again");
        tryCompare(page, "showingRecordings", true);
        tryVerify(() => scan.tracks.trackCount() === 4 && scan.tracks.trackAt(2).recording
                  && scan.tracks.trackAt(2).recordingInLibraryAs.length > 0
                  && scan.tracks.trackAt(3).recordingInLibraryAs.length > 0, 20000,
                  "both WAVs marked as in the library");
        compare(scan.tracks.trackAt(2).title, "Session-0012.wav");
        compare(scan.tracks.trackAt(2).recordingInLibraryAs, scan.tracks.trackAt(0).title, "the unfinished one's FLAC");
        compare(scan.tracks.trackAt(3).recordingInLibraryAs, scan.tracks.trackAt(1).title);
        menu = openRecordingMenu(page, "Session-0012.wav");
        compare(findChild(menu, "addToLibraryItem").enabled, false, "not a second time");
        menu.close();
        tryCompare(menu, "visible", false);
    }

    // A track dragged onto the row with the playlist in some libraries
    // only: made in the others by the same save, the track then in it in
    // every library that has the track (asked, as for any playlist, when
    // one does not: the fixture's Engine has none of its tracks).
    function test_aDraggedTrackMakesThePlaylistWhereMissing_data() {
        return [
            {tag: "the library browsed has it", planted: ["rekordbox", "onelibrary"]},
            {tag: "only Engine has it", planted: ["engine"]},
        ];
    }
    function test_aDraggedTrackMakesThePlaylistWhereMissing(data) {
        realAppSettings.experimentalFeaturesEnabled = true;
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0);
        verify(browseFixture.writeRecording(stick + "/Sessions/Session-0013.wav", 1, false, testCase.base));
        // A track both rekordbox libraries list, by its own playlist.
        compare(stickFixture.plantPlaylist(stick, "Picked", ["rekordbox", "onelibrary"], 1).length, 1);
        stickFixture.plantPlaylist(stick, "Own Recordings", data.planted, 0);
        for (const format of ["rekordbox", "onelibrary", "engine"]) {
            compare(stickFixture.playlistsIn(stick, format).indexOf("Own Recordings") >= 0,
                    data.planted.indexOf(format) >= 0, "precondition, " + format);
        }
        const file = playlistFiles(stick, "rekordbox", "Picked")[0];
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
        const scan = findChild(page, "scanController");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 60000, "the stick is read");
        tryCompare(scan, "recordingCount", 1, 10000);
        verify(recordingsRow(page).acceptsTrackDrop);

        const drag = findChild(page, "trackDragItem");
        drag.filePath = file;
        drag.title = "Picked";
        drag.addChosen("Own Recordings");
        const ask = findChild(page, "missingLibrariesDialog");
        tryCompare(ask, "opened", true, 10000);
        findChild(ask, "addAnywayButton").clicked();
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        tryCompare(session, "pendingCount", 2, 30000);
        compare(findChild(page, "playlistEditMessage").visible, false, "nothing refused");
        const summary = saveStick(stick);
        compare(summary.error, "", JSON.stringify(summary));

        for (const format of ["rekordbox", "onelibrary", "engine"]) {
            verify(stickFixture.playlistsIn(stick, format).indexOf("Own Recordings") >= 0, format + " has the playlist");
        }
        for (const format of ["rekordbox", "onelibrary"]) {
            compare(JSON.stringify(playlistFiles(stick, format, "Own Recordings")), JSON.stringify([file]), format);
        }
    }

    // A track of the row taken out of it leaves the stick playlist only:
    // the track stays in every library. The recordings have no place in
    // the playlist, so a place in the list past its tracks is its end.
    function test_removingFromTheRowLeavesTheTrackInTheLibrary() {
        realAppSettings.experimentalFeaturesEnabled = true;
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0);
        verify(browseFixture.writeRecording(stick + "/Sessions/Session-0010.wav", 1, false, testCase.base));
        const planted = stickFixture.plantPlaylist(stick, "Own Recordings", ["rekordbox", "onelibrary", "engine"], 2);
        compare(planted.length, 2, "the playlist is planted in every library");
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: stick + "/PIONEER", enginePath: stick + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
        const scan = findChild(page, "scanController");
        tryVerify(() => !scan.busy && scan.playlistNames.indexOf("Own Recordings") >= 0, 60000, "the stick is read");
        tryCompare(scan, "recordingCount", 1, 10000);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(scan.tracks.trackAt(2).recording, true);
        compare(findChild(page, "playlistDropGap").rowCount, 2, "the playlist's rows only");

        const removed = scan.tracks.trackAt(0).filePath;
        waitForRendering(page);
        page.openTrackMenu(trackRows(page)[0], trackRows(page)[0]);
        const menu = findChild(page, "trackMenu");
        tryCompare(menu, "opened", true);
        const item = findChild(menu, "removeFromPlaylistItem");
        verify(item.visible);
        compare(item.text, "Remove from \"Own Recordings\"");
        item.triggered();
        menu.close();
        const session = EditSessionRegistry.sessionFor(EditSessionRegistry.libraryIdForPath(stick + "/PIONEER"));
        tryCompare(session, "pendingCount", 1, 30000);
        let summary = null;
        const done = result => { summary = result; };
        session.saveFinished.connect(done);
        session.save();
        tryVerify(() => summary !== null, 120000, "the save finishes");
        session.saveFinished.disconnect(done);
        compare(summary.error, "", JSON.stringify(summary));
        tryVerify(() => !scan.busy && scan.tracks.trackCount() === 2, 60000, "read again: one track and the recording");
        compare(page.showingRecordings, true);

        // The fixture's Engine library has none of these tracks: its
        // playlist is there, empty, and stays so.
        for (const format of ["rekordbox", "onelibrary"]) {
            const files = playlistFiles(stick, format, "Own Recordings");
            compare(files.length, 1, format + ": " + files.join(", "));
            verify(files.indexOf(removed) < 0, format);
            verify(playlistFiles(stick, format, "").indexOf(removed) >= 0, format + " still lists the track");
        }
        verify(stickFixture.playlistsIn(stick, "engine").indexOf("Own Recordings") >= 0, "Engine keeps the playlist");
    }

    // Browse follows the Recordings page's rule: no Save as FLAC while
    // recordings are deleted from the stick, said on the item, from the
    // window's controller, which the delete tells.
    function test_noSaveAsFlacWhileADeleteRuns() {
        const keep = createTemporaryObject(realKeepComponent, testCase);
        const root = makeStick(testCase.threeRecordings);
        const page = makePage(root, true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        let menu = openRecordingMenu(page, "Session-0002.wav");
        verify(findChild(menu, "saveAsFlacItem").enabled);
        menu.close();
        tryCompare(menu, "visible", false);

        browseFixture.holdRecordingDeletes();
        const recordings = createTemporaryObject(recordingsControllerComponent, testCase, {keepController: keep});
        recordings.load("TESTSTICK", root + "/PIONEER", root + "/Engine Library");
        tryVerify(() => recordings.listed && !recordings.busy, 10000);
        recordings.setIncluded(recordings.rowOf(root + "/Sessions/Session-0001.wav"), true);
        recordings.deleteSelected();
        tryVerify(() => browseFixture.recordingDeletesWaiting() === 1, 10000, "the delete waits at its file");
        menu = openRecordingMenu(page, "Session-0002.wav");
        const item = findChild(menu, "saveAsFlacItem");
        compare(item.enabled, false, "not while the delete runs");
        compare(item.text, "Save as FLAC... (once the recordings being deleted are gone)");
        menu.close();
        tryCompare(menu, "visible", false);

        browseFixture.releaseRecordingDeletes();
        tryCompare(scan, "recordingCount", 2, 10000);
        menu = openRecordingMenu(page, "Session-0002.wav");
        tryCompare(findChild(menu, "saveAsFlacItem"), "enabled", true);
        compare(findChild(menu, "saveAsFlacItem").text, "Save as FLAC...");
        menu.close();
        tryCompare(menu, "visible", false);
    }

    // A save under way stays in view with Experimental off, as on the
    // Recordings page, so it can still be cancelled: the strip shows, and
    // nothing new is offered (no row, no recording, no menu).
    function test_aSaveUnderWayShowsWithExperimentalOff() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), false, "", keep);
        settle(page);
        const scan = findChild(page, "scanController");
        verify(scan.recordingsRoot.length > 0);
        const strip = findChild(page, "keepProgress");
        compare(strip.visible, false, "nothing runs");
        keep.busy = true;
        keep.currentStickRoot = scan.recordingsRoot;
        keep.currentRecording = "Session-0002.wav";
        keep.progressStep = "encoding";
        keep.progress = 0.2;
        keep.revision++;
        tryCompare(strip, "visible", true);
        compare(findChild(strip, "passiveProgressSummary").text, "Saving Session-0002.wav as FLAC: encoding, 20%");
        compare(sidebarNames(page), "All tracks(all)|Held Playlist", "no row");
        compare(scan.recordingCount, 0, "no recording listed");
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002");
        keep.busy = false;
        keep.revision++;
        tryCompare(strip, "visible", false);
    }

    // Browse hands the form its stick, always: with none it does not open,
    // and the controller keeps the stick it was told of last rather than
    // saving from that one.
    function test_theFormNeedsBrowsesStick() {
        const keep = createTemporaryObject(fakeKeepComponent, testCase);
        const page = makePage(makeStick(testCase.threeRecordings), true, "", keep);
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        page.showOwnRecordings();
        tryCompare(findChild(page, "trackListView"), "count", 3);
        keep.stickRoot = "/elsewhere";
        findChild(page, "keepForm").stickRoot = "";
        const menu = openRecordingMenu(page, "Session-0002.wav");
        findChild(menu, "saveAsFlacItem").triggered();
        menu.close();
        wait(50);
        compare(findChild(page, "keepDialog").opened, false, "not opened");
        compare(keep.stickRoot, "/elsewhere", "nothing handed on");
        tryCompare(findChild(page, "playlistEditMessage"), "visible", true);
    }
}
