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

    // Shorter than the real delay, so the tests do not wait it out, and
    // long enough that "not yet" can be seen.
    readonly property int delay: 400

    readonly property var sampleSummary: ({
        count: 42,
        keys: [{camelot: "1A", key: "Abm", count: 2}, {camelot: "3B", key: "Db", count: 5},
               {camelot: "5B", key: "Eb", count: 3}, {camelot: "8A", key: "Am", count: 18},
               {camelot: "9A", key: "Em", count: 12}, {camelot: "", key: "", count: 2}],
        bpmMin: 122, bpmMax: 128.5, totalDurationSeconds: 3723.4,
        bpmBins: [{bpm: 122, count: 2}, {bpm: 123, count: 0}, {bpm: 124, count: 10}, {bpm: 125, count: 18},
                  {bpm: 126, count: 6}, {bpm: 127, count: 4}, {bpm: 128, count: 2}],
        bpmBinWidth: 1, bpmPeak: 125,
        longest: {title: "Strings Of Life", artist: "Rhythim Is Rhythim", durationSeconds: 492},
        shortest: {title: "Interlude", artist: "Someone", durationSeconds: 95},
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

    // The list a row is in (rows sit in the list's contentItem).
    function listOf(item) {
        let at = item;
        while (at !== null && at.summaryShown === undefined) {
            at = at.parent;
        }
        verify(at !== null, "a list above " + item);
        return at;
    }
    // The list's one summary popup's loader, for a row in it.
    function loaderOf(r) {
        return findChild(listOf(r), "summaryLoader");
    }
    // Whether the list shows its summary (not counting one fading out).
    function shown(loader) {
        return listOf(loader).summaryShown;
    }

    function hoverOpen(r) {
        mouseMove(r, r.width / 2, r.height / 2);
        const loader = loaderOf(r);
        tryVerify(() => shown(loader) && loader.item && loader.item.opened, 3000, "the summary opens");
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
        const loader = loaderOf(r);
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay / 3);
        verify(!shown(loader) && loader.item === null, "not before the delay");
        compare(t.fake.asked.length, 0, "nothing worked out for a row merely hovered");
        tryVerify(() => shown(loader) && loader.item && loader.item.opened, 3000, "open after it");
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
        const loader = loaderOf(r);
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay / 3);
        leave();
        wait(testCase.delay * 2);
        verify(!shown(loader));
    }

    // Off every row and out of the popup: closed once the grace is over,
    // not before.
    function test_closesWhenThePointerLeavesTheRows() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        leave();
        wait(60);
        compare(shown(loader), true, "still there within the grace");
        tryVerify(() => !shown(loader), 1000, "closed after it");
        tryCompare(loader.item, "visible", false, 2000, "and gone once it has faded");
    }

    // ---- One popup, moved from row to row ----

    function countNamed(item, name) {
        let n = item.objectName === name ? 1 : 0;
        for (let i = 0; i < item.children.length; ++i) {
            n += countNamed(item.children[i], name);
        }
        return n;
    }

    // The list carries one popup however many rows it has, made the
    // first time it is needed and kept: every row it opens beside is
    // shown by that same one.
    function test_oneSummaryPopupForTheWholeList() {
        const names = [];
        for (let i = 0; i < 20; ++i) {
            names.push("Playlist " + i);
        }
        const t = makeList(names);
        compare(countNamed(t.list, "summaryLoader"), 1, "one loader, in the list");
        const loader = findChild(t.list, "summaryLoader");
        compare(loader.item, null, "nothing made before it is needed");
        hoverOpen(row(t.list, 1));
        const popup = loader.item;
        verify(popup !== null);
        leave();
        tryCompare(popup, "visible", false, 2000);
        compare(loader.item, popup, "kept once closed");
        hoverOpen(row(t.list, 4));
        compare(loader.item, popup, "the same popup beside another row");
        compare(text(popup, "summaryTitle"), "Playlist 3");
        compare(countNamed(t.list, "summaryLoader"), 1);
    }

    // With it open, another row held for the switch delay moves the popup
    // there and gives it that playlist, in place: no close, the details
    // still shown if they were. Passed over quicker than that, a row
    // changes nothing.
    function test_anotherRowSwitchesItInPlace() {
        const names = [];
        for (let i = 0; i < 12; ++i) {
            names.push("Playlist " + i);
        }
        const t = makeList(names);
        t.list.height = 560;
        waitForRendering(t.list);
        compare(t.list.summarySwitchDelay, 250);
        const loader = hoverOpen(row(t.list, 6));
        const popup = loader.item;
        let closes = 0;
        popup.aboutToHide.connect(() => { ++closes; });
        // Into the popup, so its details show, then on down the list.
        const p = popupPoint(loader);
        mouseMove(p.item, p.x, p.y);
        tryCompare(popup, "expanded", true);
        const yBefore = popup.y;
        t.fake.asked = [];
        // Rows 7 and 8 passed by, row 9 held.
        for (const i of [7, 8]) {
            const r = row(t.list, i);
            mouseMove(r, r.width / 2, r.height / 2);
            wait(80);
        }
        const target = row(t.list, 9);
        mouseMove(target, target.width / 2, target.height / 2);
        wait(100);
        compare(text(popup, "summaryTitle"), "Playlist 5", "not before the switch delay");
        tryVerify(() => text(popup, "summaryTitle") === "Playlist 8", 1000, "switched to the row held");
        compare(JSON.stringify(t.fake.asked), JSON.stringify(["Playlist 8"]), "rows passed by asked for nothing");
        compare(closes, 0, "switched without closing");
        compare(popup.visible, true);
        compare(popup.expanded, true, "the details still shown");
        tryVerify(() => popup.y > yBefore + 2 * target.height, 1000, "moved down to the row: " + yBefore + " to " + popup.y);
        leave();
        tryVerify(() => !shown(loader), 1000, "the grace still closes it");
    }

    // ---- Coming and going ----

    Component {
        id: samplerComponent
        Timer {
            property var target: null
            property string property: "opacity"
            property var seen: []
            interval: 5
            repeat: true
            running: true
            onTriggered: seen = seen.concat([target[property]])
        }
    }
    function sampled(target, prop) {
        return createTemporaryObject(samplerComponent, testCase, {target: target, property: prop});
    }
    function between(values) {
        return values.some(v => v > 0.05 && v < 0.95);
    }

    // It fades in rising and fades out sinking, never a blink; a switch
    // to another playlist fades the content, not the popup.
    function test_itFadesInAndOutAndCrossFadesASwitch() {
        const t = makeList(["Peak", "Warmup", "Closing"]);
        const r = row(t.list, 1);
        // Once open and closed, so the popup is there to watch from the
        // start of its next appearance.
        const loader = hoverOpen(r);
        const popup = loader.item;
        leave();
        tryCompare(popup, "visible", false, 2000);
        const opacity = sampled(popup, "opacity");
        const lift = sampled(popup, "lift");
        mouseMove(r, r.width / 2, r.height / 2);
        tryCompare(popup, "opened", true, 3000);
        wait(50);
        verify(between(opacity.seen), "opacity through values between: " + opacity.seen.join(" "));
        verify(lift.seen.some(v => v > 0.5) && lift.seen[lift.seen.length - 1] === 0, "a rise: " + lift.seen.join(" "));

        const content = sampled(popup.contentItem, "opacity");
        const whole = sampled(popup, "opacity");
        const next = row(t.list, 2);
        mouseMove(next, next.width / 2, next.height / 2);
        tryVerify(() => text(popup, "summaryTitle") === "Warmup", 2000);
        wait(Theme.shortTransitionDuration * 2);
        verify(content.seen.some(v => v < 0.5), "the content faded across: " + content.seen.join(" "));
        verify(whole.seen.every(v => v > 0.99), "the popup itself did not blink");
        compare(popup.contentItem.opacity, 1);

        const going = sampled(popup, "opacity");
        const sinking = sampled(popup, "lift");
        leave();
        tryCompare(popup, "visible", false, 2000);
        verify(between(going.seen), "faded out: " + going.seen.join(" "));
        verify(sinking.seen.some(v => v > 0.5), "and sank: " + sinking.seen.join(" "));
    }

    // ---- The pointer in the popup ----

    Component {
        id: defaultListComponent
        PlaylistListView { width: 240; height: 200 }
    }

    // The delay is a moment, not a wait: 600 ms unless a host says
    // otherwise.
    function test_theDelayIsShort() {
        const fake = createTemporaryObject(fakeScanComponent, testCase, {playlistNames: ["Peak"], totalTrackCount: 4});
        const list = createTemporaryObject(defaultListComponent, testCase, {scanController: fake});
        compare(list.summaryDelay, 600);
        waitForRendering(list);
        const r = row(list, 1);
        const loader = loaderOf(r);
        const started = Date.now();
        mouseMove(r, r.width / 2, r.height / 2);
        wait(300);
        verify(!shown(loader), "not at once");
        tryVerify(() => shown(loader), 2000, "open after the delay");
        verify(Date.now() - started < 1000, "within the second: " + (Date.now() - started) + " ms");
    }

    function popupPoint(loader) {
        const content = loader.item.contentItem;
        return {item: content, x: content.width / 2, y: Theme.scaled(20)};
    }

    // Into the popup: it stays, however long the pointer rests there,
    // and shows what its first look leaves out, every key and the
    // longest and shortest track, below that first look, which does not
    // move. Out of both, it closes.
    function test_thePointerMayMoveIntoThePopup() {
        // A row well down a tall list, so the popup stands free of the
        // window's edges and would move if it were centred on the row as
        // it grew.
        const names = [];
        for (let i = 0; i < 14; ++i) {
            names.push("Playlist " + i);
        }
        const t = makeList(names);
        t.list.height = 560;
        waitForRendering(t.list);
        const r = row(t.list, 9);
        const loader = hoverOpen(r);
        const popup = loader.item;
        const title = findChild(popup.contentItem, "summaryTitle");
        const titleAt = title.mapToItem(testCase, 0, 0);
        verify(!findChild(popup.contentItem, "allKeys").visible, "the first look as it was");
        verify(findChild(popup.contentItem, "keyLegendMore").visible);
        const p = popupPoint(loader);
        mouseMove(p.item, p.x, p.y);
        wait(listOf(r).summaryGrace * 3);
        compare(shown(loader), true, "still open with the pointer in it");
        compare(popup.hovered, true);
        tryVerify(() => findChild(popup.contentItem, "allKeys").visible, 1000, "the details shown");
        const entries = [];
        for (const child of findChild(popup.contentItem, "allKeys").children) {
            if (child.objectName === "allKeysEntry") {
                entries.push(child.label);
            }
        }
        compare(entries.join(", "), "1A 2, 3B 5, 5B 3, 8A 18, 9A 12, no key 2", "every key, in wheel order");
        verify(popup.height > 0);
        verify(!findChild(popup.contentItem, "keyLegendMore").visible, "no \"and N more\" once they are all there");
        compare(findChild(popup.contentItem, "longestTrack").text, "Strings Of Life, Rhythim Is Rhythim (8:12)");
        compare(findChild(popup.contentItem, "shortestTrack").text, "Interlude, Someone (1:35)");
        waitForRendering(popup.contentItem);
        const titleNow = title.mapToItem(testCase, 0, 0);
        compare(Math.round(titleNow.y), Math.round(titleAt.y), "the first look stays where it was");
        compare(Math.round(titleNow.x), Math.round(titleAt.x));
        saveScreenshot(testCase.Window.contentItem, "playlist-summary-popup-expanded");
        // Back to the row: still open.
        mouseMove(r, r.width / 2, r.height / 2);
        wait(listOf(r).summaryGrace * 2);
        compare(shown(loader), true, "back on the row");
        leave();
        tryVerify(() => !shown(loader), 1000, "closed");
    }

    // The gap between row and popup is crossed without the popup closing:
    // it waits summaryGrace with the pointer on neither.
    function test_crossingTheGapKeepsItOpen() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        compare(listOf(r).summaryGrace, 200);
        // In the gap: right of the row, left of the popup.
        const popupLeft = loader.item.contentItem.mapToItem(r, 0, 0).x - loader.item.leftPadding;
        verify(popupLeft > r.width + 1, "a gap to stand in");
        mouseMove(r, (r.width + popupLeft) / 2, r.height / 2);
        wait(60);
        compare(shown(loader), true, "still open in the gap");
        tryVerify(() => !shown(loader), 1000, "closed once the grace is over");
    }

    // A press in the popup does not close it, nor does the wheel there,
    // which scrolls nothing under it either.
    Component {
        id: underComponent
        Flickable {
            x: 250; y: 0; width: 600; height: 600
            contentHeight: 3000
            Rectangle { width: 600; height: 3000; color: "transparent" }
        }
    }
    function test_pressAndWheelInThePopupKeepIt() {
        const under = createTemporaryObject(underComponent, testCase);
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        const p = popupPoint(loader);
        mouseMove(p.item, p.x, p.y);
        mousePress(p.item, p.x, p.y);
        mouseRelease(p.item, p.x, p.y);
        wait(listOf(r).summaryGrace * 2);
        compare(shown(loader), true, "a press in it keeps it");
        mouseWheel(p.item, p.x, p.y, 0, -120);
        wait(listOf(r).summaryGrace * 2);
        compare(shown(loader), true, "the wheel in it keeps it");
        compare(under.contentY, 0, "and scrolls nothing under it");
        compare(t.list.contentY, 0);
    }

    function test_closesOnAPressAndTheClickStillPicks() {
        const t = makeList();
        const r = row(t.list, 1);
        const loader = hoverOpen(r);
        let picked = "";
        t.list.playlistPicked.connect((index, name) => { picked = name; });
        mousePress(r, r.width / 2, r.height / 2);
        compare(shown(loader), false, "closed on the press");
        mouseRelease(r, r.width / 2, r.height / 2);
        compare(picked, "Peak");
        wait(testCase.delay * 2);
        compare(shown(loader), false, "and not opened again while the pointer stays");
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
        compare(shown(loader), false, "closed on the wheel");
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
        compare(shown(loader), false);
        leave();
        mouseMove(r, r.width / 2, r.height / 2);
        wait(testCase.delay * 2);
        compare(shown(loader), false, "not while switched off");
    }

    // A row has no popup of its own: the Matching page's combo shows
    // these rows in a popup of its own, and hovering one there opens
    // nothing.
    Component {
        id: bareRowComponent
        PlaylistRowDelegate { index: 1; width: 240; name: "Peak" }
    }
    function test_aBareRowShowsNoSummary() {
        const r = createTemporaryObject(bareRowComponent, testCase, {y: 20});
        waitForRendering(r);
        mouseMove(r, r.width / 2, r.height / 2);
        compare(r.rowHovered, true);
        wait(800);
        verify(findChild(r, "summaryLoader") === null, "no loader in a row");
        verify(findChild(r, "playlistSummaryPopup") === null, "no popup in a row");
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
                                            Object.assign({title: "Peak Time", summary: summary,
                                                           x: anchor.x + anchor.width + Theme.tightSpacing,
                                                           rowCenter: anchor.y + anchor.height / 2}, props || {}));
        popup.open();
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
        compare(text(popup, "bpmMinLabel"), "122");
        compare(text(popup, "bpmMaxLabel"), "128.5");
        compare(text(popup, "bpmPeakLabel"), "Most at 125");
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
        popup.summary = Object.assign({}, popup.summary, {bpmBins: [{bpm: 124, count: 1}], bpmBinWidth: 1, bpmPeak: 124});
        compare(text(popup, "bpmMinLabel"), "124");
        verify(!findChild(popup.contentItem, "bpmMaxLabel").visible, "one value, said once");
        verify(!findChild(popup.contentItem, "bpmPeakLabel").visible, "one bin has no busiest");
        compare(bins(popup).length, 1, "a single bin");
        compare(legend(popup), "no key 1");
        verify(!findChild(popup.contentItem, "keyLegendMore").visible);

        popup.summary = {count: 0, keys: [], bpmMin: 0, bpmMax: 0, totalDurationSeconds: 0, withCues: 0,
                         withoutCues: 0, artwork: []};
        compare(text(popup, "summaryCount"), "0 tracks");
        verify(!findChild(popup.contentItem, "summaryDuration").visible);
        verify(!findChild(popup.contentItem, "summaryCues").parent.visible, "no figures for an empty playlist");
        popup.summary = {count: 2, keys: [], bpmMin: 0, bpmMax: 0, bpmBins: [], totalDurationSeconds: 0,
                         withCues: 0, withoutCues: 2, artwork: []};
        verify(!findChild(popup.contentItem, "bpmHeat").visible, "no BPM, no strip");
        verify(findChild(popup.contentItem, "neutralCover").visible);
    }

    function bins(popup) {
        const strip = findChild(popup.contentItem, "bpmStrip");
        return strip.children.filter(child => child.objectName === "bpmBin");
    }

    // The strip: a bin per BPM from the lowest to the highest, empty
    // bins in the ground, the fullest in the accent, the rest between and
    // darker or lighter by how full they are. Sampled from the screen.
    function test_theBpmStripIsAHeatmap() {
        const popup = makePopup(testCase.sampleSummary);
        const strip = findChild(popup.contentItem, "bpmStrip");
        compare(Math.round(strip.width), Math.round(Theme.scaled(160)));
        compare(Math.round(strip.height), Math.round(Theme.scaled(12)));
        const shown = bins(popup);
        compare(shown.length, 7);
        waitForRendering(popup.contentItem);
        const ground = testCase.Window.contentItem;
        const image = grabImage(ground);
        const scale = image.width / ground.width;
        const near = (a, b) => Math.abs(a.r - b.r) < 0.03 && Math.abs(a.g - b.g) < 0.03 && Math.abs(a.b - b.b) < 0.03;
        const dist = (a, b) => Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);
        const sampled = [];
        for (let i = 0; i < shown.length; ++i) {
            const bin = shown[i];
            verify(bin.width >= 1, "bin " + i + " is drawn");
            const at = bin.mapToItem(ground, bin.width / 2, bin.height / 2);
            sampled.push(image.pixel(Math.round(at.x * scale), Math.round(at.y * scale)));
        }
        const counts = testCase.sampleSummary.bpmBins.map(b => b.count);
        // An empty bin is drawn, faint but there: the strip is one bar.
        verify(near(sampled[1], popup.emptyBinColor), "123, empty, in the faint tone: " + sampled[1]);
        verify(dist(sampled[1], Theme.surface) > 0.12, "and told apart from the card it sits on: " + sampled[1]);
        verify(near(sampled[3], Theme.accent), "125, the fullest, is the accent: " + sampled[3]);
        for (let i = 0; i < counts.length; ++i) {
            if (counts[i] > 0 && counts[i] < 18) {
                verify(!near(sampled[i], popup.emptyBinColor) && !near(sampled[i], Theme.accent),
                       "bin " + i + " between the two: " + sampled[i]);
            }
            for (let j = 0; j < counts.length; ++j) {
                if (counts[i] > counts[j]) {
                    verify(dist(sampled[i], Theme.accent) < dist(sampled[j], Theme.accent),
                           "fuller bin " + i + " nearer the accent than bin " + j);
                }
            }
        }
        // Left to right in BPM order: the bins stand side by side.
        for (let i = 1; i < shown.length; ++i) {
            compare(shown[i].x, shown[i - 1].x + shown[i - 1].width, "bin " + i + " follows bin " + (i - 1));
        }
        compare(shown[shown.length - 1].x + shown[shown.length - 1].width, strip.width, "the last bin ends the strip");

        // Two BPM to a bin: the busiest says its range.
        popup.summary = Object.assign({}, testCase.sampleSummary,
                                      {bpmMin: 90, bpmMax: 140, bpmBinWidth: 2, bpmPeak: 124,
                                       bpmBins: [{bpm: 90, count: 1}, {bpm: 124, count: 5}, {bpm: 140, count: 2}]});
        compare(text(popup, "bpmPeakLabel"), "Most at 124 to 125");
        compare(text(popup, "bpmMinLabel"), "90");
        compare(text(popup, "bpmMaxLabel"), "140");
        // A single bin is full: the accent across the strip.
        popup.summary = Object.assign({}, testCase.sampleSummary,
                                      {bpmMin: 124, bpmMax: 124.04, bpmPeak: 124, bpmBins: [{bpm: 124, count: 3}]});
        waitForRendering(popup.contentItem);
        const one = bins(popup)[0];
        compare(Math.round(one.width), Math.round(strip.width));
        const oneAt = one.mapToItem(ground, one.width / 2, one.height / 2);
        const onePixel = grabImage(ground).pixel(Math.round(oneAt.x * scale), Math.round(oneAt.y * scale));
        verify(near(onePixel, Theme.accent), "one full bin, in the accent: " + onePixel);
        verify(!findChild(popup.contentItem, "bpmMaxLabel").visible, "124 and 124.04 read as one value");
    }

    // The pointer on the strip: the bin under it outlined, and a label
    // above the pointer with its tempo and count, following the pointer
    // bin to bin and gone once it leaves the strip. Resting there keeps
    // the popup open.
    function test_hoveringTheStripNamesTheBin() {
        const t = makeList();
        const loader = hoverOpen(row(t.list, 1));
        const popup = loader.item;
        const strip = findChild(popup.contentItem, "bpmStrip");
        const outline = findChild(popup.contentItem, "bpmBinOutline");
        const label = findChild(popup.contentItem, "bpmHoverLabel");
        const binWidth = strip.width / 7;
        verify(!label.visible && !outline.visible);
        const cases = [{bin: 0, text: "122, 2 tracks"}, {bin: 1, text: "123, none"}, {bin: 3, text: "125, 18 tracks"}];
        for (const c of cases) {
            const x = (c.bin + 0.5) * binWidth;
            mouseMove(strip, x, strip.height / 2);
            tryCompare(label, "text", c.text, 1000);
            verify(label.visible && outline.visible, "shown over bin " + c.bin);
            const shownBin = bins(popup)[c.bin];
            compare(outline.x, shownBin.x, "the outline on bin " + c.bin);
            compare(outline.width, shownBin.width);
            const mid = label.mapToItem(strip, label.width / 2, label.height).x;
            verify(Math.abs(mid - x) <= 1, "the label over the pointer: " + mid + " for " + x);
            verify(label.mapToItem(strip, 0, label.height).y <= 0, "above the strip");
        }
        compare(shown(loader), true, "resting on the strip is resting in the popup");
        const one = bins(popup)[6];
        mouseMove(strip, one.x + one.width / 2, strip.height / 2);
        tryCompare(label, "text", "128, 2 tracks", 1000);
        popup.summary = Object.assign({}, testCase.sampleSummary, {bpmBinWidth: 2, bpmBins: [{bpm: 124, count: 1}]});
        mouseMove(strip, strip.width / 2, strip.height / 2);
        tryCompare(label, "text", "124 to 125, 1 track", 1000);
        const p = popupPoint(loader);
        mouseMove(p.item, p.x, p.y);
        tryVerify(() => !label.visible && !outline.visible, 1000, "gone off the strip");
        compare(shown(loader), true);
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
            tryVerify(() => findChild(mosaic, "coverArt" + (data.tiles.length - 1)).reveal === 1);
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
        const bpms = [];
        let longest = 0;
        let shortest = 0;
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
                bpms.push({bpm: t.bpm, times: times});
            }
            if (t.durationSeconds > 0) {
                longest = Math.max(longest, t.durationSeconds);
                shortest = shortest > 0 ? Math.min(shortest, t.durationSeconds) : t.durationSeconds;
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
                seconds: seconds, withCues: withCues, artwork: artwork, bpms: bpms, longest: longest,
                shortest: shortest};
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
        // The heatmap's bins, counted again from the rows.
        if (want.bpmMax > 0) {
            const w = want.bpmMax - want.bpmMin > 40 ? 2 : 1;
            compare(got.bpmBinWidth, w, what + ": bin width");
            const first = Math.floor(want.bpmMin / w);
            const last = Math.floor(want.bpmMax / w);
            const counts = new Array(last - first + 1).fill(0);
            for (const b of want.bpms) {
                counts[Math.floor(b.bpm / w) - first] += b.times;
            }
            compare(got.bpmBins.map(b => b.bpm + ":" + b.count).join(" "),
                    counts.map((c, i) => (first + i) * w + ":" + c).join(" "), what + ": BPM bins");
            let peak = 0;
            counts.forEach((c, i) => { if (c > counts[peak]) peak = i; });
            compare(got.bpmPeak, (first + peak) * w, what + ": the busiest bin");
        } else {
            compare(got.bpmBins.length, 0, what + ": no bins without a BPM");
        }
        fuzzyCompare(got.longest.durationSeconds || 0, want.longest, 0.001, what + ": longest");
        fuzzyCompare(got.shortest.durationSeconds || 0, want.shortest, 0.001, what + ": shortest");
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
        tryVerify(() => !shown(loader), 1000, "closed");
        const first = scan.playlistNames[0];
        loader = hoverOpen(row(list, 1));
        compare(text(loader.item, "summaryTitle"), first);
        compare(text(loader.item, "summaryCount"), scan.playlistSummary(first).count + " tracks");
    }
}
