// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/work_counters.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

#if defined(_WIN32)

bool writeFileDurably(const std::string &path, const std::string &data)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    if (ok && written == data.size()) {
        ok = FlushFileBuffers(h) != 0;
    } else {
        ok = FALSE;
    }
    CloseHandle(h);
    return ok != 0;
}

bool appendDurably(const std::string &path, const std::string &data)
{
    // GENERIC_WRITE as well as FILE_APPEND_DATA: cutting a partial write
    // back (below) needs SetEndOfFile, which append-only access does not
    // allow. The appending itself still goes through the append offset.
    HANDLE h = CreateFileA(path.c_str(), FILE_APPEND_DATA | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER before{};
    const BOOL haveSize = GetFileSizeEx(h, &before);
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    if (ok && written == data.size()) {
        ok = FlushFileBuffers(h) != 0;
    } else {
        ok = FALSE;
    }
    if (!ok && haveSize) {
        // Same reason as the POSIX side: half a line here becomes one
        // line glued to the next record. Best effort.
        LARGE_INTEGER move = before;
        if (SetFilePointerEx(h, move, nullptr, FILE_BEGIN) && SetEndOfFile(h)) {
            FlushFileBuffers(h);
        }
    }
    CloseHandle(h);
    return ok != 0;
}

#else

bool writeFileDurably(const std::string &path, const std::string &data)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const char *p = data.data();
    size_t remaining = data.size();
    bool ok = true;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n <= 0) {
            ok = false;
            break;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    if (ok) {
#if defined(__APPLE__)
        // fsync() on macOS does not flush the drive's write cache;
        // F_FULLFSYNC does. Fall back to fsync where a filesystem
        // rejects it.
        ok = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
        ok = ::fsync(fd) == 0;
#endif
    }
    ::close(fd);
    return ok;
}

bool appendDurably(const std::string &path, const std::string &data)
{
    // O_APPEND, so every write goes to the current end of the file even
    // with another process appending to the same manifest -- the
    // property the plain ofstream had and the reason this is an append
    // rather than a rewrite.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return false;
    }
    // Where the file ended before this call. A write that stops halfway
    // (the full stick this is guarding against) otherwise leaves a line
    // with no newline on it, and the NEXT append lands on the same line:
    // one record swallowing another, with the parser taking the first
    // filePath it finds in the merged text. Cut back to here instead, so
    // a failed append leaves the file exactly as it was.
    const off_t before = ::lseek(fd, 0, SEEK_END);
    const char *p = data.data();
    size_t remaining = data.size();
    bool ok = true;
    while (remaining > 0) {
        ssize_t n = ::write(fd, p, remaining);
        if (n <= 0) {
            ok = false;
            break;
        }
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    if (!ok && before >= 0) {
        // Best effort by necessity: if this fails too there is nothing
        // further to try, and the caller is already being told the
        // append did not happen.
        if (::ftruncate(fd, before) == 0) {
#if defined(__APPLE__)
            if (::fcntl(fd, F_FULLFSYNC) != 0) {
                ::fsync(fd);
            }
#else
            ::fsync(fd);
#endif
        }
    }
    if (ok) {
#if defined(__APPLE__)
        ok = ::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0;
#else
        ok = ::fsync(fd) == 0;
#endif
    }
    ::close(fd);
    return ok;
}

#endif

}  // namespace

#if defined(_WIN32)

void fsyncDirectoryContaining(const std::string &)
{
    // No direct equivalent needed on Windows: FlushFileBuffers on the
    // file itself already forces the data durable, and MoveFileEx-based
    // renames (what std::filesystem::rename uses here) don't have the
    // same "directory entry update" durability gap POSIX rename does.
}

#else

// Standard "durable rename" pattern: fsync-ing the file's own data
// isn't enough by itself -- the directory entry the rename just updated
// also needs to be flushed, or a crash right after a successful rename
// can still lose that update on some filesystems/media.
void fsyncDirectoryContaining(const std::string &filePath)
{
    fs::path dir = fs::path(filePath).parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    int dirFd = ::open(dir.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }
}

#endif

bool appendToFileDurably(const std::string &path, const std::string &data)
{
    WorkCounters::instance().noteDurableFileWrite();
    const bool existed = fs::exists(path);
    if (!appendDurably(path, data)) {
        return false;
    }
    if (!existed) {
        // A file this call created is a new directory entry, and that
        // entry needs the same flush a rename does -- otherwise the very
        // first line written to a fresh manifest is the one a pulled
        // stick loses.
        fsyncDirectoryContaining(path);
    }
    return true;
}

bool writeFileDurablyAtomic(const std::string &path, const std::string &data)
{
    WorkCounters::instance().noteDurableFileWrite();
    std::string tempPath = path + ".tmp-seabass-write";
    if (!writeFileDurably(tempPath, data)) {
        std::error_code removeEc;
        fs::remove(tempPath, removeEc);
        return false;
    }

    std::error_code ec;
    fs::rename(tempPath, path, ec);
    if (ec) {
        fs::remove(tempPath, ec);
        return false;
    }
    fsyncDirectoryContaining(path);
    return true;
}

bool copyFileDurablyAtomic(const std::string &sourcePath, const std::string &targetPath)
{
    std::ifstream in(sourcePath, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        return false;
    }
    return writeFileDurablyAtomic(targetPath, buffer.str());
}

}  // namespace seabass::infrastructure
