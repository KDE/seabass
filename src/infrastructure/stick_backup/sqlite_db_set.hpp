// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/archive_updater.hpp"

namespace seabass::infrastructure::stick_backup
{

// Cheap "did this database move" fingerprint, read from ~100 bytes of
// headers, no page reads: the SQLite header's change counter (bytes
// 24-27, bumped on every commit in rollback-journal mode), the schema
// cookie, the main file size, and the WAL's size and salts (the WAL grows
// on every commit in WAL mode and its salts change on every reset --
// together they move whenever a transaction landed, even when the FAT
// mtime, at 2 s resolution, did not).
struct DbSetFingerprint
{
    std::uint32_t changeCounter = 0;
    std::uint32_t schemaCookie = 0;
    std::uint64_t mainSize = 0;
    bool hasWal = false;
    std::uint64_t walSize = 0;
    std::uint32_t walSalt1 = 0;
    std::uint32_t walSalt2 = 0;
    bool hasJournal = false;
    std::uint64_t journalSize = 0;

    std::string toHex() const;
    bool operator==(const DbSetFingerprint &) const = default;
};

// The main file plus whichever of `-wal` / `-journal` currently exist.
std::vector<std::filesystem::path> dbSetMembers(const std::filesystem::path &mainDb);

// nullopt when `mainDb` is not a SQLite file (wrong magic) or cannot be
// read -- callers then treat it as an ordinary file.
std::optional<DbSetFingerprint> fingerprintDbSet(const std::filesystem::path &mainDb);

// SQLite's byte-range locks sit at offset 0x40000000; on Windows a raw
// read of that page fails while a connection holds a lock. Rather than
// reason about it, databases at or beyond that size are refused (see
// docs/stick-backup-plan.md).
constexpr std::uint64_t MaxCapturableDbBytes = std::uint64_t{1} << 30;

struct DbSetCapture
{
    enum class Status
    {
        Captured,
        TooLarge,   // a member is >= MaxCapturableDbBytes; nothing was read
        Unstable,   // still changing after every retry; nothing is listed
        ReadError,  // a member vanished or could not be read
    };
    Status status = Status::Captured;
    std::vector<ArchiveUpdater::AppendedEntry> entries;  // one per member, in dbSetMembers() order
    std::vector<std::string> memberRelativePaths;
    std::vector<std::int64_t> memberMtimes;
    DbSetFingerprint fingerprint;  // as of the successful pass
    std::uint64_t bytesRead = 0;
    std::string detail;
};

// Byte-exact copy of the whole set with hash-before/after: every member
// is streamed into the archive (hash in flight), then re-hashed on the
// stick; any difference means something wrote during the window, the
// appended entries are forgotten (dead space) and the set is tried again,
// up to `retries` times. Members must be read as one set -- committed
// transactions may sit in the WAL, a leftover journal means the main file
// is mid-transaction -- so a mismatch on any member restarts all of them.
DbSetCapture captureDbSet(const std::filesystem::path &stickRoot, const std::string &relativeMainDb, ArchiveUpdater &updater,
                          int retries = 3, const std::function<void(std::uint64_t)> &progress = {});

}  // namespace seabass::infrastructure::stick_backup
