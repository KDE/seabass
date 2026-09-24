// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace seabass::infrastructure::backup
{

// Thrown when another process or thread already holds the write lock for
// this stick -- see StickWriteLock.
class StickBusyError : public std::runtime_error
{
public:
    explicit StickBusyError(const std::string &lockPath)
        : std::runtime_error("Another Seabass operation is already writing to this stick (lock: " + lockPath + ")")
    {
    }
};

// RAII mutual exclusion over a single stick's files, backed by an
// OS-level advisory lock (flock()) on a small marker file. Deliberately
// not a Qt type, so both the CLI and the GUI can take this exact lock
// before any write -- closing the same window whether the conflict is two
// GUI controllers racing in one process, two seabass windows, or a
// concurrent CLI run against the same stick.
//
// flock() locks are scoped to the *open file description*, not the
// process, so two independent open() calls on the same path -- including
// two from the same process -- correctly contend for the same lock
// exactly like two separate processes would. That makes this one
// primitive sufficient for both the in-process and cross-process race.
class StickWriteLock
{
public:
    // lockFilePath is typically <stick root>/Seabass/backups/.write.lock
    // -- callers construct it from whichever "stick root" they already
    // compute for FilesystemBackupStore. Throws StickBusyError if another
    // holder already has it; std::runtime_error if the lock file itself
    // can't be created/opened.
    //
    // A path, not a string: on Windows a std::string path is read in the
    // ANSI code page, and CreateFileA could not open a lock beside a backup
    // in a folder named outside it -- so neither could the backup, compact
    // or restore that takes the lock first.
    explicit StickWriteLock(const std::filesystem::path &lockFilePath);
    ~StickWriteLock();

    // Releases the lock and deletes its file, for a holder that is removing
    // the thing the lock guarded -- a first backup that was discarded leaves
    // no archive, so it should leave no lock file beside it either. On
    // POSIX the file is unlinked while still held, and every constructor
    // checks that the file it locked is still the one at the path, so a
    // lock taken on the deleted file cannot stand next to a new one. On
    // Windows an open lock file cannot be deleted by anyone else, so it is
    // deleted after the release, and not at all if another holder has
    // opened it meanwhile. The object holds nothing afterwards.
    void releaseAndRemoveFile();

    StickWriteLock(const StickWriteLock &) = delete;
    StickWriteLock &operator=(const StickWriteLock &) = delete;

private:
#if defined(_WIN32)
    // Really a HANDLE (void*) -- kept as void* here so <windows.h> doesn't
    // leak into every includer of this header; stick_write_lock.cpp casts
    // it back.
    void *m_handle;
#else
    int m_fd;
#endif
    std::filesystem::path m_path;
};

}  // namespace seabass::infrastructure::backup
