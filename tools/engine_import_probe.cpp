// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Hardware probe: what a Denon Engine OS player's "update the Rekordbox
// library" prompt does to the Engine side of a stick when the DJ says yes.
//
//   engine_import_probe --plant <stick root> --out <cases.tsv> [--cover <image>]
//                       [--no-arm] [--skip <path substring>]...
//   engine_import_probe --record <stick root> --out <record.tsv>
//   engine_import_probe --compare <before.tsv> <after.tsv> [--cases <cases.tsv>]
//
// The player asks when export.pdb's header sequence differs from
// Information.lastRekordBoxLibraryImportReadCounter in m.db (measured on a
// Prime 4 and a Prime Go+, 2026-09-18; see engine_import_state.hpp). What
// accepting it does has only been glimpsed: covers turned into
// "image://fileart//<path on the importing computer>", Seabass repairs
// undone, the analysis gone. This tool measures it, one difference per
// track so every outcome is attributable.
//
// --plant writes a matrix of differences between the rekordbox side and
// the Engine side of a TEST stick, each on its own track, and arms the
// prompt. Engine through libdjinterop and Seabass's own Engine writers,
// rekordbox through RekordboxCueWriter (and OneLibraryCueWriter when the
// stick has exportLibrary.db, so both rekordbox catalogs agree and it does
// not matter which one the player imports from). Plain SQL only where the
// adapters cannot set a field or would set more than asked; every such
// place says why. export.pdb itself is never written.
//
// --record dumps every Engine value the import could touch (all Track
// columns, PerformanceData blobs as length and digest, cues, covers,
// playlists, the Information row, every file under "Engine Library") and
// the rekordbox side's tracks, cues and playlists, keyed by audio file
// path so a rebuilt row still lines up. Reads only.
//
// --compare reads two records and says, per planted case, KEPT,
// OVERWRITTEN, DELETED, ADDED or MERGED, then everything that changed
// outside the matrix. The last line is "PROBE RESULT: ..." and the exit
// code is 0 whenever the comparison itself ran: the verdict is the table.
//
// The protocol around it is docs/engine-import-prompt.md, and
// tools/engine-import-probe.sh runs the three steps.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "engine_import_probe_compare.hpp"
#include "engine_import_probe_onelibrary.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using probe::Record;

namespace
{

constexpr const char *EngineOnlyPlaylist = "Seabass probe Engine only";

fs::path engineLibraryOf(const fs::path &root)
{
    return root / "Engine Library";
}

fs::path pioneerOf(const fs::path &root)
{
    return root / "PIONEER";
}

fs::path mdbOf(const fs::path &root)
{
    return engineLibraryOf(root) / "Database2" / "m.db";
}

// A file's path relative to the stick root, forward slashes, resolved
// lexically ("Engine Library/../Contents/x.mp3" is "Contents/x.mp3"). Both
// catalogs' rows for one audio file get the same key this way.
std::string stickPath(const fs::path &root, const std::string &absolute)
{
    if (absolute.empty()) {
        return {};
    }
    const fs::path normal = pathFromUtf8(absolute).lexically_normal();
    const fs::path relative = normal.lexically_relative(root.lexically_normal());
    if (relative.empty()) {
        return pathToGenericUtf8(normal);
    }
    return pathToGenericUtf8(relative);
}

std::string ms(double value)
{
    return std::to_string(std::llround(value)) + " ms";
}

std::string shortDigest(const void *data, std::size_t size)
{
    const auto digest = infrastructure::hashing::Sha256::of(
        std::span<const std::byte>(static_cast<const std::byte *>(data), size));
    return infrastructure::hashing::toHex(digest).substr(0, 16);
}

// One SQLite value, as the record holds it: NULL, a number as SQLite
// prints it, text as is, a blob as its length and digest.
std::string columnValue(sqlite3_stmt *stmt, int column)
{
    switch (sqlite3_column_type(stmt, column)) {
    case SQLITE_NULL:
        return "NULL";
    case SQLITE_BLOB: {
        const void *data = sqlite3_column_blob(stmt, column);
        const int size = sqlite3_column_bytes(stmt, column);
        return "blob:" + std::to_string(size) + ":" + shortDigest(data, static_cast<std::size_t>(size));
    }
    default: {
        const unsigned char *text = sqlite3_column_text(stmt, column);
        return text ? std::string(reinterpret_cast<const char *>(text)) : std::string("NULL");
    }
    }
}

class Database
{
public:
    Database(const fs::path &file, bool writable)
    {
        const int flags = writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
        if (sqlite3_open_v2(pathToUtf8(file).c_str(), &m_handle, flags, nullptr) != SQLITE_OK) {
            const std::string message = m_handle ? sqlite3_errmsg(m_handle) : "out of memory";
            sqlite3_close(m_handle);
            m_handle = nullptr;
            throw std::runtime_error("could not open " + pathToUtf8(file) + ": " + message);
        }
        sqlite3_busy_timeout(m_handle, 5000);
    }
    ~Database() { sqlite3_close(m_handle); }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    sqlite3 *handle() const { return m_handle; }

    // Runs `sql`, calling `row` for each result row.
    template<typename RowFn>
    void each(const std::string &sql, RowFn row) const
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("could not prepare \"" + sql + "\": " + sqlite3_errmsg(m_handle));
        }
        int step;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            row(stmt);
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE) {
            throw std::runtime_error("could not read \"" + sql + "\": " + sqlite3_errmsg(m_handle));
        }
    }

    void exec(const std::string &sql) const
    {
        char *message = nullptr;
        if (sqlite3_exec(m_handle, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
            const std::string text = message ? message : "unknown error";
            sqlite3_free(message);
            throw std::runtime_error("could not run \"" + sql + "\": " + text);
        }
    }

private:
    sqlite3 *m_handle = nullptr;
};

std::string text(sqlite3_stmt *stmt, int column)
{
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? std::string(reinterpret_cast<const char *>(value)) : std::string();
}

// The cue fields ET and RT share. Engine holds hot cues and hot loops in
// two banks of eight and one main cue; rekordbox holds hot cue slots that
// are either a cue or a loop, and any number of memory cues. Both are
// spelled the same here: cue.hot.N, cue.loop.N, cue.main (rekordbox's
// earliest memory cue, which is the one Engine can hold), plus
// cue.memory for rekordbox's whole memory list.
void recordCues(Record &record, const std::string &section, const std::string &path,
                const std::vector<domain::CuePoint> &cues)
{
    std::optional<double> earliestMemory;
    std::vector<std::string> memory;
    for (const auto &cue : cues) {
        if (cue.kind == domain::CuePoint::Kind::Memory) {
            memory.push_back(cue.isLoop ? ms(cue.positionMs) + " to " + ms(cue.loopEndMs) + " loop"
                                        : ms(cue.positionMs));
            if (!cue.isLoop && (!earliestMemory || cue.positionMs < *earliestMemory)) {
                earliestMemory = cue.positionMs;
            }
            continue;
        }
        const std::string slot = std::to_string(cue.hotCueNumber);
        const std::string base = cue.isLoop ? "cue.loop." + slot : "cue.hot." + slot;
        record.set(section, path, base,
                   cue.isLoop ? ms(cue.positionMs) + " to " + ms(cue.loopEndMs) : ms(cue.positionMs));
        record.set(section, path, base + ".color", cue.color);
        record.set(section, path, base + ".label", cue.comment);
    }
    if (earliestMemory) {
        record.set(section, path, "cue.main", ms(*earliestMemory));
    }
    if (section == "RT") {
        std::sort(memory.begin(), memory.end());
        record.set(section, path, "cue.memory", probe::joinSet(memory));
    }
}

void recordPlaylistMemberships(Record &record, const std::string &section, const std::string &path,
                               const std::vector<domain::PlaylistMembership> &playlists)
{
    std::vector<std::string> names;
    for (const auto &p : playlists) {
        names.push_back(p.name);
    }
    std::sort(names.begin(), names.end());
    record.set(section, path, "playlists", probe::joinSet(names));
}

std::string formatRating(const std::optional<int> &rating)
{
    return rating ? std::to_string(*rating) : std::string("NULL");
}

std::string formatBpm(double bpm)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << bpm;
    return out.str();
}

// The fields a DJ would call "the track", hashed: the control case's one
// value.
std::string contentDigest(const Record &record, const std::string &section, const std::string &path)
{
    std::string all;
    for (const auto &[field, value] : record.fields(section, path)) {
        if (field.rfind("col.", 0) == 0 || field.rfind("perf.", 0) == 0 || field == "content" || field == "id") {
            continue;
        }
        all += field + "=" + value + "\n";
    }
    return infrastructure::hashing::toHex(infrastructure::hashing::Sha256::of(all)).substr(0, 16);
}

struct StickTracks
{
    std::vector<domain::Track> rekordbox;
    std::vector<domain::Track> engine;
    // The key each Engine row is recorded under, by row id: its audio
    // file's stick path, or, where that names no rekordbox track and
    // exactly one rekordbox track has the same file name, that track's
    // path. The fallback exists for libraries whose Engine paths were
    // rewritten (the repository's anonymized fixture lost the "../" in
    // front of "Contents/"); on a stick as a player wrote it, every pair
    // matches by path and the fallback counts zero.
    std::map<std::string, std::string> engineKeyById;
    int pairedByFilename = 0;

    std::string engineKey(const domain::Track &t) const { return engineKeyById.at(t.sourceId); }
};

std::string rekordboxKey(const fs::path &root, const domain::Track &t)
{
    return t.filePath.empty() ? "rekordbox id:" + t.sourceId : stickPath(root, t.filePath);
}

std::string lowered(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

StickTracks readTracks(const fs::path &root)
{
    StickTracks out;
    infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(pathToUtf8(pioneerOf(root)));
    out.rekordbox = rekordbox.readAll();
    infrastructure::engine::LibdjinteropEngineReader engine(pathToUtf8(engineLibraryOf(root)));
    out.engine = engine.readAll();

    std::set<std::string> rekordboxPaths;
    std::map<std::string, std::vector<std::string>> rekordboxByName;
    for (const auto &t : out.rekordbox) {
        const std::string key = rekordboxKey(root, t);
        rekordboxPaths.insert(key);
        if (!t.filePath.empty()) {
            rekordboxByName[lowered(pathToUtf8(pathFromUtf8(t.filePath).filename()))].push_back(key);
        }
    }
    std::map<std::string, int> engineByName;
    for (const auto &t : out.engine) {
        if (!t.filePath.empty()) {
            engineByName[lowered(pathToUtf8(pathFromUtf8(t.filePath).filename()))]++;
        }
    }
    for (const auto &t : out.engine) {
        if (t.filePath.empty()) {
            out.engineKeyById[t.sourceId] = "engine id:" + t.sourceId;
            continue;
        }
        const std::string path = stickPath(root, t.filePath);
        const std::string name = lowered(pathToUtf8(pathFromUtf8(t.filePath).filename()));
        const auto byName = rekordboxByName.find(name);
        if (rekordboxPaths.count(path) == 0 && byName != rekordboxByName.end() && byName->second.size() == 1
            && engineByName[name] == 1) {
            out.engineKeyById[t.sourceId] = byName->second.front();
            out.pairedByFilename++;
        } else {
            out.engineKeyById[t.sourceId] = path;
        }
    }
    return out;
}

// Everything --record writes, as a Record.
Record recordStick(const fs::path &root)
{
    Record record;
    const auto state = infrastructure::engine::readRekordboxImportState(pathToUtf8(engineLibraryOf(root)),
                                                                        pathToUtf8(pioneerOf(root)));
    if (!state.error.empty()) {
        throw std::runtime_error("could not read the import state: " + state.error);
    }
    record.set("M", "-", "pdb.sequence", state.hasRekordboxLibrary ? std::to_string(state.librarySequence) : "NULL");
    record.set("M", "-", "engine.counter", state.hasEngineLibrary ? std::to_string(state.engineCounter) : "NULL");
    record.set("M", "-", "prompt", state.playerWillOfferImport() ? "the player would ask" : "the player would say nothing");

    const StickTracks tracks = readTracks(root);
    record.set("M", "-", "engine.pairedByFilename", std::to_string(tracks.pairedByFilename));

    // Engine, through the reader: what Seabass sees, in the shared names.
    std::map<std::int64_t, std::string> enginePathById;
    for (const auto &t : tracks.engine) {
        const std::string path = tracks.engineKey(t);
        enginePathById[std::stoll(t.sourceId)] = path;
        record.set("ET", path, "row", "present");
        record.set("ET", path, "id", t.sourceId);
        record.set("ET", path, "title", t.title);
        record.set("ET", path, "key", t.key);
        record.set("ET", path, "rating", formatRating(t.rating));
        record.set("ET", path, "bpm", formatBpm(t.bpm));
        record.set("ET", path, "comment", t.comment);
        recordCues(record, "ET", path, t.cues);
        recordPlaylistMemberships(record, "ET", path, t.playlists);
    }

    // Engine, through SQL: every column, so nothing the import does to a
    // row can pass unseen.
    {
        Database db(mdbOf(root), false);
        db.each("SELECT * FROM Track ORDER BY id;", [&](sqlite3_stmt *stmt) {
            const std::int64_t id = sqlite3_column_int64(stmt, 0);
            const auto known = enginePathById.find(id);
            const std::string path = known != enginePathById.end() ? known->second : "engine id:" + std::to_string(id);
            record.set("ET", path, "row", "present");
            for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
                record.set("ET", path, std::string("col.") + sqlite3_column_name(stmt, c), columnValue(stmt, c));
            }
        });
        db.each("SELECT * FROM PerformanceData;", [&](sqlite3_stmt *stmt) {
            const std::int64_t id = sqlite3_column_int64(stmt, 0);
            const auto known = enginePathById.find(id);
            // A PerformanceData row whose Track is gone keeps its own key,
            // so it is neither lost nor taken for a track.
            const std::string path = known != enginePathById.end()
                ? known->second
                : "PerformanceData without a Track, trackId " + std::to_string(id);
            for (int c = 1; c < sqlite3_column_count(stmt); ++c) {
                record.set("ET", path, std::string("perf.") + sqlite3_column_name(stmt, c), columnValue(stmt, c));
            }
        });
        // The cover: what AlbumArt.hash names for the track, and how many
        // bytes of image the row itself holds.
        db.each("SELECT t.id, typeof(a.hash), a.hash, length(a.albumArt), a.id FROM Track t "
                "LEFT JOIN AlbumArt a ON a.id = t.albumArtId;",
                [&](sqlite3_stmt *stmt) {
                    const std::int64_t id = sqlite3_column_int64(stmt, 0);
                    const auto known = enginePathById.find(id);
                    const std::string path =
                        known != enginePathById.end() ? known->second : "engine id:" + std::to_string(id);
                    // Engine's own import writes "image://fileart//<path>"
                    // into the hash, as text or as a blob depending on the
                    // firmware (the repository's fixture holds blobs), so
                    // the bytes decide, not the type.
                    const std::string type = text(stmt, 1);
                    const auto *bytes = static_cast<const std::uint8_t *>(sqlite3_column_blob(stmt, 2));
                    const int size = sqlite3_column_bytes(stmt, 2);
                    const std::string raw(reinterpret_cast<const char *>(bytes), bytes ? static_cast<std::size_t>(size) : 0);
                    std::string art;
                    if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
                        art = "none";
                    } else if (raw.rfind("image://", 0) == 0) {
                        art = "imported:" + raw;
                    } else if (type == "text") {
                        art = "text:" + raw;
                    } else if (type == "blob") {
                        art = "file:Artwork/"
                            + infrastructure::engine::artworkFileName(
                                  std::span<const std::uint8_t>(bytes, static_cast<std::size_t>(size)));
                    } else {
                        art = "row without hash";
                    }
                    record.set("ET", path, "art", art);
                    record.set("ET", path, "art.bytes", columnValue(stmt, 3));
                });
        db.each("SELECT count(*), coalesce(sum(length(albumArt) > 0), 0), coalesce(sum(typeof(hash) = 'text'), 0), "
                "coalesce(sum(typeof(hash) = 'blob'), 0), "
                "coalesce(sum(substr(CAST(hash AS BLOB), 1, 8) = CAST('image://' AS BLOB)), 0) FROM AlbumArt;",
                [&](sqlite3_stmt *stmt) {
                    record.set("EA", "-", "rows", columnValue(stmt, 0));
                    record.set("EA", "-", "rowsWithImage", columnValue(stmt, 1));
                    record.set("EA", "-", "textHashes", columnValue(stmt, 2));
                    record.set("EA", "-", "blobHashes", columnValue(stmt, 3));
                    record.set("EA", "-", "importedReferences", columnValue(stmt, 4));
                });
        int informationRows = 0;
        db.each("SELECT * FROM Information ORDER BY id;", [&](sqlite3_stmt *stmt) {
            ++informationRows;
            const std::string prefix = informationRows == 1 ? "" : "row" + std::to_string(informationRows) + ".";
            for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
                record.set("EI", "-", prefix + sqlite3_column_name(stmt, c), columnValue(stmt, c));
            }
        });
        record.set("EI", "-", "rows", std::to_string(informationRows));

        // Playlists: full path by parent chain, entries in the player's
        // own order (the nextEntityId chain).
        struct List
        {
            std::string title;
            std::int64_t parent = 0;
        };
        std::map<std::int64_t, List> lists;
        db.each("SELECT id, title, parentListId FROM Playlist;", [&](sqlite3_stmt *stmt) {
            lists[sqlite3_column_int64(stmt, 0)] = List{text(stmt, 1), sqlite3_column_int64(stmt, 2)};
        });
        const auto fullName = [&lists](std::int64_t id) {
            std::string name;
            std::set<std::int64_t> seen;
            while (id != 0 && lists.count(id) > 0 && seen.insert(id).second) {
                name = name.empty() ? lists[id].title : lists[id].title + "/" + name;
                id = lists[id].parent;
            }
            return name;
        };
        struct Entity
        {
            std::int64_t track = 0;
            std::int64_t next = 0;
        };
        std::map<std::int64_t, std::map<std::int64_t, Entity>> entities;
        db.each("SELECT listId, id, trackId, nextEntityId FROM PlaylistEntity;", [&](sqlite3_stmt *stmt) {
            entities[sqlite3_column_int64(stmt, 0)][sqlite3_column_int64(stmt, 1)] =
                Entity{sqlite3_column_int64(stmt, 2), sqlite3_column_int64(stmt, 3)};
        });
        for (const auto &[id, list] : lists) {
            const std::string name = fullName(id);
            const auto &members = entities[id];
            std::set<std::int64_t> pointedAt;
            for (const auto &[entityId, e] : members) {
                pointedAt.insert(e.next);
            }
            std::vector<std::string> ordered;
            std::set<std::int64_t> visited;
            for (const auto &[entityId, e] : members) {
                if (pointedAt.count(entityId) > 0) {
                    continue;
                }
                for (std::int64_t at = entityId; at != 0 && members.count(at) > 0 && visited.insert(at).second;
                     at = members.at(at).next) {
                    const auto known = enginePathById.find(members.at(at).track);
                    ordered.push_back(known != enginePathById.end() ? known->second
                                                                    : "id:" + std::to_string(members.at(at).track));
                }
            }
            // Whatever the chain did not reach, in id order: a broken
            // chain is a finding, not a reason to drop entries.
            for (const auto &[entityId, e] : members) {
                if (visited.count(entityId) == 0) {
                    const auto known = enginePathById.find(e.track);
                    ordered.push_back((known != enginePathById.end() ? known->second : "id:" + std::to_string(e.track))
                                      + " (off the chain)");
                }
            }
            record.set("EP", name, "id", std::to_string(id));
            record.set("EP", name, "count", std::to_string(members.size()));
            record.set("EP", name, "entries", probe::joinSet(ordered));
        }
    }
    for (const auto &[id, path] : enginePathById) {
        record.set("ET", path, "content", contentDigest(record, "ET", path));
    }

    // Every file under Engine Library.
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(engineLibraryOf(root), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string relative = pathToGenericUtf8(it->path().lexically_relative(engineLibraryOf(root)));
        record.set("EF", relative, "size", std::to_string(it->file_size(ec)));
        record.set("EF", relative, "mtime",
                   std::to_string(it->last_write_time(ec).time_since_epoch().count()));
    }
    if (ec) {
        throw std::runtime_error("could not list Engine Library: " + ec.message());
    }

    // rekordbox: one-way, supposedly; recorded so a change would show.
    std::map<std::string, std::vector<std::pair<int, std::string>>> rekordboxLists;
    for (const auto &t : tracks.rekordbox) {
        const std::string path = rekordboxKey(root, t);
        record.set("RT", path, "row", "present");
        record.set("RT", path, "id", t.sourceId);
        record.set("RT", path, "title", t.title);
        record.set("RT", path, "key", t.key);
        record.set("RT", path, "rating", formatRating(t.rating));
        record.set("RT", path, "bpm", formatBpm(t.bpm));
        record.set("RT", path, "comment", t.comment);
        record.set("RT", path, "playCount", t.playCount ? std::to_string(*t.playCount) : "NULL");
        record.set("RT", path, "art", t.artworkPath.empty() ? "none" : stickPath(root, t.artworkPath));
        recordCues(record, "RT", path, t.cues);
        recordPlaylistMemberships(record, "RT", path, t.playlists);
        for (const auto &p : t.playlists) {
            rekordboxLists[p.name].push_back({p.position, path});
        }
    }
    for (auto &[name, members] : rekordboxLists) {
        std::stable_sort(members.begin(), members.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });
        std::vector<std::string> ordered;
        for (const auto &m : members) {
            ordered.push_back(m.second);
        }
        record.set("RP", name, "count", std::to_string(ordered.size()));
        record.set("RP", name, "entries", probe::joinSet(ordered));
    }
    return record;
}

bool writeRecordFile(const fs::path &file, const Record &record)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    probe::writeRecord(out, record);
    return static_cast<bool>(out);
}

std::optional<Record> readRecordFile(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "could not open " << pathToUtf8(file) << "\n";
        return std::nullopt;
    }
    Record record;
    std::string error;
    if (!probe::readRecord(in, record, &error)) {
        std::cerr << pathToUtf8(file) << ": " << error << "\n";
        return std::nullopt;
    }
    return record;
}

void describeImportState(const char *when, const infrastructure::engine::RekordboxImportState &state)
{
    std::cout << when << ": export.pdb sequence " << state.librarySequence << ", Engine last imported "
              << state.engineCounter << ", the player would "
              << (state.playerWillOfferImport() ? "ASK" : "say nothing") << " on insert\n";
}

// ---------------------------------------------------------------------------
// --plant

struct Pair
{
    std::string path;
    domain::Track rekordbox;
    domain::Track engine;
};

domain::CuePoint hot(int slot, double positionMs, std::string color = {})
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = slot;
    cue.positionMs = positionMs;
    cue.color = std::move(color);
    return cue;
}

domain::CuePoint memoryCue(double positionMs)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Memory;
    cue.positionMs = positionMs;
    return cue;
}

domain::CuePoint hotLoop(int slot, double startMs, double endMs)
{
    domain::CuePoint cue = hot(slot, startMs);
    cue.isLoop = true;
    cue.loopEndMs = endMs;
    return cue;
}

// The playlist named by a full path ("Folder/Name"), found the way the
// record names them.
std::optional<djinterop::playlist> findPlaylist(djinterop::database &db, const std::string &fullName)
{
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(fullName);
    while (std::getline(in, part, '/')) {
        parts.push_back(part);
    }
    if (parts.empty()) {
        return std::nullopt;
    }
    std::optional<djinterop::playlist> at = db.root_playlist_by_name(parts.front());
    for (std::size_t i = 1; at && i < parts.size(); ++i) {
        at = at->sub_playlist_by_name(parts[i]);
    }
    return at;
}

std::string readSetFirst(const std::optional<std::string> &value)
{
    return value ? probe::shown(value) : std::string("(none)");
}

struct PlantOptions
{
    fs::path root;
    fs::path out;
    fs::path cover;
    // Leave Engine's import counter alone: for a stick whose pdb sequence
    // a real rekordbox export has already moved.
    bool noArm = false;
    // Never pick a track whose stick-relative audio path contains any of
    // these, for any case: tracks that carry other evidence.
    std::vector<std::string> skip;
};

int plant(const PlantOptions &options)
{
    const fs::path &root = options.root;
    {
        // Sebastian's working sticks and the references are never planted
        // into: the same list as rig-platform.sh's is_protected_label.
        std::string upper = pathToUtf8(root.lexically_normal().filename());
        if (upper.empty()) {
            upper = pathToUtf8(root.lexically_normal().parent_path().filename());
        }
        for (char &c : upper) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (upper.rfind("WHALESHARK", 0) == 0 || upper.rfind("CORSAIR", 0) == 0) {
            std::cerr << "refusing: " << pathToUtf8(root) << " is a protected stick, never planted into\n";
            return 2;
        }
    }
    if (!fs::is_regular_file(pioneerOf(root) / "rekordbox" / "export.pdb") || !fs::is_regular_file(mdbOf(root))) {
        std::cerr << "refusing: " << pathToUtf8(root)
                  << " needs both PIONEER/rekordbox/export.pdb and Engine Library/Database2/m.db\n";
        return 2;
    }

    const std::string enginePath = pathToUtf8(engineLibraryOf(root));
    const std::string pioneerPath = pathToUtf8(pioneerOf(root));
    const auto stateBefore = infrastructure::engine::readRekordboxImportState(enginePath, pioneerPath);
    if (!stateBefore.error.empty()) {
        std::cerr << "could not read the import state: " << stateBefore.error << "\n";
        return 1;
    }
    describeImportState("before planting", stateBefore);

    const Record before = recordStick(root);
    if (before.entities("EP").count(EngineOnlyPlaylist) > 0) {
        std::cerr << "refusing: the Engine library already has a playlist called \"" << EngineOnlyPlaylist
                  << "\", so this stick has been planted before. Restore it from its snapshot first.\n";
        return 2;
    }
    const StickTracks tracks = readTracks(root);

    // Tracks both catalogs list, by audio file.
    std::map<std::string, domain::Track> rekordboxByPath;
    for (const auto &t : tracks.rekordbox) {
        if (!t.filePath.empty()) {
            rekordboxByPath[rekordboxKey(root, t)] = t;
        }
    }
    const auto skipped = [&options](const std::string &path) {
        return std::any_of(options.skip.begin(), options.skip.end(),
                           [&path](const std::string &part) { return path.find(part) != std::string::npos; });
    };
    std::vector<Pair> pairs;
    std::vector<domain::Track> engineOnly;
    int skippedTracks = 0;
    for (const auto &t : tracks.engine) {
        if (t.filePath.empty() || !t.streamingSource.empty()) {
            continue;
        }
        const std::string path = tracks.engineKey(t);
        if (skipped(path) || skipped(stickPath(root, t.filePath))) {
            ++skippedTracks;
            continue;
        }
        const auto it = rekordboxByPath.find(path);
        if (it == rekordboxByPath.end()) {
            engineOnly.push_back(t);
            continue;
        }
        // Long enough for every planted position (the latest is 68 s).
        if (std::max(t.durationSeconds, it->second.durationSeconds) < 90.0) {
            continue;
        }
        pairs.push_back(Pair{path, it->second, t});
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.path < b.path; });
    std::sort(engineOnly.begin(), engineOnly.end(),
              [](const domain::Track &a, const domain::Track &b) { return a.filePath < b.filePath; });
    std::cout << "tracks: " << tracks.rekordbox.size() << " rekordbox, " << tracks.engine.size() << " Engine, "
              << pairs.size() << " in both (90 s or longer), " << engineOnly.size() << " Engine-only";
    if (tracks.pairedByFilename > 0) {
        std::cout << "; " << tracks.pairedByFilename
                  << " Engine rows matched to rekordbox by file name, their paths disagreeing";
    }
    std::cout << "\n";
    std::cout << "skipped: " << skippedTracks << " Engine tracks whose path matches one of " << options.skip.size()
              << " skip patterns, never planted on or observed\n";
    if (pairs.size() < 24) {
        std::cerr << "refusing: the matrix needs 24 tracks that both catalogs list, and this stick has "
                  << pairs.size() << "\n";
        return 1;
    }

    std::set<std::string> used;
    const auto take = [&](const auto &accept) -> const Pair * {
        for (const Pair &p : pairs) {
            if (used.count(p.path) == 0 && accept(p)) {
                used.insert(p.path);
                return &p;
            }
        }
        return nullptr;
    };
    const auto any = [](const Pair &) { return true; };

    // The shared playlist for (h): "Seabass test A" on the rig sticks,
    // else the first name both catalogs hold with at least three tracks.
    std::string shared;
    for (const std::string &candidate : std::vector<std::string>{"Seabass test A", "Seabass test B"}) {
        if (before.get("RP", candidate, "entries") && before.get("EP", candidate, "entries")) {
            shared = candidate;
            break;
        }
    }
    if (shared.empty()) {
        for (const auto &name : before.entities("RP")) {
            const auto engineEntries = before.get("EP", name, "entries");
            if (engineEntries && probe::splitSet(*before.get("RP", name, "entries")).size() >= 3) {
                shared = name;
                break;
            }
        }
    }
    const auto inSet = [&before](const char *section, const std::string &name, const std::string &path) {
        const auto entries = before.get(section, name, "entries");
        return entries && probe::splitSet(*entries).count(path) > 0;
    };

    std::vector<probe::Case> cases;
    const auto addCase = [&cases](std::string id, std::string status, std::string title, std::string track,
                                  std::string how, std::vector<probe::Item> items) {
        cases.push_back(probe::Case{std::move(id), std::move(status), std::move(title), std::move(track), {}, {},
                                    std::move(how), std::move(items)});
    };
    const auto et = [](const std::string &path, const std::string &field) {
        return probe::Item{"ET", path, field, "value"};
    };

    // Playlist cases first: they need particular tracks.
    const Pair *h2 = nullptr;
    const Pair *h3 = nullptr;
    if (!shared.empty()) {
        h3 = take([&](const Pair &p) { return inSet("RP", shared, p.path) && inSet("EP", shared, p.path); });
        h2 = take([&](const Pair &p) { return !inSet("RP", shared, p.path) && !inSet("EP", shared, p.path); });
    }
    const Pair *h1a = take(any);
    const Pair *h1b = take(any);
    // (m) wants a track the player has analysed: isAnalyzed = 1 with its
    // blobs present. Not something this tool can make.
    const Pair *m = take([&](const Pair &p) {
        return before.get("ET", p.path, "col.isAnalyzed") == std::optional<std::string>("1")
            && before.get("ET", p.path, "perf.beatData").value_or("NULL") != "NULL";
    });

    // (k) prefers a track whose cover Seabass's own repair would fix: an
    // imported "image://fileart//" reference with the image on this stick.
    const std::string stickRoot = pathToUtf8(root);
    std::map<std::int64_t, infrastructure::engine::ArtworkEntry> repairable;
    {
        const auto audit = infrastructure::engine::auditArtwork(enginePath);
        for (const auto &entry : audit.unreadable) {
            if (!entry.imageOnStick.empty()) {
                repairable[entry.trackId] = entry;
            }
        }
    }
    const Pair *k = take([&](const Pair &p) { return repairable.count(std::stoll(p.engine.sourceId)) > 0; });
    std::string kHow;
    std::optional<infrastructure::engine::ArtworkEntry> kEntry;
    if (k) {
        kEntry = repairable[std::stoll(k->engine.sourceId)];
        kHow = "Seabass's cover repair (repairArtwork) on an imported reference, image from " + kEntry->imageOnStick;
    } else {
        k = take([&](const Pair &p) {
            return !p.rekordbox.artworkPath.empty() && fs::is_regular_file(pathFromUtf8(p.rekordbox.artworkPath));
        });
        if (k) {
            kEntry = infrastructure::engine::ArtworkEntry{};
            kEntry->imageOnStick = k->rekordbox.artworkPath;
            kHow = "Seabass's cover repair (repairArtwork), fed rekordbox's own image for the track";
        } else if (!options.cover.empty()) {
            k = take(any);
            kEntry = infrastructure::engine::ArtworkEntry{};
            kEntry->imageOnStick = pathToUtf8(options.cover);
            kHow = "Seabass's cover repair (repairArtwork), fed the --cover image " + pathToUtf8(options.cover);
        }
        if (kEntry) {
            kEntry->trackId = std::stoll(k->engine.sourceId);
            kEntry->storage = infrastructure::engine::ArtworkStorage::ImportedPath;
        }
    }

    const Pair *a = take(any);
    const Pair *b = take(any);
    const Pair *c = take(any);
    const Pair *d = take(any);
    const Pair *e = take(any);
    const Pair *f = take(any);
    const Pair *g = take(any);
    const Pair *j1 = take(any);
    const Pair *j2 = take(any);
    const Pair *j3 = take(any);
    const Pair *j4 = take(any);
    const Pair *j5 = take(any);
    const Pair *l = take(any);
    const Pair *n = take(any);
    // An unused track whose audio file is there, for (i) when the stick
    // has no Engine-only row of its own.
    const Pair *iSource = engineOnly.empty()
        ? take([](const Pair &p) { return fs::is_regular_file(pathFromUtf8(p.engine.filePath)); })
        : nullptr;

    // The cue matrix. Every cue case's track gets both sides written in
    // full: hot cue 1 at 10 s on both (the common cue, a control inside
    // each case), plus the one difference the case is about. What the
    // track held before is replaced, on both sides.
    const domain::CuePoint common = hot(1, 10000.0);
    struct CueCase
    {
        const Pair *pair;
        std::vector<domain::CuePoint> engine;
        std::vector<domain::CuePoint> rekordbox;
    };
    std::vector<CueCase> cueCases = {
        {a, {common, hot(8, 30000.0)}, {common}},
        {b, {common}, {common, hot(8, 30000.0)}},
        {c, {common, hot(7, 30000.0)}, {common, hot(7, 45000.0)}},
        {d, {common, hot(2, 30000.0, "#FF00FF")}, {common, hot(2, 30000.0)}},
        {e, {common, hotLoop(6, 60000.0, 68000.0)}, {common}},
        {f, {common, memoryCue(20000.0)}, {common}},
        {g, {common}, {common, memoryCue(25000.0)}},
    };

    std::vector<std::string> failures;
    const fs::path engineDir = engineLibraryOf(root);

    // Engine: cues, rating, comment, BPM, last played, all through
    // Seabass's own Engine writer.
    {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(enginePath);
        for (const CueCase &cc : cueCases) {
            writer.writeHotCues(cc.pair->engine.sourceId, cc.engine);
        }
        // (j1) rating: Engine stars differ from rekordbox's. 0 is not
        // writable through writeAnnotation, so the two are 1 and 5.
        const int rekordboxStars = j1->rekordbox.rating.value_or(0);
        writer.writeAnnotation(j1->engine.sourceId, rekordboxStars >= 3 ? 1 : 5, std::nullopt);
        // (j2) comment.
        writer.writeAnnotation(j2->engine.sourceId, std::nullopt, std::string("Seabass probe: Engine comment"));
        // (j4) BPM: rekordbox's plus 7. libdjinterop's set_bpm writes
        // Track.bpm and Track.bpmAnalyzed, nothing else.
        writer.propagateMissingFields(j4->engine.sourceId, j4->rekordbox.bpm + 7.0, std::nullopt);
        // (l) last played: 2026-01-02 03:04:05 UTC, a date nobody played
        // anything on this stick.
        std::tm when{};
        when.tm_year = 2026 - 1900;
        when.tm_mon = 0;
        when.tm_mday = 2;
        when.tm_hour = 3;
        when.tm_min = 4;
        when.tm_sec = 5;
#ifdef _WIN32
        const auto at = std::chrono::system_clock::from_time_t(_mkgmtime(&when));
#else
        const auto at = std::chrono::system_clock::from_time_t(timegm(&when));
#endif
        writer.setLastPlayedAt(l->engine.sourceId, at);
    }

    // Engine through libdjinterop directly: the title (no Seabass writer
    // sets one), the playlists, and (i)'s new row.
    std::string iHow;
    std::string iPath;
    std::string iStatus = "planted";
    {
        auto db = djinterop::engine::load_database(enginePath);
        // (j5) title.
        if (auto track = db.track_by_id(std::stoll(j5->engine.sourceId))) {
            track->set_title(std::string("Seabass probe Engine title"));
        }
        // (h1) a playlist only Engine has.
        auto list = db.create_root_playlist(EngineOnlyPlaylist);
        for (const Pair *p : {h1a, h1b}) {
            if (auto track = db.track_by_id(std::stoll(p->engine.sourceId))) {
                list.add_track_back(*track);
            }
        }
        if (!shared.empty()) {
            auto sharedList = findPlaylist(db, shared);
            if (!sharedList) {
                failures.push_back("the Engine playlist \"" + shared + "\" could not be opened");
            } else {
                // (h2) a track in Engine's copy that rekordbox's lacks.
                if (h2) {
                    if (auto track = db.track_by_id(std::stoll(h2->engine.sourceId))) {
                        sharedList->add_track_back(*track);
                    }
                }
                // (h3) a track rekordbox's copy has and Engine's lacks.
                if (h3) {
                    if (auto track = db.track_by_id(std::stoll(h3->engine.sourceId))) {
                        sharedList->remove_track(*track);
                    }
                }
            }
        }
        // (i) a track only Engine has.
        if (!engineOnly.empty()) {
            iPath = tracks.engineKey(engineOnly.front());
            iStatus = "existing";
            iHow = "an Engine-only row the stick already had (id " + engineOnly.front().sourceId + "); nothing written";
        } else if (iSource) {
            // A copy of another track's audio under a new name, and an
            // Engine row for it made from that track's own snapshot: a
            // real file, so the player has something to play, and a row
            // export.pdb has never heard of.
            const fs::path source = pathFromUtf8(iSource->engine.filePath);
            const fs::path target = root / "Seabass probe" / ("engine-only" + pathToUtf8(source.extension()));
            fs::create_directories(target.parent_path());
            fs::copy_file(source, target, fs::copy_options::overwrite_existing);
            auto original = db.track_by_id(std::stoll(iSource->engine.sourceId));
            if (!original) {
                failures.push_back("the source track for (i) disappeared");
            } else {
                djinterop::track_snapshot snapshot = original->snapshot();
                snapshot.relative_path = pathToGenericUtf8(target.lexically_relative(engineDir));
                snapshot.title = std::string("Seabass probe Engine-only track");
                db.create_track(snapshot);
                iPath = stickPath(root, pathToUtf8(target));
                iHow = "a copy of " + iSource->path + " at " + iPath
                    + ", with an Engine row made by libdjinterop's create_track from that track's snapshot";
            }
        }
    }

    // Plain SQL, where libdjinterop would write more than the field.
    {
        Database db(mdbOf(root), true);
        db.exec("BEGIN;");
        // (j3) key: libdjinterop's set_key also rewrites
        // PerformanceData.trackData, which on a track the player has not
        // analysed creates an analysis blob that was not there, so the
        // probe would be planting two differences on one track. Engine's
        // key numbering runs 0 to 23; six steps on is a different key
        // whatever it was.
        const auto currentKey = before.get("ET", j3->path, "col.key");
        int key = 5;
        if (currentKey && *currentKey != "NULL") {
            key = (std::stoi(*currentKey) + 6) % 24;
        }
        db.exec("UPDATE Track SET key = " + std::to_string(key) + " WHERE id = " + j3->engine.sourceId + ";");
        // (l) played: isPlayed and playedIndicator are not in libdjinterop's
        // API. A guess at Engine's own convention (playedIndicator taken
        // from Information.currentPlayedIndiciator), recorded and compared
        // like everything else.
        db.exec("UPDATE Track SET isPlayed = 1, playedIndicator = (SELECT currentPlayedIndiciator FROM Information "
                "ORDER BY id LIMIT 1) WHERE id = "
                + l->engine.sourceId + ";");
        db.exec("COMMIT;");
    }

    // (k) the cover, through Seabass's own repair.
    std::string kStatus = "planted";
    if (kEntry) {
        const auto repair = infrastructure::engine::repairArtwork(enginePath, {*kEntry});
        if (repair.repaired != 1) {
            kStatus = "not planted";
            kHow = "repairArtwork wrote nothing: " + repair.error
                + (repair.failureReasons.empty() ? std::string() : "; " + repair.failureReasons.front())
                + (repair.notAnImage ? std::string("; the image is not JPEG or PNG") : std::string());
        }
    } else {
        kStatus = "not planted";
        kHow = "no repairable imported cover, no rekordbox image on the stick, and no --cover given";
    }

    // rekordbox: the cue side of each cue case, through Seabass's own
    // writers. ANLZ files only; export.pdb is not touched.
    {
        infrastructure::rekordbox::RekordboxCueWriter writer(pioneerPath);
        const bool oneLibrary = probe::hasOneLibrary(pioneerPath);
        for (const CueCase &cc : cueCases) {
            writer.writeHotCues(cc.pair->rekordbox.sourceId, cc.rekordbox);
            if (oneLibrary) {
                try {
                    probe::writeOneLibraryCues(pioneerPath, cc.pair->rekordbox.filePath, cc.rekordbox);
                } catch (const std::exception &ex) {
                    failures.push_back("OneLibrary cues for " + cc.pair->path + ": " + ex.what());
                }
            }
        }
        std::cout << "rekordbox cues written to the ANLZ files" << (oneLibrary ? " and to exportLibrary.db" : "")
                  << "\n";
    }

    // Arm the prompt. Engine's counter one behind the pdb's sequence is
    // what a rekordbox export after the last import looks like from the
    // player's side; the pdb itself stays byte for byte what it was.
    const auto stateMid = infrastructure::engine::readRekordboxImportState(enginePath, pioneerPath);
    if (options.noArm) {
        std::cout << "not arming: Engine's import counter left as it was (no-arm)\n";
    } else if (!stateMid.playerWillOfferImport()) {
        const std::uint64_t armed = stateMid.librarySequence > 0 ? stateMid.librarySequence - 1 : 1;
        std::string error;
        if (!infrastructure::engine::markRekordboxLibraryImported(enginePath, armed, &error)) {
            failures.push_back("could not arm the prompt: " + error);
        }
    }
    const auto stateAfter = infrastructure::engine::readRekordboxImportState(enginePath, pioneerPath);
    describeImportState("after planting", stateAfter);
    if (!stateAfter.playerWillOfferImport() && !options.noArm) {
        failures.push_back("the prompt is not armed: the two numbers are level");
    }

    // The case list, with what each side reads back as now.
    const auto cueTitle = [](const char *text) { return std::string(text); };
    addCase("a", "planted", cueTitle("hot cue only in Engine (pad 8)"), a->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(a->path, "cue.hot.8"), et(a->path, "cue.hot.1")});
    addCase("b", "planted", cueTitle("hot cue only in rekordbox (pad 8)"), b->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(b->path, "cue.hot.8"), et(b->path, "cue.hot.1")});
    addCase("c", "planted", cueTitle("pad 7 at different positions on each side"), c->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(c->path, "cue.hot.7"), et(c->path, "cue.hot.1")});
    addCase("d", "planted", cueTitle("pad 2 coloured magenta only in Engine"), d->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(d->path, "cue.hot.2.color"), et(d->path, "cue.hot.2"), et(d->path, "cue.hot.1.color")});
    addCase("e", "planted", cueTitle("hot loop only in Engine (loop pad 6)"), e->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(e->path, "cue.loop.6"), et(e->path, "cue.hot.1")});
    addCase("f", "planted", cueTitle("Engine main cue only in Engine"), f->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(f->path, "cue.main"), et(f->path, "cue.hot.1")});
    addCase("g", "planted", cueTitle("rekordbox memory cue only in rekordbox"), g->path,
            "Seabass Engine and rekordbox cue writers; both sides' cue sets replaced",
            {et(g->path, "cue.main"), et(g->path, "cue.hot.1")});
    addCase("h1", "planted", "playlist only in Engine", EngineOnlyPlaylist,
            "libdjinterop create_root_playlist, two tracks: " + h1a->path + ", " + h1b->path,
            {probe::Item{"EP", EngineOnlyPlaylist, "entries", "set"}});
    if (h2) {
        addCase("h2", "planted", "a track in Engine's \"" + shared + "\" that rekordbox's lacks", h2->path,
                "libdjinterop playlist add_track_back",
                {probe::Item{"EP", shared, "entries", "member:" + h2->path},
                 probe::Item{"EP", shared, "entries", "set"}});
    } else {
        addCase("h2", "not planted", "a track in an Engine playlist that rekordbox's lacks", "",
                "no playlist name both catalogs hold", {});
    }
    if (h3) {
        addCase("h3", "planted", "a track in rekordbox's \"" + shared + "\" that Engine's lacks", h3->path,
                "libdjinterop playlist remove_track", {probe::Item{"EP", shared, "entries", "member:" + h3->path}});
    } else {
        addCase("h3", "not planted", "a track in a rekordbox playlist that Engine's lacks", "",
                "no playlist name both catalogs hold with a member in both", {});
    }
    if (!iPath.empty()) {
        addCase("i", iStatus, "a track only Engine has", iPath, iHow, {et(iPath, "row"), et(iPath, "content")});
    } else {
        addCase("i", "not planted", "a track only Engine has", "",
                "no Engine-only row on the stick and no track whose audio file is present to copy", {});
    }
    addCase("j1", "planted", "rating differs (Engine " + std::string(j1->rekordbox.rating.value_or(0) >= 3 ? "1" : "5")
                + " stars)",
            j1->path, "Seabass Engine writer writeAnnotation (Track.rating)", {et(j1->path, "rating"), et(j1->path, "col.rating")});
    addCase("j2", "planted", "comment differs", j2->path, "Seabass Engine writer writeAnnotation (Track.comment)",
            {et(j2->path, "comment")});
    addCase("j3", "planted", "key differs", j3->path,
            "SQL UPDATE Track.key (libdjinterop set_key would also write PerformanceData.trackData)",
            {et(j3->path, "key"), et(j3->path, "col.key")});
    addCase("j4", "planted", "BPM differs (Engine is rekordbox's plus 7)", j4->path,
            "Seabass Engine writer propagateMissingFields (libdjinterop set_bpm: Track.bpm, Track.bpmAnalyzed)",
            {et(j4->path, "bpm"), et(j4->path, "col.bpm"), et(j4->path, "col.bpmAnalyzed")});
    addCase("j5", "planted", "title differs", j5->path, "libdjinterop set_title (Track.title)",
            {et(j5->path, "title")});
    if (k) {
        addCase("k", kStatus, "cover repaired by Seabass (Engine Artwork/ file) vs rekordbox's", k->path, kHow,
                {et(k->path, "art"), et(k->path, "art.bytes"), et(k->path, "col.albumArtId")});
    } else {
        addCase("k", "not planted", "cover repaired by Seabass", "", kHow, {});
    }
    addCase("l", "planted", "last played 2026-01-02 03:04:05 UTC, only in Engine", l->path,
            "Seabass Engine writer setLastPlayedAt (Track.timeLastPlayed); SQL for isPlayed and playedIndicator",
            {et(l->path, "col.timeLastPlayed"), et(l->path, "col.isPlayed"), et(l->path, "col.playedIndicator")});
    if (m) {
        addCase("m", "observed", "a track the player has analysed", m->path,
                "nothing written: watched for the import clearing the analysis",
                {et(m->path, "col.isAnalyzed"), et(m->path, "perf.beatData"), et(m->path, "perf.trackData"),
                 et(m->path, "perf.overviewWaveFormData")});
    } else {
        addCase("m", "not planted", "a track the player has analysed", "",
                "no track on the stick has isAnalyzed = 1 with beat data; load any track on the player once before "
                "planting and it becomes one (the library-wide analysis count is reported either way)",
                {});
    }
    addCase("n", "observed", "control: a track nobody touched", n->path, "nothing written",
            {et(n->path, "content"), et(n->path, "col.lastEditTime")});

    // What each case reads back as, on each side, now.
    const Record planted = recordStick(root);
    for (probe::Case &cc : cases) {
        if (cc.items.empty()) {
            continue;
        }
        const probe::Item &first = cc.items.front();
        cc.engine = readSetFirst(probe::itemValue(planted, first, first.section));
        cc.rekordbox = first.rekordboxSection().empty()
            ? std::string("(no counterpart)")
            : readSetFirst(probe::itemValue(planted, first, first.rekordboxSection()));
    }

    {
        std::ofstream out(options.out, std::ios::binary | std::ios::trunc);
        probe::writeCases(out, cases);
        if (!out) {
            std::cerr << "could not write " << pathToUtf8(options.out) << "\n";
            return 1;
        }
    }

    std::cout << "\nPlanted matrix (" << cases.size() << " cases), as each side reads back now:\n";
    for (const probe::Case &cc : cases) {
        std::cout << "\n[" << cc.id << "] " << cc.title << " (" << cc.status << ")\n";
        if (!cc.track.empty()) {
            std::cout << "  track:     " << cc.track << "\n";
        }
        if (!cc.items.empty()) {
            std::cout << "  watching:  " << cc.items.front().section << " " << cc.items.front().field
                      << (cc.items.front().member() ? " contains the track" : "") << "\n";
            std::cout << "  Engine:    " << cc.engine << "\n";
            std::cout << "  rekordbox: " << cc.rekordbox << "\n";
        }
        std::cout << "  how:       " << cc.how << "\n";
    }
    std::cout << "\ncases written to " << pathToUtf8(options.out) << "\n";

    // Every track this plant wrote to, per side, so exactly those can be
    // snapshotted. Engine rows live in m.db; rekordbox cues in each
    // track's ANLZ .EXT and .DAT, and in exportLibrary.db when present.
    std::cout << "\nTracks written (side, stick path):\n";
    std::vector<std::pair<std::string, std::string>> written;
    for (const CueCase &cc : cueCases) {
        written.push_back({"engine", cc.pair->path});
        written.push_back({"rekordbox", cc.pair->path});
    }
    for (const Pair *p : {j1, j2, j3, j4, j5, l, h1a, h1b}) {
        written.push_back({"engine", p->path});
    }
    for (const Pair *p : {h2, h3}) {
        if (p) {
            written.push_back({"engine", p->path});
        }
    }
    if (k && kStatus == "planted") {
        written.push_back({"engine", k->path});
    }
    if (iStatus == "planted" && !iPath.empty()) {
        written.push_back({"engine", iPath});
    }
    std::sort(written.begin(), written.end(),
              [](const auto &x, const auto &y) { return x.second != y.second ? x.second < y.second : x.first < y.first; });
    for (const auto &[side, path] : written) {
        std::cout << side << "\t" << path << "\n";
    }
    std::cout << "catalog files written: Engine Library/Database2/m.db"
              << (k && kStatus == "planted" ? ", a new image under Engine Library/Artwork/" : "")
              << (iStatus == "planted" && !iPath.empty() ? ", " + iPath : std::string())
              << ", the ANLZ .EXT and .DAT of each rekordbox track above"
              << (probe::hasOneLibrary(pioneerPath) ? ", PIONEER/rekordbox/exportLibrary.db" : "") << "\n";
    if (options.noArm && !stateAfter.playerWillOfferImport()) {
        std::cout << "note: not armed and the numbers are level, so the player will not ask\n";
    }

    for (const auto &failure : failures) {
        std::cout << "FAILED: " << failure << "\n";
    }
    std::cout << "PLANT RESULT: " << (failures.empty() ? "PASS" : "FAIL") << "\n";
    return failures.empty() ? 0 : 1;
}

void usage()
{
    std::cerr << "usage: engine_import_probe --plant <stick root> --out <cases.tsv> [--cover <image>] [--no-arm]\n"
              << "                           [--skip <path substring>]...\n"
              << "       engine_import_probe --record <stick root> --out <record.tsv>\n"
              << "       engine_import_probe --compare <before.tsv> <after.tsv> [--cases <cases.tsv>]\n";
}

}  // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 3) {
        usage();
        return 2;
    }
    const std::string mode = args[0];
    try {
        if (mode == "--plant" || mode == "--record") {
            PlantOptions options;
            options.root = pathFromUtf8(args[1]);
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--out" && i + 1 < args.size()) {
                    options.out = pathFromUtf8(args[++i]);
                } else if (args[i] == "--cover" && i + 1 < args.size() && mode == "--plant") {
                    options.cover = pathFromUtf8(args[++i]);
                } else if (args[i] == "--no-arm" && mode == "--plant") {
                    options.noArm = true;
                } else if (args[i] == "--skip" && i + 1 < args.size() && mode == "--plant") {
                    options.skip.push_back(args[++i]);
                } else {
                    std::cerr << "unknown argument: " << args[i] << "\n";
                    usage();
                    return 2;
                }
            }
            if (options.out.empty()) {
                usage();
                return 2;
            }
            if (mode == "--plant") {
                return plant(options);
            }
            const Record record = recordStick(options.root);
            if (!writeRecordFile(options.out, record)) {
                std::cerr << "could not write " << pathToUtf8(options.out) << "\n";
                return 1;
            }
            std::size_t engineRows = 0;
            for (const auto &entity : record.entities("ET")) {
                engineRows += record.get("ET", entity, "row") ? 1 : 0;
            }
            std::cout << "recorded " << record.values.size() << " values: " << engineRows
                      << " Engine tracks, " << record.entities("EP").size() << " Engine playlists, "
                      << record.entities("EF").size() << " files under Engine Library, "
                      << record.entities("RT").size() << " rekordbox tracks, " << record.entities("RP").size()
                      << " rekordbox playlists\n";
            std::cout << "pdb sequence " << record.get("M", "-", "pdb.sequence").value_or("?")
                      << ", Engine import counter " << record.get("M", "-", "engine.counter").value_or("?") << ": "
                      << record.get("M", "-", "prompt").value_or("?") << "\n";
            std::cout << "written to " << pathToUtf8(options.out) << "\n";
            return 0;
        }
        if (mode == "--compare") {
            fs::path casesFile;
            for (std::size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--cases" && i + 1 < args.size()) {
                    casesFile = pathFromUtf8(args[++i]);
                } else {
                    std::cerr << "unknown argument: " << args[i] << "\n";
                    usage();
                    return 2;
                }
            }
            const auto before = readRecordFile(pathFromUtf8(args[1]));
            const auto after = readRecordFile(pathFromUtf8(args[2]));
            if (!before || !after) {
                return 2;
            }
            std::vector<probe::Case> cases;
            if (!casesFile.empty()) {
                std::ifstream in(casesFile, std::ios::binary);
                std::string error;
                if (!in || !probe::readCases(in, cases, &error)) {
                    std::cerr << pathToUtf8(casesFile) << ": " << (in ? error : "could not open") << "\n";
                    return 2;
                }
            }
            probe::printCompare(std::cout, probe::compareRecords(*before, *after, cases));
            return 0;
        }
    } catch (const std::exception &ex) {
        std::cerr << "engine_import_probe: " << ex.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}
