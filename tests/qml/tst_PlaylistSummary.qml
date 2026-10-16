// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// The summary a playlist row in Browse's sidebar shows on hover: when it
// opens and closes (against a stand-in controller), what it shows, the
// cover mosaic for every number of covers, and ScanController's summary
// itself on a copy of the anonymized fixture, checked against the rows
// the list serves for the same playlist.
TestCase {
    id: testCase
    name: "PlaylistSummary"
    width: 900
    height: 600
    visible: true
    when: windowShown

    // Short, so the tests do not wait the real second and a half, and
    // long enough that "not yet" can be seen.
    readonly property int delay: 400

    readonly property var sampleSummary: ({
        count: 42,
        keys: [{camelot: "1A", key: "Abm", count: 2}, {camelot: "3B", key: "Db", count: 5},
               {camelot: "5B", key: "Eb", count: 3}, {camelot: "8A", key: "Am", count: 18},
               {camelot: "9A", key: "Em", count: 12}, {camelot: "", key: "", count: 2}],
        bpmMin: 122, bpmMax: 128.5, totalDurationSeconds: 3723.4,
        withCues: 30, withoutCues: 12, artwork: []})

    Component {
        id: fakeScanComponent
        QtObject {
            property var playlistNames: []
            property var playlistTrackCounts: ({})
            property int totalTrackCount: 0
            property var asked: []
            property var summary: testCase.sampleSummary
            function playlistSummary(name) {
                asked = asked.concat([name]);
                return summary;
            }
        }
    }

    Component {
        id: listComponent
        PlaylistListView {
            width: 240
            height: 200
            summaryDelay: testCase.delay
        }
    }

    Component {
        id: fieldComponent
        TextField { x: 500; y: 20; width: 200 }
    }

    function makeList(names) {
        const fake = createTemporaryObject(fakeScanComponent, testCase, {
            playlistNames: names || ["Peak", "Warmup"], totalTrackCount: 4});
        const list = createTemporaryObject(listComponent, testCase, {scanController: fake});
        verify(list !== null);
        waitForRendering(list);
        return {list: list, fake: fake};
    }

    function row(list, index) {
        const item = list.itemAtIndex(index);
        verify(item !== null, "row " + index);
        return item;
    }

    function hoverOpen(r) {
        mouseMove(r, r.width / 2, r.height / 2);
        const loader = findChild(r, "summaryLoader");
        tryVerify(() => loader.active && loader.item && loader.item.opened, 3000, "the summary opens");
        return loader;
    }

    function leave() {
        // Off the list, onto the empty right half of the window.
        mouseMove(testCase, 700, 500);
    }

    function cleanup() {
        leave();
        // The held library, if a test used it.
        browseFixture.restore();
    }

    // ---- When it opens and closes ----

    function test_opensBesideTheRowOnlyAfterTheDelay() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = findChild(r, "summaryLoader");
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay / 3);
        verify(!loader.active, "not before the delay");
        compare(t.fake.asked.length, 0, "nothing worked out for a row merely hovered");
        tryVerify(() => loader.active && loader.item && loader.item.opened, 3000, "open after it");
        compare(JSON.stringify(t.fake.asked), JSON.stringify(["Peak"]));
        const popup = loader.item;
        compare(findChild(popup.contentItem, "summaryTitle").text, "Peak");
        compare(findChild(popup.contentItem, "summaryCount").text, "42 tracks");
        // Beside the row, not over it.
        const at = popup.contentItem.mapToItem(r, 0, 0);
        verify(at.x >= r.width, "right of the row: " + at.x);
    }

    // "All tracks" asks for the whole library.
    function test_allTracksSummarisesTheLibrary() {
        const t = makeList();
        const loader = hoverOpen(row(t.list, 0));
        compare(JSON.stringify(t.fake.asked), JSON.stringify([""]));
        compare(findChild(loader.item.contentItem, "summaryTitle").text, "All tracks");
    }

    function test_leavingBeforeTheDelayOpensNothing() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = findChild(r, "summaryLoader");
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay / 3);
        leave();
        wait(testCase.delay * 2);
        verify(!loader.active);
    }

    function test_closesWhenThePointerLeavesTheRow() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        // Onto the next row: still the list, not this row.
        const next = row(t.list, 2);
        mouseMove(next, next.width / 2, next.height / 2);
        tryCompare(loader, "active", false, testCase.delay / 2);
    }

    function test_closesOnAPressAndTheClickStillPicks() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        let picked = "";
        t.list.playlistPicked.connect((index, name) => { picked = name; });
        mousePress(r, r.width / 2, r.height / 2);
        compare(loader.active, false, "closed on the press");
        mouseRelease(r, r.width / 2, r.height / 2);
        compare(picked, "Peak");
        wait(testCase.delay * 2);
        compare(loader.active, false, "and not opened again while the pointer stays");
    }

    function test_closesOnTheWheelAndTheListStillScrolls() {
        const names = [];
        for (let i = 0; i < 30; ++i) {
            names.push("Playlist " + i);
        }
        const t = makeList(names);
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        compare(t.list.contentY, 0);
        mouseWheel(r, r.width / 2, r.height / 2, 0, -120);
        compare(loader.active, false, "closed on the wheel");
        tryVerify(() => t.list.contentY > 0, 2000, "the list scrolled");
    }

    function test_neverTakesFocus() {
        const field = createTemporaryObject(fieldComponent, testCase);
        field.forceActiveFocus();
        verify(field.activeFocus);
        const t = makeList();
        const loader = hoverOpen(row(t.list, 1));
        verify(field.activeFocus, "the field keeps the focus");
        verify(!loader.item.activeFocus);
    }

    // The switch a drag will use: nothing opens while it is off, and one
    // open closes when it goes off.
    function test_switchedOffItStaysShut() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        t.list.summariesEnabled = false;
        compare(loader.active, false);
        leave();
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay * 2);
        compare(loader.active, false, "not while switched off");
    }

    // Off unless the host turns it on: the Matching page's combo shows
    // these rows in a popup of its own and never does.
    Component {
        id: bareRowComponent
        PlaylistRowDelegate { index: 1; width: 240; name: "Peak"; summaryDelay: testCase.delay }
    }
    function test_aBareRowShowsNoSummary() {
        const r = createTemporaryObject(bareRowComponent, testCase, {y: 20});
        let asked = 0;
        r.summaryFor = () => { ++asked; return testCase.sampleSummary; };
        waitForRendering(r);
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay * 2);
        compare(findChild(r, "summaryLoader").active, false);
        compare(asked, 0);
    }

    // ---- What it shows ----

    Component {
        id: anchorComponent
        Rectangle { x: 20; y: 120; width: 240; height: 32; color: Theme.rowEven }
    }
    Component {
        id: popupComponent
        PlaylistSummaryPopup {}
    }

    function makePopup(summary, props) {
        const anchor = createTemporaryObject(anchorComponent, testCase);
        const popup = createTemporaryObject(popupComponent, testCase,
                                            Object.assign({row: anchor, title: "Peak Time", summary: summary}, props || {}));
        tryCompare(popup, "opened", true);
        return popup;
    }

    function text(popup, name) {
        return findChild(popup.contentItem, name).text;
    }

    function legend(popup) {
        const out = [];
        const column = findChild(popup.contentItem, "keyLegend");
        for (const child of column.children) {
            if (child.objectName === "keyLegendEntry") {
                out.push(findChild(child, "keyLegendKey").text + " " + findChild(child, "keyLegendCount").text);
            }
        }
        return out.join(", ");
    }

    function test_showsCountDurationBpmCuesAndKeys() {
        const popup = makePopup(testCase.sampleSummary);
        compare(text(popup, "summaryTitle"), "Peak Time");
        compare(text(popup, "summaryCount"), "42 tracks");
        compare(text(popup, "summaryDuration"), "1:02:03");
        compare(text(popup, "summaryBpm"), "122 to 128.5");
        compare(text(popup, "summaryCues"), "30 with, 12 without");
        // The commonest four, then how many more.
        compare(legend(popup), "8A 18, 9A 12, 3B 5, 5B 3");
        compare(text(popup, "keyLegendMore"), "and 2 more");
        popup.notation = "traditional";
        compare(legend(popup), "Am 18, Em 12, D♭ 5, E♭ 3");
    }

    function test_smallAndEmptyPlaylistsReadRight() {
        const popup = makePopup({count: 1, keys: [{camelot: "", key: "", count: 1}], bpmMin: 124, bpmMax: 124,
                                 totalDurationSeconds: 433, withCues: 0, withoutCues: 1, artwork: []});
        compare(text(popup, "summaryCount"), "1 track");
        compare(text(popup, "summaryDuration"), "7:13");
        compare(text(popup, "summaryBpm"), "124");
        compare(legend(popup), "no key 1");
        verify(!findChild(popup.contentItem, "keyLegendMore").visible);

        popup.summary = {count: 0, keys: [], bpmMin: 0, bpmMax: 0, totalDurationSeconds: 0, withCues: 0,
                         withoutCues: 0, artwork: []};
        compare(text(popup, "summaryCount"), "0 tracks");
        verify(!findChild(popup.contentItem, "summaryDuration").visible);
        verify(!findChild(popup.contentItem, "summaryBpm").parent.visible, "no figures for an empty playlist");
        verify(findChild(popup.contentItem, "neutralCover").visible);
    }

    // Each key's slice in its Camelot colour, round from twelve o'clock in
    // the order given; the keys without one in the muted tone.
    function test_thePieIsPaintedInTheWheelColours() {
        const popup = makePopup(testCase.sampleSummary);
        const pie = findChild(popup.contentItem, "keyPie");
        // "" when every slice is where it should be, what is wrong if not.
        // Grabbed with the window: a Canvas grabbed on its own comes back
        // blank.
        const ground = testCase.Window.contentItem;
        const check = () => {
            const image = grabImage(ground);
            const scale = image.width / ground.width;
            const total = 42;
            let from = 0;
            for (const entry of testCase.sampleSummary.keys) {
                const mid = from + entry.count / 2;
                from += entry.count;
                const angle = -Math.PI / 2 + 2 * Math.PI * mid / total;
                const r = pie.width / 2 * 0.75;
                const at = pie.mapToItem(ground, pie.width / 2 + r * Math.cos(angle), pie.height / 2 + r * Math.sin(angle));
                const px = Math.round(at.x * scale);
                const py = Math.round(at.y * scale);
                const got = image.pixel(px, py);
                const want = Theme.colorForKey(entry.camelot);
                if (Math.abs(got.r - want.r) > 0.05 || Math.abs(got.g - want.g) > 0.05 || Math.abs(got.b - want.b) > 0.05) {
                    return (entry.camelot || "no key") + " at " + px + "," + py + ": got " + got + ", want " + want;
                }
            }
            return "";
        };
        // A Canvas paints a frame or more after it is asked to.
        let problem = check();
        for (let tries = 0; problem !== "" && tries < 30; ++tries) {
            wait(100);
            problem = check();
        }
        compare(problem, "");
    }

    // ---- The cover mosaic ----

    readonly property var coverColours: ["#d03030", "#30a040", "#3060d0", "#e0c020", "#a030c0"]

    Component {
        id: mosaicComponent
        PlaylistCoverMosaic { x: 20; y: 20; width: 96; height: 96 }
    }

    function covers(n) {
        const out = [];
        for (let i = 0; i < n; ++i) {
            out.push({source: browseFixture.colouredArtworkUrl(testCase.coverColours[i]), fallbackSource: ""});
        }
        return out;
    }

    function test_mosaic_data() {
        // Each tile as x, y, w, h in quarters of the square.
        return [
            {tag: "none", n: 0, tiles: []},
            {tag: "one", n: 1, tiles: [[0, 0, 4, 4]]},
            {tag: "two", n: 2, tiles: [[0, 0, 2, 4], [2, 0, 2, 4]]},
            {tag: "three", n: 3, tiles: [[0, 0, 2, 4], [2, 0, 2, 2], [2, 2, 2, 2]]},
            {tag: "four", n: 4, tiles: [[0, 0, 2, 2], [2, 0, 2, 2], [0, 2, 2, 2], [2, 2, 2, 2]]},
            {tag: "five", n: 5, tiles: [[0, 0, 2, 2], [2, 0, 2, 2], [0, 2, 2, 2], [2, 2, 2, 2]]},
        ];
    }

    function test_mosaic(data) {
        const mosaic = createTemporaryObject(mosaicComponent, testCase, {covers: covers(data.n)});
        compare(mosaic.tileCount, data.tiles.length);
        compare(findChild(mosaic, "neutralCover").visible, data.n === 0, "the neutral tile only without art");
        const q = mosaic.width / 4;
        for (let i = 0; i < data.tiles.length; ++i) {
            const tile = findChild(mosaic, "coverTile" + i);
            const [x, y, w, h] = data.tiles[i];
            // Within the gap of where the quarters put it.
            verify(Math.abs(tile.x - x * q) <= Theme.chartSliceGap && Math.abs(tile.width - w * q) <= Theme.chartSliceGap
                   && Math.abs(tile.y - y * q) <= Theme.chartSliceGap && Math.abs(tile.height - h * q) <= Theme.chartSliceGap,
                   "tile " + i + ": " + tile.x + "," + tile.y + " " + tile.width + "x" + tile.height);
            tryCompare(findChild(mosaic, "coverArt" + i), "showing", "source");
        }
        if (data.n > 0) {
            // Each tile shows its own cover: sample its middle.
            tryVerify(() => findChild(findChild(mosaic, "coverArt" + (data.tiles.length - 1)), "artworkImage").opacity === 1);
            waitForRendering(mosaic);
            const image = grabImage(mosaic);
            const scale = image.width / mosaic.width;
            for (let i = 0; i < data.tiles.length; ++i) {
                const tile = findChild(mosaic, "coverTile" + i);
                const got = image.pixel(Math.round((tile.x + tile.width / 2) * scale),
                                        Math.round((tile.y + tile.height / 2) * scale));
                const want = Qt.color(testCase.coverColours[i]);
                verify(Math.abs(got.r - want.r) < 0.05 && Math.abs(got.g - want.g) < 0.05 && Math.abs(got.b - want.b) < 0.05,
                       "tile " + i + ": got " + got + ", want " + want);
            }
        }
        saveScreenshot(mosaic, "playlist-cover-mosaic-" + data.tag);
    }

    function saveScreenshot(item, name) {
        if (!screenshotDir || screenshotDir.length === 0) {
            return;
        }
        waitForRendering(item);
        grabImage(item).save(screenshotDir + "/" + name + ".png");
    }

    // The whole popup as it shows beside a list, for looking at.
    function test_screenshot_data() {
        return [{tag: "art", n: 4}, {tag: "three", n: 3}, {tag: "noart", n: 0}];
    }
    function test_screenshot(data) {
        const t = makeList(["Peak Time", "Warmup", "Closing"]);
        const summary = Object.assign({}, testCase.sampleSummary, {artwork: covers(data.n)});
        t.fake.summary = summary;
        const loader = hoverOpen(row(t.list, 1));
        const pie = findChild(loader.item.contentItem, "keyPie");
        tryVerify(() => pie.paintedCount > 0, 3000);
        wait(300);  // the enter fade and the covers' own
        saveScreenshot(testCase.Window.contentItem, "playlist-summary-popup-" + data.tag);
    }

    // ---- ScanController.playlistSummary() on real data ----

    Component {
        id: scanControllerComponent
        ScanController {}
    }

    // tests/qml/ -> tests/fixtures/anonymized_library (see tst_JunkCuePage).
    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // What the summary should say, worked out again from the rows the
    // list serves for the playlist (its own order).
    function expectedFromRows(scan, playlist) {
        const keys = {};
        let unknown = 0;
        let bpmMin = 0;
        let bpmMax = 0;
        let seconds = 0;
        let withCues = 0;
        const artwork = [];
        let count = 0;
        for (let i = 0; i < scan.tracks.trackCount(); ++i) {
            const t = scan.tracks.trackAt(i);
            // A row is listed once, however often the playlist holds it;
            // the summary counts it each time, as the sidebar does.
            const times = playlist.length > 0 ? t.playlistNames.filter(n => n === playlist).length : 1;
            count += times;
            const camelot = Theme.camelotLabel(t.key);
            if (camelot.length > 0) {
                keys[camelot] = (keys[camelot] || 0) + times;
            } else {
                unknown += times;
            }
            if (t.bpm > 0) {
                bpmMin = bpmMin > 0 ? Math.min(bpmMin, t.bpm) : t.bpm;
                bpmMax = Math.max(bpmMax, t.bpm);
            }
            seconds += times * t.durationSeconds;
            if (t.cueCount > 0) {
                withCues += times;
            }
            if (artwork.length < 4 && t.artworkPath.length > 0 && !artwork.some(a => a.source === t.artworkPath)) {
                artwork.push({source: t.artworkPath, fallbackSource: t.fallbackArtworkPath});
            }
        }
        return {count: count, keys: keys, unknown: unknown, bpmMin: bpmMin, bpmMax: bpmMax,
                seconds: seconds, withCues: withCues, artwork: artwork};
    }

    function checkSummary(scan, playlist) {
        scan.filterByPlaylist(playlist);
        const want = expectedFromRows(scan, playlist);
        const got = scan.playlistSummary(playlist);
        const what = playlist.length > 0 ? playlist : "the library";
        compare(got.count, want.count, what + ": count");
        compare(got.count, playlist.length > 0 ? scan.playlistTrackCounts[playlist] : scan.totalTrackCount,
                what + ": the sidebar's count");
        compare(got.withCues, want.withCues, what + ": with cues");
        compare(got.withoutCues, want.count - want.withCues, what + ": without cues");
        fuzzyCompare(got.bpmMin, want.bpmMin, 0.001, what + ": lowest BPM");
        fuzzyCompare(got.bpmMax, want.bpmMax, 0.001, what + ": highest BPM");
        fuzzyCompare(got.totalDurationSeconds, want.seconds, 0.01, what + ": duration");
        const art = list => list.map(a => a.source + " | " + a.fallbackSource).join(", ");
        compare(art(got.artwork), art(want.artwork), what + ": covers");
        // Keys: every wheel position once, in wheel order, no key last.
        const order = [];
        for (let n = 1; n <= 12; ++n) {
            for (const m of ["A", "B"]) {
                if (want.keys[n + m]) {
                    order.push(n + m + " " + want.keys[n + m]);
                }
            }
        }
        if (want.unknown > 0) {
            order.push(" " + want.unknown);
        }
        compare(got.keys.map(k => k.camelot + " " + k.count).join(", "), order.join(", "), what + ": keys");
        for (const k of got.keys) {
            compare(Theme.camelotLabel(k.key), k.camelot, "a spelling of its wheel position: " + k.key);
        }
        return got;
    }

    // A library made up for the purpose (BrowseFixture.holdCues): every
    // track has the same cover, so the summary names it once; the cues
    // arrive in a pass of their own and count once they have.
    function test_summaryOfAHeldLibrary() {
        browseFixture.holdCues(6, true);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 10000, "the list read");
        let summary = scan.playlistSummary("Held Reversed");
        compare(summary.count, 6);
        compare(summary.withCues, 0, "no cues before their pass");
        fuzzyCompare(summary.totalDurationSeconds, 200 + 201 + 202 + 203 + 204 + 205, 0.001);
        compare(summary.bpmMax, 0, "no track has a BPM");
        compare(summary.keys.map(k => "[" + k.camelot + "|" + k.key + "] " + k.count).join(", "), "[|] 6", "all without a key");
        compare(summary.artwork.length, 1, "one cover, however many tracks have it");
        compare(summary.artwork[0].source, browseFixture.presentArtworkUrl());
        browseFixture.releaseCues();
        tryCompare(scan, "cuesPending", false, 10000);
        // Track i has i % 5 cues: tracks 0 and 5 have none.
        summary = scan.playlistSummary("Held Playlist");
        compare(summary.withCues, 4);
        compare(summary.withoutCues, 2);
    }

    // A playlist staged to be renamed (ScanController.setPlaylistAliases):
    // its row, in grey italics under the new name, still opens the
    // summary, titled with the new name and counting the old playlist's
    // tracks.
    function test_aRenamedPlaylistKeepsItsSummary() {
        browseFixture.holdCues(6, true);
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("rekordbox", "/nonexistent/HELD/PIONEER");
        tryVerify(() => !scan.busy && scan.playlistNames.length > 0, 10000, "the list read");
        scan.setPlaylistAliases({"Held Reversed": "Held Renamed"});
        compare(scan.playlistSummary("Held Renamed").count, 6, "the old playlist's tracks");
        const list = createTemporaryObject(listComponent, testCase,
                                           {scanController: scan, isRenamed: name => name === "Held Renamed"});
        waitForRendering(list);
        const index = list.model.indexOf("Held Renamed");
        verify(index > 0, JSON.stringify(list.model));
        const r = row(list, index);
        verify(r.pendingRename, "drawn as a renamed row");
        const loader = hoverOpen(r);
        compare(text(loader.item, "summaryTitle"), "Held Renamed");
        compare(text(loader.item, "summaryCount"), "6 tracks");
        browseFixture.releaseCues();
    }

    function test_summaryOfARealLibrary() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const scan = createTemporaryObject(scanControllerComponent, testCase);
        scan.scan("rekordbox", stick + "/PIONEER");
        tryVerify(() => !scan.busy && !scan.cuesPending && scan.playlistNames.length > 0, 120000, "scanned");

        const all = checkSummary(scan, "");
        verify(all.count > 100 && all.keys.length > 12 && all.bpmMin > 0 && all.bpmMax > all.bpmMin
               && all.withCues > 0 && all.withoutCues > 0 && all.totalDurationSeconds > 3600,
               "a summary with something in it: " + JSON.stringify(all).slice(0, 400));
        let checked = 0;
        for (const name of scan.playlistNames) {
            if (scan.playlistTrackCounts[name] > 0) {
                checkSummary(scan, name);
                ++checked;
            }
        }
        verify(checked > 3, "playlists with tracks: " + checked);
        compare(scan.playlistSummary("No Such Playlist").count, 0);
        compare(scan.playlistSummary("No Such Playlist").keys.length, 0);
        compare(scan.playlistSummary("No Such Playlist").bpmMax, 0);

        // And through the sidebar: hovering a row asks for that playlist.
        scan.filterByPlaylist("");
        const list = createTemporaryObject(listComponent, testCase, {scanController: scan});
        waitForRendering(list);
        let loader = hoverOpen(row(list, 0));
        compare(text(loader.item, "summaryCount"), all.count + " tracks");
        leave();
        tryCompare(loader, "active", false);
        const first = scan.playlistNames[0];
        loader = hoverOpen(row(list, 1));
        compare(text(loader.item, "summaryTitle"), first);
        compare(text(loader.item, "summaryCount"), scan.playlistSummary(first).count + " tracks");
    }
}
