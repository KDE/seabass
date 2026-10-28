// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Browse's track list scrolled with the wheel while staged playlist edits
// change its rows: the real page on a copy of the anonymized fixture, the
// real controllers, Experimental on, and a seeded walk (mulberry32, as the
// storm runner) of wheel bursts, edits, playlist switches and rescans.
// In the storm lane (docs/testing.md "The storm"): its walks are a
// quarter of a hunt's by default, which still finds the 2026-10-17 crash
// within seconds, and SEABASS_PAGE_STORM_SCALE=4 is the full length.
TestCase {
    id: testCase
    name: "ScanPageStorm"

    readonly property real scale: {
        const given = parseFloat(stormFixture.env("SEABASS_PAGE_STORM_SCALE"));
        return isNaN(given) || given <= 0 ? 1 : given;
    }
    function walk(steps) { return Math.round(steps * testCase.scale); }
    width: 1000
    height: 700
    visible: true
    when: windowShown

    PlaybackController { id: realPlayback }
    AppSettingsController { id: realAppSettings }

    Component {
        id: pageComponent
        ScanPage {}
    }

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_ScanPage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // ---- the PRNG: mulberry32, so a seed walks the same way everywhere ----
    property real prngState: 0
    function random() {
        let t = (testCase.prngState + 0x6D2B79F5) | 0;
        testCase.prngState = t;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    }
    function below(n) { return Math.floor(testCase.random() * n); }
    function chance(p) { return testCase.random() < p; }
    function pick(list) { return list[testCase.below(list.length)]; }

    property var stick: null
    property string lastPlaylistBefore: ""
    property bool experimentalBefore: false

    function init() {
        lastPlaylistBefore = realAppSettings.lastPlaylistName;
        experimentalBefore = realAppSettings.experimentalFeaturesEnabled;
    }

    function cleanup() {
        EditSessionRegistry.discardAll();
        if (testCase.stick !== null) {
            const browse = createTemporaryObject(editComponent, testCase);
            browse.discardLeftOverEdits(testCase.stick.pioneer, testCase.stick.engine);
            testCase.stick = null;
        }
        realAppSettings.experimentalFeaturesEnabled = experimentalBefore;
        realAppSettings.lastPlaylistName = lastPlaylistBefore;
        mouseMove(testCase, testCase.width - 1, testCase.height - 1);
    }
    Component {
        id: editComponent
        PlaylistEditController {}
    }

    // The page on a fresh copy, scanned, showing its longest playlist.
    function openPage() {
        const root = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(root.length > 0, "the fixture must copy");
        const t = {pioneer: root + "/PIONEER", engine: root + "/Engine Library"};
        testCase.stick = t;
        realAppSettings.experimentalFeaturesEnabled = true;
        realAppSettings.lastPlaylistName = "";
        const page = createTemporaryObject(pageComponent, testCase, {
            width: 880, height: 660, stickLabel: "TESTSTICK", rekordboxPath: t.pioneer, enginePath: t.engine,
            playbackController: realPlayback, appSettingsController: realAppSettings});
        verify(page !== null);
        t.page = page;
        t.scan = findChild(page, "scanController");
        t.list = findChild(page, "trackListView");
        t.browse = page.playlistEditController;
        tryVerify(() => !t.scan.busy && t.scan.playlistNames.length > 0, 120000, "scanned");
        tryCompare(page, "leftOverEditsChecked", true);
        // Every file on the stick, for adds from elsewhere.
        t.allFiles = [];
        for (let i = 0; i < t.scan.tracks.trackCount(); ++i) {
            const track = t.scan.tracks.trackAt(i);
            if (track.filePath.length > 0) {
                t.allFiles.push({filePath: track.filePath, title: track.title});
            }
        }
        const counts = t.scan.playlistTrackCounts;
        // The longest two the edits can name: the fixture's anonymized
        // names repeat ("Playlist" in two folders, which the controller
        // refuses to guess between), so only the numbered ones.
        t.playlists = t.scan.playlistNames.filter(n => /^Playlist [0-9]{3}$/.test(n) && n !== "Playlist 000")
            .sort((a, b) => (counts[b] || 0) - (counts[a] || 0));
        t.playlist = t.playlists[0];
        t.other = t.playlists[1];
        // The view's own record of delegates it holds unasked, where both
        // crashes found a dead one (see ItemViewFixture).
        t.probe = itemViewFixture && itemViewFixture.available ? itemViewFixture : null;
        if (t.probe !== null) {
            verify(t.probe.watch(t.list));
        }
        showPlaylist(t, t.playlist);
        verify(t.list.count > 0);
        verify(t.list.contentHeight > t.list.height,
               "the list must scroll: " + t.list.count + " rows of " + t.playlist);
        t.fresh = false;
        return t;
    }

    function showPlaylist(t, name) {
        const index = t.scan.playlistNames.indexOf(name);
        // -1: one staged to be created, listed below the others.
        t.page.selectPlaylist(index >= 0 ? index + 1 : -1, name);
    }

    // A wheel notch or a few over the list, up or down, without waiting for
    // the flick to settle.
    function wheel(t) {
        const n = 1 + testCase.below(4);
        const down = testCase.chance(0.5);
        for (let i = 0; i < n; ++i) {
            mouseWheel(t.list, t.list.width / 2, t.list.height / 2, 0, down ? -120 : 120);
        }
    }

    // A row in view, as its track's map, or null.
    function visibleRow(t) {
        const y = t.list.contentY + testCase.random() * t.list.height;
        const index = t.list.indexAt(t.list.width / 2, y);
        return index >= 0 ? {index: index, track: t.scan.tracks.trackAt(index)} : null;
    }

    // "Not in every library": answered, as the page's dialog would be.
    function answerAsks(t) {
        const ask = findChild(t.page, "missingLibrariesDialog");
        if (ask.opened || ask.visible) {
            findChild(ask, "addAnywayButton").clicked();
        }
    }

    // One edit that changes the rows (or might), through the page's own
    // controller, as its menus and drop places call it.
    function edit(t) {
        const browse = t.browse;
        const shown = t.page.shownPlaylist;
        // A new playlist fills up first, as one being put together does.
        const roll = t.fresh && t.list.count < 45 && testCase.chance(0.6) ? 40 : testCase.below(100);
        if (shown.length === 0) {
            showPlaylist(t, t.playlist);
            return "show";
        }
        if (roll < 35) {
            const row = visibleRow(t);
            if (row !== null && row.track.filePath.length > 0) {
                browse.moveInPlaylist(t.pioneer, t.engine, shown, row.track.filePath, row.track.title,
                                      testCase.below(t.list.count));
                return "move";
            }
        } else if (roll < 50) {
            const file = testCase.pick(t.allFiles);
            browse.addToPlaylist(t.pioneer, t.engine, shown, file.filePath, file.title,
                                 testCase.chance(0.5) ? testCase.below(t.list.count) : -1);
            return "add";
        } else if (roll < 62) {
            const row = visibleRow(t);
            if (row !== null && row.track.filePath.length > 0) {
                const file = row.track.filePath;
                if (browse.isRemovalStaged(shown, file)) {
                    browse.keepInPlaylist(shown, file);
                } else {
                    browse.removeFromPlaylist(t.pioneer, t.engine, shown, file, row.track.title);
                }
                return "remove";
            }
        } else if (roll < 77) {
            // Taken back: an add, a move.
            const row = visibleRow(t);
            if (row !== null) {
                if (row.track.pendingAddition) {
                    browse.keepOutOfPlaylist(shown, row.track.filePath);
                } else if (row.track.pendingMove) {
                    browse.keepPositionInPlaylist(shown, row.track.filePath);
                }
                return "unstage";
            }
        } else if (roll < 95) {
            showPlaylist(t, shown === t.playlist ? t.other : t.playlist);
            return "switch";
        } else if (roll < 97) {
            t.page.rescan();
            return "rescan";
        }
        return "none";
    }

    // The walk. `mode` says when the wheel comes after an edit:
    // "now" as the call returns, before any event is processed; "pass" after
    // one pass of the event loop; "signal" from Qt.callLater on the model's
    // own row signals, which is before the view's next polish.
    function storm(seed, steps, mode, fresh) {
        const t = openPage();
        if (fresh) {
            // Put together from nothing: created, shown, then filled.
            verify(t.browse.createPlaylist(t.pioneer, t.engine, "Storm Picks"));
            tryVerify(() => !t.browse.busy && t.browse.isNewPlaylistStaged("Storm Picks"), 60000, "created");
            t.playlist = "Storm Picks";
            t.fresh = true;
            showPlaylist(t, t.playlist);
            compare(t.page.shownPlaylist, "Storm Picks");
        }
        testCase.prngState = seed * 2654435761;
        const model = t.scan.tracks;
        let onSignal = 0;
        const later = () => {
            if (t.list.interactive) {
                wheel(t);
            }
        };
        const hook = () => {
            if (mode === "signal") {
                ++onSignal;
                Qt.callLater(later);
            }
        };
        model.rowsInserted.connect(hook);
        model.rowsRemoved.connect(hook);
        model.rowsMoved.connect(hook);
        model.modelReset.connect(hook);
        const done = {};
        let unrequestedMax = 0;
        for (let step = 0; step < steps; ++step) {
            if (t.scan.busy) {
                tryVerify(() => !t.scan.busy, 60000, "rescan done");
                showPlaylist(t, t.playlist);
            }
            answerAsks(t);
            // One lookup at a time: the controller reads the stick for each
            // edit, and a queue of them would land long after the wheel.
            // While one is being looked up the wheel goes on, and the
            // staging lands in the middle of it.
            const looking = t.browse.busy;
            const what = looking || testCase.chance(0.45) ? (wheel(t), "wheel") : edit(t);
            done[what] = (done[what] || 0) + 1;
            if (what !== "wheel" && mode === "now") {
                wheel(t);
            } else if (what !== "wheel" && mode === "pass") {
                wait(0);
                wheel(t);
            }
            if (t.probe !== null) {
                unrequestedMax = Math.max(unrequestedMax, t.probe.unrequestedCount(t.list));
                const dead = t.probe.deadUnrequested(t.list);
                verify(dead === 0, "step " + step + " (" + what + "): the list holds " + dead
                       + " destroyed delegate(s) it will read on its next refill");
            }
            const settle = testCase.below(10);
            if (looking) {
                wait(testCase.below(25));
            } else if (settle < 4) {
                // Same frame as the next step.
            } else if (settle < 8) {
                wait(0);
            } else if (settle < 9) {
                waitForRendering(t.list);
            } else {
                wait(testCase.below(40));
            }
        }
        model.rowsInserted.disconnect(hook);
        model.rowsRemoved.disconnect(hook);
        model.rowsMoved.disconnect(hook);
        model.modelReset.disconnect(hook);
        tryVerify(() => {
            answerAsks(t);
            return !t.browse.busy;
        }, 60000, "the last edit staged: " + t.browse.errorMessage);
        console.log("storm seed " + seed + " mode " + mode + (fresh ? " (new playlist, " + t.list.count + " rows)" : "") + ": " + JSON.stringify(done)
                    + (mode === "signal" ? ", wheels on the model's signals: " + onSignal : "")
                    + ", most unrequested delegates held: " + unrequestedMax);
        verify(t.list.count > 0);
    }

    function test_wheelAsTheEditReturns() {
        storm(1, walk(1500), "now");
    }

    function test_wheelAfterAnEventPass() {
        storm(2, walk(1500), "pass");
    }

    function test_wheelOnTheRowSignals() {
        storm(3, walk(1500), "signal");
    }

    function test_wheelWhileANewPlaylistFills() {
        storm(4, walk(1500), "now", true);
    }

    // ---- The Add to... submenu: the list both crashes were in. ----

    // The row menu opened on a row in view, and its Add to... submenu
    // opened from it, as a right click and a hover would.
    function openAddTo(t) {
        const trackMenu = findChild(t.page, "trackMenu");
        const addToMenu = findChild(t.page, "addToMenu");
        const row = t.list.itemAtIndex(t.list.indexAt(10, t.list.contentY + 10));
        verify(row !== null, "a row in view");
        t.page.openTrackMenu(row, row);
        tryCompare(trackMenu, "opened", true);
        tryVerify(() => trackMenu.addTargets.length > 0, 5000, "somewhere to add to");
        addToMenu.open();
        tryCompare(addToMenu, "opened", true);
        waitForRendering(addToMenu.contentItem);
        return {trackMenu: trackMenu, addToMenu: addToMenu, list: addToMenu.contentItem};
    }
    function closeMenus(menus) {
        menus.addToMenu.close();
        menus.trackMenu.close();
        tryCompare(menus.trackMenu, "visible", false);
        tryCompare(menus.addToMenu, "visible", false);
    }
    function wheelToTheEnd(list, down) {
        for (let i = 0; i < 40; ++i) {
            mouseWheel(list, list.width / 2, list.height / 2, 0, down ? -120 : 120);
        }
        tryVerify(() => !list.moving, 5000);
        waitForRendering(list);
    }

    function test_theAddToMenuSurvivesItsTargetsChanging() {
        const t = openPage();
        // A list of targets longer than the menu, as Sebastian's is: the
        // stick's playlists and the new ones being put together.
        for (let i = 0; i < 30; ++i) {
            const name = "Storm " + (i < 10 ? "0" : "") + i;
            verify(t.browse.createPlaylist(t.pioneer, t.engine, name));
            tryVerify(() => !t.browse.busy && t.browse.isNewPlaylistStaged(name), 60000, name);
        }
        for (let round = 0; round < 6; ++round) {
            const menus = openAddTo(t);
            verify(menus.list.contentHeight > menus.list.height,
                   "the submenu must scroll: " + menus.list.contentHeight + " in " + menus.list.height);
            if (t.probe !== null && round === 0) {
                verify(t.probe.watch(menus.list));
            }
            // Down to its end and back: the entries scrolled out are let go
            // by the menu's list, which keeps them as unrequested items.
            wheelToTheEnd(menus.list, round % 2 === 0);
            if (t.probe !== null) {
                console.log("round " + round + ": unrequested " + t.probe.unrequestedCount(menus.list));
            }
            closeMenus(menus);
            // What staging does to the targets: the submenu's entries are
            // made again (its Instantiator), the old ones destroyed.
            const name = "Storm extra " + round;
            verify(t.browse.createPlaylist(t.pioneer, t.engine, name));
            tryVerify(() => !t.browse.busy && t.browse.isNewPlaylistStaged(name), 60000, name);
            wait(50);
            if (t.probe !== null) {
                compare(t.probe.deadUnrequested(menus.list), 0,
                        "round " + round + ": the submenu's list holds destroyed entries it will read on its next scroll");
            }
            // The next look at the submenu: the wheel that crashed.
            const again = openAddTo(t);
            wheelToTheEnd(again.list, round % 2 !== 0);
            closeMenus(again);
        }
    }

    // ---- The Matching panel's This Playlist list. ----

    function openPlaylistCombo(t) {
        const combo = findChild(t.page, "playlistCombo");
        verify(combo !== null);
        combo.popup.open();
        tryCompare(combo.popup, "opened", true);
        const list = combo.popup.contentItem;
        waitForRendering(list);
        return {combo: combo, list: list};
    }

    function test_theMatchingPlaylistListSurvivesItsModelChanging() {
        const t = openPage();
        const anchor = (i) => t.list.itemAtIndex(i);
        t.page.toggleAnchor(anchor(0));
        tryCompare(t.page, "matchingPanelOpen", true);
        let watched = false;
        for (let round = 0; round < 6; ++round) {
            const c = openPlaylistCombo(t);
            console.log("combo list " + c.list.width + "x" + c.list.height + ", " + c.list.count + " rows");
            verify(c.list.contentHeight > c.list.height, "the list must scroll");
            if (t.probe !== null && !watched) {
                watched = t.probe.watch(c.list);
            }
            wheelToTheEnd(c.list, round % 2 === 0);
            if (t.probe !== null) {
                console.log("round " + round + ": unrequested " + t.probe.unrequestedCount(c.list));
            }
            c.combo.popup.close();
            tryCompare(c.combo.popup, "visible", false);
            // Another track to match: the quick picks change, and the
            // combo's rows are made again.
            t.page.toggleAnchor(anchor(1 + round));
            wait(50);
            if (t.probe !== null) {
                compare(t.probe.deadUnrequested(c.list), 0, "round " + round + ": destroyed rows still held");
            }
        }
    }

    // The This Playlist list scrolled while what it lists changes: another
    // track to match (its quick picks), Browse's playlist (the panel's
    // target follows it), a staged rename (the names), the popup opened
    // and shut on the way.
    function comboStorm(seed, steps) {
        const t = openPage();
        testCase.prngState = seed * 2654435761;
        t.page.toggleAnchor(t.list.itemAtIndex(0));
        tryCompare(t.page, "matchingPanelOpen", true);
        const combo = findChild(t.page, "playlistCombo");
        const list = combo.popup.contentItem;
        let watched = false;
        const done = {};
        let unrequestedMax = 0;
        for (let step = 0; step < steps; ++step) {
            if (t.scan.busy) {
                tryVerify(() => !t.scan.busy, 60000, "rescan done");
            }
            const roll = testCase.below(100);
            let what = "none";
            if (roll < 45) {
                if (!combo.popup.visible) {
                    combo.popup.open();
                    what = "open";
                } else if (list.interactive) {
                    const n = 1 + testCase.below(5);
                    const down = testCase.chance(0.5);
                    for (let i = 0; i < n; ++i) {
                        mouseWheel(list, list.width / 2, list.height / 2, 0, down ? -120 : 120);
                    }
                    what = "wheel";
                }
            } else if (roll < 55) {
                combo.popup.close();
                what = "close";
            } else if (roll < 75) {
                const row = t.list.itemAtIndex(t.list.indexAt(10, t.list.contentY + testCase.random() * t.list.height));
                if (row !== null) {
                    // Not the anchor itself: that closes the panel.
                    if (row.sourceId !== t.page.anchorSourceId) {
                        t.page.toggleAnchor(row);
                        what = "anchor";
                    }
                }
            } else if (roll < 85) {
                showPlaylist(t, testCase.pick(t.playlists.slice(0, 6)));
                what = "browse";
            } else if (roll < 95 && !t.browse.busy) {
                const name = testCase.pick(t.playlists.slice(0, 6));
                if (t.browse.isRenameStaged(name + " Storm")) {
                    t.browse.keepPlaylistName(name + " Storm");
                } else if (!t.browse.isRenameStaged(name)) {
                    t.browse.renamePlaylist(t.pioneer, t.engine, name, name + " Storm");
                }
                what = "rename";
            } else if (roll < 97) {
                t.page.rescan();
                what = "rescan";
            }
            done[what] = (done[what] || 0) + 1;
            if (!watched && combo.popup.visible && t.probe !== null) {
                watched = t.probe.watch(list);
            }
            if (watched) {
                unrequestedMax = Math.max(unrequestedMax, t.probe.unrequestedCount(list));
                const dead = t.probe.deadUnrequested(list);
                verify(dead === 0, "step " + step + " (" + what + "): the list holds " + dead
                       + " destroyed row(s) it will read on its next refill");
            }
            const settle = testCase.below(10);
            if (settle < 3) {
            } else if (settle < 7) {
                wait(0);
            } else if (settle < 9) {
                waitForRendering(t.page);
            } else {
                wait(testCase.below(60));
            }
        }
        combo.popup.close();
        tryVerify(() => !t.browse.busy, 60000);
        console.log("combo storm seed " + seed + ": " + JSON.stringify(done) + ", most unrequested rows held: "
                    + unrequestedMax + (watched ? "" : " (not watched)"));
    }

    function test_theMatchingPlaylistListWhileItsRowsChange() {
        comboStorm(5, walk(750));
    }

    // ---- The same list on its own: the Matching panel with a stand-in scan
    // of 30 playlists, the 33 rows both crashed lists had (All tracks, two
    // quick picks, 30 playlists), and nothing to wait for between steps. ----

    Component {
        id: fakeScanComponent
        QtObject {
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            property int totalTrackCount: 0
            property var playlistThumbnails: ({})
            function findCompatibleTracks() { return []; }
        }
    }
    Component {
        id: matchingComponent
        MatchingPage {
            width: 520
            height: 640
            keyNotation: "standard"
            anchorSourceId: "1"
            anchorTitle: "Anchor"
            anchorArtist: "A"
            anchorKey: "Am"
            anchorBpm: 128.0
            anchorArtworkPath: ""
            anchorPlaylistNames: []
            browseSelectedPlaylistIndex: -1
        }
    }
    function fakeNames(scan, n, prefix, countBase) {
        const names = [];
        const counts = {};
        for (let i = 1; i <= n; ++i) {
            const name = prefix + String(i).padStart(3, "0");
            names.push(name);
            counts[name] = countBase + i;
        }
        scan.playlistTrackCounts = counts;
        scan.totalTrackCount = n + countBase;
        scan.playlistNames = names;
    }

    function matchingStorm(seed, steps, unprobed) {
        testCase.prngState = seed * 2654435761;
        const scan = createTemporaryObject(fakeScanComponent, testCase);
        fakeNames(scan, 30, "Playlist ", 0);
        const page = createTemporaryObject(matchingComponent, testCase, {scanController: scan});
        page.anchorPlaylistNames = ["Playlist 003", "Playlist 017"];
        const combo = findChild(page, "playlistCombo");
        waitForRendering(page);
        const probe = !unprobed && itemViewFixture && itemViewFixture.available ? itemViewFixture : null;
        const watchedLists = [];
        const done = {};
        let unrequestedMax = 0;
        let countBase = 0;
        const check = (step, what) => {
            if (probe !== null && combo.popup.contentItem) {
                const now = combo.popup.contentItem;
                unrequestedMax = Math.max(unrequestedMax, probe.unrequestedCount(now));
                const dead = probe.deadUnrequested(now);
                verify(dead === 0, "step " + step + " (" + what + "): the list holds " + dead
                       + " destroyed row(s) it will read on its next refill");
            }
        };
        for (let step = 0; step < steps; ++step) {
            check(step, "before");
            const list = combo.popup.contentItem;
            const roll = testCase.below(100);
            let what = "none";
            if (roll < 15) {
                combo.popup.visible ? combo.popup.close() : combo.popup.open();
                what = combo.popup.visible ? "open" : "close";
            } else if (roll < 55) {
                if (combo.popup.visible && list.count > 0) {
                    const n = 1 + testCase.below(6);
                    const down = testCase.chance(0.5);
                    for (let i = 0; i < n; ++i) {
                        mouseWheel(list, list.width / 2, list.height / 2, 0, down ? -120 : 120);
                    }
                    what = "wheel";
                }
            } else if (roll < 62) {
                if (combo.popup.visible) {
                    testCase.chance(0.5) ? list.positionViewAtEnd() : list.positionViewAtBeginning();
                    what = "jump";
                }
            } else if (roll < 72) {
                // A staged edit's counts, the same names: the combo's model
                // is made again.
                countBase = (countBase + 1) % 7;
                fakeNames(scan, 30, "Playlist ", countBase);
                what = "counts";
            } else if (roll < 77) {
                fakeNames(scan, 28 + testCase.below(5), testCase.chance(0.8) ? "Playlist " : "Crate ", countBase);
                what = "names";
            } else if (roll < 87) {
                page.anchorSourceId = String(2 + testCase.below(50));
                page.anchorPlaylistNames = testCase.chance(0.5) ? ["Playlist 003", "Playlist 017"]
                                                                 : ["Playlist 00" + (1 + testCase.below(9))];
                what = "anchor";
            } else if (roll < 95) {
                page.browseSelectedPlaylistIndex = testCase.below(31);
                what = "browse";
            }
            done[what] = (done[what] || 0) + 1;
            if (probe !== null && list && watchedLists.indexOf(list) < 0 && combo.popup.visible) {
                if (probe.watch(list)) {
                    watchedLists.push(list);
                }
            }
            check(step, what);
            const settle = testCase.below(10);
            if (settle < 3) {
            } else if (settle < 7) {
                wait(0);
            } else if (settle < 9) {
                waitForRendering(page);
            } else {
                wait(testCase.below(40));
            }
        }
        combo.popup.close();
        console.log("matching storm seed " + seed + ": " + JSON.stringify(done)
                    + ", most unrequested rows held: " + unrequestedMax);
    }

    function test_theMatchingPlaylistListOnItsOwn() {
        matchingStorm(6, walk(1000));
    }
    function test_zzUnprobed() {
        matchingStorm(6, walk(1000), true);
    }
}
