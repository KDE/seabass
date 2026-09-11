// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>

#include "infrastructure/stick_backup/archive_file.hpp"

namespace seabass::infrastructure::stick_backup
{

// What an incremental update writes down before it appends its first
// byte: how long the archive was, and where its end-of-central-directory
// record sat. With those two numbers an interrupted update can always be
// undone by truncation, and a finished one can be recognised.
struct JournalRecord
{
    std::uint64_t preLength = 0;      // 0 = there was no archive yet (first backup)
    std::uint64_t preEocdOffset = 0;  // meaningful only when preLength > 0
};

struct JournalState
{
    enum class Kind
    {
        Absent,   // empty file: no update in flight
        Valid,    // a record, CRC intact
        Corrupt,  // bytes present but not a valid record: the journal itself was
                  // being written when the crash hit, i.e. before any append
    };
    Kind kind = Kind::Absent;
    JournalRecord record;
};

// The journal lives in its own small file next to the archive
// (`<archive>.journal`) and is accessed through ArchiveFile so the
// fault-injection tests can crash it in memory together with the
// archive. On disk it is 24 bytes: "SBJ1", preLength, preEocdOffset,
// CRC32 of the preceding 20.
namespace journal
{

// `<archive>.journal`, next to the archive on the same volume.
inline std::filesystem::path journalPathFor(const std::filesystem::path &archivePath)
{
    std::filesystem::path path = archivePath;
    path += ".journal";
    return path;
}

// `<archive>.lock`, next to the archive on the same volume -- what
// infrastructure::backup::StickWriteLock takes to serialize BackupStick,
// RestoreStickBackup and CompactStickBackup against each other on this
// one archive, across threads, processes and the CLI/GUI boundary alike.
inline std::filesystem::path lockPathFor(const std::filesystem::path &archivePath)
{
    std::filesystem::path path = archivePath;
    path += ".lock";
    return path;
}

// Appends the record and issues a barrier. The file must be empty.
void write(ArchiveFile &journalFile, const JournalRecord &record);

JournalState read(const ArchiveFile &journalFile);

// Truncates to empty and issues a barrier.
void clear(ArchiveFile &journalFile);

}  // namespace journal

}  // namespace seabass::infrastructure::stick_backup
