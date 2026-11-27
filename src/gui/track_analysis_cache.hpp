// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "domain/track_analysis.hpp"

struct sqlite3;

namespace seabass::infrastructure::rekordbox
{
class AnlzPathIndex;
}

namespace seabass::gui
{

// A rekordbox track's analysis as a list row draws it (the overview, its
// look, the beat grid) kept on the computer, so a row seen before is drawn
// without reading the stick again: not after a save, not in the next run.
//
// Every row read two analysis files off the stick, the .DAT for the grid
// and, since rekordbox's 3Band overview is drawn, the end of the .2EX:
// about 2.5 ms of USB reads a track, cold, on the GUI thread as the list
// scrolls (measured on WHALESHARK, 2026-11-26). The in-memory cache of
// PlaybackController keeps 300 tracks and is dropped by every write and
// every change of sticks. Analysis files change only when rekordbox
// exports again or Seabass writes them, so they are worth keeping.
//
// Keyed by the files, never by the track's id or its file's name alone:
// the key is the .DAT's path with its size and modification time, and the
// .2EX's size and time (or that there is none). Two `stat`s a row on a
// hit. A rewrite by anyone else changes the key, so a stale entry is never
// found, only evicted. A rewrite by Seabass may not: FAT keeps a file's
// time to two seconds, and Analyse again, Restore original analysis, an
// import and the cue writers rewrite an analysis file in place, often at
// the same size, within a step of the last write. So every entry of a
// track folder this process has written an analysis file in since (infra-
// structure::noteAnalysisFileWritten(), which every such write goes
// through) is dropped before the next lookup, and an analysis read while
// one was written is not kept. Hashing the files instead would cost the
// very read the cache saves.
//
// One SQLite file next to the cover cache (defaultDatabaseFile()), in WAL
// mode, capped (about 64 MB, a few thousand tracks a stick), least
// recently used first out. A cache and never a source of truth: a
// database that cannot be opened or is damaged is made again, and if that
// fails the files are read as before.
//
// Thread-safe: the player reads on the GUI thread, the Beat Grids page in
// its workers.
class TrackAnalysisCache
{
public:
    static constexpr std::int64_t DefaultCapBytes = 64LL * 1024 * 1024;

    explicit TrackAnalysisCache(QString databaseFile, std::int64_t capBytes = DefaultCapBytes);
    ~TrackAnalysisCache();
    TrackAnalysisCache(const TrackAnalysisCache &) = delete;
    TrackAnalysisCache &operator=(const TrackAnalysisCache &) = delete;

    // The one the app uses: defaultDatabaseFile(), made at first use.
    static TrackAnalysisCache &shared();
    // ~/.cache/seabass/analysis.db on Linux, or under SEABASS_HOME when
    // that is set, so a sandboxed run never touches the real one.
    static QString defaultDatabaseFile();

    // readTrackAnalysis(pioneerRoot, trackSourceId, nullptr, pathIndex,
    // threeBandOverview = true), from the cache where the track's .DAT and
    // .2EX are as they were when it was kept. A library whose analysis
    // files are not files on disk (a browsed backup) is read as before,
    // and not kept.
    domain::TrackAnalysis rekordboxAnalysis(const std::string &pioneerRoot, const std::string &trackSourceId,
                                            const infrastructure::rekordbox::AnlzPathIndex *pathIndex);

    struct Counters
    {
        int hits = 0;
        int reads = 0;      // analyses read from the stick
        int stores = 0;
        int rebuilds = 0;
    };
    Counters counters() const;
    bool persistent() const;

    // The serialised form an entry is kept in, for the test.
    static std::string serialise(const domain::TrackAnalysis &analysis);
    static std::optional<domain::TrackAnalysis> deserialise(const std::string &bytes);

private:
    void open();
    void closeDatabase();
    void rebuild();
    template <typename Work>
    bool withDatabase(Work &&work);
    std::optional<domain::TrackAnalysis> lookup(const std::string &key);
    void store(const std::string &key, const std::string &value);
    void evictIfNeeded();
    void forgetWrittenFiles();

    QString m_file;
    std::int64_t m_cap = DefaultCapBytes;
    std::int64_t m_storedBytes = 0;
    sqlite3 *m_db = nullptr;
    Counters m_counters;
    // The analysis file writes (infrastructure::analysisFileWriteSerial())
    // whose entries are gone.
    std::uint64_t m_writesSeen = 0;
    mutable std::mutex m_lock;
};

}  // namespace seabass::gui
