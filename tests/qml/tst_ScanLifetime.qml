// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest

// A LibraryConsistencyController's scan against the committed real-scale
// library (tests/fixtures/anonymized_library): a stop lands within a
// row's work in every leg, including the Engine audits that used to
// ignore it, and leaving the page (destroying the controller) waits for
// the scan to be over, so no task is left reading the stick for a page
// that has gone. The timing is done in C++ (controllerFixture), where the
// delete is.
TestCase {
    id: testCase
    name: "ScanLifetime"
    when: windowShown

    // What Back may cost on the fixture, and what a stop may take: a
    // catalog row or an audit row, not the rest of the scan. Most stops
    // land in 2 to 35 ms here (Debug, software GL); the whole scan takes
    // about 3 s, the Engine leg's audits alone most of a second.
    //
    // The one step no token can cut short is OneLibrary's first read,
    // where SQLCipher derives the database key inside a single library
    // call: 120 to 130 ms on an idle machine, and several times that
    // when other builds share it. A stop may take that step plus a
    // margin, measured when the stop is (see withinBound()), and never
    // less than the flat bound. The margin is 250 ms: on the shared CI
    // runner a stop 25 ms into the rekordbox leg came in 160 to 200 ms
    // past the key derivation, three measurements running, against 150.
    readonly property int stopBoundMs: 250
    readonly property int stopMarginMs: 250

    readonly property string fixtureRoot: {
        const url = Qt.resolvedUrl("../fixtures/anonymized_library").toString();
        return decodeURIComponent(url.replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    // Offsets into each leg: its catalog read first, and for Engine the
    // audits that follow it.
    readonly property var offsets: [0, 25, 75, 150, 300, 600]

    // The bound a stop is held to right now: the flat one, or the key
    // derivation measured this moment plus the margin, whichever is more.
    function boundNow(stick) {
        const keyMs = controllerFixture.oneLibraryOpenMs(stick);
        verify(keyMs >= 0, "the fixture's OneLibrary opens");
        return Math.max(testCase.stopBoundMs, keyMs + testCase.stopMarginMs);
    }

    // A stop that overran is measured twice more at the same point, each
    // against the bound as it stands then, and passes if any attempt was
    // inside it: this machine is shared with other builds. Work that does
    // not check the token overruns every time.
    function checkSamples(stick, format, destroy, what) {
        const samples = controllerFixture.stopScanAt(stick, format, testCase.offsets, destroy);
        let measured = 0;
        for (let i = 0; i < samples.length; ++i) {
            let sample = samples[i];
            let bound = testCase.stopBoundMs;
            for (let retry = 0; retry < 2 && sample.wasRunning && sample.stopMs >= bound; ++retry) {
                if (retry === 0) {
                    bound = boundNow(stick);
                    if (sample.stopMs < bound) {
                        break;
                    }
                }
                const again = controllerFixture.stopScanAt(stick, format, [sample.offsetMs], destroy)[0];
                const againBound = boundNow(stick);
                console.log(what + " at +" + sample.offsetMs + " ms took " + sample.stopMs.toFixed(1)
                            + " ms (bound " + bound.toFixed(0) + "), measured again: " + again.stopMs.toFixed(1)
                            + " ms (bound " + againBound.toFixed(0) + ")");
                if (again.wasRunning && again.stopMs - againBound < sample.stopMs - bound) {
                    sample = again;
                    bound = againBound;
                }
            }
            console.log(what + " at +" + sample.offsetMs + " ms (" + sample.formatAtStop + "): "
                        + sample.stopMs.toFixed(1) + " ms, running " + sample.wasRunning
                        + ", tasks after " + sample.tasksAfter + ", outlived by " + sample.outlivedMs.toFixed(1) + " ms");
            if (destroy) {
                compare(sample.tasksAfter, 0, "leaving at +" + sample.offsetMs + " ms in the "
                        + sample.formatAtStop + " leg left a scan task running");
            }
            if (!sample.wasRunning) {
                continue;
            }
            measured++;
            verify(sample.stopMs < bound, what + " at +" + sample.offsetMs + " ms in the " + sample.formatAtStop
                   + " leg took " + sample.stopMs.toFixed(1) + " ms, bound " + bound.toFixed(0) + " ms");
        }
        verify(measured >= 3, what + ": only " + measured + " stop(s) landed while the scan ran");
    }

    function test_aStopLandsWithinTheBoundInEveryLeg() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        console.log("whole scan: " + controllerFixture.fullScanMs(stick).toFixed(0) + " ms");
        for (const format of ["rekordbox", "engine", "engine:audits", "onelibrary"]) {
            checkSamples(stick, format, false, "cancel in " + format);
        }
    }

    // Back: the controller goes, and with it every scan it started. It
    // used to cancel and walk away, so the Engine audits went on reading
    // the stick for up to a second after the page had gone.
    function test_leavingMidScanWaitsForTheScanAndLeavesNoTask() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        for (const format of ["rekordbox", "engine", "engine:audits", "onelibrary"]) {
            checkSamples(stick, format, true, "leave in " + format);
        }
    }

    // The plan of the scan's one bar (#58) opens each catalog to count
    // it, OneLibrary through its uninterruptible key derivation. It
    // counted each twice and never looked at the token, so a stop early
    // in the rekordbox leg waited out two derivations. Once each now,
    // and a stop during the first count makes no other.
    function test_thePlanCountsEachCatalogOnceAndStopsBetweenCounts() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const whole = controllerFixture.planCounts(stick, false);
        compare(whole.counts.rekordbox, 1, "rekordbox counted once");
        compare(whole.counts.engine, 1, "Engine counted once");
        compare(whole.counts.onelibrary, 1, "OneLibrary counted once");
        const stopped = controllerFixture.planCounts(stick, true);
        compare(stopped.tasksAfter, 0, "the stop ended the leg");
        compare(Object.keys(stopped.counts).length, 1,
                "a stop during the first count makes no other: " + JSON.stringify(stopped.counts));
    }

    // A rescan of an unchanged stick plans from the counts the cache
    // remembered: the first leg of a Full scan used to count every
    // catalog again for its per-row checks, OneLibrary's count a key
    // derivation.
    function test_aRescanOfAnUnchangedStickCountsNothing() {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const twice = controllerFixture.countsDuring("health", stick, false, 2);
        compare(twice.runs.length, 2, "both scans ran");
        compare(JSON.stringify(twice.runs[0]), JSON.stringify({engine: 1, onelibrary: 1, rekordbox: 1}),
                "the first scan counts each catalog once");
        compare(JSON.stringify(twice.runs[1]), "{}", "the rescan counts nothing");
    }

    // Every other operation that plans one bar from the catalogs' counts
    // (#58) counts each catalog once, and a stop during its first count
    // makes no other: Sync counted each catalog twice, Create Engine
    // Library rekordbox twice, and neither they nor Clean Up looked at
    // the token between counts.
    function test_everyPlannerCountsEachCatalogOnceAndStopsBetweenCounts_data() {
        return [
            {tag: "sync", expected: {engine: 1, onelibrary: 1, rekordbox: 1}},
            {tag: "createEngine", expected: {rekordbox: 1}},
            {tag: "duplicates", expected: {onelibrary: 1}},
            {tag: "cleanup", expected: {engine: 1, onelibrary: 1, rekordbox: 1}}
        ];
    }
    function test_everyPlannerCountsEachCatalogOnceAndStopsBetweenCounts(data) {
        const stick = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(stick.length > 0, "the fixture must copy");
        const whole = controllerFixture.countsDuring(data.tag, stick, false, 1);
        verify(whole.known, "the fixture knows the operation");
        compare(JSON.stringify(whole.counts), JSON.stringify(data.expected),
                data.tag + " counts each catalog once");
        compare(whole.busyAfter, false, "and ended");
        const again = stickFixture.stickCopy(testCase.fixtureRoot);
        verify(again.length > 0, "the fixture must copy");
        const stopped = controllerFixture.countsDuring(data.tag, again, true, 1);
        compare(stopped.busyAfter, false, "the stop ended " + data.tag);
        compare(Object.keys(stopped.counts).length, 1,
                "a stop during the first count makes no other: " + JSON.stringify(stopped.counts));
        compare(stopped.counts[Object.keys(stopped.counts)[0]], 1,
                "not even the same one again: " + JSON.stringify(stopped.counts));
    }

    // A filesystem repair unmounts and remounts the stick and cannot be
    // stopped: the controller waits for it rather than abandon it.
    function test_leavingMidRepairWaitsForTheRepair() {
        compare(controllerFixture.leaveThePageMidRepair(), "");
    }
}
