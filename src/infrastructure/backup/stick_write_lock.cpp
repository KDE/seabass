// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "stick_write_lock.hpp"

#include <filesystem>

#include "infrastructure/paths/utf8_path.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seabass::infrastructure::backup
{

namespace fs = std::filesystem;

#if defined(__linux__) || defined(__APPLE__)

StickWriteLock::StickWriteLock(const fs::path &lockFilePath) : m_fd(-1), m_path(lockFilePath)
{
    fs::create_directories(lockFilePath.parent_path());
    m_fd = ::open(lockFilePath.c_str(), O_CREAT | O_RDWR, 0644);
    if (m_fd < 0) {
        throw std::runtime_error("Could not open stick lock file: " + pathToUtf8(lockFilePath));
    }
    if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(m_fd);
        m_fd = -1;
        throw StickBusyError(pathToUtf8(lockFilePath));
    }
    // The file locked must still be the one at the path. A holder removing
    // its lock file (releaseAndRemoveFile) unlinks it while it still holds
    // the lock; opening it just before that and locking it just after would
    // otherwise hold a lock on a deleted file that excludes nobody who opens
    // the path next. Busy, not an error: the holder that removed it was
    // there a moment ago.
    struct stat held {};
    struct stat named {};
    if (::fstat(m_fd, &held) != 0 || ::stat(lockFilePath.c_str(), &named) != 0 || held.st_ino != named.st_ino
        || held.st_dev != named.st_dev) {
        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
        m_fd = -1;
        throw StickBusyError(pathToUtf8(lockFilePath));
    }
}

StickWriteLock::~StickWriteLock()
{
    if (m_fd >= 0) {
        ::flock(m_fd, LOCK_UN);
        ::close(m_fd);
    }
}

void StickWriteLock::releaseAndRemoveFile()
{
    if (m_fd < 0) {
        return;
    }
    ::unlink(m_path.c_str());
    ::flock(m_fd, LOCK_UN);
    ::close(m_fd);
    m_fd = -1;
}

#elif defined(_WIN32)

// LockFileEx locks are scoped to the HANDLE (closing it releases the
// lock), same as flock()'s open-file-description scoping above -- two
// independent CreateFileW calls on the same path, even from the same
// process, correctly contend for the same lock.
StickWriteLock::StickWriteLock(const fs::path &lockFilePath) : m_handle(nullptr), m_path(lockFilePath)
{
    fs::create_directories(lockFilePath.parent_path());
    HANDLE handle = ::CreateFileW(lockFilePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not open stick lock file: " + pathToUtf8(lockFilePath));
    }
    OVERLAPPED overlapped = {};
    if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD,
                       &overlapped)) {
        ::CloseHandle(handle);
        throw StickBusyError(pathToUtf8(lockFilePath));
    }
    m_handle = handle;
}

StickWriteLock::~StickWriteLock()
{
    if (m_handle != nullptr) {
        HANDLE handle = static_cast<HANDLE>(m_handle);
        OVERLAPPED overlapped = {};
        ::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
        ::CloseHandle(handle);
    }
}

void StickWriteLock::releaseAndRemoveFile()
{
    if (m_handle == nullptr) {
        return;
    }
    HANDLE handle = static_cast<HANDLE>(m_handle);
    OVERLAPPED overlapped = {};
    ::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
    ::CloseHandle(handle);
    m_handle = nullptr;
    // Fails, harmlessly, when another holder has the file open: it is theirs now.
    ::DeleteFileW(m_path.c_str());
}

#else

// No advisory-lock implementation for this platform yet. This
// constructor always succeeds, so it currently provides no actual
// cross-process protection outside Linux/macOS/Windows -- don't remove
// this comment when that changes, callers rely on real exclusion where
// it's implemented.
StickWriteLock::StickWriteLock(const fs::path &lockFilePath) : m_fd(-1), m_path(lockFilePath) {}
StickWriteLock::~StickWriteLock() = default;
void StickWriteLock::releaseAndRemoveFile() {}

#endif

}  // namespace seabass::infrastructure::backup
