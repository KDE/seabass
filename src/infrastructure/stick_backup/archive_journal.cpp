// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/archive_journal.hpp"

#include <zlib.h>

#include <string>
#include <vector>

#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup::journal
{

namespace
{

constexpr std::string_view Magic = "SBJ1";
constexpr std::size_t RecordSize = 4 + 8 + 8 + 4;

std::uint32_t crcOf(std::string_view bytes)
{
    return static_cast<std::uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef *>(bytes.data()), static_cast<uInt>(bytes.size())));
}

}  // namespace

void write(ArchiveFile &journalFile, const JournalRecord &record)
{
    if (journalFile.size() != 0) {
        throw ArchiveIoError("journal is not empty: another update is in flight or was never recovered");
    }
    std::string bytes;
    bytes += Magic;
    zip::putU64(bytes, record.preLength);
    zip::putU64(bytes, record.preEocdOffset);
    zip::putU32(bytes, crcOf(bytes));
    journalFile.append(zip::bytesOf(bytes));
    journalFile.barrier();
}

JournalState read(const ArchiveFile &journalFile)
{
    JournalState state;
    if (journalFile.size() == 0) {
        return state;
    }
    if (journalFile.size() != RecordSize) {
        state.kind = JournalState::Kind::Corrupt;
        return state;
    }
    std::vector<std::byte> bytes(RecordSize);
    journalFile.readAt(0, bytes);
    std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (text.substr(0, Magic.size()) != Magic || zip::readU32(bytes, 20) != crcOf(text.substr(0, 20))) {
        state.kind = JournalState::Kind::Corrupt;
        return state;
    }
    state.kind = JournalState::Kind::Valid;
    state.record.preLength = zip::readU64(bytes, 4);
    state.record.preEocdOffset = zip::readU64(bytes, 12);
    return state;
}

void clear(ArchiveFile &journalFile)
{
    journalFile.truncate(0);
    journalFile.barrier();
}

}  // namespace seabass::infrastructure::stick_backup::journal
