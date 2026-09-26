// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtTest
import SeabassGui

// The pages whose background work is a write or a preview, under
// docs/async-requests.md: a write ends once and gives its lock back
// however its page goes away; a request made while another runs is
// answered, not dropped; a preview's answer belongs to the request that
// asked for it.
TestCase {
    id: testCase
    name: "WriteAndPreviewRequests"
    when: windowShown

    Component {
        id: creatorComponent
        EngineLibraryCreatorController {}
    }
    Component {
        id: fullBackupsComponent
        FullBackupsController {}
    }
    Component {
        id: restoreComponent
        RestoreStickBackupController {}
    }
    Component {
        id: stickBackupComponent
        StickBackupController {}
    }
    Component {
        id: spyComponent
        SignalSpy {}
    }

    function fixture() {
        return decodeURIComponent(Qt.resolvedUrl("../fixtures/anonymized_library").toString()
            .replace(/^file:\/\//, "").replace(/^\/([A-Za-z]:)/, "$1"));
    }

    function cleanup() {
        catalogGate.restore();
        browseFixture.waitForScans();
    }

    // Create Engine Library takes the library's lock for its write. A page
    // destroyed mid-create (the stick-gone dialog pops past the disabled
    // Back button) never ran the write's ending, so the lock and
    // EditSessionRegistry.anyWriting stayed on until the app quit, and the
    // player stopped advancing its queue. The write now runs to its end
    // and gives the lock back then; leaving neither waits for it nor lets
    // go of the lock while it runs.
    function test_aCreatorGoneMidWriteGivesTheLockBackWhenTheWriteEnds() {
        const root = stickFixture.stickCopy(fixture());
        verify(root.length > 0, "the fixture copy must be made");
        catalogGate.hold(3, true);
        verify(!EditSessionRegistry.anyWriting, "nothing is writing before");
        const creator = creatorComponent.createObject(testCase);
        creator.create(root + "/PIONEER", 2, "GATED");
        tryVerify(() => catalogGate.waiting() === 1, 5000, "the create must reach its catalog read");
        verify(EditSessionRegistry.anyWriting, "the create holds the library's lock");
        const started = Date.now();
        creator.destroy();
        wait(0);
        const took = Date.now() - started;
        verify(took < 1000, "leaving does not wait for the write, took " + took + " ms");
        verify(EditSessionRegistry.anyWriting, "the write still runs, so its lock is still held");
        catalogGate.release();
        const announced = catalogGate.contentsChangedCount();
        tryVerify(() => !EditSessionRegistry.anyWriting, 10000, "the write is over, so its lock is given back");
        tryVerify(() => catalogGate.contentsChangedCount() > announced, 5000,
                  "and the stick list is told to look again, as the page would have");
        verify(browseFixture.waitForScans(), "and its worker has returned");
    }

    // A second delete asked for while one runs is done too, and both say
    // so. Deleting was isRunning(), which goes false before the first
    // delete's result is handled: a second delete in that window replaced
    // the first one's future, and the first result (and backupDeleted) was
    // lost. Asked while it was still running, the second was dropped.
    function test_twoDeletesAreBothDone() {
        const folder = controllerFixture.slowBackupFolder(2);
        verify(folder.length > 0, "the backup folder must be made");
        const backups = createTemporaryObject(fullBackupsComponent, testCase, {backupDirectory: folder});
        const deleted = spyComponent.createObject(testCase, {target: backups, signalName: "backupDeleted"});
        backups.deleteBackup(folder + "/backup-0.zip");
        backups.deleteBackup(folder + "/backup-1.zip");
        tryVerify(() => deleted.count === 2, 5000, "both deletes must be done and said, got " + deleted.count);
        tryVerify(() => !backups.deleting, 2000, "and nothing is deleting after");
    }

    // Deletes asked for and still waiting their turn when the page goes
    // are done all the same. They were accepted; the page going used to
    // make them vanish without a word.
    function test_queuedDeletesOutliveThePage() {
        const folder = controllerFixture.slowBackupFolder(3);
        verify(folder.length > 0, "the backup folder must be made");
        const backups = fullBackupsComponent.createObject(testCase, {backupDirectory: folder});
        backups.deleteBackup(folder + "/backup-0.zip");
        backups.deleteBackup(folder + "/backup-1.zip");
        backups.deleteBackup(folder + "/backup-2.zip");
        backups.destroy();
        wait(0);
        tryVerify(() => !stickFixture.exists(folder + "/backup-0.zip")
                  && !stickFixture.exists(folder + "/backup-1.zip")
                  && !stickFixture.exists(folder + "/backup-2.zip"), 5000,
                  "every delete asked for is done");
    }

    // An analysis of the archive being left does not land under the one
    // chosen next. It used to: the answer was not tied to its archive, so
    // A's label and status showed as B's.
    function test_anArchiveLeftMidAnalysisDoesNotDescribeTheNext() {
        const folder = controllerFixture.slowBackupFolder(2);
        // The precondition: analysed on its own, the first archive is
        // described.
        const control = createTemporaryObject(restoreComponent, testCase);
        control.archivePath = folder + "/backup-0.zip";
        control.analyze("");
        tryVerify(() => !control.analyzing && Object.keys(control.archiveInfo).length > 0, 5000,
                  "an analysed archive is described");

        const restore = createTemporaryObject(restoreComponent, testCase);
        restore.archivePath = folder + "/backup-0.zip";
        restore.analyze("");
        restore.archivePath = folder + "/backup-1.zip";
        tryVerify(() => !restore.analyzing, 5000, "no analysis is left running");
        verify(browseFixture.waitForScans(), "every worker has returned");
        wait(50);
        compare(Object.keys(restore.archiveInfo).length, 0,
                "nothing was analysed for the archive now chosen, so nothing describes it");
    }

    // The preview for a configuration replaced before it landed does not
    // land. refresh() was dropped while the old preview ran, so the page
    // described the old backup folder under the new one.
    function test_aStickBackupPreviewForAnOldConfigurationDoesNotLand() {
        const root = stickFixture.stickCopy(fixture());
        const before = controllerFixture.slowBackupFolder(0);
        verify(stickFixture.touchNew(before + "/GATED.zip"), "a broken archive under the stick's name in the first folder");
        const after = before + "/elsewhere";
        verify(stickFixture.makeDirectory(after), "an empty second folder");
        // The precondition, so the check below could go red: on its own,
        // the first folder's preview does find a backup.
        const control = createTemporaryObject(stickBackupComponent, testCase);
        control.configure("GATED", root + "/PIONEER", root + "/Engine Library", before);
        tryVerify(() => !control.previewing && (control.lastBackup.error || "").length > 0, 10000,
                  "the first folder's preview finds a (broken) backup there and says so");

        const controller = createTemporaryObject(stickBackupComponent, testCase);
        controller.configure("GATED", root + "/PIONEER", root + "/Engine Library", before);
        controller.configure("GATED", root + "/PIONEER", root + "/Engine Library", after);
        tryVerify(() => !controller.previewing, 10000, "the preview must end");
        verify(browseFixture.waitForScans(), "every worker has returned");
        wait(50);
        compare(controller.lastBackup.error || "", "",
                "the preview shown is the second folder's, which has no backup to find broken");
    }
}
