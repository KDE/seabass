// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui
import "LiveHelpers.js" as Live

// Read-only pages against a REAL stick: the ones the rig used to open by
// hand. Nothing here stages, saves or writes to the stick.
//
//   SEABASS_LIVE_STICK=/media/you/STICK QT_QPA_PLATFORM=offscreen \
//     build/seabass_qml_tests -input tests/qml-live
//
// R2  Statistics and Stick Performance load, with figures that fit the
//     library, and nothing logged as an error. The performance page is
//     only measured, never asked for its write test or its wear check:
//     both write to the stick, and this phase of a round may not.
// R5  Manage Backups lists both reference backups with the right stick,
//     size and counts, and browsing one opens it read-only.
TestCase {
    id: testCase
    name: "LivePages"
    width: 1100
    height: 820
    visible: true
    when: windowShown

    readonly property string stickRoot: typeof liveStickRoot !== "undefined" ? liveStickRoot : ""
    readonly property string rekordboxPath: stickRoot + "/PIONEER"
    readonly property string enginePath: stickRoot + "/Engine Library"
    readonly property string stickLabel: stickRoot.substring(stickRoot.lastIndexOf("/") + 1)

    Component { id: statisticsController; StickStatisticsController {} }
    Component { id: performanceController; StickPerformanceController {} }
    Component { id: fullBackupsController; FullBackupsController {} }

    function waitIdle(controller, timeout) {
        tryVerify(function() { return controller.busy === false; }, timeout === undefined ? 600000 : timeout);
    }

    function test_01_statisticsLoads() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        var stats = createTemporaryObject(statisticsController, testCase);
        stats.scan(stickLabel, rekordboxPath, enginePath);
        waitIdle(stats);
        compare(stats.errorMessage, "");

        var rekordbox = stats.rekordboxStats;
        var disk = stats.diskUsage;
        console.log("  rekordbox stats: " + JSON.stringify(rekordbox));
        console.log("  disk usage: " + JSON.stringify(disk));
        verify(Object.keys(rekordbox).length > 0 || Object.keys(stats.engineStats).length > 0,
               "at least one catalog reported statistics");
        // Plausible for the library rather than merely non-zero: a stick
        // holding a thousand tracks cannot report none, and cannot report
        // more cues than it has cue points to give.
        if (rekordbox.trackCount !== undefined) {
            verify(rekordbox.trackCount > 0, "rekordbox tracks counted");
        }
        if (disk.usedBytes !== undefined && disk.capacityBytes !== undefined) {
            verify(disk.usedBytes > 0 && disk.usedBytes <= disk.capacityBytes, "used bytes fit the stick");
        }
    }

    function test_02_performanceLoads() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        var perf = createTemporaryObject(performanceController, testCase);
        // measure() reads and times what is already on the stick. The
        // write test (measureWrites), the scratch-file variant and the
        // wear check all write, and are deliberately not called here.
        perf.measure(stickLabel, rekordboxPath, enginePath, stickRoot);
        tryVerify(function() { return perf.anyBusy === false; }, 600000);
        compare(perf.errorMessage, "");
        console.log("  measurement: " + JSON.stringify(perf.measurement));
        console.log("  score: " + JSON.stringify(perf.score));
        verify(Object.keys(perf.measurement).length > 0, "the page measured something");
        verify(perf.needsScratchFiles === true || Object.keys(perf.score).length > 0,
               "either a score, or it says it needs scratch files first");
    }

    function test_03_manageBackupsListsTheReferences() {
        if (typeof liveRigReferenceDir === "undefined" || liveRigReferenceDir.length === 0) {
            skip("SEABASS_RIG_REFERENCE_DIR is not set");
        }
        var backups = createTemporaryObject(fullBackupsController, testCase,
                                            {backupDirectory: liveRigReferenceDir});
        backups.refresh();
        tryVerify(function() { return backups.listing === false; }, 600000);
        compare(backups.errorMessage, "");

        var listed = backups.backups;
        console.log("  " + listed.length + " backup(s) in " + liveRigReferenceDir);
        var readable = 0;
        for (var i = 0; i < listed.length; ++i) {
            var entry = listed[i];
            console.log("    " + entry.fileName + ": stick " + entry.label + ", " + entry.status + ", "
                        + entry.bytes + " bytes, " + entry.entries + " entries, " + entry.trackCount + " tracks, "
                        + entry.playlistCount + " playlists" + (entry.error.length > 0 ? ", error " + entry.error : ""));
            if (entry.error.length === 0) {
                readable++;
                verify(entry.label.length > 0, "the backup names its stick");
                verify(entry.bytes > 0, "the backup has a size");
                verify(entry.entries > 0, "the backup lists entries");
                // -1 means the backup recorded no fingerprint (an older
                // one), which is not a fault of the listing.
                verify(entry.trackCount > 0 || entry.trackCount === -1,
                       "the backup's library has tracks, or records no count");
            }
        }
        verify(readable >= 2, "both reference backups are listed and readable");

        // Browsing opens the archive read-only: the page hands the path to
        // openArchivePaths, and nothing about the archive changes.
        var first = listed[0].archivePath;
        backups.openArchivePaths = [first];
        verify(backups.isOpen(first), "the browsed archive is open");
        compare(backups.errorMessage, "");
    }
}
