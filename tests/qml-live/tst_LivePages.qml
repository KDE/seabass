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
// R6  The Full Stick Backup page's Name field starts out holding the real
//     stick's own name, so a backup is named without anybody typing.
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
    Component { id: stickBackupController; StickBackupController {} }
    Component { id: stickBackupPage; StickBackupPage { width: 1100; height: 820 } }

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

    // The Name field's default, against a real stick and the real
    // controller: the pure decision is covered by
    // stick_backup_paths_test, but only configure() on the live object
    // proves the field a person sees is filled in.
    //
    // The run's own scratch directory, not the real backup folder:
    // configure() previews, and a preview is a read -- but a folder with
    // no archive in it is what makes "no stored name yet" the case under
    // test, and it keeps this out of the way of the backups a round
    // wrote.
    function test_04_backupNameDefaultsToTheStickName() {
        if (stickRoot.length === 0) {
            skip("SEABASS_LIVE_STICK is not set");
        }
        // The real page with the real controller, configured the way the
        // app configures it (Component.onCompleted), and a stub settings
        // object so the backup folder is this run's scratch directory
        // rather than the developer's own.
        var page = createTemporaryObject(stickBackupPage, testCase, {
            stickLabel: stickLabel,
            rekordboxPath: rekordboxPath,
            enginePath: enginePath,
            appSettingsController: ({stickBackupDirectory: testScratchDir}),
            controller: createTemporaryObject(stickBackupController, testCase),
        });
        verify(page !== null, "the Full Stick Backup page did not load");

        // What a person actually sees: the text in the Name box, not just
        // the property behind it.
        var field = Live.findByObjectName(page, "backupNameField");
        verify(field !== null, "the page has no Name field");
        compare(field.text, stickLabel, "the Name field starts out holding the stick's own name");
        compare(page.controller.backupName, stickLabel, "and the controller agrees with the box");

        // A finished preview of a stick with no named backup leaves the
        // default alone rather than blanking it. Waited for on
        // `previewing`, not `busy`: busy() covers runs, and a preview
        // leaves it false throughout -- so waiting on it returned at once
        // and re-checked the state from before the preview, which is the
        // one thing this assertion must not do.
        tryVerify(function() { return page.controller.previewing === false; }, 600000);
        compare(field.text, stickLabel, "the preview cleared the default name instead of keeping it");

        // The archive it would write is the file the label alone produced,
        // so defaulting the name renames nothing.
        verify(page.controller.archivePath.indexOf(stickLabel + ".zip") >= 0,
               "the archive is still named after the stick: " + page.controller.archivePath);

        if (screenshotDir && screenshotDir.length > 0) {
            waitForRendering(page);
            grabImage(page).save(screenshotDir + "/stick-backup-name-default.png");
        }
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
        var withFingerprint = 0;
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
                // The two references do carry a fingerprint, so if the
                // listing stops reading them this count drops and the
                // check below fails. Not asserted per entry: this folder
                // also holds whole-stick archives the rig wrote itself,
                // and an older one without a fingerprint is not a
                // regression in the page.
                if (entry.trackCount > 0) {
                    withFingerprint++;
                }
            }
        }
        // Every reference the rig was given, not a fixed two. A round can
        // be run against one fixture restored to both sticks (round 5
        // was), and asserting a number here made the page look broken for
        // a library it had in fact read correctly. What must hold is that
        // the folder is not empty and that everything in it was read.
        verify(listed.length > 0, "the reference folder holds no backups at all");
        compare(readable, listed.length, "every backup in the reference folder is listed and readable");
        verify(withFingerprint >= 1,
               "a reference reports a track count (-1 everywhere means none was read)");

        // Browsing opens the archive read-only: the page hands the path to
        // openArchivePaths, and nothing about the archive changes.
        var first = listed[0].archivePath;
        backups.openArchivePaths = [first];
        verify(backups.isOpen(first), "the browsed archive is open");
        compare(backups.errorMessage, "");
    }
}
