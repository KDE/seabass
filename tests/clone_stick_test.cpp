// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include "application/use_cases/clone_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "stick_fixture.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
using namespace seabass::test_fixture;
using seabass::infrastructure::stick_backup::BackupStatus;
namespace fs = std::filesystem;

namespace
{

struct Fixture
{
    fs::path root;
    fs::path source;
    fs::path archive;
    fs::path target;
    CloneStickOptions options;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_clone_stick_test_" + name)), source(root / "source"),
          archive(root / "Seabass Backups" / "SOURCE.zip"), target(root / "target")
    {
        fs::remove_all(root);
        writeFile(source / "Contents" / "a.mp3", pseudoRandom(90'000, 1), 1'700'000'000);
        writeFile(source / "Contents" / "Sub" / "b.mp3", pseudoRandom(40'000, 2), 1'700'000'001);
        writeFile(source / "PIONEER" / "rekordbox" / "export.pdb", pseudoRandom(4096, 4), 1'700'000'003);
        createEngineDb(source / "Engine Library" / "Database2" / "m.db");
        fs::create_directories(source / "Engine Library" / "Music");
        fs::create_directories(target);
        options.backup.stickRoot = source;
        options.backup.archivePath = archive;
        options.backup.stickIdentifier = "uuid-source";
        options.backup.stickLabel = "SOURCE";
        options.targetRoot = target;
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

int main()
{
    // ---- First clone onto an empty target: archive created, target equals source ----
    {
        Fixture f("first");
        CloneStickPreview preview = CloneStick::preview(f.options);
        assert(preview.error.empty());
        assert(!preview.backup.archiveExists);
        assert(!preview.archiveCurrent);
        assert(!preview.restore.has_value());
        assert(preview.sourceBytes > 130'000);
        assert(preview.bytesToTarget == preview.sourceBytes);
        assert(preview.enoughTargetSpace);
        assert(!preview.targetHasEngineLibrary);

        int backupTicks = 0;
        int restoreTicks = 0;
        f.options.onProgress = [&](const CloneProgress &p) {
            (p.stage == CloneProgress::Stage::Backup ? backupTicks : restoreTicks)++;
        };
        CloneStickOutcome outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.backupStatus == BackupOutcomeStatus::Complete);
        assert(outcome.restoreStarted);
        assert(outcome.restore.status == RestoreSummary::Status::Restored);
        assert(outcome.restore.filesWritten >= 4);
        assert(backupTicks > 0 && restoreTicks > 0);
        assert(fs::exists(f.archive));
        assert(snapshot(f.target) == snapshot(f.source));

        // ---- Second run with nothing changed: backup skipped, nothing written ----
        preview = CloneStick::preview(f.options);
        assert(preview.error.empty());
        assert(preview.archiveCurrent);
        assert(preview.restore.has_value());
        assert(preview.restore->filesToWrite == 0);
        assert(preview.bytesToTarget == 0);
        assert(preview.targetHasEngineLibrary);

        outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.backupStatus == BackupOutcomeStatus::NothingToDo);
        assert(outcome.restore.filesWritten == 0);
        assert(outcome.restore.filesUnchanged >= 4);

        // ---- Source changes (a file and the database): target follows ----
        writeFile(f.source / "Contents" / "a.mp3", pseudoRandom(95'000, 9), 1'700'000'500);
        writeFile(f.source / "Contents" / "c.mp3", pseudoRandom(1'000, 10), 1'700'000'501);
        appendEngineDbRow(f.source / "Engine Library" / "Database2" / "m.db", "Contents/c.mp3");
        preview = CloneStick::preview(f.options);
        assert(!preview.archiveCurrent);
        assert(preview.backup.added == 1);
        assert(preview.backup.changed >= 1);
        assert(preview.backup.databaseChanged);
        assert(preview.bytesToTarget >= 96'000);

        outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.backupStatus == BackupOutcomeStatus::Complete);
        assert(outcome.restore.filesWritten >= 3);  // a.mp3, c.mp3, m.db
        assert(snapshot(f.target) == snapshot(f.source));
        std::cout << "case 1 (first clone, no-op rerun, update after source change) OK\n";
    }

    // ---- Overlay keeps a stray file on the target; exact removes it ----
    {
        Fixture f("modes");
        writeFile(f.target / "holiday-photos.txt", "keep me", 1'700'000'100);
        CloneStickOutcome outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.extrasRemoved == 0);
        assert(fs::exists(f.target / "holiday-photos.txt"));

        f.options.exact = true;
        outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(outcome.restore.extrasRemoved == 1);
        assert(!fs::exists(f.target / "holiday-photos.txt"));
        assert(snapshot(f.target) == snapshot(f.source));
        std::cout << "case 2 (overlay keeps extras, exact removes them) OK\n";
    }

    // ---- Cancel during the backup step: partial backup kept, target untouched ----
    {
        Fixture f("cancel");
        f.options.cancel = CancellationToken();
        f.options.onProgress = [&](const CloneProgress &p) {
            if (p.stage == CloneProgress::Stage::Backup && p.backup.phase == BackupProgress::Phase::Reading
                && p.backup.filesDone >= 1) {
                f.options.cancel.cancel();
            }
            assert(p.stage == CloneProgress::Stage::Backup);  // the restore step must never start
        };
        CloneStickOutcome outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cancelled);
        assert(outcome.backupStatus == BackupOutcomeStatus::Cancelled);
        assert(!outcome.restoreStarted);
        assert(snapshot(f.target).empty());
        assert(fs::exists(f.archive));
        assert(RestoreStickBackup::describe(f.archive).status == BackupStatus::PartialCancelled);

        // A fresh run resumes and completes.
        f.options.cancel = CancellationToken::none();
        f.options.onProgress = {};
        outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Cloned);
        assert(snapshot(f.target) == snapshot(f.source));
        std::cout << "case 3 (cancel during backup keeps the partial, rerun completes) OK\n";
    }

    // ---- DJ software appears during the backup step: no restore ----
    {
        Fixture f("conflict");
        f.options.backup.conflictingProcessProbe = [] { return true; };
        f.options.backup.probeInterval = std::chrono::milliseconds(0);
        CloneStickOutcome outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::BackupIncomplete);
        assert(outcome.backupStatus == BackupOutcomeStatus::ConflictAborted);
        assert(!outcome.restoreStarted);
        assert(snapshot(f.target).empty());
        assert(outcome.message.find("not touched") != std::string::npos);
        std::cout << "case 4 (conflicting DJ software: backup incomplete, target untouched) OK\n";
    }

    // ---- Source and target are the same drive: refused before anything runs ----
    {
        Fixture f("same");
        f.options.targetRoot = f.source;
        assert(!CloneStick::preview(f.options).error.empty());
        CloneStickOutcome outcome = CloneStick::execute(f.options);
        assert(outcome.status == CloneStickOutcome::Status::Refused);
        assert(!fs::exists(f.archive));
        std::cout << "case 5 (source is target: refused) OK\n";
    }

    // ---- Target is not a directory: preview says so ----
    {
        Fixture f("notarget");
        f.options.targetRoot = f.root / "missing";
        assert(!CloneStick::preview(f.options).error.empty());
        std::cout << "case 6 (missing target: preview error) OK\n";
    }

    assert(toString(CloneStickOutcome::Status::Cloned) == "cloned");
    assert(toString(CloneStickOutcome::Status::BackupIncomplete) == "backup-incomplete");

    std::cout << "All clone_stick tests passed." << std::endl;
    return 0;
}
