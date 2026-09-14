// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Manage Backups over real archives: the listing (this stick's backup
// first, track and playlist counts from the recorded fingerprint, an
// unreadable file listed rather than hidden) and deleting (refused while
// something holds the archive's write lock, and complete when it goes:
// archive, journal and lock file).

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/manage_stick_backups.hpp"
#include "domain/library_fingerprint.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "scratch_path.hpp"

using namespace seabass::application;
namespace fs = std::filesystem;
namespace journal = seabass::infrastructure::stick_backup::journal;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

fs::path backUp(const fs::path &root, const std::string &label, const std::string &fingerprint)
{
    const fs::path stick = root / ("stick-" + label);
    writeFile(stick / "Contents" / "a.mp3", std::string(20'000, 'a') + label);
    writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", std::string(4096, 'p') + label);
    BackupStickOptions options;
    options.stickRoot = stick;
    options.archivePath = root / "Backups" / (label + ".zip");
    options.stickIdentifier = "uuid-" + label;
    options.stickLabel = label;
    options.libraryFingerprint = fingerprint;
    const BackupStickOutcome outcome = BackupStick::execute(options);
    assert(outcome.status == BackupOutcomeStatus::Complete);
    return options.archivePath;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_manage_stick_backups_test";
    std::error_code ec;
    fs::remove_all(root, ec);

    seabass::domain::LibraryFingerprint fingerprint;
    fingerprint.trackCount = 1161;
    fingerprint.cuedTrackCount = 222;
    fingerprint.playlistCount = 24;
    const fs::path main = backUp(root, "MAIN", fingerprint.serialize());
    const fs::path spare = backUp(root, "SPARE", "");
    const fs::path broken = root / "Backups" / "BROKEN.zip";
    writeFile(broken, "not a zip");
    const fs::path directory = root / "Backups";

    // ---- listing ----
    {
        const auto backups = ManageStickBackups::list(directory, spare);
        assert(backups.size() == 3 && "an unreadable file is listed, not hidden");
        assert(backups[0].isCurrentStick && backups[0].description.stickLabel == "SPARE"
               && "this stick's backup comes first, whatever its age");
        assert(!backups[1].isCurrentStick && backups[1].description.stickLabel == "MAIN");
        assert(!backups[2].description.error.empty() && "unreadable ones last");
        assert(backups[1].trackCount == 1161 && backups[1].playlistCount == 24);
        assert(!backups[0].trackCount && "no fingerprint recorded: no count, not zero");
        assert(backups[1].description.archiveBytes > 0);

        const auto noCurrent = ManageStickBackups::list(directory, {});
        for (const auto &backup : noCurrent) {
            assert(!backup.isCurrentStick);
        }
        assert(ManageStickBackups::list(root / "missing", {}).empty());
    }
    std::cout << "case 1 (this stick first, counts from the fingerprint, unreadable listed last) OK\n";

    // ---- deleting while something writes the archive ----
    {
        seabass::infrastructure::backup::StickWriteLock writer(journal::lockPathFor(main).string());
        const DeleteStickBackupResult refused = ManageStickBackups::remove(main);
        assert(refused.status == DeleteStickBackupResult::Status::Busy);
        assert(!refused.message.empty());
        assert(fs::exists(main) && "a busy archive is left alone");
    }
    std::cout << "case 2 (an archive something is writing is refused and kept) OK\n";

    // ---- deleting ----
    {
        writeFile(journal::journalPathFor(main), "");
        const DeleteStickBackupResult deleted = ManageStickBackups::remove(main);
        assert(deleted.status == DeleteStickBackupResult::Status::Deleted);
        assert(!fs::exists(main));
        assert(!fs::exists(journal::journalPathFor(main)) && "the journal goes with it");
        assert(!fs::exists(journal::lockPathFor(main)) && "and so does the lock file");
        assert(fs::exists(spare) && "nothing else is touched");

        assert(ManageStickBackups::remove(broken).status == DeleteStickBackupResult::Status::Deleted
               && "an unreadable backup can be deleted too");

        const DeleteStickBackupResult gone = ManageStickBackups::remove(main);
        assert(gone.status == DeleteStickBackupResult::Status::Failed && !gone.message.empty());

        const auto left = ManageStickBackups::list(directory, {});
        assert(left.size() == 1 && left[0].description.stickLabel == "SPARE");
    }
    std::cout << "case 3 (delete removes archive, journal and lock; a missing one fails) OK\n";

    fs::remove_all(root, ec);
    std::cout << "manage_stick_backups_test passed\n";
    return 0;
}
