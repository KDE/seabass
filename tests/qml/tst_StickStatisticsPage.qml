// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "PixelScale.js" as PixelScale

// StickStatisticsPage.qml's cue points figure against a fake controller:
// the legend's counts and shares, the pie sized to the figures beside it
// and drawn in the right proportion (read back from its pixels, in Kelp
// and in the light system theme), and the cues-per-track histogram's
// seven buckets. Saves screenshots when SEABASS_SCREENSHOT_DIR is set.
TestCase {
    id: testCase
    name: "StickStatisticsPage"
    width: 1100
    height: 900
    visible: true
    when: windowShown

    // The window's palette from Theme, as Main.qml sets it, so the page
    // is drawn on the ground it has in the app.
    Item {
        id: host
        width: 1080
        height: 880
        ThemePalette { target: host }
    }

    Component {
        id: pageComponent
        StickStatisticsPage {
            width: host.width
            height: host.height
            playbackController: null
            appSettingsController: ({keyNotation: "camelot"})
        }
    }

    // 913 of 1240 cued; the buckets add up to 1240.
    readonly property var rekordboxCoverage: ({
        withCues: 913, withoutCues: 327,
        cuesPerTrack: [
            {label: "0", count: 327}, {label: "1", count: 58}, {label: "2", count: 91}, {label: "3", count: 120},
            {label: "4-5", count: 260}, {label: "6-8", count: 310}, {label: "9+", count: 74},
        ],
    })

    Component {
        id: fakeControllerComponent
        QtObject {
            property bool busy: false
            property bool scanCancellable: false
            property int scanCurrent: 0
            property int scanTotal: 0
            property string scanLabel: ""
            property string errorMessage: ""
            property var filesystemInfo: ({})
            property var rekordboxStats: ({})
            property var engineStats: ({})
            property var oneLibraryStats: ({})
            property var diskUsage: ({})
            property int scans: 0
            signal resultsChanged()
            signal scanProgressChanged()
            signal scanCancelled()
            function scan(label, rb, en) { scans++; }
            function cancelScan() {}
        }
    }

    function makePage(coverage) {
        const controller = createTemporaryObject(fakeControllerComponent, testCase);
        controller.rekordboxStats = {
            trackCount: coverage.withCues + coverage.withoutCues, playlistCount: 42, totalCuePoints: 6120,
            hotCueCount: 4800, memoryCueCount: 1320, ratedTrackCount: 300, commentedTrackCount: 120,
            streamingTrackCount: 0, tracksPerKey: {"8A": 200, "5A": 150}, tracksPerFileFormat: {"mp3": 900, "flac": 340},
            streamingTracksByService: {}, bpmDistribution: [{rangeStart: 120, count: 800}, {rangeStart: 130, count: 440}],
            cueCoverage: coverage,
        };
        const page = createTemporaryObject(pageComponent, host, {
            stickLabel: "WHALESHARK2", rekordboxPath: "/media/WHALESHARK2/PIONEER", enginePath: "",
            controller: controller,
        });
        verify(page !== null);
        compare(controller.scans, 1, "the page asks for its numbers on open");
        waitForRendering(page);
        // The pie is a Canvas and paints after the page renders: on macOS
        // and Windows the pixel checks read the ground where the pie goes
        // until it had.
        for (const pie of findAll(page, "cueCoveragePie")) {
            if (pie.visible) {
                tryVerify(() => pie.paintedCount > 0, 2000, "the pie has painted");
            }
        }
        waitForRendering(page);
        return page;
    }

    function findAll(item, objectName) {
        const found = [];
        const seen = [];
        const walk = function(node) {
            if (seen.indexOf(node) >= 0) return;
            seen.push(node);
            if (node.objectName === objectName) found.push(node);
            for (let i = 0; i < node.children.length; ++i) walk(node.children[i]);
            if (node.contentItem !== undefined && node.contentItem !== null) walk(node.contentItem);
        };
        walk(item);
        return found;
    }

    function findOne(item, objectName) {
        const all = findAll(item, objectName);
        verify(all.length >= 1, "no item named " + objectName);
        return all[0];
    }

    function chooseLightSystemTheme() {
        // Material's light palette, as Main.qml pushes it in.
        Theme.useSystemTheme = true;
        Theme.materialBackground = "#fafafa";
        Theme.materialForeground = "#212121";
        Theme.materialDivider = "#1f000000";
    }

    function cleanup() {
        Theme.useSystemTheme = false;
        Theme.materialBackground = "#121212";
        Theme.materialForeground = "#e0e0e0";
        Theme.materialDivider = "#33ffffff";
    }

    function distance(a, b) {
        return Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);
    }

    // A sample of the pie at an angle (radians clockwise from twelve
    // o'clock) and a fraction of its radius. The PAGE is grabbed, at its
    // own 0,0, and the point mapped into it: a grab of the Canvas alone
    // is not where the trap was in tst_PlayerBar, but a grab of anything
    // inside a padded control comes back offset by the padding.
    function pieSample(image, page, pie, angle, along) {
        const r = pie.radius * along;
        const at = pie.mapToItem(page, pie.width / 2 + r * Math.sin(angle), pie.height / 2 - r * Math.cos(angle));
        return PixelScale.pixel(image, page, at.x, at.y);
    }

    // How close to the ground under the pie it gets on a short line
    // drawn across a slice boundary: the gap between the slices is cut
    // out of the disc, so somewhere on that line the ground shows
    // through. An antialiased edge with no gap only ever blends the two
    // slices. The ground is read just outside the disc rather than taken
    // from Theme: under org.kde.desktop the GroupBox the pie sits in
    // paints its own frame colour, not Theme.background.
    function groundShowsAcross(image, page, pie, angle) {
        const outside = pieSample(image, page, pie, angle, 1.15);
        const r = pie.radius * 0.7;
        const cx = pie.width / 2 + r * Math.sin(angle);
        const cy = pie.height / 2 - r * Math.cos(angle);
        let closest = 3;
        for (let t = -3; t <= 3; t += 0.25) {
            const at = pie.mapToItem(page, cx + t * Math.cos(angle), cy + t * Math.sin(angle));
            closest = Math.min(closest, distance(PixelScale.pixel(image, page, at.x, at.y), outside));
        }
        return closest;
    }

    function test_legendShowsCountsAndShares() {
        const page = makePage(testCase.rekordboxCoverage);
        compare(findOne(page, "withCuesCount").text, "913");
        compare(findOne(page, "withCuesPercent").text, "74%");
        compare(findOne(page, "withoutCuesCount").text, "327");
        compare(findOne(page, "withoutCuesPercent").text, "26%");
        compare(findOne(page, "withCuesCount").font.family, Theme.dataFamily);
        const pie = findOne(page, "cueCoveragePie");
        compare(pie.withCues, 913);
        compare(pie.withoutCues, 327);
    }

    // At full width and at one narrow enough that the legend's
    // explanation wraps, which makes the legend taller than the figures.
    function test_pieIsAsTallAsTheFigures_data() {
        return [{tag: "wide", width: 1080}, {tag: "narrow", width: 760}];
    }
    function test_pieIsAsTallAsTheFigures(data) {
        const page = makePage(testCase.rekordboxCoverage);
        page.width = data.width;
        waitForRendering(page);
        const pie = findOne(page, "cueCoveragePie");
        const grid = findOne(page, "statsGrid");
        verify(grid.height > 0);
        compare(pie.height, grid.height, "the pie takes the height of the figures beside it");
        compare(pie.width, pie.height, "and is round");
        const g = grid.mapToItem(page, 0, 0);
        const p = pie.mapToItem(page, 0, 0);
        compare(p.y, g.y, "top-aligned with them");
        verify(p.x >= g.x + grid.width, "beside them, not over them");
        verify(p.x + pie.width <= page.width, "and inside the page");
    }

    function test_histogramHasSevenBuckets() {
        const page = makePage(testCase.rekordboxCoverage);
        const section = findOne(page, "cuesPerTrackSection");
        verify(section.visible);
        const rows = findAll(section, "distributionRow");
        compare(rows.length, 7);
        const labels = rows.map(row => row.modelData.label);
        compare(labels.join(" "), "0 1 2 3 4-5 6-8 9+");
        compare(rows[5].modelData.count, 310);
        // In the pie's colours: the bare tracks muted, the cued accent.
        const bar = function(row) {
            for (let i = 0; i < row.children.length; ++i) {
                const child = row.children[i];
                if (child.children.length === 1 && child.children[0].color !== undefined) return child.children[0];
            }
            return null;
        };
        verify(bar(rows[0]) !== null);
        verify(Qt.colorEqual(bar(rows[0]).color, Theme.textMuted), "bucket 0 is the without-cues colour");
        for (let i = 1; i < rows.length; ++i) {
            verify(Qt.colorEqual(bar(rows[i]).color, Theme.accent), "bucket " + labels[i] + " is the with-cues colour");
        }
    }

    function checkPieInTheme(themeName) {
        const page = makePage(testCase.rekordboxCoverage);
        const pie = findOne(page, "cueCoveragePie");
        verify(pie.radius > 20, "a pie big enough to sample, got radius " + pie.radius);
        const image = grabImage(page);

        const total = 913 + 327;
        const sweep = 2 * Math.PI * 913 / total;

        // Inside each slice, at its middle.
        const withMid = pieSample(image, page, pie, sweep / 2, 0.6);
        const withoutMid = pieSample(image, page, pie, sweep + (2 * Math.PI - sweep) / 2, 0.6);
        verify(distance(withMid, Theme.accent) < 0.1,
               themeName + ": the with-cues slice is Theme.accent, got " + withMid + " for " + Theme.accent);
        verify(distance(withoutMid, Theme.textMuted) < 0.1,
               themeName + ": the without-cues slice is Theme.textMuted, got " + withoutMid + " for " + Theme.textMuted);
        verify(distance(Theme.accent, Theme.textMuted) > 0.3, themeName + ": the two slices are told apart");

        // The boundary sits where the counts put it: accent just before
        // it, muted just after, and the cut on it is neither.
        const delta = 6 * Math.PI / 180;
        verify(distance(pieSample(image, page, pie, sweep - delta, 0.7), Theme.accent) < 0.1,
               themeName + ": accent up to the drawn boundary");
        verify(distance(pieSample(image, page, pie, sweep + delta, 0.7), Theme.textMuted) < 0.1,
               themeName + ": muted from the drawn boundary on");
        verify(groundShowsAcross(image, page, pie, sweep) < 0.1,
               themeName + ": a gap at the drawn boundary shows the page's ground");
        verify(groundShowsAcross(image, page, pie, 0) < 0.1,
               themeName + ": and one at twelve o'clock, where the with-cues slice starts");

        // And the proportion read back from the whole ring of pixels.
        let accent = 0;
        let muted = 0;
        for (let degree = 0; degree < 360; ++degree) {
            const c = pieSample(image, page, pie, degree * Math.PI / 180, 0.7);
            if (distance(c, Theme.accent) < 0.1) accent++;
            else if (distance(c, Theme.textMuted) < 0.1) muted++;
        }
        verify(accent + muted > 340, themeName + ": nearly every sample is one slice or the other, got " + (accent + muted));
        const drawn = accent / (accent + muted);
        verify(Math.abs(drawn - 913 / total) < 0.01,
               themeName + ": the drawn share " + drawn + " matches the counts' " + (913 / total));
        return page;
    }

    function test_piePixelsKelp() {
        const page = checkPieInTheme("Kelp");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/stick-statistics-page.png");
        }
    }

    function test_piePixelsLightSystemTheme() {
        chooseLightSystemTheme();
        const page = checkPieInTheme("light system theme");
        if (screenshotDir && screenshotDir.length > 0) {
            grabImage(page).save(screenshotDir + "/stick-statistics-page-light.png");
        }
    }

    // The edges a ratio hides: nothing cued draws a full muted disc, and
    // nothing at all draws no slice of either colour.
    function test_pieEdgeCases() {
        const none = makePage({withCues: 0, withoutCues: 50, cuesPerTrack: []});
        let pie = findOne(none, "cueCoveragePie");
        let image = grabImage(none);
        verify(distance(pieSample(image, none, pie, 0.3, 0.6), Theme.textMuted) < 0.1, "no cued track: all muted");
        compare(findOne(none, "withCuesPercent").text, "0%");

        const empty = makePage({withCues: 0, withoutCues: 0, cuesPerTrack: []});
        pie = findOne(empty, "cueCoveragePie");
        image = grabImage(empty);
        const c = pieSample(image, empty, pie, 0.3, 0.6);
        verify(distance(c, Theme.accent) > 0.15 && distance(c, Theme.textMuted) > 0.15,
               "no tracks: neither slice drawn, got " + c);
    }

    // The scan's overlay is the controller's one counted bar (#58), not
    // a sweep: count and total from the controller, the step named under
    // it, and it follows the controller as the scan goes.
    function test_scanShowsTheCountedBar() {
        const page = makePage(rekordboxCoverage);
        const controller = page.controller;
        const overlay = findOne(page, "statisticsBusyOverlay");
        verify(!overlay.visible, "no overlay while nothing runs");
        controller.scanTotal = 900;
        controller.scanCurrent = 314;
        controller.scanLabel = "Sizing the rekordbox folder";
        controller.busy = true;
        verify(overlay.visible, "the overlay is up while scanning");
        const report = findOne(overlay, "progressReport");
        compare(report.unitsDone, 314);
        compare(report.unitsTotal, 900);
        compare(report.indeterminate, false, "a counted bar, not a sweep");
        compare(report.currentItem, "Sizing the rekordbox folder");
        controller.scanCurrent = 900;
        compare(report.unitsDone, 900, "the bar follows the count");
        controller.busy = false;
    }
}
