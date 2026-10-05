// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// Library Statistics' scan and Stick Performance's measurement each show
// one counted bar (#58), against the real controllers on a copy of the
// fixture stick. The owner's report: both were a busy bar that swept and
// counted nothing. Every change of the bar is logged and the log has to
// show one operation: no count before the total is known, one total that
// is never replaced, a count that never goes back and ends on the total,
// a label per step, and the folder walks ticking through their slices
// rather than jumping.
TestCase {
    id: testCase
    name: "StickProgressBars"
    when: windowShown

    Component {
        id: statisticsComponent
        StickStatisticsController {}
    }
    Component {
        id: performanceComponent
        StickPerformanceController {}
    }

    function fixture() {
        return decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function stick() {
        const root = stickFixture.stickCopy(fixture());
        verify(root.length > 0, "the fixture copy must be made");
        return root;
    }

    function cleanup() {
        browseFixture.waitForScans();
    }

    // Every state the bar is in, in order.
    function record(controller, signalName, currentName, totalName, labelName) {
        const log = [];
        controller[signalName].connect(() => log.push({
            current: controller[currentName], total: controller[totalName], label: controller[labelName],
        }));
        return log;
    }

    // The checks every bar has to pass; returns its labels in order and
    // how many distinct counts each label was shown with.
    function verifyOneBar(log) {
        verify(log.length > 0, "the bar must have moved at all");
        let total = 0;
        let last = 0;
        const labels = [];
        const counts = {};
        for (let i = 0; i < log.length; ++i) {
            const e = log[i];
            if (total === 0) {
                if (e.total <= 0) {
                    compare(e.current, 0, "nothing is counted before the total is known (state " + i + ")");
                    continue;
                }
                total = e.total;
            }
            compare(e.total, total, "one total, announced once and never replaced (state " + i + ")");
            verify(e.current >= last, "the count never goes back: " + last + " then " + e.current + " (state " + i + ")");
            verify(e.current <= total, "and never past the total (state " + i + ")");
            last = e.current;
            if (labels.indexOf(e.label) < 0) {
                labels.push(e.label);
                counts[e.label] = [];
            }
            if (counts[e.label].indexOf(e.current) < 0) {
                counts[e.label].push(e.current);
            }
        }
        verify(total > 0, "a total must have been announced");
        compare(last, total, "the count ends on the total");
        return {labels: labels, counts: counts, total: total};
    }

    function verifyLabels(bar, expected) {
        let at = -1;
        for (const label of expected) {
            const index = bar.labels.indexOf(label);
            verify(index >= 0, "the bar must name \"" + label + "\"; it named " + JSON.stringify(bar.labels));
            verify(index > at, "\"" + label + "\" in its place; the bar named " + JSON.stringify(bar.labels));
            at = index;
        }
    }

    function test_statisticsScanIsOneCountedBar() {
        const root = stick();
        const controller = createTemporaryObject(statisticsComponent, testCase);
        const log = record(controller, "scanProgressChanged", "scanCurrent", "scanTotal", "scanLabel");
        controller.scan("STATS", root + "/PIONEER", root + "/Engine Library");
        verify(controller.busy, "scanning");
        tryVerify(() => !controller.busy, 30000, "the scan must end");
        compare(controller.errorMessage, "", "and succeed");
        verify(Object.keys(controller.diskUsage).length > 0, "with its figures");

        const bar = verifyOneBar(log);
        verifyLabels(bar, ["Reading the stick's facts", "Scanning rekordbox tracks", "Scanning Engine tracks",
                           "Measuring artwork", "Sizing the rekordbox folder", "Sizing the Engine Library folder"]);
        // The fixture's export has an analysis folder per track: the walk
        // of it is counted folder by folder, not one jump at its end.
        verify(bar.counts["Sizing the rekordbox folder"].length > 10,
               "the rekordbox folder's walk must tick through its slices: "
               + JSON.stringify(bar.counts["Sizing the rekordbox folder"]));
    }

    function test_performanceMeasurementIsOneCountedBar() {
        const root = stick();
        const controller = createTemporaryObject(performanceComponent, testCase);
        const log = record(controller, "measureProgressChanged", "measureCurrent", "measureTotal", "measureLabel");
        controller.measure("PERF", root + "/PIONEER", root + "/Engine Library", root, false);
        verify(controller.busy, "measuring");
        tryVerify(() => !controller.busy, 60000, "the measurement must end");
        compare(controller.errorMessage, "", "and succeed");
        verify(controller.score.score !== undefined, "with a score");

        const bar = verifyOneBar(log);
        verifyLabels(bar, ["Reading the stick's facts", "Scanning rekordbox tracks", "Scanning Engine tracks",
                           "Finding analysis files", "Streaming audio files", "Reading audio files at random places",
                           "Opening analysis files"]);
        verify(bar.counts["Finding analysis files"].length > 10,
               "the analysis folder's walk must tick through its slices: "
               + JSON.stringify(bar.counts["Finding analysis files"]));
        // The fixture copy carries the analysis files but none of the
        // audio its catalogs name, so the streaming and random reads find
        // nothing to read and pass their share over at once; the analysis
        // files are read one by one, and counted so.
        verify(bar.counts["Opening analysis files"].length > 10,
               "the analysis files read must be counted as they go: "
               + JSON.stringify(bar.counts["Opening analysis files"]));
    }
}
