// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: Manage Backups deletes a full stick backup -- rig check FB9.
//
//   rig_delete_backup <backup dir> <archive> [--no-reference-guard]
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
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

// A path string normalised for use as a map key: directory_iterator joins
// the directory it was given to each filename with the platform's own
// separator, so an entry built from a directory argument that used the
// other style (this rig's own paths are all forward slashes, argv from a
// POSIX shell) never matched archive.string() by raw text on Windows --
// found by hand, comparing the two: same file, same bytes, one separator
// apart, listed as MISSING every time.
std::string normalisedKey(const fs::path &path)
{
    return path.lexically_normal().string();
}

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
        out[normalisedKey(path)] = entry;
    }
    return out;
}

// Containment without resolving anything, for a name that must be taken
// as written (see isAReference, and symlinked archives below).
bool insideDirectoryLexically(const fs::path &file, const fs::path &directory)
{
    const fs::path relative = file.lexically_normal().lexically_relative(directory.lexically_normal());
    return !relative.empty() && relative.begin()->string() != "..";
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
    // Resolved, deliberately: a symlink lying in the folder but pointing
    // out of it is not in the folder, and this tool deletes. (A reference
    // reached that way is named as a reference first: the reference guards
    // run before this one.)
    const fs::path relative = fs::relative(canonicalFile, canonicalDir, relativeEc);
    return !relativeEc && !relative.empty() && relative.begin()->string() != "..";
}

// A reference given as a symlink names one folder and resolves into
// another, and BOTH have to be protected -- the folder in the variable is
// the one a mistyped argument reaches, the folder it resolves into is
// where the file really lives.
//
// Both reference paths, or none: with only one set the other reference is
// unguarded by name while the tool would still report itself as guarded.
// Without them this tool cannot tell a reference from a test archive, so
// it refuses to run rather than offer protection it does not have.
bool referencePathsGiven()
{
    for (const char *variable : {"RIG_REFERENCE_A", "RIG_REFERENCE_B"}) {
        const char *value = std::getenv(variable);
        if (value == nullptr || *value == '\0') {
            return false;
        }
    }
    return true;
}

// A reference backup, by name: the rig's references are given in
// RIG_REFERENCE_A and RIG_REFERENCE_B (rig-shakedown.sh exports them).
// The file itself and anything in its folder count, both as written and
// as resolved, because the rig's references are symlinks into another
// folder: guarding only the resolved side left the folder the variable
// actually names -- the one a mistyped argument reaches -- unguarded.
bool isAReference(const fs::path &archive)
{
    std::error_code archiveEc;
    const fs::path canonicalArchive = fs::weakly_canonical(archive, archiveEc);
    if (archiveEc) {
        return true;  // cannot tell: refuse rather than delete
    }
    for (const char *variable : {"RIG_REFERENCE_A", "RIG_REFERENCE_B"}) {
        const char *value = std::getenv(variable);
        if (value == nullptr || *value == '\0') {
            continue;
        }
        // Made absolute before anything is compared: a bare "REF.zip" has
        // no parent at all, and an empty base makes every relative path
        // look like it sits beside a reference.
        std::error_code absoluteEc;
        const fs::path given = fs::absolute(fs::path(value), absoluteEc);
        std::error_code referenceEc;
        const fs::path resolved = fs::weakly_canonical(given, referenceEc);
        if (absoluteEc || referenceEc) {
            return true;
        }
        std::error_code archiveAbsoluteEc;
        const fs::path archiveAsGiven = fs::absolute(archive, archiveAbsoluteEc);
        if (archiveAbsoluteEc) {
            return true;
        }
        if (canonicalArchive == resolved || archiveAsGiven.lexically_normal() == given.lexically_normal()) {
            return true;
        }
        if (insideDirectory(canonicalArchive, resolved.parent_path())
            || insideDirectoryLexically(archiveAsGiven, given.parent_path())) {
            return true;
        }
    }
    return false;
}

}  // namespace

#if defined(_WIN32)
// Windows has no fork(): the busy-refusal proof below re-execs this very
// binary in this hidden mode instead, so a second process holds the lock
// exactly as the POSIX child does. Readiness is a marker file rather than
// a pipe write -- CreateProcess's anonymous pipes need overlapped I/O for
// a bounded read, and a marker the parent polls for is the same handshake
// with none of that.
int holdLock(const fs::path &archive)
{
    const fs::path lockPath = infrastructure::stick_backup::journal::lockPathFor(archive);
    const fs::path readyMarker = lockPath.string() + ".holding";
    try {
        infrastructure::backup::StickWriteLock held(lockPath.string());
        std::ofstream(readyMarker.string()).close();
        std::this_thread::sleep_for(std::chrono::seconds(30));
    } catch (const std::exception &) {
        return 2;
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
#if defined(_WIN32)
    if (argc == 3 && std::string(argv[1]) == "--hold-lock") {
        return holdLock(argv[2]);
    }
#endif
    const bool skipReferenceGuard = argc == 4 && std::string(argv[3]) == "--no-reference-guard";
    if (argc != 3 && !skipReferenceGuard) {
        std::cerr << "usage: rig_delete_backup <backup dir> <archive> [--no-reference-guard]\n";
        return 2;
    }
    const fs::path directory = argv[1];
    const fs::path archive = argv[2];
    bool pass = true;

    try {
        if (!skipReferenceGuard && !referencePathsGiven()) {
            std::cout << "refusing: RIG_REFERENCE_A and RIG_REFERENCE_B must both name a reference backup, or this "
                         "cannot tell a reference from a test one (pass --no-reference-guard to delete anyway)"
                         "\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (!skipReferenceGuard && isAReference(archive)) {
            std::cout << "refusing: " << archive.string() << " is a reference backup (or sits beside one)"
                      << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        // After the reference guards, so a reference is named as one
        // rather than reported as merely out of place.
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
        // A reference is kept read-only; refusing here costs nothing and
        // catches a reference this build was not told about. It is not
        // protection in itself: fs::remove only needs a writable parent.
        // Asked of the OS rather than read off the mode bits, which say
        // nothing about whether THIS user may write the file.
#if defined(_WIN32)
        std::error_code writableEc;
        const fs::perms permissions = fs::status(archive, writableEc).permissions();
        const bool writable = !writableEc && (permissions & fs::perms::owner_write) != fs::perms::none;
#else
        const bool writable = ::access(archive.c_str(), W_OK) == 0;
#endif
        if (!writable) {
            std::cout << "refusing: " << archive.string() << " is read-only, so it is not this rig's to delete"
                      << "\nRIG RESULT: FAIL\n";
            return 1;
        }

        const std::map<std::string, Listed> before = listing(directory);
        const auto listedEntry = before.find(normalisedKey(archive));
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
        // Bounded: a helper stuck in open()/flock() on an unresponsive
        // stick would otherwise hang the whole release run here, where the
        // old code at least gave up after its (racy) 700 ms.
        char token = 0;
        pollfd waitFor{ready[0], POLLIN, 0};
        const int readyNow = poll(&waitFor, 1, 10000);
        const bool helperHasIt = readyNow == 1 && read(ready[0], &token, 1) == 1 && token == 'L';
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
        // Bounded like the handshake above: a child wedged in open()/flock()
        // on an unresponsive stick ignores SIGTERM in uninterruptible sleep,
        // and an unbounded wait here would hang the release run the poll()
        // was added to protect.
        kill(helper, SIGTERM);
        int helperStatus = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 100 && !reaped; ++attempt) {
            const pid_t done = waitpid(helper, &helperStatus, WNOHANG);
            if (done == helper) {
                reaped = true;
                break;
            }
            if (done < 0) {
                break;
            }
            if (attempt == 50) {
                kill(helper, SIGKILL);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!reaped) {
            std::cout << "the lock holder would not end; refusing to delete anything\nRIG RESULT: FAIL\n";
            return 1;
        }
        const bool helperBehaved = helperHasIt && WIFSIGNALED(helperStatus);
        if (helperHasIt && !helperBehaved) {
            std::cout << "the lock holder ended on its own (status " << helperStatus
                      << "), so it may not have held the lock throughout\n";
        }
        if (!busyRefused || !stillThere || !helperBehaved) {
            // Nothing is deleted after an inconclusive probe: the archive
            // this check is about is exactly what a free lock would destroy.
            std::cout << "refusing to delete: the refusal under a held lock was not proven\nRIG RESULT: FAIL\n";
            return 1;
        }
#else
        // No fork(): a second run of this executable in --hold-lock mode
        // holds the archive's write lock instead, so the lock is still
        // contended for real, in a real second process.
        const fs::path lockPath = infrastructure::stick_backup::journal::lockPathFor(archive);
        const fs::path readyMarker = lockPath.string() + ".holding";
        std::error_code markerEc;
        // A marker left behind by a killed prior run would look like
        // instant readiness from a helper that never actually started.
        fs::remove(readyMarker, markerEc);

        wchar_t selfPathBuffer[MAX_PATH];
        const DWORD selfPathLength = ::GetModuleFileNameW(nullptr, selfPathBuffer, MAX_PATH);
        if (selfPathLength == 0 || selfPathLength == MAX_PATH) {
            std::cout << "could not find this program's own path to start a lock holder\nRIG RESULT: FAIL\n";
            return 1;
        }
        const std::wstring selfPath(selfPathBuffer, selfPathLength);
        std::wstring commandLine = L"\"" + selfPath + L"\" --hold-lock \"" + archive.wstring() + L"\"";

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};
        const BOOL started = ::CreateProcessW(selfPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                                               nullptr, nullptr, &startupInfo, &processInfo);
        if (!started) {
            std::cout << "could not start a lock holder\nRIG RESULT: FAIL\n";
            return 1;
        }
        ::CloseHandle(processInfo.hThread);

        // Bounded like the POSIX poll above: a helper stuck taking the lock
        // on an unresponsive stick would otherwise hang the whole release
        // run here.
        bool helperHasIt = false;
        for (int waited = 0; waited < 100; ++waited) {
            if (fs::exists(readyMarker, markerEc)) {
                helperHasIt = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
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
        // TerminateProcess rather than a graceful ask: this helper only
        // ever sleeps, so there is nothing for it to clean up, and the rig
        // waits on it next regardless.
        ::TerminateProcess(processInfo.hProcess, 1);
        ::WaitForSingleObject(processInfo.hProcess, 5000);
        ::CloseHandle(processInfo.hProcess);
        fs::remove(readyMarker, markerEc);
        if (!busyRefused || !stillThere) {
            // Nothing is deleted after an inconclusive probe: the archive
            // this check is about is exactly what a free lock would destroy.
            std::cout << "refusing to delete: the refusal under a held lock was not proven\nRIG RESULT: FAIL\n";
            return 1;
        }
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
        const std::string archiveKey = normalisedKey(archive);
        const bool goneFromList = after.count(archiveKey) == 0;
        bool othersKept = true;
        for (const auto &[path, entry] : before) {
            if (path == archiveKey) {
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
