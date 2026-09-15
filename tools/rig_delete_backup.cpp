// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: Manage Backups deletes a full stick backup -- rig check FB9.
//
//   rig_delete_backup <backup dir> <archive>
//
// The archive must lie inside <backup dir>: the reference backups live
// elsewhere and this tool deletes for real, so a path outside it is
// refused rather than trusted.
//
// A run passes when:
//
// - Manage Backups lists the archive before the delete;
// - a delete is REFUSED while something else is writing the archive (a
//   helper process holds the very lock BackupStick, RestoreStickBackup
//   and CompactStickBackup take), and the archive is still there after;
// - the delete then reports Deleted, and the archive and its journal are
//   gone (the lock file may stay: see ManageStickBackups::remove);
// - the listing no longer holds it, and every other archive in the
//   folder is still listed, byte for byte the same size.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "application/use_cases/manage_stick_backups.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::DeleteStickBackupResult;
using application::ManageStickBackups;

namespace
{

std::string toString(DeleteStickBackupResult::Status status)
{
    switch (status) {
    case DeleteStickBackupResult::Status::Deleted:
        return "Deleted";
    case DeleteStickBackupResult::Status::Busy:
        return "Busy";
    case DeleteStickBackupResult::Status::Failed:
        return "Failed";
    }
    return "?";
}

// Every archive the folder lists, by path, with the size it reports.
std::map<std::string, std::uint64_t> listing(const fs::path &directory)
{
    std::map<std::string, std::uint64_t> out;
    for (const application::ManagedStickBackup &backup : ManageStickBackups::list(directory, {})) {
        out[backup.description.archivePath.string()] = backup.description.archiveBytes;
    }
    return out;
}

bool insideDirectory(const fs::path &file, const fs::path &directory)
{
    std::error_code ec;
    const fs::path relative = fs::relative(fs::weakly_canonical(file, ec), fs::weakly_canonical(directory, ec), ec);
    return !ec && !relative.empty() && relative.begin()->string() != "..";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: rig_delete_backup <backup dir> <archive>\n";
        return 2;
    }
    const fs::path directory = argv[1];
    const fs::path archive = argv[2];
    bool pass = true;

    try {
        if (!insideDirectory(archive, directory)) {
            std::cout << "refusing: " << archive.string() << " is not inside " << directory.string()
                      << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::error_code ec;
        if (!fs::is_regular_file(archive, ec)) {
            std::cout << "no archive at " << archive.string() << "\nRIG RESULT: FAIL\n";
            return 1;
        }

        const std::map<std::string, std::uint64_t> before = listing(directory);
        const bool listedBefore = before.count(archive.string()) != 0;
        std::cout << "Manage Backups lists " << before.size() << " backup(s); this one is "
                  << (listedBefore ? "among them" : "MISSING") << "\n";
        pass = pass && listedBefore;

#if !defined(_WIN32)
        // Refused while something else writes it. The helper holds the
        // archive's own write lock, exactly as a running backup would, in
        // a second process so the lock is contended for real.
        const fs::path lockPath = infrastructure::stick_backup::journal::lockPathFor(archive);
        const pid_t helper = fork();
        if (helper == 0) {
            try {
                infrastructure::backup::StickWriteLock held(lockPath.string());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            } catch (const std::exception &) {
                _exit(2);
            }
            _exit(0);
        }
        if (helper < 0) {
            std::cout << "could not fork a lock holder\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const DeleteStickBackupResult refused = ManageStickBackups::remove(archive);
        const bool stillThere = fs::is_regular_file(archive, ec);
        std::cout << "while another process writes it: " << toString(refused.status) << " -- " << refused.message
                  << "; archive " << (stillThere ? "still there" : "GONE") << "\n";
        pass = pass && refused.status == DeleteStickBackupResult::Status::Busy && stillThere;
        int helperStatus = 0;
        waitpid(helper, &helperStatus, 0);
#else
        std::cout << "the busy case needs fork(); skipped on this platform\n";
#endif

        const DeleteStickBackupResult deleted = ManageStickBackups::remove(archive);
        const bool archiveGone = !fs::exists(archive, ec);
        const fs::path journal = infrastructure::stick_backup::journal::journalPathFor(archive);
        const bool journalGone = !fs::exists(journal, ec);
        std::cout << "delete: " << toString(deleted.status) << (deleted.message.empty() ? "" : " -- " + deleted.message)
                  << "; archive " << (archiveGone ? "gone" : "STILL THERE") << ", journal "
                  << (journalGone ? "gone" : "STILL THERE") << "\n";
        pass = pass && deleted.status == DeleteStickBackupResult::Status::Deleted && archiveGone && journalGone;

        const std::map<std::string, std::uint64_t> after = listing(directory);
        const bool goneFromList = after.count(archive.string()) == 0;
        bool othersKept = true;
        for (const auto &[path, bytes] : before) {
            if (path == archive.string()) {
                continue;
            }
            const auto still = after.find(path);
            if (still == after.end() || still->second != bytes) {
                std::cout << "  other backup changed or lost: " << path << "\n";
                othersKept = false;
            }
        }
        std::cout << "listing afterwards: " << after.size() << " backup(s); this one "
                  << (goneFromList ? "gone" : "STILL LISTED") << "; the others "
                  << (othersKept ? "untouched" : "CHANGED") << "\n";
        pass = pass && goneFromList && othersKept;
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
