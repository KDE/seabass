// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "application/ports/library_reader.hpp"
#include "domain/track.hpp"

struct sqlite3;

namespace seabass::infrastructure::local
{

// One "Backup Now" moment, frozen -- unlike the tracks/cues tables (which
// upsert() merges into, so they only ever reflect the latest state), each
// snapshot keeps its own independent copy of every backed-up track's cues,
// so it can be restored on its own later even after newer backups have
// changed the merged state.
struct BackupSessionSummary
{
    std::int64_t id;
    std::string createdAt;  // ISO 8601 UTC
    std::string stickLabel;
    std::string sourceFormat;
    std::string description;
    int trackCount = 0;
    int cueCount = 0;
    std::uint64_t uncompressedSizeBytes = 0;
    std::uint64_t compressedSizeBytes = 0;
    int schemaVersion = 0;  // the blob's own format version -- see local_cue_store.cpp
};

// A stick-independent copy of cue data, kept on this computer so cues
// survive a stick being reformatted or a track being re-imported. Backed
// by a small SQLite database (schema: tracks + cues tables) -- SQLite was
// chosen over vendoring a JSON library because libdjinterop already
// requires it to build (see third_party/libdjinterop/CMakeLists.txt), so
// this adds no new third-party dependency.
//
// Implements LibraryReader so it can be matched against a stick's tracks
// with the exact same domain::matchTracks() heuristic used for
// rekordbox<->Engine sync -- see domain::LocalRestorePlanner for how a
// restore proposal gets built from that match.
class LocalCueStore : public application::LibraryReader
{
public:
    // path: defaults to $XDG_DATA_HOME/seabass/cues.db (or
    // $HOME/.local/share/seabass/cues.db), creating the schema on first
    // use. An explicit path is accepted for tests.
    explicit LocalCueStore(std::string path = defaultPath());
    ~LocalCueStore() override;

    LocalCueStore(const LocalCueStore &) = delete;
    LocalCueStore &operator=(const LocalCueStore &) = delete;

    std::vector<domain::Track> readAll() override;

    // Upserts every track in `tracks` that has cues: matched against an
    // existing row by normalizeFilename + duration tolerance (mirroring
    // domain::matchTracks' fallback criterion -- title+artist is tried
    // first when available), replacing that row's cues and metadata.
    // Tracks with no cues are skipped -- there is nothing useful to back
    // up for them. Only ever writes to this database, never to the
    // source the tracks were read from.
    void upsert(const std::vector<domain::Track> &tracks, const std::string &sourceFormat,
                const std::string &sourceLabel);

    // Freezes a compressed, independently-restorable copy of every backed-up
    // track's cues (same "has cues" filter as upsert()) as a new session.
    // Called alongside upsert() on every "Backup Now" -- upsert() keeps the
    // fast instant-restore path current, this keeps history. Returns the
    // new session's id.
    std::int64_t createSnapshot(const std::vector<domain::Track> &tracks, const std::string &sourceFormat,
                                 const std::string &stickLabel, const std::string &description = "");

    std::vector<BackupSessionSummary> listSnapshots();

    // Throws if id doesn't exist or its data can't be decompressed.
    std::vector<domain::Track> readSnapshot(std::int64_t id);

    // No-op if id doesn't exist.
    void setSnapshotDescription(std::int64_t id, const std::string &description);

    // Returns false if id doesn't exist.
    bool deleteSnapshot(std::int64_t id);

    static std::string defaultPath();

private:
    sqlite3 *m_db = nullptr;
};

}  // namespace seabass::infrastructure::local
