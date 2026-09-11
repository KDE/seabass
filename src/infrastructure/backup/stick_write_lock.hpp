// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

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
    explicit StickWriteLock(const std::string &lockFilePath);
    ~StickWriteLock();

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
    std::string m_path;
};

}  // namespace seabass::infrastructure::backup
