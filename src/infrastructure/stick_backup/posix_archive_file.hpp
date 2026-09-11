// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

#include "infrastructure/stick_backup/archive_file.hpp"

namespace seabass::infrastructure::stick_backup
{

// ArchiveFile over a real file. "Posix" in the name is the primary
// implementation; the Win32 branch (CreateFileW/ReadFile/WriteFile/
// FlushFileBuffers) lives in the same translation unit under _WIN32 so
// the MinGW cross-build keeps working.
//
// Not movable or copyable -- hold it in a std::unique_ptr. size() is
// tracked in memory after the initial fstat; nothing else is expected to
// write the file while it is open (the journal + StickWriteLock-style
// exclusivity is the caller's business).
class PosixArchiveFile final : public ArchiveFile
{
public:
    enum class OpenMode
    {
        ReadOnly,   // must exist
        ReadWrite,  // created if missing, never truncated on open
    };

    PosixArchiveFile(const std::filesystem::path &path, OpenMode mode);
    ~PosixArchiveFile() override;

    PosixArchiveFile(const PosixArchiveFile &) = delete;
    PosixArchiveFile &operator=(const PosixArchiveFile &) = delete;

    const std::filesystem::path &path() const { return m_path; }

    std::uint64_t size() const override { return m_size; }
    void append(std::span<const std::byte> bytes) override;
    void readAt(std::uint64_t offset, std::span<std::byte> out) const override;
    void truncate(std::uint64_t newSize) override;
    void barrier() override;

private:
    std::filesystem::path m_path;
    OpenMode m_mode;
    std::uint64_t m_size = 0;
#if defined(_WIN32)
    void *m_handle = nullptr;  // HANDLE, kept as void* so <windows.h> stays out of this header
#else
    int m_fd = -1;
#endif
};

}  // namespace seabass::infrastructure::stick_backup
