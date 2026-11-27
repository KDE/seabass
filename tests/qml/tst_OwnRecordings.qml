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

    function makePage(root, experimental, extraPlaylist) {
        realAppSettings.experimentalFeaturesEnabled = experimental;
        browseFixture.holdCues(3, false, true, "", extraPlaylist || "");
        browseFixture.releaseCues();
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 1100, height: 660, stickLabel: "TESTSTICK",
            rekordboxPath: root + "/PIONEER", enginePath: root + "/Engine Library",
            playbackController: realPlayback, appSettingsController: realAppSettings});
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
        verify(findChild(rows[0], "notInLibraryBadge").visible, "marked as not in the library");
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
                required property bool unfinished
                required property bool holdsNoAudio
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
            said[row.path] = {unfinished: row.unfinished, holdsNoAudio: row.holdsNoAudio};
        }
        let unfinished = 0;
        let noAudio = 0;
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            const row = scan.tracks.trackAt(i);
            const there = said[row.filePath];
            verify(there !== undefined, row.filePath);
            compare(row.recordingUnfinished, there.unfinished, row.title + " unfinished");
            compare(row.recordingHoldsNoAudio, there.holdsNoAudio, row.title + " holds no audio");
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
        compare(menu.opened, false, "no row menu");
        compare(findChild(row, "trackRowMenuButton").enabled, false);
        compare(page.trackDragCard.canDrag(row), false, "not dragged into a playlist");
        compare(findChild(page, "trackFileDropArea").enabled, false, "no files dropped on the recordings");
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
            compare(fixed.acceptsTrackDrop, false, fixed.name + " takes no track");
            fixed.contextRequested();
        }
        compare(menus.count, 0, "no menu on a fixed row");
        compare(recordingsRow(page).acceptsFileDrop, false, "no files dropped on Own Recordings");
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

    // A stick playlist named "Own Recordings" is a playlist of its own,
    // below the fixed row: picking either shows its own rows, the other
    // is not current, and the remembered playlist of that name is that
    // playlist when it is read again.
    function test_aStickPlaylistOfTheSameNameIsAnother() {
        const page = makePage(makeStick(testCase.threeRecordings), true, "Own Recordings");
        const scan = findChild(page, "scanController");
        tryCompare(scan, "recordingCount", 3);
        compare(sidebarNames(page), "All tracks(all)|Own Recordings(recordings)|Held Playlist|Own Recordings");
        let rows = sidebarRows(page);
        let fixed = rows[1];
        let playlist = rows[3];

        mouseClick(playlist);
        tryCompare(page, "shownPlaylist", "Own Recordings");
        compare(page.showingRecordings, false);
        tryCompare(findChild(page, "trackListView"), "count", 3);
        compare(titles(page), "Held Track 1000|Held Track 1001|Held Track 1002");
        compare(playlist.isCurrent, true);
        compare(fixed.isCurrent, false);
        compare(sidebar(page).currentIndex, 3);
        compare(realAppSettings.lastPlaylistName, "Own Recordings");
        // Filtered, the playlist is found below the fixed row of its name.
        const filter = findChild(page, "playlistFilterField");
        filter.text = "own";
        tryCompare(sidebar(page), "count", 3);
        compare(sidebar(page).currentIndex, 2);
        filter.text = "";
        tryCompare(sidebar(page), "count", 4);
        // The filter made the rows again.
        waitForRendering(page);
        rows = sidebarRows(page);
        fixed = rows[1];
        playlist = rows[3];
        compare(playlist.count, 3);
        compare(fixed.count, 3);

        mouseClick(fixed);
        tryCompare(page, "showingRecordings", true);
        compare(page.shownPlaylist, "");
        compare(titles(page), "Session-0002.wav|REC001.WAV|Session-0001.wav");
        compare(fixed.isCurrent, true);
        compare(playlist.isCurrent, false);
        compare(realAppSettings.lastPlaylistName, "Own Recordings", "the fixed row is not remembered as a playlist");

        // Read again with the playlist picked last: the playlist, not the row.
        page.selectPlaylist(0, "");
        realAppSettings.lastPlaylistName = "Own Recordings";
        page.rescan();
        tryVerify(() => !scan.busy && !scan.cuesPending, 10000);
        tryCompare(page, "shownPlaylist", "Own Recordings");
        compare(page.showingRecordings, false);
        compare(page.selectedPlaylistIndex, scan.playlistNames.indexOf("Own Recordings") + 1);
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
}
