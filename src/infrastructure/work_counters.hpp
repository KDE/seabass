// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace seabass::infrastructure
{

// How many times the expensive-per-item operations actually happened.
//
// Every performance problem this project has found in its own write paths
// had the same shape: something that should happen once per save happening
// once per item instead. Wall-clock cannot catch that in a test, because
// timing is a property of the medium -- the same Engine write costs 151 ms
// on a USB stick and 0.8 ms on a ramdisk (docs/write-path-performance.md).
// A count is the same number on every machine, on every platform, in a
// fraction of a second.
//
// So these are the portable half of performance testing: a test drives a
// realistic batch of work and asserts how many database opens and file
// parses it took. Cheap enough to leave compiled in always (four relaxed
// atomic increments on paths that are already doing disk I/O), which
// matters because a counter behind a build flag is a counter nobody reads.
struct WorkCountersSnapshot
{
    std::uint64_t engineDatabaseOpens = 0;
    std::uint64_t encryptedDatabaseOpens = 0;
    std::uint64_t trackDatabaseParses = 0;
    std::uint64_t durableFileWrites = 0;

    std::string describe() const;
};

class WorkCounters
{
public:
    static WorkCounters &instance();

    // Opening an Engine library: a full SQLite open plus schema
    // detection, ~151 ms per call against a stick.
    void noteEngineDatabaseOpen() { m_engineDatabaseOpens.fetch_add(1, std::memory_order_relaxed); }
    // Opening a SQLCipher database. The costly part is not the I/O but
    // deriving the key from a passphrase, ~115 ms of CPU every time,
    // which is why a scratch copy on a ramdisk does not help here.
    void noteEncryptedDatabaseOpen() { m_encryptedDatabaseOpens.fetch_add(1, std::memory_order_relaxed); }
    // Parsing export.pdb end to end, ~8 ms, historically done twice per
    // item where one pass per save indexes every track in 0.03 s.
    void noteTrackDatabaseParse() { m_trackDatabaseParses.fetch_add(1, std::memory_order_relaxed); }
    // A whole-file durable replacement: write, flush, rename, flush.
    void noteDurableFileWrite() { m_durableFileWrites.fetch_add(1, std::memory_order_relaxed); }

    WorkCountersSnapshot snapshot() const;
    void reset();

private:
    std::atomic<std::uint64_t> m_engineDatabaseOpens{0};
    std::atomic<std::uint64_t> m_encryptedDatabaseOpens{0};
    std::atomic<std::uint64_t> m_trackDatabaseParses{0};
    std::atomic<std::uint64_t> m_durableFileWrites{0};
};

}  // namespace seabass::infrastructure
