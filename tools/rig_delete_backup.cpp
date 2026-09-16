// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: Manage Backups deletes a full stick backup -- rig check FB9.
//
//   rig_delete_backup <backup dir> <archive>
//
// This tool deletes for real, so it refuses anything it is not sure of:
// an archive outside <backup dir>, an archive that is (or lies beside)
// a reference backup named by RIG_REFERENCE_A/RIG_REFERENCE_B, an
// archive the running user cannot write, and an archive Manage Backups
// cannot read as a backup. The containment check alone would not save a
// reference -- pointing both arguments at the reference folder passes it,
// and a read-only file is still removable through a writable parent --
// which is why the reference paths are consulted by name.
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
//   folder is still listed, with the same size and modification time on
//   disk (the listing's own byte count is 0 for an archive it cannot
//   parse, so it cannot carry this check by itself).
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
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

// What the folder lists, and what is actually on disk for each entry:
// the listing's own archiveBytes is 0 for anything it cannot parse, so a
// sibling replaced by rubbish would otherwise look unchanged.
struct Listed
{
    bool readable = false;
    std::uintmax_t sizeOnDisk = 0;
    std::int64_t mtime = 0;
};

std::map<std::string, Listed> listing(const fs::path &directory)
{
    std::map<std::string, Listed> out;
    for (const application::ManagedStickBackup &backup : ManageStickBackups::list(directory, {})) {
        const fs::path &path = backup.description.archivePath;
        std::error_code sizeEc;
        std::error_code timeEc;
        Listed entry;
        entry.readable = backup.description.error.empty();
        entry.sizeOnDisk = fs::file_size(path, sizeEc);
        if (sizeEc) {
            entry.sizeOnDisk = 0;
        }
        const auto stamp = fs::last_write_time(path, timeEc);
        entry.mtime = timeEc ? 0 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
        out[path.string()] = entry;
    }
    return out;
}

// Each call gets its own error_code: sharing one let a failure to
// canonicalise the first path be overwritten by the next call's success.
bool insideDirectory(const fs::path &file, const fs::path &directory)
{
    std::error_code fileEc;
    std::error_code dirEc;
    std::error_code relativeEc;
    const fs::path canonicalFile = fs::weakly_canonical(file, fileEc);
    const fs::path canonicalDir = fs::weakly_canonical(directory, dirEc);
    if (fileEc || dirEc) {
        return false;
    }
    const fs::path relative = fs::relative(canonicalFile, canonicalDir, relativeEc);
    return !relativeEc && !relative.empty() && relative.begin()->string() != "..";
}

// A reference backup, by name: the rig's references are given in
// RIG_REFERENCE_A and RIG_REFERENCE_B (rig-shakedown.sh exports them).
// Both the file itself and anything in its folder count, because the
// folder is what a mistyped argument reaches.
bool isAReference(const fs::path &archive)
{
    for (const char *variable : {"RIG_REFERENCE_A", "RIG_REFERENCE_B"}) {
        const char *value = std::getenv(variable);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        std::error_code referenceEc;
        std::error_code archiveEc;
        const fs::path reference = fs::weakly_canonical(fs::path(value), referenceEc);
        const fs::path canonicalArchive = fs::weakly_canonical(archive, archiveEc);
        if (referenceEc || archiveEc) {
            return true;  // cannot tell: refuse rather than delete
        }
        if (canonicalArchive == reference || insideDirectory(canonicalArchive, reference.parent_path())) {
            return true;
        }
    }
    return false;
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
        if (isAReference(archive)) {
            std::cout << "refusing: " << archive.string() << " is a reference backup (or sits beside one)"
                      << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::error_code ec;
        if (!fs::is_regular_file(archive, ec)) {
            std::cout << "no archive at " << archive.string() << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        // A reference is kept read-only; refusing here costs nothing and
        // catches a reference this build was not told about. It is not
        // protection in itself: fs::remove only needs a writable parent.
        std::error_code writableEc;
        const fs::perms permissions = fs::status(archive, writableEc).permissions();
        if (writableEc || (permissions & fs::perms::owner_write) == fs::perms::none) {
            std::cout << "refusing: " << archive.string() << " is read-only, so it is not this rig's to delete"
                      << "\nRIG RESULT: FAIL\n";
            return 1;
        }

        const std::map<std::string, Listed> before = listing(directory);
        const auto listedEntry = before.find(archive.string());
        const bool listedBefore = listedEntry != before.end() && listedEntry->second.readable;
        std::cout << "Manage Backups lists " << before.size() << " backup(s); this one is "
                  << (listedEntry == before.end() ? "MISSING"
                                                  : (listedBefore ? "among them" : "listed but UNREADABLE"))
                  << "\n";
        if (!listedBefore) {
            // Refused rather than deleted: this tool removes what Manage
            // Backups shows as a backup, and something it cannot read as
            // one is exactly what it must not remove on the rig's say-so.
            std::cout << "refusing: not a readable backup at " << archive.string() << "\nRIG RESULT: FAIL\n";
            return 1;
        }

#if !defined(_WIN32)
        // Refused while something else writes it. The helper holds the
        // archive's own write lock, exactly as a running backup would, in
        // a second process so the lock is contended for real.
        const fs::path lockPath = infrastructure::stick_backup::journal::lockPathFor(archive);
        // The child says when it holds the lock; a timed guess raced it,
        // and losing that race DELETES the archive the check is about.
        int ready[2] = {-1, -1};
        if (pipe(ready) != 0) {
            std::cout << "could not make a readiness pipe\nRIG RESULT: FAIL\n";
            return 1;
        }
        const pid_t helper = fork();
        if (helper == 0) {
            close(ready[0]);
            try {
                infrastructure::backup::StickWriteLock held(lockPath.string());
                const char token = 'L';
                const ssize_t written = write(ready[1], &token, 1);
                (void)written;
                // Long enough that the parent's delete attempt is always
                // inside it; the parent kills this child as soon as it has
                // its answer, so the wait is not actually paid.
                std::this_thread::sleep_for(std::chrono::seconds(30));
            } catch (const std::exception &) {
                _exit(2);
            }
            _exit(0);
        }
        if (helper < 0) {
            std::cout << "could not fork a lock holder\nRIG RESULT: FAIL\n";
            return 1;
        }
        close(ready[1]);
        char token = 0;
        const bool helperHasIt = read(ready[0], &token, 1) == 1 && token == 'L';
        close(ready[0]);
        bool busyRefused = false;
        bool stillThere = false;
        if (helperHasIt) {
            const DeleteStickBackupResult refused = ManageStickBackups::remove(archive);
            stillThere = fs::is_regular_file(archive, ec);
            busyRefused = refused.status == DeleteStickBackupResult::Status::Busy;
            std::cout << "while another process writes it: " << toString(refused.status) << " -- " << refused.message
                      << "; archive " << (stillThere ? "still there" : "GONE") << "\n";
        } else {
            std::cout << "the lock holder never took the lock: the refusal was not checked\n";
        }
        kill(helper, SIGTERM);
        int helperStatus = 0;
        waitpid(helper, &helperStatus, 0);
        const bool helperBehaved = helperHasIt && WIFSIGNALED(helperStatus);
        if (helperHasIt && !helperBehaved) {
            std::cout << "the lock holder ended on its own (status " << helperStatus
                      << "), so it may not have held the lock throughout\n";
        }
        pass = pass && busyRefused && stillThere && helperBehaved;
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

        const std::map<std::string, Listed> after = listing(directory);
        const bool goneFromList = after.count(archive.string()) == 0;
        bool othersKept = true;
        for (const auto &[path, entry] : before) {
            if (path == archive.string()) {
                continue;
            }
            const auto still = after.find(path);
            if (still == after.end() || still->second.readable != entry.readable
                || still->second.sizeOnDisk != entry.sizeOnDisk || still->second.mtime != entry.mtime) {
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
