#include "infrastructure/stick_backup/posix_archive_file.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace seabass::infrastructure::stick_backup
{

namespace
{

[[noreturn]] void throwIo(const std::string &what, const std::filesystem::path &path)
{
#if defined(_WIN32)
    throw ArchiveIoError(what + " " + path.string() + " (error " + std::to_string(GetLastError()) + ")");
#else
    throw ArchiveIoError(what + " " + path.string() + ": " + std::strerror(errno));
#endif
}

}  // namespace

#if defined(_WIN32)

PosixArchiveFile::PosixArchiveFile(const std::filesystem::path &path, OpenMode mode) : m_path(path), m_mode(mode)
{
    DWORD access = mode == OpenMode::ReadOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD disposition = mode == OpenMode::ReadOnly ? OPEN_EXISTING : OPEN_ALWAYS;
    // Share mode has to emulate POSIX open() semantics, which is what
    // every caller of this class was written against -- a POSIX fd puts
    // no restriction on anyone else opening, writing, or unlinking the
    // same file, whereas CreateFileW's default (0) excludes all of that
    // and FILE_SHARE_READ alone still excludes writers and deleters.
    // Each omission caused a real, separate Windows-only failure:
    //
    //   - without FILE_SHARE_WRITE, a second open for writing fails with
    //     ERROR_SHARING_VIOLATION (32). BackupStick's cancel/keep/resume
    //     path hit this and threw ArchiveIoError.
    //   - without FILE_SHARE_DELETE, fs::remove() on a still-open handle
    //     silently fails. RestoreStickBackup removes its .seabass-restore-tmp
    //     file from inside the scope that still holds it open (see
    //     writeEntry()'s checksum-mismatch branch, which discards the
    //     error code), so a damaged entry left its temp file behind
    //     instead of cleaning up.
    HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throwIo("could not open", path);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        throwIo("could not stat", path);
    }
    m_handle = h;
    m_size = static_cast<std::uint64_t>(size.QuadPart);
}

PosixArchiveFile::~PosixArchiveFile()
{
    if (m_handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
}

void PosixArchiveFile::append(std::span<const std::byte> bytes)
{
    if (m_mode == OpenMode::ReadOnly) {
        throw ArchiveIoError("append on read-only archive " + m_path.string());
    }
    // Same guard as the POSIX build below: never write into a hole. If
    // something shortened the file behind us, a write at the length we
    // remember lands past the real end, NTFS zero-fills the gap, and the
    // archive keeps its expected length with nothing inside.
    LARGE_INTEGER current{};
    if (!GetFileSizeEx(static_cast<HANDLE>(m_handle), &current)) {
        throwIo("could not stat", m_path);
    }
    if (static_cast<std::uint64_t>(current.QuadPart) < m_size) {
        throw ArchiveIoError("archive " + m_path.string() + " shrank underneath us: expected at least "
                             + std::to_string(m_size) + " bytes, found " + std::to_string(current.QuadPart)
                             + " -- something else wrote to it while a backup was running");
    }
    const char *p = reinterpret_cast<const char *>(bytes.data());
    std::size_t remaining = bytes.size();
    std::uint64_t offset = m_size;
    while (remaining > 0) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_handle), p, chunk, &written, &ov) || written == 0) {
            throwIo("write failed on", m_path);
        }
        p += written;
        remaining -= written;
        offset += written;
    }
    m_size = offset;
}

void PosixArchiveFile::readAt(std::uint64_t offset, std::span<std::byte> out) const
{
    if (offset + out.size() > m_size) {
        throw ArchiveIoError("read past end of " + m_path.string());
    }
    char *p = reinterpret_cast<char *>(out.data());
    std::size_t remaining = out.size();
    while (remaining > 0) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(m_handle), p, chunk, &got, &ov) || got == 0) {
            throwIo("short read on", m_path);
        }
        p += got;
        remaining -= got;
        offset += got;
    }
}

void PosixArchiveFile::truncate(std::uint64_t newSize)
{
    if (m_mode == OpenMode::ReadOnly) {
        throw ArchiveIoError("truncate on read-only archive " + m_path.string());
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(newSize);
    if (!SetFilePointerEx(static_cast<HANDLE>(m_handle), pos, nullptr, FILE_BEGIN)
        || !SetEndOfFile(static_cast<HANDLE>(m_handle))) {
        throwIo("truncate failed on", m_path);
    }
    m_size = newSize;
}

void PosixArchiveFile::barrier()
{
    if (m_mode == OpenMode::ReadOnly) {
        return;
    }
    if (!FlushFileBuffers(static_cast<HANDLE>(m_handle))) {
        throwIo("FlushFileBuffers failed on", m_path);
    }
}

#else

PosixArchiveFile::PosixArchiveFile(const std::filesystem::path &path, OpenMode mode) : m_path(path), m_mode(mode)
{
    int flags = mode == OpenMode::ReadOnly ? O_RDONLY : (O_RDWR | O_CREAT);
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        throwIo("could not open", path);
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throwIo("could not stat", path);
    }
    m_fd = fd;
    m_size = static_cast<std::uint64_t>(st.st_size);
}

PosixArchiveFile::~PosixArchiveFile()
{
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

void PosixArchiveFile::append(std::span<const std::byte> bytes)
{
    if (m_mode == OpenMode::ReadOnly) {
        throw ArchiveIoError("append on read-only archive " + m_path.string());
    }
    // Appending means writing at the length we believe the file has. If
    // something else shortened it behind our back, that write lands past
    // the real end and the kernel fills the gap with a hole of zeros --
    // producing a file of exactly the expected length containing nothing.
    // A 12 GB backup was lost that way (a listing pass recovered a
    // journal mid-write and truncated the archive). Never write into a
    // hole: stop, and let the caller report it.
    struct stat before{};
    if (::fstat(m_fd, &before) != 0) {
        throwIo("could not stat", m_path);
    }
    if (static_cast<std::uint64_t>(before.st_size) < m_size) {
        throw ArchiveIoError("archive " + m_path.string() + " shrank underneath us: expected at least "
                             + std::to_string(m_size) + " bytes, found " + std::to_string(before.st_size)
                             + " -- something else wrote to it while a backup was running");
    }
    const char *p = reinterpret_cast<const char *>(bytes.data());
    std::size_t remaining = bytes.size();
    std::uint64_t offset = m_size;
    while (remaining > 0) {
        ssize_t n = ::pwrite(m_fd, p, remaining, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            throwIo("write failed on", m_path);
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
        offset += static_cast<std::uint64_t>(n);
    }
    m_size = offset;
}

void PosixArchiveFile::readAt(std::uint64_t offset, std::span<std::byte> out) const
{
    if (offset + out.size() > m_size) {
        throw ArchiveIoError("read past end of " + m_path.string());
    }
    char *p = reinterpret_cast<char *>(out.data());
    std::size_t remaining = out.size();
    while (remaining > 0) {
        ssize_t n = ::pread(m_fd, p, remaining, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            throwIo("short read on", m_path);
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
        offset += static_cast<std::uint64_t>(n);
    }
}

void PosixArchiveFile::truncate(std::uint64_t newSize)
{
    if (m_mode == OpenMode::ReadOnly) {
        throw ArchiveIoError("truncate on read-only archive " + m_path.string());
    }
    if (::ftruncate(m_fd, static_cast<off_t>(newSize)) != 0) {
        throwIo("truncate failed on", m_path);
    }
    m_size = newSize;
}

void PosixArchiveFile::barrier()
{
    if (m_mode == OpenMode::ReadOnly) {
        return;
    }
#if defined(__APPLE__)
    // fsync() on macOS only pushes data to the drive, not through its
    // write cache; F_FULLFSYNC is the call that actually waits for the
    // medium. Fall back to fsync on filesystems that reject it.
    if (::fcntl(m_fd, F_FULLFSYNC) == 0) {
        return;
    }
#endif
    if (::fsync(m_fd) != 0) {
        throwIo("fsync failed on", m_path);
    }
}

#endif

}  // namespace seabass::infrastructure::stick_backup
