// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/archive_file.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"

namespace seabass::infrastructure::stick_backup
{

// Where an entry's bytes come from while it is streamed into the
// archive. Returns how many bytes were placed in `out`; 0 means end of
// data. Implementations read the stick; tests count calls.
class EntrySource
{
public:
    virtual ~EntrySource() = default;
    virtual std::size_t read(std::span<std::byte> out) = 0;
};

// One generation of the archive, written the crash-safe way described in
// docs/stick-backup-plan.md ("Crash-safe incremental update"):
//
//   begin()        journal(preLength, preEocdOffset)  -> barrier
//   append*()      entries onto the tail; nothing before them is touched
//   commit()       barrier -> manifest + CD + EOCD -> barrier
//                  -> reopen and verify new entries + manifest -> clear journal
//   abort()        truncate back to preLength -> barrier -> clear journal
//
// Between begin() and commit()/abort() the file on disk is always the
// old archive followed by bytes no reader looks at, and the journal says
// how to get back. A cancelled backup returns control to the caller in
// exactly that state so the user can pick commit (keep for later) or
// abort (discard) -- see archive_recovery.hpp for what happens if nobody
// gets to choose.
class ArchiveUpdater
{
public:
    struct AppendedEntry
    {
        CentralEntry entry;
        hashing::Sha256Digest sha256{};
    };

    struct CommitResult
    {
        WriterBoundaries boundaries;
        std::uint64_t bytesAppended = 0;  // everything after preLength, trailer included
    };

    // `carriedEntries` / `priorEocdOffset` describe the archive as it is
    // now (empty vector and 0 for a first backup). The journal must be
    // empty -- run archive_recovery first if it is not.
    ArchiveUpdater(ArchiveFile &archive, ArchiveFile &journalFile, std::vector<CentralEntry> carriedEntries,
                   std::uint64_t priorEocdOffset, std::size_t chunkSize = 1u << 20);

    void begin();

    // Streams `source` into a new entry. Checks `cancel` between chunks;
    // when it fires mid-entry the partial bytes are abandoned (dead space,
    // never listed) and nullopt is returned. `progress` receives the
    // running byte count.
    std::optional<AppendedEntry> appendFile(const std::string &name, std::int64_t mtimeUnix, EntrySource &source,
                                            application::CancellationToken cancel,
                                            const std::function<void(std::uint64_t)> &progress = {});
    AppendedEntry appendFromMemory(const std::string &name, std::int64_t mtimeUnix, std::span<const std::byte> content);
    CentralEntry appendDirectory(const std::string &name, std::int64_t mtimeUnix);

    // Retracts the last `count` appended entries (they become dead space);
    // see Zip64Writer::forgetLastEntries.
    void forgetLastEntries(std::size_t count);

    // Throws ArchiveFormatError if the freshly written archive does not
    // read back correctly; the journal is then left in place on purpose,
    // so the next open rolls the update back.
    CommitResult commit(const BackupManifest &manifest);
    void abort();

    std::uint64_t preLength() const { return m_record.preLength; }
    const std::vector<CentralEntry> &entries() const { return m_writer.entries(); }
    std::size_t carriedEntryCount() const { return m_carriedCount; }
    std::size_t newEntryCount() const { return m_writer.entries().size() - m_carriedCount; }
    bool begun() const { return m_state == State::Begun; }

private:
    enum class State
    {
        Created,
        Begun,
        Committed,
        Aborted
    };

    void requireBegun() const;

    ArchiveFile &m_archive;
    ArchiveFile &m_journal;
    Zip64Writer m_writer;
    std::size_t m_carriedCount;
    JournalRecord m_record;
    std::size_t m_chunkSize;
    State m_state = State::Created;
};

// Re-reads `archive` and checks that every entry whose bytes lie at or
// beyond `fromOffset` (i.e. the ones a given update wrote) has a correct
// CRC, and that the manifest entry parses with a correct hash and lists
// exactly the archive's entries. Shared between commit() and recovery so
// both apply the same definition of "this update really landed".
bool verifyArchiveTail(const ArchiveFile &archive, std::uint64_t fromOffset, std::string *error = nullptr);

}  // namespace seabass::infrastructure::stick_backup
