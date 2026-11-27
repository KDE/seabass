// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// Sync's Beat Grids & Waveforms page with the real controller, on scratch
// copies of two fixtures:
//
// - TESTSHARK after the Prime Go+ (tests/fixtures/testshark_t73,
//   m-after-R1b.db, put together as tests/assemble_t73_stick.cmake does):
//   11 tracks in both libraries, 5 analysed by a player (B04, B11, N1, N2,
//   N3), 6 not; rekordbox has a grid and a colour waveform for every one,
//   and the audio heads give each file's rate, so the 6 can be completed.
//   Playlists "T73 Renamed" (B01 to B04, B12), "T73 Order" (B07, B11, B12,
//   B08) and "T73 Folder/T73 New" (N1 to N3): B12 is in two. The numbers
//   are the command line's cli_copy_analysis_* tests' on the same stick.
// - the anonymized library: 1161 tracks in both, 188 analysed, no
//   three-band or colour waveform in its export.
//
// Ids below are rekordbox's (the page ticks rekordbox tracks).
TestCase {
    id: testCase
    name: "BeatGridsWaveformsPage"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    // rekordbox ids on the TESTSHARK stick.
    readonly property string b01: "9"     // not analysed
    readonly property string b02: "10"    // not analysed
    readonly property string b04: "12"    // analysed
    readonly property string b07: "1"     // not analysed
    readonly property string b12: "6"     // not analysed, in two playlists
    readonly property string n1: "13"     // analysed

    // The player a row's waveform loads its track into, as Main.qml hands
    // the page one; a test that opens the zoomed popup passes it.
    // Bound as Main.qml binds the app's one player: nothing is read
    // while a library is being written (see tst_CleanupPage).
    PlaybackController {
        id: realPlayback
        libraryBusy: EditSessionRegistry.anyWriting
    }

    function fixture(name) {
        const url = Qt.resolvedUrl("../fixtures/" + name).toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }
    function t73() {
        const stick = stickFixture.t73StickCopy(fixture("testshark_t73"), "m-after-R1b.db");
        verify(stick.length > 0, "the TESTSHARK stick must assemble");
        return stick;
    }

    Component {
        id: pageComponent
        BeatGridsWaveformsPage {
            width: 1080
            height: 800
            stickLabel: "TESTSHARK"
        }
    }

    function init() {
        failOnWarning(/TypeError/);
        failOnWarning(/ReferenceError/);
        failOnWarning(/Cannot read property/);
        failOnWarning(/Unable to assign/);
    }

    // The page's playlist combo last opened (openPicker()), so clickOn()
    // can shut it before a click elsewhere.
    property var openedPicker: null

    function cleanup() {
        testCase.openedPicker = null;
        EditSessionRegistry.discardAll();
        SystemFontMetrics.generalPointSizeOverride = 0;
    }

    // A page on a stick copy, with no session left over from an earlier
    // test (one filesystem, one library id for every copy).
    function openOn(stick, properties) {
        const libraryId = EditSessionRegistry.libraryIdForPath(stick + "/PIONEER");
        verify(libraryId.length > 0);
        if (EditSessionRegistry.hasSession(libraryId)) {
            EditSessionRegistry.closeSession(libraryId);
        }
        const page = createTemporaryObject(pageComponent, testCase, Object.assign({
            rekordboxPath: stick + "/PIONEER",
            enginePath: stick + "/Engine Library",
        }, properties || {}));
        verify(page !== null, "the page must instantiate");
        tryVerify(() => page.controller.loaded && !page.controller.loading, 120000, "the libraries are read");
        compare(page.controller.errorMessage, "");
        return {page: page, c: page.controller, libraryId: libraryId};
    }

    // Every row shown and every ticked track has its state.
    function settled(c) {
        if (c.loading || c.previewBusy || c.tickedPendingCount > 0) {
            return false;
        }
        for (let i = 0; i < c.tracks.count; ++i) {
            if (c.tracks.get(i).pending) {
                return false;
            }
        }
        return true;
    }
    function waitSettled(c, why) {
        tryVerify(() => settled(c), 120000, (why || "every state is worked out") + " " + JSON.stringify({
            loading: c.loading, previewBusy: c.previewBusy, pending: c.tickedPendingCount,
            error: c.errorMessage, first: c.tracks.count > 0 ? c.tracks.get(0) : null}));
    }

    function row(c, id) {
        const i = c.tracks.indexOf(id);
        verify(i >= 0, "track " + id + " is listed");
        return c.tracks.get(i);
    }

    // A click where the item is drawn once the page is laid out. The page
    // lays itself out again when its read lands (the header grows with the
    // options it then shows, and the lists move down under it), and that
    // waits for the next polish: a position read before it is where the
    // item was, and on a loaded Mac the click landed on nothing there,
    // the box ticking no track.
    //
    // The item and every layout above it, not the whole window: the
    // layouts that move it are its ancestors, while the window also holds
    // the saved toast, whose countdown mark asks for a polish every frame
    // for its six seconds. Waiting on the window then timed out after a
    // save whenever frames came slowly (a loaded suite, CI), with nothing
    // under the click still moving.
    function clickOn(item) {
        verify(item !== null, "something to click");
        const ancestors = [];
        for (let p = item; p !== null && p !== undefined; p = p.parent) {
            ancestors.push(p);
        }
        // A combo left open after ticking playlists in it is shut first,
        // as a user does, when the click is for something outside it.
        if (testCase.openedPicker !== null && testCase.openedPicker.popup.visible
                && ancestors.indexOf(testCase.openedPicker.popup.contentItem) < 0) {
            testCase.openedPicker.popup.close();
            tryVerify(() => !testCase.openedPicker.popup.visible, 5000, "the playlist combo closes");
        }
        // Until none of them is waiting, and the item has stayed where it
        // is over two looks (a sibling's polish can change its size and
        // move the item without any ancestor waiting yet): a parent's
        // polish may move a child and ask for its polish in turn.
        let last = "";
        tryVerify(() => {
            if (!ancestors.every((p) => !isPolishScheduled(p))) {
                last = "";
                return false;
            }
            const at = item.mapToItem(null, 0, 0);
            const here = at.x + "," + at.y + "," + item.width + "," + item.height;
            const still = here === last;
            last = here;
            return still;
        }, 5000, "the page is laid out before the click");
        mouseClick(item);
    }

    // The "Playlist:" combo (#115), opened.
    function openPicker(page) {
        const picker = findChild(page, "playlistPicker");
        verify(picker !== null, "the page has a playlist combo");
        testCase.openedPicker = picker;
        if (!picker.popup.opened) {
            clickOn(picker);
            tryVerify(() => picker.popup.opened, 5000, "the playlist combo opens");
        }
        return picker;
    }
    function closePicker(page) {
        const picker = findChild(page, "playlistPicker");
        if (picker.popup.visible) {
            picker.popup.close();
            tryVerify(() => !picker.popup.visible, 5000, "the playlist combo closes");
        }
    }
    // A playlist's row in the open combo ("All tracks" for ""), and its box.
    function playlistRow(page, name) {
        const list = openPicker(page).popup.contentItem;
        const wanted = name.length === 0 ? "All tracks" : name;
        // The popup builds its rows once it is open: on a loaded machine
        // that is later than the caller.
        let found = null;
        tryVerify(() => {
            for (let i = 0; i < list.count; ++i) {
                const item = list.itemAtIndex(i);
                if (item !== null && item.name === wanted) {
                    found = item;
                    return true;
                }
            }
            return false;
        }, 30000, "no playlist row " + wanted);
        return found;
    }
    function clickPlaylistBox(page, name) {
        const box = findChild(playlistRow(page, name), "playlistRowCheck");
        verify(box !== null && box.visible, "the playlist row has a box");
        clickOn(box);
    }
    function showPlaylist(page, name) {
        const picker = findChild(page, "playlistPicker");
        clickOn(findChild(playlistRow(page, name), "pickerRowName"));
        tryVerify(() => !picker.popup.visible, 5000, "picking a playlist closes the combo");
        compare(page.controller.shownPlaylist, name);
    }
    // A track's row delegate, built because it is in view.
    function trackDelegate(page, id) {
        const list = findChild(page, "trackList");
        for (let i = 0; i < list.contentItem.children.length; ++i) {
            const item = list.contentItem.children[i];
            if (item.objectName === "trackRow_" + id) {
                return item;
            }
        }
        return null;
    }
    // The same row, scrolled into view first and waited for. A row the
    // list built ahead of the view (its cache) is no row to click on, and
    // with taller rows (macOS's fonts) or a shorter window one asked for
    // may not be built at all.
    function delegateInView(page, id) {
        const list = findChild(page, "trackList");
        const index = page.controller.tracks.indexOf(id);
        verify(index >= 0, "track " + id + " is listed");
        list.positionViewAtIndex(index, ListView.Contain);
        verify(waitForPolish(page.Window.window));
        let found = null;
        tryVerify(() => {
            found = trackDelegate(page, id);
            if (found === null) {
                return false;
            }
            const top = found.mapToItem(list, 0, 0).y;
            return top >= 0 && top + found.height <= list.height;
        }, 5000, "track " + id + "'s row is in view");
        return found;
    }
    function shownBadges(delegate) {
        const shown = [];
        for (const name of ["gridMark", "waveformMark"]) {
            const badge = findChild(findChild(delegate, name), "stateBadge");
            shown.push(badge !== null && badge.visible ? badge.label : null);
        }
        return shown;
    }
    // Every row one shape, whatever its states say: the same height, the
    // beat grid's badge over the waveform's on two lines, both badges at
    // one x on every row, and the badges clear of the tempo.
    function verifyRowsAlike(page, ids, why) {
        let height = -1;
        let badgeX = -1;
        for (const id of ids) {
            const delegate = delegateInView(page, id);
            if (height < 0) {
                height = delegate.height;
            }
            compare(delegate.height, height, why + ": " + id + " is as tall as the first row");
            const grid = findChild(findChild(delegate, "gridMark"), "stateBadge");
            const waveform = findChild(findChild(delegate, "waveformMark"), "stateBadge");
            verify(grid.visible && waveform.visible, why + ": " + id + " shows both badges");
            const gridX = grid.mapToItem(delegate, 0, 0).x;
            if (badgeX < 0) {
                badgeX = gridX;
            }
            compare(gridX, badgeX, why + ": " + id + "'s grid badge (" + grid.label + ") lines up with the first row's");
            compare(waveform.mapToItem(delegate, 0, 0).x, badgeX,
                    why + ": " + id + "'s waveform badge (" + waveform.label + ") too");
            verify(waveform.mapToItem(delegate, 0, 0).y >= grid.mapToItem(delegate, 0, grid.height).y,
                   why + ": " + id + ": the waveform's line is under the grid's");
            const bpm = findChild(delegate, "bpmLabel");
            for (const badge of [grid, waveform]) {
                verify(badge.mapToItem(delegate, badge.width, 0).x <= bpm.mapToItem(delegate, 0, 0).x,
                       why + ": " + id + "'s " + badge.label + " stops before the tempo");
            }
        }
    }
    // A box's words a gap from Theme's scale off the box itself.
    function verifyBoxLabelGap(page, name) {
        const box = findChild(page, name);
        const indicator = findChild(box, "seabassCheckIndicator");
        const label = box.contentItem;
        const boxRight = indicator.mapToItem(page, indicator.width, 0).x;
        const textLeft = label.mapToItem(page, label.leftPadding, 0).x;
        verify(textLeft - boxRight >= Theme.tightSpacing,
               name + ": its words start " + (textLeft - boxRight) + " px after the box, under " + Theme.tightSpacing);
    }

    function finished(session, run, what) {
        let summary = null;
        const done = (result) => { summary = result; };
        session.saveFinished.connect(done);
        run();
        tryVerify(() => summary !== null, 300000, (what || "the save") + " finishes");
        session.saveFinished.disconnect(done);
        return summary;
    }

    // Nothing is copied that the user did not choose: nothing ticked, the
    // action off, the parts both on, the player's waveforms replaced and
    // its hand-edited grids not.
    function test_nothingTickedAtFirstAndTheActionIsOff() {
        const o = openOn(t73());
        waitSettled(o.c);
        compare(o.c.totalTrackCount, 11);
        compare(o.c.matchedCount, 11);
        compare(JSON.stringify(o.c.playlistNames), JSON.stringify(["T73 Folder/T73 New", "T73 Order", "T73 Renamed"]));
        compare(o.c.tickedCount, 0);
        for (let i = 0; i < o.c.tracks.count; ++i) {
            compare(o.c.tracks.get(i).ticked, false, "row " + i + " starts unticked");
        }
        compare(o.c.grids, true);
        compare(o.c.waveforms, true);
        compare(o.c.replaceEngineEdits, false);
        compare(o.c.replacePlayerWaveforms, true, "rekordbox's waveform replaces the player's by default");
        compare(findChild(o.page, "gridsCheckBox").checked, true);
        compare(findChild(o.page, "waveformsCheckBox").checked, true);
        compare(findChild(o.page, "overwriteCheckBox").checked, false);
        const button = findChild(o.page, "stageButton");
        compare(button.text, "Stage Selected");
        compare(button.enabled, false, "nothing ticked: nothing to stage");
        compare(o.c.canStage, false);
        compare(findChild(o.page, "summaryLabel").text,
                "Nothing is ticked. Tick a playlist or tracks to copy their beat grids and waveforms.");
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.Unchecked);
        compare(playlistRow(o.page, "").checkState, Qt.Unchecked);
        for (const name of ["errorLabel", "engineTooOldLabel", "readOnlyLabel", "ledgerWarningLabel",
                            "nothingMatchedLabel", "noPlaylistsLabel", "notInEngineLabel"]) {
            compare(findChild(o.page, name).visible, false, name + " says nothing here");
        }
    }

    // Ticking a playlist's box ticks its tracks, and the summary counts
    // what the controller's previews say of them: 5 grids and 5 waveforms,
    // 4 of the 5 completed (B04 is analysed already).
    // The header explains in one paragraph; the orange experimental note
    // under it is gone (the badge beside the title says it).
    function test_noExperimentalNote() {
        const o = openOn(t73());
        compare(findChild(o.page, "experimentalLabel"), null, "no note");
        verify(findChild(o.page, "introLabel").visible, "the intro stays");
    }

    function test_tickingAPlaylistTicksItsTracks() {
        const o = openOn(t73());
        waitSettled(o.c);
        compare(playlistRow(o.page, "T73 Renamed").checkTooltip,
                "No tracks of this playlist are ticked. Click to tick them all.");
        compare(playlistRow(o.page, "").checkTooltip, "No tracks are ticked. Click to tick them all.");
        clickPlaylistBox(o.page, "T73 Renamed");
        compare(o.c.tickedCount, 5);
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.Checked);
        compare(playlistRow(o.page, "").checkState, Qt.PartiallyChecked, "All tracks: some of it");
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked, "B12 is in T73 Order too");
        compare(playlistRow(o.page, "T73 Folder/T73 New").checkState, Qt.Unchecked);
        // Each box says in words what it shows.
        compare(playlistRow(o.page, "T73 Renamed").checkTooltip,
                "All tracks of this playlist are ticked. Click to untick them all.");
        compare(playlistRow(o.page, "").checkTooltip, "Some tracks are ticked. Click to tick them all.");
        compare(playlistRow(o.page, "T73 Order").checkTooltip,
                "Some tracks of this playlist are ticked. Click to tick them all.");
        compare(playlistRow(o.page, "T73 Folder/T73 New").checkTooltip,
                "No tracks of this playlist are ticked. Click to tick them all.");
        for (const id of [b01, b02, b04, b12]) {
            compare(row(o.c, id).ticked, true, id + " is ticked");
        }
        compare(row(o.c, b07).ticked, false);
        waitSettled(o.c);
        compare(o.c.gridsToCopy, 5);
        compare(o.c.waveformsToCopy, 5, "B04's waveform goes with its grid");
        compare(o.c.tracksToComplete, 4);
        compare(o.c.tracksToWrite, 5);
        compare(o.c.leftAloneCount, 0);
        compare(findChild(o.page, "summaryLabel").text,
                "5 tracks ticked: 5 beat grids and 5 waveforms to copy, 4 tracks then load without analysis.");
        compare(findChild(o.page, "leftAloneLabel").visible, false, "nothing of the player's kept");
        verify(findChild(o.page, "stageButton").enabled, "something to stage");

        // And off again, all of it.
        clickPlaylistBox(o.page, "T73 Renamed");
        compare(o.c.tickedCount, 0);
        compare(playlistRow(o.page, "").checkState, Qt.Unchecked);
        compare(findChild(o.page, "stageButton").enabled, false);
    }

    // The playlists are picked from a "Playlist:" combo, as on Sync Cue
    // Points and Clean Up Duplicates, with a box per playlist inside it
    // (#115): no column of playlists beside the tracks, which take the
    // page's whole width. The combo shows the playlist listed; a box ticks
    // its playlist's tracks and leaves the combo open for the next; the
    // label beside the closed combo counts the playlists ticked.
    function test_playlistsArePickedFromACombo() {
        const o = openOn(t73());
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        compare(findChild(o.page, "playlistList"), null, "no playlist column");
        const body = findChild(o.page, "pageBody");
        const list = findChild(o.page, "trackList");
        compare(list.mapToItem(body, 0, 0).x, 0, "the tracks start on the page's left line");
        compare(list.width, body.width, "and take its whole width");
        const picker = findChild(o.page, "playlistPicker");
        compare(findChild(o.page.header, "playlistPicker"), picker, "the combo is in the header");
        compare(picker.displayText, "All tracks");
        const ticked = findChild(o.page, "tickedPlaylistsLabel");
        compare(ticked.text, "");

        // Every row of the combo has its box, its count and its state.
        const rows = ["", "T73 Folder/T73 New", "T73 Order", "T73 Renamed"];
        for (const name of rows) {
            const row = playlistRow(o.page, name);
            const box = findChild(row, "playlistRowCheck");
            verify(box.visible && box.enabled, (name || "All tracks") + " has a box to tick");
            compare(row.checkState, Qt.Unchecked);
        }
        clickPlaylistBox(o.page, "T73 Renamed");
        compare(o.c.tickedCount, 5);
        verify(picker.popup.visible, "a box leaves the combo open, for the next playlist");
        compare(findChild(playlistRow(o.page, "T73 Renamed"), "playlistRowCheck").checkState, Qt.Checked,
                "the box shows its playlist ticked");
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked, "B12 is in T73 Order too");
        compare(findChild(playlistRow(o.page, "T73 Order"), "playlistRowCheck").checkState, Qt.PartiallyChecked);
        compare(playlistRow(o.page, "").checkState, Qt.PartiallyChecked);
        compare(o.c.shownPlaylist, "", "a box does not change the playlist listed");
        clickPlaylistBox(o.page, "T73 Folder/T73 New");
        compare(o.c.tickedCount, 8);
        closePicker(o.page);
        compare(ticked.text, "2 playlists ticked, 1 in part");

        // A name picks the playlist listed, and the closed combo says it.
        showPlaylist(o.page, "T73 Order");
        compare(picker.displayText, "T73 Order");
        compare(o.c.tracks.count, 4);
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked);
        closePicker(o.page);
        // Select All ticks the playlist listed, as its box does.
        clickOn(findChild(o.page, "selectAllButton"));
        compare(o.c.tickedCount, 11, "B07, B08 and B11 too: every track");
        compare(ticked.text, "3 playlists ticked");
        clickOn(findChild(o.page, "selectNoneButton"));
        compare(ticked.text, "");
        showPlaylist(o.page, "");
        compare(picker.displayText, "All tracks");
        compare(o.c.tracks.count, 11);

        // Staged, the boxes wait for the save; the names still pick.
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        clickOn(findChild(o.page, "stageButton"));
        verify(o.c.stagedCount > 0);
        verify(!findChild(playlistRow(o.page, "T73 Renamed"), "playlistRowCheck").enabled, "staged: no box ticks");
        compare(findChild(playlistRow(o.page, "T73 Renamed"), "playlistRowCheck").ToolTip.text,
                "Unstage to change what is copied", "and a box that is off says why");
        showPlaylist(o.page, "T73 Renamed");
        if (screenshotDir) {
            openPicker(o.page);
            wait(300);
            waitForRendering(o.page);
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-playlist-combo.png");
            closePicker(o.page);
        }
    }

    // A tick is the track's, not the row's: B12 ticked in one playlist is
    // ticked in the other, and survives switching between them.
    function test_aTrackInTwoPlaylistsIsOneTrack() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        compare(o.c.tracks.count, 5);
        verify(waitForPolish(o.page.Window.window));
        const delegate = delegateInView(o.page, b12);
        clickOn(findChild(delegate, "selectCheckBox"));
        compare(o.c.tickedCount, 1);
        compare(row(o.c, b12).ticked, true);

        showPlaylist(o.page, "T73 Order");
        compare(o.c.tracks.count, 4);
        compare(row(o.c, b12).ticked, true, "ticked in the other playlist too");
        compare(playlistRow(o.page, "T73 Order").checkState, Qt.PartiallyChecked);
        compare(playlistRow(o.page, "T73 Renamed").checkState, Qt.PartiallyChecked);
        o.c.setTicked(b12, false);
        showPlaylist(o.page, "T73 Renamed");
        compare(row(o.c, b12).ticked, false, "and unticked in both");
        compare(o.c.tickedCount, 0);
    }

    // What each kind of row reads, in the model and on screen: analysed,
    // not analysed, no beat grid in rekordbox (its .DAT taken off), locked
    // on the player.
    function test_perTrackStates() {
        const stick = t73();
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b04), "B04's grid file is taken off");
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b02), "B02's grid file is taken off");
        compare(stickFixture.lockAnalysedEngineBeatGrids(stick), 5, "the 5 analysed rows are locked");
        const o = openOn(stick);
        waitSettled(o.c);
        // N1: analysed, locked.
        compare(row(o.c, n1).gridState, "Locked on the player");
        compare(row(o.c, n1).waveformState, "Stays with the grid", "the player's waveform stays with its grid");
        compare(row(o.c, n1).note, "");
        // B01: not analysed, completed from rekordbox.
        compare(row(o.c, b01).gridState, "New");
        compare(row(o.c, b01).waveformState, "New");
        compare(row(o.c, b01).note, "", "the two columns say it: no note repeating them");
        // B04: analysed, no grid in rekordbox: Engine's grid stays, and
        // its waveform with it.
        compare(row(o.c, b04).gridState, "No beat grid in rekordbox");
        compare(row(o.c, b04).waveformState, "Stays with the grid");
        // B02: not analysed and no grid: cannot be completed.
        compare(row(o.c, b02).gridState, "No beat grid in rekordbox");
        compare(row(o.c, b02).waveformState, "Left for the player");
        compare(row(o.c, b02).note, "Not analysed by the player yet: rekordbox has no beat grid to complete it with");

        // On screen, the delegate says the same.
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        compare(JSON.stringify(shownBadges(trackDelegate(o.page, b01))), JSON.stringify(["New", "New"]));
        compare(findChild(trackDelegate(o.page, b01), "rowNote").text.trim(), "", "its line stays, empty");
        compare(JSON.stringify(shownBadges(trackDelegate(o.page, b02))),
                JSON.stringify(["No beat grid in rekordbox", "Left for the player"]));
        compare(findChild(trackDelegate(o.page, b04), "bpmLabel").text, "108 BPM");
        // The longest states the page shows, beside the shortest, at its
        // widest and at its narrowest: one shape, the badges in one column.
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "wide");
        o.page.width = 720;
        verify(waitForPolish(o.page.Window.window));
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "720 px");
        o.page.width = 1080;
        verify(waitForPolish(o.page.Window.window));

        // Ticked, the ones that would change nothing are said and grouped.
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 3, "B01, B03, B12 completed");
        compare(o.c.leftAloneCount, 2);
        compare(findChild(o.page, "leftAloneLabel").text,
                "2 ticked tracks are left alone: 1 left for the player, 1 no beat grid in rekordbox.");
    }

    // The box that replaces grids set on the player: off, a locked grid is
    // left; on, the states are worked out again and it is replaced.
    function test_alsoReplaceBox() {
        const stick = t73();
        compare(stickFixture.lockAnalysedEngineBeatGrids(stick), 5);
        const o = openOn(stick);
        clickPlaylistBox(o.page, "");
        compare(o.c.tickedCount, 11);
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Locked on the player");
        compare(o.c.gridsToCopy, 6, "only the 6 rows the player has not analysed");
        compare(o.c.waveformsToCopy, 6, "the 5 locked rows keep their waveform with their grid");
        compare(row(o.c, n1).waveformState, "Stays with the grid");
        compare(o.c.leftAloneCount, 5, "locked: nothing to copy");

        const box = findChild(o.page, "overwriteCheckBox");
        compare(box.checked, false, "off by default");
        clickOn(box);
        compare(o.c.replaceEngineEdits, true);
        compare(o.c.replacePlayerWaveforms, true, "the player's waveforms are replaced either way");
        compare(row(o.c, n1).pending, true, "worked out again");
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Replaces Engine's");
        compare(row(o.c, n1).waveformState, "Replaces Engine's", "the waveform goes with the grid");
        compare(o.c.gridsToCopy, 11);
        compare(o.c.waveformsToCopy, 11);

        clickOn(box);
        compare(o.c.replaceEngineEdits, false);
        waitSettled(o.c);
        compare(row(o.c, n1).gridState, "Locked on the player", "and back");
        compare(o.c.gridsToCopy, 6);
        compare(o.c.waveformsToCopy, 6);
    }

    // Select Unedited ticks every listed track but the ones whose grid is
    // locked or moved on the player; Select Unanalysed every one Engine
    // holds no grid or waveform for. Each ticks exactly those and unticks
    // the rest of the list, and is off where it would change nothing.
    function test_selectUneditedAndUnanalysed() {
        const stick = t73();
        compare(stickFixture.lockOneAnalysedEngineBeatGrid(stick), 1);
        const o = openOn(stick);
        waitSettled(o.c);
        const unedited = findChild(o.page, "selectUneditedButton");
        const unanalysed = findChild(o.page, "selectUnanalysedButton");
        compare(unedited.text, "Select Unedited");
        compare(unedited.ToolTip.text, "Select all but hand-edited (locked) tracks");
        compare(unanalysed.text, "Select Unanalysed");
        compare(unanalysed.ToolTip.text, "Select all tracks that have no beat grid or waveform on Engine");
        compare(o.c.selectionCount("unedited"), 10);
        compare(o.c.selectionCount("unanalysed"), 6);
        verify(unedited.enabled && unanalysed.enabled);

        clickOn(unedited);
        compare(o.c.tickedCount, 10, "all but the locked one");
        const locked = [];
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const r = o.c.tracks.get(i);
            if (!r.ticked) {
                locked.push(r.rekordboxId);
            }
        }
        compare(locked.length, 1);
        waitSettled(o.c);
        compare(row(o.c, locked[0]).gridState, "Locked on the player", "the one left out is the locked one");

        clickOn(unanalysed);
        compare(o.c.tickedCount, 6, "the others unticked");
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const r = o.c.tracks.get(i);
            compare(r.ticked, r.gridState === "New", r.rekordboxId);
        }
        compare(unanalysed.enabled, true, "enabled while it has tracks to tick");

        // In a listed playlist, only its tracks.
        clickOn(findChild(o.page, "selectNoneButton"));
        showPlaylist(o.page, "T73 Renamed");
        closePicker(o.page);
        waitSettled(o.c);
        clickOn(unanalysed);
        compare(o.c.tickedCount, 4, "the playlist's 4 the player has not analysed");

        // Once every playlist has been shown, switching runs no preview:
        // the buttons still follow the playlist shown. Some step must turn
        // a button on or off, or the check could not fail.
        const names = ["", "T73 Folder/T73 New", "T73 Order", "T73 Renamed"];
        for (const name of names) {
            showPlaylist(o.page, name);
            closePicker(o.page);
            waitSettled(o.c);
        }
        let flips = 0;
        let before = null;
        for (const name of names.concat(names)) {
            showPlaylist(o.page, name);
            closePicker(o.page);
            for (const [button, kind] of [[unedited, "unedited"], [unanalysed, "unanalysed"]]) {
                const count = o.c.selectionCount(kind);
                verify(count >= 0, name + ": every preview is in");
                compare(button.enabled, count > 0, (name || "All tracks") + ": " + kind + " follows the playlist shown");
            }
            const now = unedited.enabled + "/" + unanalysed.enabled;
            flips += before !== null && now !== before ? 1 : 0;
            before = now;
        }
        verify(flips > 0, "a switch turned a button on or off");
    }

    // The player's waveform is replaced by default, beside the grid that
    // replaces Engine's (rekordbox's analysis holds more). The box is for
    // locked and hand-edited grids only: there to tick while "Beat Grids"
    // is, and off with it.
    function test_overwriteBoxIsForHandEditedGrids() {
        const o = openOn(t73());
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        const box = findChild(o.page, "overwriteCheckBox");
        compare(box.text, "Overwrite Locked (Hand-Edited) Beat Grids");
        compare(box.checked, false, "off by default");
        compare(box.enabled, true, "Beat Grids is ticked");
        compare(row(o.c, b04).gridState, "Replaces Engine's");
        compare(row(o.c, b04).waveformState, "Replaces Engine's", "the player's waveform goes with its grid");
        compare(row(o.c, b01).waveformState, "New", "a row the player has not analysed gets rekordbox's");
        compare(o.c.waveformsToCopy, 5);

        clickOn(findChild(o.page, "waveformsCheckBox"));
        compare(o.c.waveforms, false);
        compare(box.enabled, true, "still there for the grids");
        clickOn(findChild(o.page, "waveformsCheckBox"));
        clickOn(findChild(o.page, "gridsCheckBox"));
        compare(o.c.grids, false);
        compare(box.enabled, false, "off without beat grids");
        // Waveforms alone: what was asked for, the player's replaced too.
        waitSettled(o.c);
        compare(row(o.c, b04).waveformState, "Replaces Engine's");
        clickOn(findChild(o.page, "gridsCheckBox"));
        compare(box.enabled, true, "and back with them");
    }

    // The save writes a grid and its waveform together: B04, the one
    // analysed track ticked, gets rekordbox's grid and rekordbox's waveform
    // in place of the player's, and nothing else on the stick changes.
    function test_saveWritesTheWaveformWithTheGrid() {
        const stick = t73();
        const overviewsBefore = stickFixture.engineOverviewDigests(stick);
        const gridsBefore = stickFixture.engineBeatDataDigests(stick);
        const o = openOn(stick);
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 1);
        compare(o.c.gridsToCopy, 1);
        compare(o.c.waveformsToCopy, 1);
        compare(findChild(o.page, "summaryLabel").text, "One track ticked: one beat grid and one waveform to copy.");
        compare(findChild(o.page, "leftAloneLabel").visible, false);

        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the save");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 1);
        compare(o.c.lastSaveGrids, 1);
        compare(o.c.lastSaveWaveforms, 1);
        const changedIn = (before, after) => Object.keys(before).filter((id) => after[id] !== before[id]).length;
        compare(changedIn(overviewsBefore, stickFixture.engineOverviewDigests(stick)), 1, "B04's overview, and only that");
        compare(changedIn(gridsBefore, stickFixture.engineBeatDataDigests(stick)), 1, "B04's grid, and only that");
    }

    // A locked row keeps its grid and, with it, its waveform: the save
    // leaves every overview byte for byte.
    function test_saveLeavesTheWaveformOfALockedGrid() {
        const stick = t73();
        compare(stickFixture.lockAnalysedEngineBeatGrids(stick), 5);
        const overviewsBefore = stickFixture.engineOverviewDigests(stick);
        const o = openOn(stick);
        o.c.setTicked(b04, true);
        o.c.setTicked(b01, true);
        waitSettled(o.c);
        compare(row(o.c, b04).gridState, "Locked on the player");
        compare(row(o.c, b04).waveformState, "Stays with the grid");
        compare(o.c.waveformsToCopy, 1, "only B01's, which the player has not analysed");
        compare(findChild(o.page, "leftAloneLabel").text,
                "One ticked track is left alone: 1 locked on the player.");
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the save");
        compare(saved.error, "", JSON.stringify(saved));
        const after = stickFixture.engineOverviewDigests(stick);
        let changed = 0;
        for (const id in overviewsBefore) {
            changed += after[id] !== overviewsBefore[id] ? 1 : 0;
        }
        compare(changed, 1, "B01's overview only: B04's stays with its locked grid");
    }

    // A row the player has not analysed is completed with both parts or
    // not at all: with waveforms off it is left for the player, and says
    // why, in the row and in the summary.
    function test_waveformsOffLeavesUnanalysedRowsForThePlayer() {
        const o = openOn(t73());
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        clickOn(findChild(o.page, "waveformsCheckBox"));
        compare(o.c.waveforms, false);
        waitSettled(o.c);
        compare(row(o.c, b01).gridState, "Left for the player");
        compare(row(o.c, b01).waveformState, "Not chosen");
        compare(row(o.c, b01).note, "Not analysed by the player yet: only completed with both beat grids and waveforms");
        compare(row(o.c, b04).gridState, "Replaces Engine's", "an analysed row still gets its grid");
        compare(row(o.c, b04).waveformState, "Not chosen");
        compare(o.c.gridsToCopy, 1);
        compare(o.c.waveformsToCopy, 0);
        compare(o.c.tracksToComplete, 0);
        compare(o.c.leftAloneCount, 4);
        compare(findChild(o.page, "summaryLabel").text, "5 tracks ticked: one beat grid to copy.");
        compare(findChild(o.page, "leftAloneLabel").text, "4 ticked tracks are left alone: 4 left for the player.");

        clickOn(findChild(o.page, "gridsCheckBox"));
        compare(o.c.grids, false);
        compare(findChild(o.page, "summaryLabel").text, "Choose beat grids, waveforms or both.");
        compare(findChild(o.page, "stageButton").enabled, false);
    }

    // Stage and Save on TESTSHARK: the database really holds the grids,
    // overviews and completed rows, the overview files are there, the
    // ledger is written, the toast says what landed, the page then finds
    // everything already the same. Undo puts the database back byte for
    // byte and takes the new overview files and the ledger away.
    function test_stageSaveThenAlreadyTheSameThenUndo() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        const before = stickFixture.sha256(db);
        const rowsBefore = stickFixture.engineAnalysisRows(stick);
        compare(rowsBefore.analysed, 5);
        compare(rowsBefore.overviewFiles, 0);
        const o = openOn(stick);
        // Every waveform, the player's too, by default.
        compare(o.c.replacePlayerWaveforms, true);
        clickPlaylistBox(o.page, "");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 11);
        compare(o.c.waveformsToCopy, 11);
        compare(o.c.tracksToComplete, 6);
        compare(stickFixture.sha256(db), before, "the previews wrote nothing");

        const button = findChild(o.page, "stageButton");
        verify(button.enabled);
        clickOn(button);
        compare(o.c.stagedCount, 11, "one change per track");
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        compare(session.pendingCount, 11);
        compare(session.editorOwner, "beat-grids");
        compare(button.text, "Unstage");
        compare(findChild(o.page, "stagedNote").visible, true);
        compare(findChild(o.page, "waveformsCheckBox").enabled, false, "what to copy waits for the save");
        compare(playlistRow(o.page, "").checkEnabled, false, "and so do the ticks");
        compare(stickFixture.sha256(db), before, "staging wrote nothing");

        const saved = finished(session, () => session.save(), "the save");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 11, JSON.stringify(saved));
        compare(saved.total, 11);
        compare(saved.skipped, 0);
        compare(saved.unit, "tracks");

        // What landed, read from the stick.
        const after = stickFixture.engineAnalysisRows(stick);
        compare(after.rows, 11);
        compare(after.analysed, 11, "the 6 unanalysed rows are completed");
        compare(after.gridsSet, 11);
        compare(after.overviews, 11);
        compare(after.overviewFiles, 11, "an overview file for every row");
        compare(after.matchingFiles, 11, "each over its row's own length");
        compare(stickFixture.keptEngineAnalyses(stick), 5, "the 5 analyses of the player kept first");
        const ledger = stickFixture.readText(stick + "/Seabass/beat-grids.tsv");
        const lines = ledger.split("\n").filter(line => line.length > 0);
        compare(lines[0], "seabass-beat-grids\t1");
        compare(lines.length, 1 + 11, "the ledger records every grid written");

        // The toast says what landed, counted from the changes that did.
        compare(o.c.lastSaveGrids, 11);
        compare(o.c.lastSaveWaveforms, 11);
        compare(o.c.lastSaveCompleted, 6);
        compare(o.c.lastSaveTracks, 11);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing, 5000, "the save is said with the toast");
        compare(toast.text, "11 beat grids and 11 waveforms copied, 6 tracks now load without analysis");
        compare(o.c.stagedCount, 0);

        // A second look finds it all there.
        waitSettled(o.c, "worked out again after the save");
        compare(o.c.tickedCount, 11, "the ticks stay");
        compare(o.c.tracksToWrite, 0);
        compare(o.c.leftAloneCount, 11);
        compare(findChild(o.page, "leftAloneLabel").text, "11 ticked tracks are left alone: 11 already the same.");
        for (const id of [b01, b04, n1]) {
            compare(row(o.c, id).gridState, "Already the same", id);
            compare(row(o.c, id).waveformState, "Already the same", id);
        }
        compare(button.text, "Stage Selected");
        compare(button.enabled, false, "nothing left to copy: the action is off");

        // Undo Last Save: the database back byte for byte, the files the
        // save made gone, the ledger with them.
        const undo = findChild(o.page, "undoLastSaveButton");
        tryVerify(() => undo.visible && undo.enabled, 5000);
        // The save's progress is modal, and a style's exit transition keeps
        // it over the page for a moment: the press goes in once it is gone.
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        const undone = finished(session, () => clickOn(undo), "the undo");
        compare(undone.error, "", JSON.stringify(undone));
        // Said as every page says it (#98), not as the summary's "1 undo
        // steps written".
        tryVerify(() => toast.showing && toast.text === "Changes undone.", 5000, toast.text);
        compare(stickFixture.sha256(db), before, "m.db is as it was");
        const restored = stickFixture.engineAnalysisRows(stick);
        compare(restored.analysed, 5);
        compare(restored.overviewFiles, 0, "the overview files the save made are gone");
        compare(stickFixture.fileExists(stick + "/Seabass/beat-grids.tsv"), false, "and the ledger it made");
        compare(stickFixture.keptEngineAnalyses(stick), 0, "and the kept analyses it made");
        waitSettled(o.c, "worked out again after the undo");
        compare(o.c.tracksToWrite, 11);
        compare(row(o.c, b01).gridState, "New");
    }

    // Copies T73 New's three rows (all analysed by a player), so their
    // analysis is kept, and leaves them ticked.
    function copyT73New(o) {
        clickPlaylistBox(o.page, "T73 Folder/T73 New");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 3);
        compare(o.c.keptTickedCount, 0, "nothing kept before a copy");
        compare(findChild(o.page, "restoreButton").enabled, false, "nothing to restore yet");
        compare(findChild(o.page, "letPlayerAnalyseButton").enabled, true, "three analysed rows");
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        waitSettled(o.c, "worked out again after the copy");
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        return session;
    }

    // Restore Engine's Analysis: the kept note on each row, the count, one
    // change per track, the analysis back and the entries gone; Undo puts
    // the copy back, entries and all.
    function test_restoreEngineAnalysisThenUndo() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        const original = stickFixture.sha256(db);
        const o = openOn(stick);
        const session = copyT73New(o);
        compare(stickFixture.keptEngineAnalyses(stick), 3);
        const copied = stickFixture.sha256(db);
        verify(copied !== original);
        compare(row(o.c, n1).note, "Engine's analysis kept", "the row says its analysis is kept");
        compare(o.c.keptTickedCount, 3);
        compare(o.c.restorableCount, 3);
        compare(findChild(o.page, "engineAnalysisLabel").text,
                "Engine's own analysis is kept for 3 ticked tracks, ready to be put back.");
        showPlaylist(o.page, "T73 Folder/T73 New");
        tryVerify(() => trackDelegate(o.page, n1) !== null
                  && findChild(trackDelegate(o.page, n1), "rowNote").text === "Engine's analysis kept", 5000,
                  "and on screen");
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-kept.png");
        }

        const restore = findChild(o.page, "restoreButton");
        compare(restore.text, "Restore Engine's Analysis");
        verify(restore.enabled);
        clickOn(restore);
        compare(o.c.stagedCount, 3, "one change per track");
        compare(o.c.stagedAction, "restore");
        compare(session.pendingCount, 3);
        compare(restore.enabled, false, "staged: the actions wait");
        compare(findChild(o.page, "letPlayerAnalyseButton").enabled, false);
        compare(findChild(o.page, "stageButton").text, "Unstage");
        compare(stickFixture.sha256(db), copied, "staging wrote nothing");
        const saved = finished(session, () => session.save(), "the restore");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        compare(o.c.lastSaveRestored, 3);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing && toast.text === "Engine's analysis restored for 3 tracks", 5000, toast.text);
        compare(stickFixture.keptEngineAnalyses(stick), 0, "the kept entries are used up");
        compare(stickFixture.engineAnalysisRows(stick).overviewFiles, 0, "the player had no overview files");
        waitSettled(o.c, "worked out again after the restore");
        compare(row(o.c, n1).note, "", "nothing kept any more");
        compare(row(o.c, n1).gridState, "Replaces Engine's", "the player's own grid again");
        compare(o.c.keptTickedCount, 0);

        // Undo: the copy as it was, the kept entries back.
        const undo = findChild(o.page, "undoLastSaveButton");
        tryVerify(() => undo.visible && undo.enabled, 5000);
        tryVerify(() => !findChild(o.page, "writeProgressDialog").visible, 10000, "the save's progress is gone");
        const undone = finished(session, () => clickOn(undo), "the undo");
        compare(undone.error, "", JSON.stringify(undone));
        compare(stickFixture.sha256(db), copied, "m.db is the copy's again");
        compare(stickFixture.keptEngineAnalyses(stick), 3, "the kept analyses are back");
        waitSettled(o.c, "worked out again after the undo");
        compare(row(o.c, n1).note, "Engine's analysis kept");
        compare(o.c.restorableCount, 3);
    }

    // Let the Player Analyse Again: the rows read as unanalysed after it,
    // their kept analysis stays.
    function test_letThePlayerAnalyseAgain() {
        const stick = t73();
        const o = openOn(stick);
        const session = copyT73New(o);
        const button = findChild(o.page, "letPlayerAnalyseButton");
        compare(button.text, "Let the Player Analyse Again");
        compare(o.c.analysedTickedCount, 3);
        clickOn(button);
        compare(o.c.stagedCount, 3);
        compare(o.c.stagedAction, "letPlayerAnalyse");
        const saved = finished(session, () => session.save(), "marking");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 3);
        compare(o.c.lastSaveMarked, 3);
        const toast = findChild(o.page, "savedToast");
        tryVerify(() => toast.showing && toast.text === "3 tracks left for the player to analyse", 5000, toast.text);
        compare(stickFixture.engineAnalysisRows(stick).analysed, 2, "B04 and B11 only");
        compare(stickFixture.keptEngineAnalyses(stick), 3, "what is kept stays");
        waitSettled(o.c, "worked out again");
        // As for a track the player has not analysed: completed from rekordbox.
        compare(row(o.c, n1).gridState, "New");
        compare(row(o.c, n1).waveformState, "New");
        compare(row(o.c, n1).note, "Engine's analysis kept");
        compare(o.c.analysedTickedCount, 0);
        compare(button.enabled, false, "nothing analysed left to mark");
        compare(o.c.restorableCount, 3, "and it can still be put back");
    }

    // A rekordbox track with no Engine row: the row says so, and the page
    // asks for the libraries to be synced first, with a link to Sync.
    function test_tracksNotInTheEngineLibrary() {
        const stick = t73();
        compare(stickFixture.removeEngineTracks(stick, [1, 2]), 2, "B07 and B08 leave the Engine library");
        const o = openOn(stick);
        waitSettled(o.c);
        compare(o.c.matchedCount, 9);
        compare(o.c.notInEngineShown, 2);
        const label = findChild(o.page, "notInEngineLabel");
        compare(label.visible, true);
        compare(label.text, "2 tracks here are not in the Engine library yet, so nothing can be copied to them. "
                + "Sync the libraries first with <a href=\"sync\">Sync after Rekordbox Export</a>, which also copies "
                + "their beat grids and waveforms as it adds them when both are switched on in Preferences, or come "
                + "back here once the libraries are in step.");
        compare(row(o.c, b07).matched, false);
        compare(row(o.c, b07).note, "Not in the Engine library yet: sync the libraries first");
        o.c.setTicked(b07, true);
        compare(o.c.tickedCount, 0, "it cannot be ticked");
        let asked = null;
        o.page.rekordboxExportSyncRequested.connect((stickLabel, rekordboxPath) => { asked = [stickLabel, rekordboxPath]; });
        label.linkActivated("sync");
        compare(JSON.stringify(asked), JSON.stringify(["TESTSHARK", stick + "/PIONEER"]), "the link asks for Sync");
        verify(waitForPolish(o.page.Window.window));
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-not-in-engine.png");
        }
        showPlaylist(o.page, "T73 Order");
        compare(o.c.notInEngineShown, 2, "B07 and B08 are in T73 Order");
        showPlaylist(o.page, "T73 Folder/T73 New");
        compare(o.c.notInEngineShown, 0);
        compare(label.visible, false, "a list with none says nothing");
        showPlaylist(o.page, "T73 Renamed");
        verify(waitForPolish(o.page.Window.window));
        compare(o.c.notInEngineText, "");
    }

    // What is staged holds no copy of rekordbox's analysis: the save reads
    // it again, and a track whose analysis changed in rekordbox since the
    // preview is skipped, saying so, never written with what the page did
    // not show.
    function test_aSaveWritesOnlyWhatThePreviewShowed() {
        const stick = t73();
        const o = openOn(stick);
        clickPlaylistBox(o.page, "T73 Folder/T73 New");
        waitSettled(o.c);
        compare(o.c.tracksToWrite, 3);
        clickOn(findChild(o.page, "stageButton"));
        compare(o.c.stagedCount, 3);
        verify(stickFixture.removeRekordboxBeatGridFile(stick, n1), "N1's beat grid leaves rekordbox's export");
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.skipped, 1, "N1 is skipped: " + JSON.stringify(saved));
        compare(stickFixture.keptEngineAnalyses(stick), 2, "and its row untouched, nothing kept of it");
        compare(o.c.lastSaveTracks, 2);
    }

    // Back from Sync after Rekordbox Export, whose save put the missing
    // tracks into the Engine library: the page reads the libraries again
    // when it is in front again (reloadIfStale(), StackView.onActivated),
    // and keeps the ticks of the tracks that can still be ticked. Without
    // another page's save it reads nothing again.
    function test_backFromAnotherPagesSaveReadsAgain() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        verify(stickFixture.copyFile(db, db + ".all"));
        compare(stickFixture.removeEngineTracks(stick, [1, 2]), 2, "B07 and B08 leave the Engine library");
        const o = openOn(stick);
        waitSettled(o.c);
        compare(o.c.notInEngineShown, 2);
        o.c.setTicked(b04, true);
        o.c.setTicked(b12, true);
        compare(o.c.tickedCount, 2);
        o.c.reloadIfStale();
        compare(o.c.loading, false, "nothing saved meanwhile: nothing read again");

        // Another page's save: the rows are back in the Engine library.
        verify(stickFixture.moveFile(db + ".all", db));
        verify(stickFixture.touch(db));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        session.saveFinished({written: 2, total: 2, unit: "tracks", verb: "synced", cancelled: false, error: "",
                              skipped: 0});
        waitSettled(o.c);
        compare(o.c.notInEngineShown, 2, "not read again behind another page");
        o.c.reloadIfStale();
        tryVerify(() => o.c.loaded && !o.c.loading && o.c.notInEngineShown === 0, 120000,
                  "read again: every track is in both libraries " + o.c.notInEngineShown);
        compare(o.c.matchedCount, 11);
        compare(row(o.c, b07).matched, true);
        compare(o.c.tickedCount, 2, "the ticks stay");
        verify(o.c.isTicked(b04) && o.c.isTicked(b12));
        waitSettled(o.c);
        compare(findChild(o.page, "notInEngineLabel").visible, false);
    }

    // A lost ledger: the ticked tracks already the same go into the next
    // copy's record with the ones it copies, as the command line records
    // them, so the grids Seabass wrote are not taken for the player's.
    function test_aLostLedgerLearnsTheTracksAlreadyTheSame() {
        const stick = t73();
        const o = openOn(stick);
        copyT73New(o);
        const ledgerFile = stick + "/Seabass/beat-grids.tsv";
        const ledgerLines = () => stickFixture.readText(ledgerFile).split("\n").filter(line => line.length > 0);
        compare(ledgerLines().length, 1 + 3);
        verify(stickFixture.moveFile(ledgerFile, ledgerFile + ".lost"));
        verify(o.c.ledgerWarning.length === 0, "a missing ledger is no warning");
        clickPlaylistBox(o.page, "");
        waitSettled(o.c);
        compare(o.c.tickedCount, 11);
        compare(o.c.tracksToWrite, 8, "the 3 copied before are already the same");
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => session.save(), "the copy");
        compare(saved.error, "", JSON.stringify(saved));
        compare(saved.written, 8);
        compare(ledgerLines().length, 1 + 11, "the record has the 8 copied and the 3 already the same");
    }

    // One left line at the narrowest width the page is laid out for, the
    // title and the row's marks never on top of each other, and a picture
    // of it with a playlist shown and some tracks ticked.
    function test_layoutAtTheNarrowestWidth() {
        const o = openOn(t73(), {width: 720, height: 760});
        showPlaylist(o.page, "T73 Renamed");
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        o.c.setTicked(b12, true);
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window), "the page is laid out");
        const body = findChild(o.page, "pageBody");
        const left = body.mapToItem(o.page, 0, 0).x;
        compare(left, Theme.pageMargin);
        for (const name of ["introLabel", "trackList", "breadcrumb"]) {
            compare(findChild(o.page, name).mapToItem(o.page, 0, 0).x, left, name + " is on the page's left line");
        }
        const box = findChild(findChild(o.page, "gridsCheckBox"), "seabassCheckIndicator");
        compare(box.mapToItem(o.page, 0, 0).x, left, "the first box itself is on the left line");
        for (const name of ["gridsCheckBox", "waveformsCheckBox", "overwriteCheckBox"]) {
            verifyBoxLabelGap(o.page, name);
        }
        verifyRowsAlike(o.page, [b01, b02, b04, b12], "720 px, three ticked");

        const list = findChild(o.page, "trackList");
        for (const id of [b01, b02, b04, b12]) {
            const delegate = delegateInView(o.page, id);
            const title = findChild(delegate, "rowTitle");
            const bpm = findChild(delegate, "bpmLabel");
            const titleRight = title.mapToItem(delegate, title.width, 0).x;
            verify(titleRight <= bpm.mapToItem(delegate, 0, 0).x,
                   id + ": the title (to " + titleRight + ") stops before the tempo");
            const marks = findChild(delegate, "rowDetailItems");
            const titleBottom = title.mapToItem(delegate, 0, title.height).y;
            verify(marks.mapToItem(delegate, 0, 0).y >= titleBottom, id + ": the marks are under the title");
            verify(marks.mapToItem(delegate, 0, marks.height).y <= delegate.height, id + ": inside the row");
            verify(delegate.mapToItem(list, delegate.width, 0).x <= list.width, id + ": the row fits the list");
        }
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage.png");
        }

        // At the narrowest width the three boxes still share one line, each
        // whole and inside the page.
        const options = findChild(o.page, "optionsRow");
        const names = ["gridsCheckBox", "waveformsCheckBox", "overwriteCheckBox"];
        const first = findChild(o.page, names[0]);
        for (let i = 1; i < names.length; ++i) {
            const box = findChild(o.page, names[i]);
            compare(box.y, first.y, names[i] + " is on the first box's line");
            verify(box.x + box.width <= options.width, names[i] + " fits the page");
            verifyBoxLabelGap(o.page, names[i]);
        }
    }

    // The state badges are bounded by their row's title column (`room`,
    // the Flow they sit in), and their widths in turn are part of what
    // that column asks the row for: a loop, if the row gave the column
    // less when it asked for less. It does not, and this says so: at the
    // narrowest width, on a Mac-sized font, each change of the page's
    // width moves `room` at most once, `room` is the title column's
    // width as the row laid it out, and nothing moves afterwards.
    function test_stateMarksSettleInOnePass_data() {
        return [
            {tag: "system font", pointSize: 0},
            {tag: "13pt", pointSize: 13},
        ];
    }
    function test_stateMarksSettleInOnePass(data) {
        SystemFontMetrics.generalPointSizeOverride = data.pointSize;
        const o = openOn(t73(), {width: 800, height: 760});
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        const delegate = delegateInView(o.page, b04);
        const marks = findChild(delegate, "stateMarks");
        const flow = findChild(delegate, "rowDetailItems");
        const column = flow.parent;
        const badge = findChild(findChild(delegate, "gridMark"), "stateBadge");
        verify(badge !== null && badge.visible, "B04 has a beat grid badge");
        let roomMoves = 0;
        let badgeMoves = 0;
        const onRoom = () => ++roomMoves;
        const onBadge = () => ++badgeMoves;
        marks.roomChanged.connect(onRoom);
        badge.widthChanged.connect(onBadge);
        try {
            for (const width of [720, 760, 720]) {
                roomMoves = 0;
                badgeMoves = 0;
                o.page.width = width;
                verify(waitForPolish(o.page.Window.window));
                waitForRendering(o.page);
                // Time for frames more, for anything a second pass would move.
                wait(100);
                verify(roomMoves <= 1, width + " px: room moved " + roomMoves + " times");
                verify(badgeMoves <= 1, width + " px: the badge's width moved " + badgeMoves + " times");
                compare(marks.room, flow.width, width + " px: room is the Flow's width");
                compare(flow.width, column.width, width + " px: and the Flow fills the title column");
                verify(badge.mapToItem(delegate, badge.width, 0).x <= column.mapToItem(delegate, column.width, 0).x + 0.5,
                       width + " px: the badge ends inside the title column");
            }
        } finally {
            marks.roomChanged.disconnect(onRoom);
            badge.widthChanged.disconnect(onBadge);
        }
    }

    // The anonymized library: its export has no waveform for the analysed
    // rows, and the preview of every track is timed for the report.
    function test_anonymizedLibraryHasNoWaveforms() {
        const stick = stickFixture.stickCopy(fixture("anonymized_library"));
        verify(stick.length > 0);
        const o = openOn(stick);
        compare(o.c.matchedCount, 1161);
        const started = Date.now();
        waitSettled(o.c, "every track of All tracks is worked out");
        console.log("Beat Grids & Waveforms: preview of " + o.c.tracks.count + " tracks took "
                    + (Date.now() - started) + " ms");
        let analysed = 0;
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const r = o.c.tracks.get(i);
            if (r.matched && r.gridState === "Replaces Engine's") {
                ++analysed;
                compare(r.waveformState, "No waveform in rekordbox", r.title);
            }
        }
        compare(analysed, 188);
    }

    // While the page works through the tracks, its bar says what it does
    // (#135): it converts rekordbox's waveforms and beat grids, to see what
    // syncing them would change. Only a preview runs under this bar (a
    // save has its own dialog), so it never says it is writing.
    function test_theProgressSaysWhatThePageDoes() {
        const stick = stickFixture.stickCopy(fixture("anonymized_library"));
        verify(stick.length > 0);
        const o = openOn(stick);
        const bar = findChild(o.page, "previewProgress");
        verify(bar !== null);
        // The 1161 tracks of All tracks take a while to work out.
        tryVerify(() => bar.active && bar.visible, 30000, "the bar shows while the tracks are worked out");
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-progress.png");
        }
        const wanted = "Converting waveform and beat grid data to check what syncing would change...";
        const words = findChild(bar, "currentItemLabel");
        verify(words.visible, "the words are on screen");
        compare(words.text, wanted);
        verify(!/copying|writ|saving/i.test(words.text), "a preview never reads as a write: " + words.text);
        waitSettled(o.c);

        // With one of the two parts ticked, the words name only that one.
        // Each box change works the tracks out again, under the same bar.
        clickOn(findChild(o.page, "gridsCheckBox"));
        compare(o.c.grids, false);
        tryVerify(() => bar.active && bar.visible, 30000, "the tracks are worked out again");
        compare(words.text, "Converting waveform data to check what syncing would change...");
        waitSettled(o.c);
        clickOn(findChild(o.page, "gridsCheckBox"));
        clickOn(findChild(o.page, "waveformsCheckBox"));
        compare(o.c.grids, true);
        compare(o.c.waveforms, false);
        tryVerify(() => bar.active && bar.visible, 30000, "the tracks are worked out again");
        compare(words.text, "Converting beat grid data to check what syncing would change...");
        // Done, it fades out rather than vanishing.
        tryVerify(() => !bar.active, 30000, "the preview is done");
        verify(bar.visible && bar.opacity > 0, "still fading: " + bar.opacity);
        tryVerify(() => !bar.visible, 3000, "then gone");
        waitSettled(o.c);
    }

    // Scrolling the tracks folds the header's explanation away (the
    // intro and the experimental note) and keeps what is used: the boxes,
    // the summary and its buttons. Back at the top of the list it unfolds
    // (#134; ScrollCollapse, as Clean Up Duplicates and Sync after
    // Rekordbox Export do).
    function test_scrollingFoldsTheIntro() {
        const o = openOn(t73(), {height: 560});
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        const list = findChild(o.page, "trackList");
        verify(list.contentHeight > list.height, "the tracks overflow the list: something to scroll");
        const intro = findChild(o.page.header, "introLabel");
        const details = findChild(o.page.header, "headerDetails");
        verify(intro.visible, "the intro shows before any scroll");
        const fullHeader = o.page.header.height;
        // A wheel step at a time, each waited for (see
        // tst_RekordboxExportSyncPage's test_scrollingFoldsTheHeader).
        for (let i = 0; i < 10 && (details === null || details.visible); ++i) {
            const before = list.contentY;
            mouseWheel(list, list.width / 2, list.height / 2, 0, -120);
            tryVerify(() => list.contentY > before || list.atYEnd, 2000, "a wheel step scrolls the list");
            tryVerify(() => !list.moving, 2000, "the wheel step settles");
        }
        if (screenshotDir) {
            wait(300);
            waitForRendering(o.page);
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-scrolled.png");
        }
        verify(details !== null, "the explanation is one block that folds");
        tryVerify(() => !details.visible, 2000, "scrolling folds the intro away");
        compare(intro.visible, false);
        verify(o.page.header.height <= fullHeader - intro.implicitHeight,
               "folded " + o.page.header.height + ", was " + fullHeader);
        compare(findChild(o.page.header, "summaryRow").visible, false,
                "nothing ticked: the summary's row is gone, gap and all");
        for (const name of ["breadcrumb", "optionsRow", "stageButton", "selectAllButton", "playlistPicker",
                            "summaryLabel"]) {
            const control = findChild(o.page.header, name);
            verify(control.visible, name + " stays");
            const top = control.mapToItem(o.page, 0, 0).y;
            verify(top >= 0 && top + control.height <= o.page.header.height + 0.5,
                   name + " is inside the folded header");
        }
        // The crumb's gap is the one-row header's, folded too.
        const crumb = findChild(o.page.header, "crumbRow");
        const controls = findChild(o.page.header, "controlsRow");
        compare(controls.mapToItem(o.page, 0, 0).y - crumb.mapToItem(o.page, 0, crumb.height).y, Theme.crumbGap,
                "the boxes' row sits the crumb's gap under it");

        list.positionViewAtBeginning();
        tryCompare(o.page.header, "height", fullHeader, 2000, "the header unfolds at the top");
        verify(details.visible && intro.visible);
        verify(findChild(o.page.header, "summaryLabel").visible, "what is ticked shows all along");
    }

    // The boxes, the playlist and the buttons on the ticks share one row
    // where they fit: the boxes on the page's left line, the actions on its
    // right. Where they do not, the actions go onto a line of their own
    // under the boxes, on the left line.
    function test_theControlsShareOneRow() {
        const o = openOn(t73(), {width: 1600});
        waitSettled(o.c);
        verify(waitForPolish(o.page.Window.window));
        const row = findChild(o.page.header, "controlsRow");
        compare(row.oneLine, true, "1600 px: one row");
        const middle = item => item.mapToItem(o.page, 0, item.height / 2).y;
        const grids = findChild(o.page.header, "gridsCheckBox");
        for (const name of ["playlistPicker", "stageButton"]) {
            const item = findChild(o.page.header, name);
            verify(Math.abs(middle(item) - middle(grids)) <= 1, name + " is on the boxes' row");
            verify(item.mapToItem(o.page, 0, 0).x > grids.mapToItem(o.page, grids.width, 0).x,
                   name + " is right of the boxes");
        }
        const stage = findChild(o.page.header, "stageButton");
        compare(stage.mapToItem(o.page, stage.width, 0).x, o.page.width - Theme.pageMargin,
                "the actions end on the page's right line");
        compare(findChild(o.page, "overwriteCheckBox").text, "Overwrite Locked (Hand-Edited) Beat Grids");
        // The selection helpers on a line of their own under the controls,
        // the first one's words on the page's left line.
        const helpers = findChild(o.page.header, "selectButtons");
        verify(helpers.mapToItem(o.page, 0, 0).y >= row.mapToItem(o.page, 0, row.height).y, "under the controls");
        const all = findChild(o.page.header, "selectAllButton");
        compare(all.mapToItem(o.page, all.leftPadding, 0).x, Theme.pageMargin, "Select All's words on the left line");
        for (const name of ["selectUneditedButton", "selectUnanalysedButton", "selectNoneButton"]) {
            const b = findChild(o.page.header, name);
            compare(b.parent, helpers, name + " among them");
            verify(b.flat, name + " is a helper, flat");
        }
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-one-row.png");
        }

        o.page.width = 720;
        verify(waitForPolish(o.page.Window.window));
        compare(row.oneLine, false, "720 px: the actions on a line of their own");
        const options = findChild(o.page.header, "optionsRow");
        const actions = findChild(o.page.header, "actionsRow");
        verify(actions.mapToItem(o.page, 0, 0).y >= options.mapToItem(o.page, 0, options.height).y,
               "the actions are under the boxes");
        compare(actions.mapToItem(o.page, 0, 0).x, Theme.pageMargin, "on the page's left line");
        verify(actions.mapToItem(o.page, actions.width, 0).x <= o.page.width - Theme.pageMargin + 0.5,
               "and inside the page");
        // Stage in the same place as on a wide page: at the right line.
        const stageNarrow = findChild(o.page.header, "stageButton");
        compare(stageNarrow.mapToItem(o.page, stageNarrow.width, 0).x, o.page.width - Theme.pageMargin,
                "Stage ends on the page's right line");
    }

    // The grey notes in the header say rather than offer: scrolling the
    // tracks folds them away with the explanation, and leaves the actions
    // beside them.
    function test_scrollingFoldsTheNotes() {
        const o = openOn(t73(), {height: 560});
        copyT73New(o);
        showPlaylist(o.page, "");
        waitSettled(o.c);
        // Waited for by what it shows, not by an idle layout: the save's
        // toast is still animating, and on a slow runner (Tumbleweed,
        // 2026-11-25) the page was not idle within waitForPolish's 5 s.
        const label = findChild(o.page.header, "engineAnalysisLabel");
        const restore = findChild(o.page.header, "restoreButton");
        tryVerify(() => label.visible && label.height > 0 && label.text.length > 0, 10000,
                  "the note shows before any scroll");
        verify(restore.visible);
        const list = findChild(o.page, "trackList");
        const details = findChild(o.page.header, "headerDetails");
        for (let i = 0; i < 10 && details.visible; ++i) {
            const before = list.contentY;
            mouseWheel(list, list.width / 2, list.height / 2, 0, -120);
            tryVerify(() => list.contentY > before || list.atYEnd, 2000, "a wheel step scrolls the list");
            tryVerify(() => !list.moving, 2000, "the wheel step settles");
        }
        tryVerify(() => !details.visible, 2000, "scrolling folds the intro away");
        compare(label.height, 0, "the note folds with it");
        compare(label.opacity, 0);
        compare(findChild(o.page.header, "leftAloneLabel").visible, false);
        verify(restore.visible, "the actions stay");
        const top = restore.mapToItem(o.page, 0, 0).y;
        verify(top >= 0 && top + restore.height <= o.page.header.height + 0.5, "inside the folded header");

        list.positionViewAtBeginning();
        tryVerify(() => details.visible && label.height > 0 && label.opacity === 1, 2000, "the note unfolds at the top");
    }

    // The edge states, each with its own sentence.
    function test_engine1xLibrary() {
        const stick = t73();
        // Engine 1.x keeps m.db at the library's root.
        verify(stickFixture.moveFile(stick + "/Engine Library/Database2/m.db", stick + "/Engine Library/m.db"));
        const o = openOn(stick);
        compare(o.c.engineTooOld, true);
        compare(findChild(o.page, "engineTooOldLabel").visible, true);
        compare(findChild(o.page, "playlistPicker").visible, false, "nothing to tick");
        compare(findChild(o.page, "trackList").visible, false);
        compare(findChild(o.page, "optionsRow").visible, false);
        compare(findChild(o.page, "summaryRow").visible, false);
    }

    function test_unreadableLedgerIsSaid() {
        const stick = t73();
        verify(stickFixture.makeDirectory(stick + "/Seabass"));
        verify(stickFixture.writeText(stick + "/Seabass/beat-grids.tsv", "not a ledger\n"));
        const o = openOn(stick);
        waitSettled(o.c);
        const label = findChild(o.page, "ledgerWarningLabel");
        compare(label.visible, true);
        verify(label.text.indexOf("Seabass's record of the beat grids it copied before cannot be read") === 0,
               label.text);
    }

    function test_readOnlyStickTicksNothing() {
        const o = openOn(t73(), {stickReadOnly: true});
        waitSettled(o.c);
        compare(findChild(o.page, "readOnlyLabel").visible, true);
        compare(playlistRow(o.page, "T73 Renamed").checkEnabled, false);
        compare(findChild(o.page, "gridsCheckBox").enabled, false);
        o.c.setPlaylistTicked("T73 Renamed", true);
        waitSettled(o.c);
        compare(findChild(o.page, "stageButton").enabled, false, "a read-only stick takes no copy");
        compare(findChild(playlistRow(o.page, "T73 Renamed"), "playlistRowCheck").ToolTip.text,
                "This stick is mounted read-only", "a playlist's box says why it is off");
    }

    // Each thing that turns a playlist's box off has its own sentence; a
    // playlist with nothing to tick says so first, staged or not, since
    // unstaging would not change it.
    function test_aPlaylistBoxSaysWhyItIsOff() {
        const o = openOn(t73());
        waitSettled(o.c);
        const nothing = "Nothing here to copy: only tracks in the Engine library can be ticked";
        compare(o.page.boxOffReason(0, false, "", false, false), nothing);
        compare(o.page.boxOffReason(0, false, "", true, false), nothing, "staged, still nothing here");
        compare(o.page.boxOffReason(0, true, "Sync Cue Points", true, true), nothing);
        compare(o.page.boxOffReason(3, true, "", true, false), "Saving changes: wait until it is done");
        compare(o.page.boxOffReason(3, false, "Sync Cue Points", false, false),
                "Unsaved changes on Sync Cue Points for this stick: save or discard them there first");
        compare(o.page.boxOffReason(3, false, "", true, false), "Unstage to change what is copied");
        compare(o.page.boxOffReason(3, false, "", false, true), "This stick is mounted read-only");
        compare(o.page.boxOffReason(3, false, "", false, false), "", "nothing turns it off: its own words");
        // Wired to the page: a box on, then off with a batch staged.
        const box = () => findChild(playlistRow(o.page, "T73 Renamed"), "playlistRowCheck");
        compare(box().ToolTip.text, "No tracks of this playlist are ticked. Click to tick them all.");
        clickPlaylistBox(o.page, "T73 Renamed");
        waitSettled(o.c);
        closePicker(o.page);
        clickOn(findChild(o.page, "stageButton"));
        verify(o.c.stagedCount > 0);
        compare(box().ToolTip.text, "Unstage to change what is copied");
        closePicker(o.page);
        clickOn(findChild(o.page, "stageButton"));
        compare(o.c.stagedCount, 0);
    }

    // ---- the rows' waveforms ---------------------------------------------

    function rowWaveformOf(delegate) {
        const view = findChild(delegate, "rowSourceWaveform");
        verify(view !== null, "the row has a waveform slot");
        return view;
    }
    // A waveform's columns as a string, to tell one track's from another's.
    function columnsKey(analysis) {
        if (!analysis || analysis.ready !== true || !analysis.columns) {
            return "";
        }
        return Array.from(new Uint8Array(analysis.columns)).join(",");
    }

    // A row shows rekordbox's waveform with its beat grid as lines; a row
    // whose track rekordbox has no analysis of (its .DAT taken off) shows
    // none and offers no click. The waveform is lower than the row's text,
    // so the row is no taller for it. A click on it opens the zoomed
    // popup with that track loaded, and ticks nothing.
    function test_rowWaveformAndItsPopup() {
        const stick = t73();
        verify(stickFixture.removeRekordboxBeatGridFile(stick, b02), "B02's analysis file is taken off");
        const o = openOn(stick, {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);

        const withRow = delegateInView(o.page, b01);
        const withView = rowWaveformOf(withRow);
        tryVerify(() => withView.visible, 30000, "B01's waveform is drawn");
        compare(withView.trackId, b01);
        verify(withView.byteColumns > 0, "with rekordbox's columns");
        compare(withView.gridStyle, "lines");
        verify(withView.beatTicks.length > 0, "and its beat grid's lines: " + withView.beatTicks.length);
        verify(withView.trackDurationMs > 0, "over the track's length");
        compare(withView.pointerShape, Qt.PointingHandCursor, "a hand: there is a file to play");

        const withoutRow = delegateInView(o.page, b02);
        const withoutView = rowWaveformOf(withoutRow);
        tryVerify(() => o.c.rowAnalysis(b02).ready === true, 30000, "B02's analysis is read");
        verify(waitForPolish(o.page.Window.window));
        compare(withoutView.visible, false, "no waveform for a track rekordbox has not analysed");
        compare(withoutView.opensZoom, false);
        compare(withoutView.pointerShape, Qt.ArrowCursor, "and no hand");

        // The rows keep one shape, and the waveform fits beside the text.
        compare(withRow.height, withoutRow.height, "a row with a waveform is as tall as one without");
        const slot = withView.parent;
        const textColumn = findChild(withRow, "rowTitle").parent;
        verify(slot.height <= textColumn.height,
               "the waveform (" + slot.height + ") is no taller than the row's text (" + textColumn.height + ")");

        // A click on B02's empty slot opens nothing.
        const popup = findChild(o.page, "waveformZoomPopup");
        verify(popup !== null, "the page has one popup");
        clickOn(withoutView.parent);
        compare(popup.visible, false, "nothing to open on a row without a waveform");
        compare(o.c.isTicked(b02), false);

        // A click on B01's opens the popup with B01 in the player.
        compare(o.c.isTicked(b01), false);
        const box = findChild(withRow, "selectCheckBox");
        clickOn(withView);
        tryCompare(popup, "opened", true, 10000, "the zoomed popup opens");
        compare(realPlayback.currentFormat, "rekordbox");
        compare(realPlayback.currentSourceId, b01, "with the clicked row's track");
        compare(realPlayback.currentLibraryPath, stick + "/PIONEER");
        compare(findChild(popup, "zoomTitle").text, row(o.c, b01).title);
        compare(o.c.isTicked(b01), false, "the click ticks nothing");
        compare(box.checked, false, "and the row's box stays as it was");
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    Component {
        id: opacityRecorder
        Connections {
            property var seen: []
            function onOpacityChanged() {
                seen.push(target.opacity);
            }
        }
    }

    // Puts an opacityRecorder on every row waveform the list makes.
    Component {
        id: rowWatcher
        Connections {
            property var views: []
            property var recorders: []
            function onChildrenChanged() {
                for (const child of target.children) {
                    const view = testCase.findChild(child, "rowSourceWaveform");
                    if (view !== null && views.indexOf(view) < 0) {
                        views.push(view);
                        recorders.push(testCase.createTemporaryObject(opacityRecorder, testCase, {target: view}));
                    }
                }
            }
        }
    }

    // A row's waveform is blended in when its read comes back, not
    // switched on: on its way to fully drawn it passes through values in
    // between. The row's answer is taken away and handed back, which is
    // what a read coming back does.
    function test_rowWaveformBlendsIn() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const trackRow = delegateInView(o.page, b01);
        const view = rowWaveformOf(trackRow);
        tryVerify(() => view.visible, 30000, "B01's waveform is drawn");
        tryCompare(view, "opacity", 1, 5000, "and fully");

        trackRow.analysis = ({ready: false});
        compare(view.visible, false, "no answer, no waveform");
        tryCompare(view, "opacity", 0, 5000);

        const recorder = createTemporaryObject(opacityRecorder, testCase, {target: view});
        trackRow.takeAnalysis();
        compare(view.visible, true, "the answer is back");
        tryCompare(view, "opacity", 1, 5000, "the waveform ends fully drawn");
        verify(recorder.seen.some(value => value > 0 && value < 1),
               "it got there by blending in: " + JSON.stringify(recorder.seen));
    }

    // Engine's waveform opens only where Engine's audio file is on the
    // stick, as rekordbox's does: no hand, no popup, for a file gone.
    function test_engineLaneWithoutItsFileOpensNothing() {
        const stick = t73();
        const o = openOn(stick, {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const file = o.c.engineZoomTrack(b04).filePath;
        verify(file.length > 0 && stickFixture.fileExists(file), "B04's file: " + file);
        verify(stickFixture.moveFile(file, file + ".gone"), "B04's file is taken off");

        const open = delegateInView(o.page, b04);
        clickOn(findChild(open, "rowTitle"));
        compare(open.expanded, true);
        const engine = findChild(open, "comparisonEngine");
        tryVerify(() => engine.byteColumns > 0, 30000, "Engine's waveform is drawn");
        compare(open.engineSide.playable, false);
        compare(engine.opensZoom, false, "but there is nothing to play");
        compare(engine.pointerShape, Qt.ArrowCursor, "and no hand");
    }

    // A row built again for a track whose waveform was read before (the
    // list built anew, a row scrolled back to) shows it at once: the fade
    // is for an answer arriving, not one already there.
    function test_aRowBuiltAgainShowsItsWaveformAtOnce() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const first = rowWaveformOf(delegateInView(o.page, b01));
        tryVerify(() => first.visible, 30000, "B01's waveform is drawn");
        tryCompare(first, "opacity", 1, 5000);

        // Every row's waveform is watched from the moment the list makes
        // the row, before it has taken anything.
        const list = findChild(o.page, "trackList");
        const watcher = createTemporaryObject(rowWatcher, testCase, {target: list.contentItem});
        showPlaylist(o.page, "");
        waitSettled(o.c);
        const trackRow = delegateInView(o.page, b01);
        const again = rowWaveformOf(trackRow);
        verify(again !== first, "every row is built anew");
        compare(again.visible, true, "B01's waveform, read before, is there");
        compare(again.opacity, 1);
        const at = watcher.views.indexOf(again);
        verify(at >= 0, "B01's new row was watched from its making");
        const seen = watcher.recorders[at].seen;
        verify(!seen.some(value => value > 0 && value < 1),
               "shown at once, not blended in: " + JSON.stringify(seen));
    }

    // The popup opened from a row, with the real player loading the track
    // as it opens (its length and detail arriving after): every tile in
    // view has its ruler lane on screen, opened and at every zoom
    // (Sebastian's screenshot 2026-11-21, a tile without its lane).
    function verifyPopupLanes(popup, why) {
        const a = findChild(popup.contentItem, "zoomArea");
        const s = findChild(popup.contentItem, "waveformStrip");
        const image = grabImage(testCase);
        const lane = popup.laneHeight;
        verify(lane >= 12, why + ": a lane, " + lane);
        const level = (x, y) => {
            const p = a.mapToItem(null, x, y);
            const q = testCase.mapFromItem(null, p.x, p.y);
            const c = PixelScale.pixel(image, testCase, q.x, q.y);
            return c.r + c.g + c.b;
        };
        const median = (values) => values.sort((x, y) => x - y)[Math.floor(values.length / 2)];
        let checked = 0;
        for (let i = 0; i < s.children.length; ++i) {
            const tile = s.children[i];
            if (tile.objectName !== "zoomTile") {
                continue;
            }
            // Only the track's own stretch has a lane: not before 0:00.
            const from = Math.max(0, Math.ceil(popup.stripX + tile.x), Math.ceil(popup.stripX));
            const to = Math.min(a.width, Math.floor(popup.stripX + tile.x + tile.width),
                                Math.floor(popup.stripX + popup.durationMs * popup.pxPerMs));
            if (to - from < 64) {
                continue;
            }
            const laneRow = [];
            const bandRow = [];
            for (let x = from + 1; x < to - 1; ++x) {
                laneRow.push(level(x, Math.round(lane / 2)));
                bandRow.push(level(x, lane + 2));
            }
            // The ground is the text colour faintly over the background:
            // lighter on a dark theme, darker on a light one.
            verify(tile.beatTicks.length > 0, why + ": tile " + tile.tile + " has its beat marks");
            const ground = Math.abs(median(laneRow) - median(bandRow));
            verify(ground > 0.06, why + ": tile " + tile.tile + " (x " + from + " to " + to
                   + ") has its lane's ground, " + ground.toFixed(3));
            ++checked;
        }
        verify(checked > 0, why + ": tiles in view");
    }
    function o_window() { return testCase.Window.window; }
    function test_thePopupFromARowHasEveryLane() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const view = rowWaveformOf(delegateInView(o.page, b04));
        tryVerify(() => view.opensZoom, 30000);
        const popup = findChild(o.page, "waveformZoomPopup");
        try {
            lanesOfAPopupFromARow(popup, view);
        } finally {
            popup.close();
            realPlayback.stop();
        }
    }
    function lanesOfAPopupFromARow(popup, view) {
        clickOn(view);
        tryCompare(popup, "opened", true, 10000);
        tryVerify(() => popup.durationMs > 0, 10000);
        wait(800);
        verifyPopupLanes(popup, "opened");
        for (const level of [0.1, 0.3, 0.5, 0.7, 0.9, 0.6, 0.2]) {
            popup.zoomToLevel(level);
            waitForPolish(o_window());
            wait(400);
            verifyPopupLanes(popup, "level " + level);
        }
        popup.close();
        tryCompare(popup, "visible", false);
    }

    // A row's read is the track's it was asked for and lands on that row
    // only: B01's read, under way when its row is replaced by another
    // playlist's B07 in the same place, is kept for B01 and never drawn on
    // B07's row. The other rows of the playlist left before their turn are
    // never read.
    // A page with T73 New shown and only its three rows read. Every track
    // is listed at first, and its row read: the libraries are read again
    // with only T73 New shown, which forgets what was read.
    function openOnT73NewOnly() {
        const o = openOn(t73());
        waitSettled(o.c);
        showPlaylist(o.page, "T73 Folder/T73 New");
        o.c.load(o.page.rekordboxPath, o.page.enginePath);
        tryVerify(() => o.c.loaded && !o.c.loading, 120000, "the libraries are read again");
        waitSettled(o.c);
        compare(o.c.shownPlaylist, "T73 Folder/T73 New");
        for (let i = 0; i < o.c.tracks.count; ++i) {
            const id = o.c.tracks.get(i).rekordboxId;
            tryVerify(() => o.c.rowAnalysis(id).ready === true, 30000, id + " is read");
        }
        return o;
    }
    function test_aLateReadLandsOnItsOwnTrackOnly() {
        const o = openOnT73NewOnly();
        compare(o.c.rowAnalysis(b01).ready, false, "B01 is not read yet");
        const list = findChild(o.page, "trackList");
        const ready = createTemporaryObject(signalSpyComponent, testCase,
                                            {target: o.c, signalName: "rowAnalysisReady"});
        // Built at once, B01 first, so its read starts first; then another
        // playlist before anything has landed.
        const reads = o.c.rowAnalysisReads;
        o.c.shownPlaylist = "T73 Renamed";
        verify(trackDelegate(o.page, b01) !== null, "B01's row is built at once");
        compare(o.c.rowAnalysisReads, reads + 1, "one read at a time: B01's");
        compare(ready.count, 0, "and nothing has landed yet");
        const leftEarly = [];
        for (let i = 1; i < o.c.tracks.count; ++i) {
            leftEarly.push(o.c.tracks.get(i).rekordboxId);
        }
        verify(leftEarly.length > 0);
        o.c.shownPlaylist = "T73 Order";
        compare(o.c.tracks.get(0).rekordboxId, b07, "B07 is first in T73 Order");
        const b07Row = trackDelegate(o.page, b07);
        verify(b07Row !== null, "B07's row is built at once, where B01's was");
        // What B07's row draws the moment B01's read lands (the rows take
        // an answer before this handler runs: they connected first).
        let atB01 = null;
        const onReady = (id) => {
            if (id === b01 && atB01 === null) {
                atB01 = {id: b07Row.analysisId, columns: columnsKey(b07Row.analysis)};
            }
        };
        o.c.rowAnalysisReady.connect(onReady);

        tryVerify(() => {
            for (let i = 0; i < ready.count; ++i) {
                if (ready.signalArguments[i][0] === b01) {
                    return true;
                }
            }
            return false;
        }, 30000, "B01's read lands after its row has gone");
        o.c.rowAnalysisReady.disconnect(onReady);
        verify(atB01 !== null);
        verify(columnsKey(o.c.rowAnalysis(b01)).length > 0, "B01 has a waveform");
        verify(atB01.columns !== columnsKey(o.c.rowAnalysis(b01)), "B01's columns never land on B07's row");
        verify(atB01.id !== b01, "nor is B07's row said to draw B01's");
        tryVerify(() => o.c.rowAnalysis(b07).ready === true, 30000, "B07's is read for its row");
        verify(waitForPolish(o.page.Window.window));
        const first = delegateInView(o.page, b07);
        const view = rowWaveformOf(first);
        tryVerify(() => view.visible, 10000, "B07's row draws its waveform");
        compare(view.trackId, b07, "B07's row draws B07's analysis");
        compare(columnsKey(first.analysis), columnsKey(o.c.rowAnalysis(b07)), "B07's columns");
        verify(columnsKey(first.analysis) !== columnsKey(o.c.rowAnalysis(b01)), "not B01's");
        for (let i = 0; i < list.contentItem.children.length; ++i) {
            const item = list.contentItem.children[i];
            if (item.analysisId !== undefined && item.analysisId.length > 0) {
                compare(item.analysisId, item.rekordboxId, item.objectName + " draws its own track's");
            }
        }
        for (const id of leftEarly) {
            if (o.c.tracks.indexOf(id) < 0) {
                compare(o.c.rowAnalysis(id).ready, false, "T73 Renamed's " + id + " left before its turn: never read");
            }
        }
    }

    // Nothing is read for a row while a library is being written: a row
    // that asks then waits, and is read once the write is over.
    function test_noRowReadWhileWriting() {
        const o = openOnT73NewOnly();
        compare(o.c.rowAnalysis(b07).ready, false, "B07 is not read yet");
        o.c.setTicked(n1, true);
        waitSettled(o.c);
        clickOn(findChild(o.page, "stageButton"));
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        let reads = -1;
        let asked = false;
        const saved = finished(session, () => {
            session.save();
            verify(EditSessionRegistry.anyWriting, "the save is writing");
            reads = o.c.rowAnalysisReads;
            // B07 is in no row shown: asked for as a row would.
            o.c.requestRowAnalysis(b07);
            asked = true;
            compare(o.c.rowAnalysisReads, reads, "no read starts while the library is written");
        }, "the copy");
        verify(asked);
        compare(saved.error, "", JSON.stringify(saved));
        tryVerify(() => o.c.rowAnalysis(b07).ready === true, 30000, "read once the write is over");
        verify(o.c.rowAnalysisReads > reads);
        o.c.releaseRowAnalysis(b07);
    }

    Component {
        id: signalSpyComponent
        SignalSpy {}
    }

    // ---- an opened row: rekordbox against Engine ---------------------------

    // A click on a row opens it, and the open row shows rekordbox's
    // waveform and grid over Engine's, and what differs in words. B04 was
    // analysed by the player, so Engine has a grid of its own to compare;
    // B01 was not. One row is open at a time, and Engine's side is read
    // only for the open one.
    function test_anOpenedRowComparesRekordboxWithEngine() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);

        const row = delegateInView(o.page, b04);
        compare(row.expanded, false);
        compare(findChild(row, "analysisComparison").visible, false, "closed, no comparison");
        compare(o.c.engineAnalysis(b04).ready, false, "and nothing of Engine's read for it");
        clickOn(findChild(row, "rowTitle"));
        compare(row.expanded, true, "a click opens the row");
        compare(o.c.isTicked(b04), false, "and ticks nothing");
        const comparison = findChild(row, "analysisComparison");
        tryVerify(() => comparison.visible, 5000, "the open row shows the comparison");

        const rekordbox = findChild(row, "comparisonRekordbox");
        const engine = findChild(row, "comparisonEngine");
        tryVerify(() => o.c.engineAnalysis(b04).ready === true, 30000, "Engine's side is read");
        tryVerify(() => rekordbox.byteColumns > 0 && engine.byteColumns > 0, 30000, "both waveforms are drawn");
        verify(rekordbox.beatTicks.length > 0, "rekordbox's grid");
        verify(engine.beatTicks.length > 0, "Engine's grid: the player analysed B04");
        compare(rekordbox.trackDurationMs, engine.trackDurationMs, "on one time axis");
        compare(rekordbox.width, engine.width);
        const e = o.c.engineAnalysis(b04);
        compare(e.hasEngineTrack, true);
        compare(e.hasGrid, true);
        verify(e.bpm > 60 && e.bpm < 200, "a tempo: " + e.bpm);
        // B04 is an MP3 with a 25 ms lead-in: Engine's grid is on the
        // player's timeline, rekordbox's is put there before comparing, so
        // only the tempo differs (the player analysed 108.00 BPM, rekordbox
        // 107.91), not the place of the beats.
        verify(e.rekordboxShiftMs < -20, "the lead-in moves rekordbox's side: " + e.rekordboxShiftMs);
        compare(o.c.rowGridFacts(b04).shiftMs, e.rekordboxShiftMs);
        compare(rekordbox.columnStartMs, e.rekordboxShiftMs, "its waveform moved with it");
        tryCompare(findChild(row, "comparisonSummary"), "text", "Engine's tempo is 0.09 BPM faster.");

        // Another row opened closes this one; B01 has no grid of Engine's.
        const other = delegateInView(o.page, b01);
        clickOn(findChild(other, "rowTitle"));
        compare(other.expanded, true);
        compare(row.expanded, false, "one row open at a time");
        tryVerify(() => o.c.engineAnalysis(b01).ready === true, 30000);
        compare(o.c.engineAnalysis(b01).hasEngineTrack, true);
        compare(o.c.engineAnalysis(b01).hasGrid, false, "the player never analysed B01");
        tryCompare(findChild(other, "comparisonSummary"), "text", "Engine has no beat grid for this track yet.");

        // Clicked again, it closes.
        clickOn(findChild(other, "rowTitle"));
        compare(other.expanded, false);
        compare(o.page.openTrackId, "");
    }

    // Each of the open row's waveforms opens the zoomed popup with its own
    // side's copy of the track: rekordbox's lane rekordbox's, Engine's lane
    // Engine's (its library, its track id, its file). Neither click closes
    // the row or ticks it.
    function test_theComparisonLanesOpenTheirOwnSide() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const open = delegateInView(o.page, b04);
        clickOn(findChild(open, "rowTitle"));
        compare(open.expanded, true);
        const rekordbox = findChild(open, "comparisonRekordbox");
        const engine = findChild(open, "comparisonEngine");
        tryVerify(() => rekordbox.opensZoom && engine.opensZoom, 30000, "both lanes can be opened");
        // The open row, taller now, scrolled to the top of the list (with
        // larger fonts it can be taller than the list), both lanes in it.
        const list = findChild(o.page, "trackList");
        list.positionViewAtIndex(o.page.controller.tracks.indexOf(b04), ListView.Beginning);
        verify(waitForPolish(o.page.Window.window));
        tryVerify(() => [rekordbox, engine].every((lane) => {
            const top = lane.mapToItem(list, 0, 0).y;
            return top >= 0 && top + lane.height <= list.height;
        }), 5000, "both lanes are in view");
        compare(rekordbox.pointerShape, Qt.PointingHandCursor);
        compare(engine.pointerShape, Qt.PointingHandCursor);
        const popup = findChild(o.page, "waveformZoomPopup");

        clickOn(rekordbox);
        tryCompare(popup, "opened", true, 10000, "rekordbox's lane opens the popup");
        compare(realPlayback.currentFormat, "rekordbox");
        compare(realPlayback.currentSourceId, b04);
        popup.close();
        tryCompare(popup, "visible", false);

        clickOn(engine);
        tryCompare(popup, "opened", true, 10000, "Engine's lane opens the popup");
        compare(realPlayback.currentFormat, "engine", "with Engine's copy of the track");
        const engineTrack = o.c.engineZoomTrack(b04);
        compare(realPlayback.currentLibraryPath, engineTrack.libraryPath);
        compare(realPlayback.currentSourceId, engineTrack.sourceId);
        verify(realPlayback.currentLibraryPath.endsWith("/Engine Library"),
               "from the Engine library: " + realPlayback.currentLibraryPath);
        verify(engineTrack.sourceId.length > 0, "Engine's own id");
        compare(findChild(popup, "zoomTitle").text, row(o.c, b04).title);
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();

        compare(open.expanded, true, "the row stays open");
        compare(o.c.isTicked(b04), false, "and nothing is ticked");
        compare(Object.keys(o.c.engineZoomTrack("no such track")).length, 0, "an unknown track opens nothing");
    }

    // The zoomed popup loads its track into the player, whose bar takes
    // artworkPath and fallbackArtworkPath as URLs: a bare path is
    // resolved against the page's qrc: base and the cover stays empty
    // (macOS: "qrc:/Volumes/<stick>/PIONEER/Artwork/..."). Both sides, each
    // with its own cover first and the other's as the fallback; paths with
    // a space and a non-ASCII letter; no cover stays empty.
    function fileUrlNames(url, path, what) {
        verify(url.startsWith("file:///"), what + " is a file URL: " + url);
        compare(decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1")), path, what);
    }

    function test_zoomTracksCarryCoversAsFileUrls() {
        const stick = t73();
        const rekordboxCover = stick + "/Mörk Borg/a b.jpg";
        const engineCover = stick + "/Mörk Borg/Engine ö.png";
        catalogGate.artworkOnEveryRead(rekordboxCover, engineCover);
        try {
            const o = openOn(stick);
            const rekordbox = o.c.zoomTrack(b04);
            fileUrlNames(rekordbox.artworkPath, rekordboxCover, "rekordbox's cover");
            fileUrlNames(rekordbox.fallbackArtworkPath, engineCover, "and Engine's as its fallback");
            const engine = o.c.engineZoomTrack(b04);
            verify(engine.sourceId.length > 0, "Engine has the track");
            fileUrlNames(engine.artworkPath, engineCover, "Engine's cover");
            fileUrlNames(engine.fallbackArtworkPath, rekordboxCover, "and rekordbox's as its fallback");
        } finally {
            catalogGate.restore();
        }
    }

    function test_zoomTracksWithoutCoversStayEmpty() {
        const o = openOn(t73());
        for (const track of [o.c.zoomTrack(b04), o.c.engineZoomTrack(b04)]) {
            compare(track.artworkPath, "", track.format + ": no cover is no URL");
            compare(track.fallbackArtworkPath, "", track.format + ": nor a fallback");
        }
    }

    // A delegate built for the row that is already open (scrolled back
    // into view, or the list built again after a read) starts expanded:
    // it still asks for Engine's side and shows the comparison.
    function test_aRowBuiltOpenStillCompares() {
        const o = openOn(t73());
        o.page.openTrackId = b04;
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const row = delegateInView(o.page, b04);
        compare(row.expanded, true, "built open");
        tryVerify(() => o.c.engineAnalysis(b04).ready === true, 30000, "Engine's side is asked for and read");
        tryCompare(findChild(row, "comparisonSummary"), "text", "Engine's tempo is 0.09 BPM faster.");
    }

    // What the summary says, for each way two grids can differ.
    function test_theComparisonInWords() {
        const o = openOn(t73());
        const p = o.page;
        const grid = (bpm, downbeat) => ({ready: true, hasGrid: true, bpm: bpm, firstDownbeatMs: downbeat});
        const engine = (bpm, downbeat) => Object.assign(grid(bpm, downbeat), {hasEngineTrack: true});
        compare(p.comparisonText(true, grid(128, 100), engine(128, 100)), "The two beat grids agree.");
        compare(p.comparisonText(true, grid(128, 100), engine(128, 100.4)), "The two beat grids agree.",
                "under a millisecond is the same place");
        compare(p.comparisonText(true, grid(128, 100), engine(128, 112)), "Engine's beats fall 12 ms later.");
        compare(p.comparisonText(true, grid(128, 100), engine(128, 88)), "Engine's beats fall 12 ms earlier.");
        // A whole bar apart is the same lines in the same places.
        compare(p.comparisonText(true, grid(120, 100), engine(120, 2100)), "The two beat grids agree.");
        // One beat apart: the same lines, the bars counted from another one.
        compare(p.comparisonText(true, grid(120, 100), engine(120, 600)), "Engine's bars start on another beat.");
        compare(p.comparisonText(true, grid(128, 100), engine(128.5, 100)), "Engine's tempo is 0.50 BPM faster.");
        compare(p.comparisonText(true, grid(128, 100), engine(127.9, 110)),
                "Engine's tempo is 0.10 BPM slower; Engine's beats fall 10 ms later.");
        compare(p.comparisonText(true, grid(128, 100), {ready: true, hasEngineTrack: true, hasGrid: false}),
                "Engine has no beat grid for this track yet.");
        compare(p.comparisonText(true, {ready: true, hasGrid: false}, engine(128, 100)),
                "rekordbox has no beat grid for this track; Engine has one.");
        compare(p.comparisonText(true, grid(128, 100), {ready: true, hasEngineTrack: false}),
                "Not in the Engine library yet: there is nothing to compare with.");
        compare(p.comparisonText(false, grid(128, 100), {ready: false}),
                "Not in the Engine library yet: there is nothing to compare with.");
        compare(p.comparisonText(true, grid(128, 100), {ready: false}), "Reading both analyses...");
        compare(p.comparisonText(true, grid(128, 100), {ready: true, readFailed: true}),
                "Engine's analysis of this track could not be read. Is the stick still there?");
        compare(p.comparisonText(true, grid(128, 100), Object.assign(engine(128.5, 112), {shiftUnknown: true})),
                "Engine's tempo is 0.50 BPM faster; where the beats fall is not compared: "
                + "this MP3's lead-in could not be read.");
        compare(p.comparisonText(true, grid(128, 100), Object.assign(engine(128, 112), {shiftUnknown: true})),
                "Where the beats fall is not compared: this MP3's lead-in could not be read.");
    }

    // Select All beside Stage Selected ticks every row the list shows, as
    // the playlist's own box does; Select None unticks every ticked track.
    // Each is off where it would do nothing, and both are off while a
    // batch is staged, as the row boxes are.
    function test_selectAllAndSelectNoneButtons() {
        const o = openOn(t73());
        waitSettled(o.c);
        const all = findChild(o.page, "selectAllButton");
        const none = findChild(o.page, "selectNoneButton");
        verify(all !== null && all.visible, "a Select All button");
        verify(none !== null && none.visible, "a Select None button");
        compare(all.text, "Select All");
        compare(none.text, "Select None");
        const stage = findChild(o.page, "stageButton");
        compare(all.parent.objectName, "selectButtons", "among the selection helpers");
        compare(none.parent, all.parent);
        compare(all.enabled, true);
        compare(none.enabled, false, "nothing ticked: nothing to untick");

        // A playlist shown: its rows, and only those.
        showPlaylist(o.page, "T73 Renamed");
        clickOn(all);
        compare(o.c.tickedCount, 5);
        for (let i = 0; i < o.c.tracks.count; ++i) {
            compare(o.c.tracks.get(i).ticked, true, "row " + i + " is ticked");
        }
        compare(o.c.isTicked(b07), false, "a track of another playlist stays as it was");
        compare(all.enabled, false, "every row ticked: nothing more to tick");
        compare(none.enabled, true);
        clickOn(none);
        compare(o.c.tickedCount, 0);
        for (let i = 0; i < o.c.tracks.count; ++i) {
            compare(o.c.tracks.get(i).ticked, false, "row " + i + " is unticked");
        }
        compare(none.enabled, false);
        compare(all.enabled, true);

        // Every track, then staged: neither changes a tick.
        showPlaylist(o.page, "");
        clickOn(all);
        compare(o.c.tickedCount, 11);
        compare(playlistRow(o.page, "").checkState, Qt.Checked, "the same as All tracks' box");
        waitSettled(o.c);
        verify(stage.enabled);
        clickOn(stage);
        verify(o.c.stagedCount > 0, "staged");
        compare(all.enabled, false, "staged: Select All waits for the save");
        compare(none.enabled, false, "staged: Select None waits for the save");
        clickOn(stage);
        compare(o.c.stagedCount, 0);
        compare(none.enabled, true, "unstaged: ticks can change again");
        clickOn(none);
        compare(o.c.tickedCount, 0);
    }

    // Select All follows what can be ticked when the stick is read again,
    // also when no tick moves: two tracks back in the Engine library make
    // it worth pressing again.
    function test_selectAllAfterTheStickIsReadAgain() {
        const stick = t73();
        const db = stick + "/Engine Library/Database2/m.db";
        verify(stickFixture.copyFile(db, db + ".all"));
        compare(stickFixture.removeEngineTracks(stick, [1, 2]), 2, "B07 and B08 leave the Engine library");
        const o = openOn(stick);
        waitSettled(o.c);
        const all = findChild(o.page, "selectAllButton");
        compare(o.c.shownPlaylist, "");
        clickOn(all);
        compare(o.c.tickedCount, 9, "every track that can be ticked");
        compare(all.enabled, false, "nothing more to tick");
        const revision = o.c.tickRevision;

        verify(stickFixture.copyFile(db + ".all", db), "B07 and B08 are back");
        o.c.load(o.page.rekordboxPath, o.page.enginePath);
        tryVerify(() => o.c.loaded && !o.c.loading, 120000, "the libraries are read again");
        waitSettled(o.c);
        compare(o.c.matchedCount, 11);
        compare(o.c.tickRevision, revision, "no tick moved");
        compare(o.c.tickedCount, 9);
        compare(all.enabled, true, "two more tracks to tick");
        clickOn(all);
        compare(o.c.tickedCount, 11);
    }

    // The open row draws Engine's side as Engine does, in Engine's
    // colours; rekordbox's side and the row's small waveform keep
    // rekordbox's look.
    function test_theEngineLaneHasEnginesLook() {
        const o = openOn(t73());
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const row = delegateInView(o.page, b04);
        clickOn(findChild(row, "rowTitle"));
        compare(row.expanded, true);
        const rekordbox = findChild(row, "comparisonRekordbox");
        const engine = findChild(row, "comparisonEngine");
        tryVerify(() => rekordbox.byteColumns > 0 && engine.byteColumns > 0, 30000, "both waveforms are drawn");
        compare(engine.look, "engine");
        compare(engine.unplayedOpacity, 1, "at full strength, as the popup draws Engine");
        // The fixture's analysis files have no 3Band data: rekordbox's Blue.
        compare(rekordbox.look, "rekordbox-blue", "rekordbox's lane");
        compare(findChild(row, "rowSourceWaveform").look, "rekordbox-blue", "the row's own waveform is rekordbox's");
    }

    // The popup opens in the look of the side it was opened from, and a
    // box in it switches to the other side's copy of the track at the
    // same zoom and place (#131).
    function test_thePopupSwitchesBetweenTheSides() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const open = delegateInView(o.page, b04);
        clickOn(findChild(open, "rowTitle"));
        compare(open.expanded, true);
        const rekordbox = findChild(open, "comparisonRekordbox");
        const engine = findChild(open, "comparisonEngine");
        tryVerify(() => rekordbox.opensZoom && engine.opensZoom, 30000, "both lanes can be opened");
        const list = findChild(o.page, "trackList");
        list.positionViewAtIndex(o.page.controller.tracks.indexOf(b04), ListView.Beginning);
        verify(waitForPolish(o.page.Window.window));
        const popup = findChild(o.page, "waveformZoomPopup");
        const box = findChild(popup, "zoomSideBox");
        verify(box !== null, "the popup has a side box");

        clickOn(engine);
        tryCompare(popup, "opened", true, 10000);
        compare(realPlayback.currentFormat, "engine");
        compare(popup.look, "engine", "opened from Engine's lane: Engine's look");
        compare(box.visible, true, "both sides of the track: the box is shown");
        compare(box.currentText, "Engine");
        tryVerify(() => popup.sides.length === 2 && popup.sides[0].available && popup.sides[1].available,
                  30000, "both sides have something to show");
        tryVerify(() => popup.lengthKnown, 10000, "the Engine copy's length");
        realPlayback.seek(30000);
        tryVerify(() => Math.abs(realPlayback.position - 30000) < 50, 5000);
        const span = popup.shownSpanMs;

        box.activated(0);
        compare(realPlayback.currentFormat, "rekordbox", "rekordbox's copy loaded");
        compare(realPlayback.currentSourceId, b04);
        compare(popup.look, "rekordbox-blue", "in rekordbox's look");
        compare(box.currentText, "rekordbox");
        tryVerify(() => popup.lengthKnown && Math.abs(realPlayback.position - 30000) < 50, 10000,
                  "at the same place: " + realPlayback.position);
        verify(Math.abs(popup.shownSpanMs - span) < span * 0.01, "at the same zoom: " + popup.shownSpanMs + " " + span);
        popup.close();
        tryCompare(popup, "visible", false);

        clickOn(rekordbox);
        tryCompare(popup, "opened", true, 10000);
        compare(popup.look, "rekordbox-blue", "opened from rekordbox's lane: rekordbox's look");
        compare(box.currentText, "rekordbox");
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // The row's own waveform opens the popup on both sides at once,
    // rekordbox's over Engine's, each in its own look and in step (#131).
    function test_theRowWaveformOpensBoth() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const withRow = delegateInView(o.page, b04);
        const view = rowWaveformOf(withRow);
        tryVerify(() => view.visible && view.opensZoom, 30000, "B04's waveform is drawn");
        const popup = findChild(o.page, "waveformZoomPopup");
        const box = findChild(popup, "zoomSideBox");
        clickOn(view);
        tryCompare(popup, "opened", true, 10000);
        compare(realPlayback.currentFormat, "rekordbox", "rekordbox's copy plays");
        tryCompare(popup, "showingBoth", true, 30000, "both sides, once both are read");
        compare(box.currentText, "Both");
        const area = findChild(popup, "zoomArea");
        const lanes = [];
        for (let i = 0; i < area.children.length; ++i) {
            if (area.children[i].objectName === "zoomLane") {
                lanes.push(area.children[i]);
            }
        }
        lanes.sort((x, y) => x.y - y.y);
        compare(lanes.length, 2);
        const tilesOf = (lane) => {
            const strip = findChild(lane, "waveformStrip");
            const out = [];
            for (let i = 0; i < strip.children.length; ++i) {
                if (strip.children[i].objectName === "zoomTile") {
                    out.push(strip.children[i]);
                }
            }
            return out;
        };
        tryVerify(() => tilesOf(lanes[0]).length > 0 && tilesOf(lanes[1]).length > 0, 10000);
        compare(tilesOf(lanes[0])[0].look, "rekordbox-blue");
        compare(tilesOf(lanes[1])[0].look, "engine");
        tryVerify(() => tilesOf(lanes[0]).some((t) => t.byteColumns > 0)
                  && tilesOf(lanes[1]).some((t) => t.byteColumns > 0), 10000, "both waveforms drawn");
        tryVerify(() => tilesOf(lanes[0]).some((t) => t.beatTicks.length > 0)
                  && tilesOf(lanes[1]).some((t) => t.beatTicks.length > 0), 10000, "both grids drawn");

        // Each lane at its own view's detail: rekordbox's analysis file's
        // and Engine's made from its audio, 150 columns a second, not the
        // rows' overview. rekordbox's copy is the one loaded: its lane is
        // the player's detail, and only Engine's is read for Both.
        tryVerify(() => popup.hasDetail && popup.sides[1].detailed === true, 30000, "both sides' detail is read");
        compare(popup.sides[0].detailed, false, "rekordbox's is the player's, not read again");
        const laneDetail = (lane) => {
            const tile = tilesOf(lane)[0];
            return {ms: tile.columnMs, bytes: tile.columnBytes ? tile.columnBytes.byteLength : 0};
        };
        tryVerify(() => laneDetail(lanes[0]).bytes === popup.detail.columns.byteLength
                  && laneDetail(lanes[1]).bytes === popup.sides[1].columns.byteLength, 5000, "the lanes draw it");
        const both = [laneDetail(lanes[0]), laneDetail(lanes[1])];
        for (const d of both) {
            verify(Math.abs(d.ms - 1000 / 150) < 0.01, "150 columns a second: " + d.ms);
        }

        // Both lanes put the same moment under the playhead line: the
        // waveforms and grids are drawn on one timeline. (Their bar
        // numbers differ by one on B04: Engine's own grid starts its bar 1
        // at 2.222 s, a bar after rekordbox's at 0 s.)
        const timeUnderLine = (lane) => {
            const strip = findChild(lane, "waveformStrip");
            const tile = tilesOf(lane).find((t) => strip.x + t.x <= popup.lineX
                                            && strip.x + t.x + t.width > popup.lineX);
            verify(tile !== undefined, "a tile under the line");
            return tile.viewStartMs + (popup.lineX - strip.x - tile.x) / popup.pxPerMs;
        };
        const sameTime = (why) => {
            const top = timeUnderLine(lanes[0]);
            const bottom = timeUnderLine(lanes[1]);
            verify(Math.abs(top - bottom) < 1e-6, why + ": " + top + " and " + bottom);
            verify(Math.abs(top - popup.centerMs) <= 1 / popup.pxPerMs, why + ": the playhead's "
                   + popup.centerMs + ", drawn at " + top);
        };
        sameTime("opened");
        popup.zoomBy(2, popup.lineX);
        sameTime("zoomed");
        popup.scrollBy(300);
        sameTime("scrolled");
        popup.follow();
        realPlayback.seek(30000);
        tryVerify(() => Math.abs(realPlayback.position - 30000) < 50, 5000);
        sameTime("seeked");

        // The single views draw the same detail.
        box.activated(0);
        compare(popup.showingBoth, false);
        tryVerify(() => popup.hasDetail, 10000, "rekordbox's own detail");
        compare(popup.detail.columns.byteLength, both[0].bytes, "rekordbox: the same columns as in Both");
        verify(Math.abs(popup.detail.msPerColumn - both[0].ms) < 0.01, "at the same step");
        box.activated(1);
        tryCompare(realPlayback, "currentFormat", "engine");
        tryVerify(() => popup.hasDetail && popup.look === "engine", 30000, "Engine's own detail");
        compare(popup.detail.columns.byteLength, both[1].bytes, "Engine: the same columns as in Both");
        verify(Math.abs(popup.detail.msPerColumn - both[1].ms) < 0.01, "at the same step");
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // Opens the popup on B04's both sides from its row's own waveform,
    // and waits for Engine's side to be read for it.
    function openBoth(o) {
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const view = rowWaveformOf(delegateInView(o.page, b04));
        tryVerify(() => view.visible && view.opensZoom, 30000, "B04's waveform is drawn");
        const popup = findChild(o.page, "waveformZoomPopup");
        clickOn(view);
        tryCompare(popup, "opened", true, 10000);
        tryVerify(() => popup.showingBoth && popup.sides[1].detailed === true, 30000, "Engine's side is read");
        return popup;
    }
    // Stages B04's copy and saves it, running `during` while the save
    // writes.
    function saveWhile(o, during) {
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        verify(o.c.canStage, "something to stage");
        o.c.stage();
        const session = EditSessionRegistry.sessionFor(o.libraryId);
        const saved = finished(session, () => {
            session.save();
            verify(EditSessionRegistry.anyWriting, "the save is writing");
            during();
        }, "the copy");
        compare(saved.error, "", JSON.stringify(saved));
    }

    // The player going on to another track (the queue's next) while Both
    // shows B04: the popup follows the player as one view, and no read of
    // B04 starts for it.
    function test_bothLetsGoWhenThePlayerMovesOn() {
        const o = openOn(t73(), {playbackController: realPlayback});
        const popup = openBoth(o);
        const reads = o.c.zoomDetailReads();
        const next = o.c.zoomTrack(b01);
        realPlayback.load(next.format, next.libraryPath, next.sourceId, next.filePath, next.title, next.artist,
                          next.artworkPath, next.cues, next.fallbackArtworkPath, false);
        compare(realPlayback.currentSourceId, b01);
        compare(popup.loadedSide, -1);
        compare(popup.showingBoth, false, "Both stops with B04 gone from the player");
        compare(popup.lanes.length, 1, "one lane, the player's");
        compare(findChild(popup, "zoomSideBox").visible, false);
        tryVerify(() => popup.hasDetail, 10000, "B01's own detail");
        compare(findChild(popup, "zoomTitle").text, row(o.c, b01).title);
        wait(500);
        compare(o.c.zoomDetailReads(), reads, "no read of B04 starts");
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // Closing the popup stops its read: opened and closed while a save
    // holds the read back, nothing is read once the save is over.
    function test_closingStopsTheZoomRead() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const popup = findChild(o.page, "waveformZoomPopup");
        // At once, so that it is closed before the save is over.
        popup.enter = null;
        popup.exit = null;
        const reads = o.c.zoomDetailReads();
        saveWhile(o, () => {
            o.page.openZoom(b04, "both");
            compare(popup.visible, true);
            compare(o.c.zoomDetailReads(), reads, "held while the save writes");
            popup.close();
            compare(popup.visible, false);
        });
        wait(1000);
        compare(o.c.zoomDetailReads(), reads, "closed: nothing is read once the save is over");
        compare(o.c.zoomDetail(b04).ready, false);
        realPlayback.stop();
    }

    // What was read for Both is read again once a write has ended, while
    // the popup stays open.
    function test_bothIsReadAgainAfterAWrite() {
        const o = openOn(t73(), {playbackController: realPlayback});
        const popup = openBoth(o);
        const reads = o.c.zoomDetailReads();
        saveWhile(o, () => {});
        tryVerify(() => o.c.zoomDetailReads() > reads, 10000, "Engine's side is read again after the save");
        tryVerify(() => popup.sides[1].detailed === true, 30000);
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // Each lane of Both is on the timeline its own single view draws it
    // on. B04 is an MP3 with a 25 ms lead-in: rekordbox's waveform and grid
    // move by it onto the player's timeline, Engine's do not move. Each
    // lane is drawn from what the page read while the other side is
    // loaded, and compared with that side's own view.
    function test_bothLanesKeepEachSidesTimeline() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const open = delegateInView(o.page, b04);
        clickOn(findChild(open, "rowTitle"));
        const engine = findChild(open, "comparisonEngine");
        tryVerify(() => engine.opensZoom, 30000, "Engine's lane can be opened");
        findChild(o.page, "trackList").positionViewAtIndex(o.c.tracks.indexOf(b04), ListView.Beginning);
        verify(waitForPolish(o.page.Window.window));
        const popup = findChild(o.page, "waveformZoomPopup");
        const box = findChild(popup, "zoomSideBox");
        const nearestBeat = (ticks, ms) => ticks.reduce(
            (best, t) => Math.abs(t.timeMs - ms) < Math.abs(best - ms) ? t.timeMs : best, Infinity);
        const settle = () => {
            realPlayback.seek(30000);
            tryVerify(() => popup.lengthKnown && Math.abs(realPlayback.position - 30000) < 50, 10000);
        };

        // Engine's single view, then Both: rekordbox's lane from the page.
        clickOn(engine);
        tryCompare(popup, "opened", true, 10000);
        compare(realPlayback.currentFormat, "engine");
        tryVerify(() => popup.hasDetail, 30000, "Engine's own detail");
        settle();
        const engineStart = popup.detail.startMs || 0;
        compare(engineStart, 0, "Engine's own view does not move its waveform");
        compare(o.c.zoomDetail(b04).ready, false, "one view: the page reads nothing, the player decoded the file");
        tryVerify(() => popup.ticks.length > 0, 10000);
        const engineBeat = nearestBeat(popup.ticks, 30000);
        box.activated(2);
        tryVerify(() => popup.showingBoth && popup.sides[0].detailed === true, 30000, "rekordbox's detail is read");
        compare(o.c.zoomDetail(b04).engineRead, false, "Both reads rekordbox's side only: Engine's file is not decoded twice");
        const shift = popup.sides[0].startMs;
        verify(shift < -20, "the lead-in moves rekordbox's side: " + shift);
        compare(popup.lanes[0].startMs, shift, "rekordbox's lane is drawn from the page's read, moved by it");
        compare(popup.lanes[1].startMs, engineStart, "Engine's lane where its own view draws it");
        tryVerify(() => popup.lanes[0].ticks.length > 0, 10000);
        const rekordboxBeatInBoth = nearestBeat(popup.lanes[0].ticks, 30000);
        compare(nearestBeat(popup.lanes[1].ticks, 30000), engineBeat, "Engine's beats where its own view has them");

        // rekordbox's single view: the same offset and the same beat.
        box.activated(0);
        tryCompare(realPlayback, "currentFormat", "rekordbox");
        tryVerify(() => popup.hasDetail && popup.look === "rekordbox-blue", 30000, "rekordbox's own detail");
        settle();
        verify(Math.abs(popup.detail.startMs - shift) < 1e-6,
               "rekordbox's own view moves its waveform as its lane in Both did: " + popup.detail.startMs + " and " + shift);
        tryVerify(() => popup.ticks.length > 0, 10000);
        verify(Math.abs(nearestBeat(popup.ticks, 30000) - rekordboxBeatInBoth) < 0.5,
               "and its beats: " + nearestBeat(popup.ticks, 30000) + " and " + rekordboxBeatInBoth);

        // Both again, rekordbox loaded: Engine's lane now from the page.
        box.activated(2);
        tryVerify(() => popup.showingBoth && popup.sides[1].detailed === true, 30000, "Engine's detail is read");
        compare(popup.lanes[1].startMs, engineStart, "Engine's lane from the page's read is not moved");
        compare(popup.lanes[0].startMs, shift, "rekordbox's lane, the player's own now, is where it was");
        compare(nearestBeat(popup.lanes[1].ticks, 30000), engineBeat);
        popup.close();
        tryCompare(popup, "visible", false);
        realPlayback.stop();
    }

    // A row's cover plays its track (#133): the key shows over the cover
    // while the row is hovered, a click plays rekordbox's copy, and it
    // stays as a pause key while that is the track playing.
    function test_aRowPlaysFromItsCover() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        // The preview's bar fades out and gives back its room: the rows
        // move up meanwhile.
        tryVerify(() => !findChild(o.page, "previewProgress").visible, 5000, "the bar is gone");
        const item = delegateInView(o.page, b01);
        tryVerify(() => o.c.rowAnalysis(b01).ready === true, 30000, "B01 is read");
        const key = findChild(item, "rowPlayButton");
        verify(key !== null, "the row has a play key");
        mouseMove(o.page, 1, 1);
        compare(key.visible, false, "hidden until the row is hovered");
        const cover = findChild(item, "rowArtwork");
        mouseMove(cover, cover.width / 2, cover.height / 2);
        tryCompare(key, "visible", true);
        compare(key.enabled, true, "B01 has its file on the stick");
        wait(500);
        waitForRendering(o.page);
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-cover-play.png");
        }
        mouseClick(key);
        tryCompare(realPlayback, "currentSourceId", b01);
        compare(realPlayback.currentFormat, "rekordbox");
        compare(realPlayback.currentLibraryPath, o.page.rekordboxPath);
        tryCompare(realPlayback, "playing", true);
        compare(o.c.isTicked(b01), false, "the click ticks nothing");
        compare(item.expanded, false, "and opens nothing");
        mouseMove(o.page, 1, 1);
        verify(key.visible, "shown while it is the track playing");
        compare(key.text, "Pause");
        mouseClick(key);
        tryCompare(realPlayback, "playing", false);
        realPlayback.stop();
    }

    // A picture of the rows with their waveforms, at the page's usual width.
    function test_rowWaveformsScreenshot() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        o.c.setTicked(b01, true);
        o.c.setTicked(b04, true);
        waitSettled(o.c);
        for (const id of [b01, b04, b12]) {
            tryVerify(() => o.c.rowAnalysis(id).ready === true, 30000, id + " is read");
        }
        verify(waitForPolish(o.page.Window.window));
        tryVerify(() => rowWaveformOf(trackDelegate(o.page, b01)).visible, 10000);
        waitForRendering(o.page);
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-row-waveforms.png");
        }
    }

    // A picture of an open row: rekordbox's lane over Engine's.
    function test_openRowScreenshot() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const row = delegateInView(o.page, b04);
        clickOn(findChild(row, "rowTitle"));
        compare(row.expanded, true);
        const rekordbox = findChild(row, "comparisonRekordbox");
        const engine = findChild(row, "comparisonEngine");
        tryVerify(() => rekordbox.byteColumns > 0 && engine.byteColumns > 0, 30000, "both waveforms are drawn");
        tryVerify(() => o.c.engineAnalysis(b04).ready === true, 30000);
        findChild(o.page, "trackList").positionViewAtIndex(o.c.tracks.indexOf(b04), ListView.Beginning);
        verify(waitForPolish(o.page.Window.window));
        wait(300);
        waitForRendering(o.page);
        if (screenshotDir) {
            grabImage(o.page).save(screenshotDir + "/BeatGridsWaveformsPage-open-row.png");
        }
    }

    // Pictures of the popup opened from each side of an open row.
    function test_popupScreenshots() {
        const o = openOn(t73(), {playbackController: realPlayback});
        showPlaylist(o.page, "T73 Renamed");
        waitSettled(o.c);
        const open = delegateInView(o.page, b04);
        clickOn(findChild(open, "rowTitle"));
        const rekordbox = findChild(open, "comparisonRekordbox");
        const engine = findChild(open, "comparisonEngine");
        tryVerify(() => rekordbox.opensZoom && engine.opensZoom, 30000, "both lanes can be opened");
        const list = findChild(o.page, "trackList");
        list.positionViewAtIndex(o.page.controller.tracks.indexOf(b04), ListView.Beginning);
        verify(waitForPolish(o.page.Window.window));
        const popup = findChild(o.page, "waveformZoomPopup");
        const rowView = rowWaveformOf(open);
        for (const [lane, name] of [[engine, "engine"], [rekordbox, "rekordbox"], [rowView, "both"]]) {
            clickOn(lane);
            tryCompare(popup, "opened", true, 10000);
            tryVerify(() => popup.lengthKnown && popup.hasDetail, 30000, name + "'s detail");
            if (name === "both") {
                tryVerify(() => popup.showingBoth && popup.lanes.length === 2 && popup.lanes.every(
                              (l) => !!l.columns && Math.abs(l.columnMs - 1000 / 150) < 0.01), 30000,
                          "both sides' detail");
            }
            realPlayback.seek(30000);
            tryVerify(() => Math.abs(realPlayback.position - 30000) < 50, 5000);
            wait(300);
            waitForRendering(o.page);
            if (screenshotDir) {
                grabImage(testCase).save(screenshotDir + "/BeatGridsWaveformsPage-popup-" + name + ".png");
            }
            popup.close();
            tryCompare(popup, "visible", false);
        }
        realPlayback.stop();
    }
}
