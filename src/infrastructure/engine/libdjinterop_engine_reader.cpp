// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>

namespace seabass::infrastructure::engine
{

namespace application = seabass::application;
namespace domain = seabass::domain;

// alpha == 0 is djinterop::pad_color's own default-constructed value (see
// its own doc comment: "Construct a pad_color with a default black
// color"), and the blob decoder (quick_cues_blob.cpp) reads whatever
// alpha byte is actually stored -- it isn't synthesized by libdjinterop.
// Confirmed on real data this session: hot cues never explicitly colored
// on real hardware come through with alpha == 0, previously rendered as
// the misleading "#000000" (indistinguishable from a genuinely
// black-colored cue) instead of "no color at all". That false distinction
// from rekordbox's own "no color" representation (color_id == 0, see
// kaitai_rekordbox_reader.cpp's cueColor()) was making cueSetsEqual()
// treat identical, uncolored cues from the two formats as a real
// conflict -- confirmed as the actual cause of a full batch of "genuine"
// cross-source sync conflicts that weren't genuine at all.
std::string colorHex(const djinterop::pad_color &c)
{
    if (c.a == 0) {
        return "";
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

namespace
{

// Some individual fields on some tracks (observed: sample_rate() on a
// track just re-cued on real Denon hardware) throw a decode error from
// libdjinterop even when the rest of the track, including hot_cues(),
// reads fine. Isolating each field like this means one flaky field never
// costs us the whole track, which a single try/catch around everything
// used to do.
template<typename T, typename Fn>
T safeGet(application::ProgressReporter &progress, int64_t trackId, const char *fieldName, Fn &&fn, T fallback = T{})
{
    try {
        return fn();
    } catch (const std::exception &e) {
        progress.warn("track id=" + std::to_string(trackId) + ": " + fieldName + " unreadable (" + e.what() + ")");
        return fallback;
    }
}

// Recursively walks a playlist tree, recording each track's full playlist
// path(s) (e.g. "Techno/Peak Time"). Best-effort: any failure here just
// leaves playlists empty rather than failing the whole scan, since
// membership is supplementary information, not core track data.
void collectPlaylistMemberships(const djinterop::playlist &pl, const std::string &pathPrefix,
                                 std::unordered_map<int64_t, std::vector<domain::PlaylistMembership>> &membership)
{
    std::string path = pathPrefix.empty() ? pl.name() : pathPrefix + "/" + pl.name();
    int position = 0;
    for (const auto &tr : pl.tracks()) {
        membership[tr.id()].push_back(domain::PlaylistMembership{path, position++});
    }
    for (const auto &child : pl.children()) {
        collectPlaylistMemberships(child, path, membership);
    }
}

// Track id -> best-effort resolved artwork file path, read directly via
// plain SQLite3 rather than libdjinterop's own public API - track.hpp
// exposes no way to get at album art at all (djinterop::album_art exists
// as a type but is an acknowledged stub, "TODO - implement rest of
// album_art class", and database.hpp has no method returning one), even
// though the underlying schema has real, resolvable data: every Track row
// has an albumArtId, and AlbumArt.hash is not a hash at all despite the
// column name. It's a URI like "image://fileart//media/<label>/PIONEER/
// Artwork/00001/a5_m.jpg" pointing at a real external JPEG file on the
// stick (AlbumArt.albumArt, the actual BLOB column, is confirmed always
// empty on real hardware-written data. Engine stores art as files, not
// inline). <label> is whatever volume label the *original* Denon hardware
// mounted the stick under, not necessarily this machine's, so this
// anchors on the stable "PIONEER/Artwork/..." suffix instead of trying to
// match the volume label. seabass_core already links plain SQLite3
// directly for LocalCueStore, so this doesn't add a new dependency; opened
// as a second, independent, read-only connection to the same m.db
// djinterop::engine::load_database() above already has open, never
// written through.
std::unordered_map<int64_t, std::string> readArtworkPaths(const std::string &engineLibraryPath)
{
    std::unordered_map<int64_t, std::string> result;
    std::filesystem::path stickRoot = std::filesystem::path(engineLibraryPath).parent_path();
    std::string dbPath = (std::filesystem::path(engineLibraryPath) / "Database2" / "m.db").string();

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT t.id, a.hash FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
        "WHERE t.albumArtId IS NOT NULL AND t.albumArtId != 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t trackId = sqlite3_column_int64(stmt, 0);
        const unsigned char *hashText = sqlite3_column_text(stmt, 1);
        if (!hashText) {
            continue;
        }
        std::string hash = reinterpret_cast<const char *>(hashText);
        auto pos = hash.find("PIONEER/Artwork");
        if (pos == std::string::npos) {
            continue;
        }
        // make_preferred(): hash.substr(pos) carries its own forward
        // slashes ("PIONEER/Artwork/...") appended as one path component
        // in a single operator/ call, so fs::path preserves them as
        // literal characters rather than re-splitting into components --
        // .string() would otherwise mix them with the native separator
        // from the stickRoot join on Windows. Same bug/fix as
        // OneLibraryReader's artworkPath (onelibrary_reader.cpp), found
        // via a real Windows test failure there.
        //
        // hash is a raw Engine sqlite column, so on Windows operator/ can
        // throw filesystem_error over bytes that are not valid UTF-8 (see
        // path_key.cpp's own doc comment on the same risk). Caught per
        // row, not just by readAll()'s outer try/catch around this whole
        // function: that one would otherwise lose every OTHER track's
        // artwork too, not just this row's.
        try {
            std::filesystem::path candidate = (stickRoot / hash.substr(pos)).make_preferred();
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                result[trackId] = candidate.string();
            }
        } catch (const std::exception &) {
            // Best-effort, same as the rest of this loop's field reads:
            // one unreadable artwork path is not worth losing the scan.
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

// Track id -> streaming source (e.g. "TIDAL"), same raw-SQLite workaround
// as readArtworkPaths() above and for the same reason: libdjinterop's
// public API never exposes Track.streamingSource/uri at all (open TODOs
// in the library's own headers acknowledge this). A track with this set
// has no real local file. Its `path` column points at a streaming-
// cache location on the *computer* that manages playback, never at
// anything present on this stick, so callers must never treat it as an
// ordinary local track (play it, merge it, sync it, clean it up).
std::unordered_map<int64_t, std::string> readStreamingSources(const std::string &engineLibraryPath)
{
    std::unordered_map<int64_t, std::string> result;
    std::string dbPath = (std::filesystem::path(engineLibraryPath) / "Database2" / "m.db").string();

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id, streamingSource FROM Track WHERE streamingSource IS NOT NULL AND streamingSource != ''";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t trackId = sqlite3_column_int64(stmt, 0);
        const unsigned char *sourceText = sqlite3_column_text(stmt, 1);
        if (!sourceText) {
            continue;
        }
        result[trackId] = reinterpret_cast<const char *>(sourceText);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

}  // namespace

LibdjinteropEngineReader::LibdjinteropEngineReader(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
{
}

std::vector<domain::Track> LibdjinteropEngineReader::readAll()
{
    if (!djinterop::engine::database_exists(m_engineLibraryPath)) {
        throw std::runtime_error("no Engine Library found at " + m_engineLibraryPath);
    }

    auto db = djinterop::engine::load_database(m_engineLibraryPath);
    auto allTracks = db.tracks();

    std::unordered_map<int64_t, std::vector<domain::PlaylistMembership>> playlistsByTrackId;
    try {
        for (const auto &root : db.root_playlists()) {
            collectPlaylistMemberships(root, "", playlistsByTrackId);
        }
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read playlists: ") + e.what());
    }

    std::unordered_map<int64_t, std::string> artworkByTrackId;
    try {
        artworkByTrackId = readArtworkPaths(m_engineLibraryPath);
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read album art: ") + e.what());
    }

    std::unordered_map<int64_t, std::string> streamingSourceByTrackId;
    try {
        streamingSourceByTrackId = readStreamingSources(m_engineLibraryPath);
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read streaming sources: ") + e.what());
    }

    m_progress->start("Scanning Engine tracks", allTracks.size());
    size_t processed = 0;

    std::vector<domain::Track> tracks;
    for (auto &tr : allTracks) {
        int64_t id = tr.id();
        domain::Track track;
        track.sourceId = std::to_string(id);
        track.format = "engine";
        track.title = safeGet<std::string>(*m_progress, id, "title", [&] { return tr.title().value_or(""); });
        track.artist = safeGet<std::string>(*m_progress, id, "artist", [&] { return tr.artist().value_or(""); });
        track.album = safeGet<std::string>(*m_progress, id, "album", [&] { return tr.album().value_or(""); });
        track.filename = safeGet<std::string>(*m_progress, id, "filename", [&] { return tr.filename(); });
        track.filePath = safeGet<std::string>(*m_progress, id, "relative_path", [&] {
            auto resolved = std::filesystem::path(m_engineLibraryPath) / tr.relative_path();
            return resolved.lexically_normal().string();
        });
        if (!track.filePath.empty()) {
            std::error_code ec;
            auto size = std::filesystem::file_size(track.filePath, ec);
            track.fileSizeBytes = ec ? 0 : size;
        }
        track.bpm = safeGet<double>(*m_progress, id, "bpm", [&] { return tr.bpm().value_or(0.0); });
        track.bitrate = safeGet<int>(*m_progress, id, "bitrate", [&] { return tr.bitrate().value_or(0); });
        track.key = safeGet<std::string>(*m_progress, id, "key", [&] {
            auto k = tr.key();
            if (!k) {
                return std::string();
            }
            std::ostringstream oss;
            oss << *k;
            return oss.str();
        });
        track.durationSeconds = safeGet<double>(*m_progress, id, "duration", [&] {
            auto duration = tr.duration();
            return duration ? duration->count() / 1000.0 : 0.0;
        });
        track.lastPlayedAt = safeGet<std::optional<std::chrono::system_clock::time_point>>(
            *m_progress, id, "last_played_at", [&] { return tr.last_played_at(); });
        track.rating = safeGet<std::optional<int>>(*m_progress, id, "rating", [&] {
            auto r = tr.rating();
            if (!r || *r <= 0) {
                return std::optional<int>{};
            }
            return std::optional<int>{*r / 20};
        });
        track.comment = safeGet<std::string>(*m_progress, id, "comment", [&] { return tr.comment().value_or(""); });
        auto playlistsIt = playlistsByTrackId.find(id);
        if (playlistsIt != playlistsByTrackId.end()) {
            track.playlists = playlistsIt->second;
        }
        auto artworkIt = artworkByTrackId.find(id);
        if (artworkIt != artworkByTrackId.end()) {
            track.artworkPath = artworkIt->second;
        }
        auto streamingIt = streamingSourceByTrackId.find(id);
        if (streamingIt != streamingSourceByTrackId.end()) {
            track.streamingSource = streamingIt->second;
        }

        auto sampleRate = safeGet<std::optional<double>>(*m_progress, id, "sample_rate",
                                                          [&] { return tr.sample_rate(); });
        if (!sampleRate) {
            // 44.1kHz is by far the most common sample rate for the
            // compressed audio these libraries hold; falling back to it
            // gives a position that's very likely close to right, instead
            // of treating a raw sample count as if it were milliseconds
            // (which is wrong by roughly a factor of 44).
            sampleRate = 44100.0;
        }
        auto hotCues = safeGet<std::vector<std::optional<djinterop::hot_cue>>>(*m_progress, id, "hot_cues",
                                                                                [&] { return tr.hot_cues(); });
        for (size_t i = 0; i < hotCues.size(); ++i) {
            if (!hotCues[i]) {
                continue;
            }
            const auto &hotCue = *hotCues[i];
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Hot;
            cp.hotCueNumber = static_cast<int>(i) + 1;  // Engine slots are 0-based; rekordbox numbers from 1
            cp.positionMs = hotCue.sample_offset / *sampleRate * 1000.0;
            cp.color = colorHex(hotCue.color);
            cp.comment = hotCue.label;
            track.cues.push_back(std::move(cp));
        }

        // Engine's hot loops live in their own 8-slot array (loops()),
        // separate from hot_cues() above -- indexed the same way, but a
        // genuinely different column family, not a variant of hot_cue. On
        // the hardware a given pad shows either the hot cue or the hot
        // loop for its number depending on pad mode, never both; Seabass
        // itself enforces that one-or-the-other rule when writing (see
        // AddCueController), matching the design this reads back into.
        auto loops = safeGet<std::vector<std::optional<djinterop::loop>>>(*m_progress, id, "loops",
                                                                            [&] { return tr.loops(); });
        for (size_t i = 0; i < loops.size(); ++i) {
            if (!loops[i]) {
                continue;
            }
            const auto &loop = *loops[i];
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Hot;
            cp.hotCueNumber = static_cast<int>(i) + 1;
            cp.isLoop = true;
            cp.positionMs = loop.start_sample_offset / *sampleRate * 1000.0;
            cp.loopEndMs = loop.end_sample_offset / *sampleRate * 1000.0;
            cp.color = colorHex(loop.color);
            cp.comment = loop.label;
            track.cues.push_back(std::move(cp));
        }

        // Engine's format has exactly one memory-style cue point (called
        // "Cue" in the app), stored as a plain sample offset with no
        // color/comment, unlike rekordbox's unlimited, independently
        // colored/commented memory cues. Represented here as a single
        // Kind::Memory CuePoint (hotCueNumber 0, matching how rekordbox's
        // own reader marks memory cues) so it can be matched/synced like
        // any other cue; see libdjinterop_engine_cue_writer.cpp for the
        // corresponding (necessarily lossy beyond one cue) write side.
        auto mainCue = safeGet<std::optional<double>>(*m_progress, id, "main_cue", [&] { return tr.main_cue(); });
        if (mainCue) {
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Memory;
            cp.positionMs = *mainCue / *sampleRate * 1000.0;
            track.cues.push_back(std::move(cp));
        }

        tracks.push_back(std::move(track));
        m_progress->tick(++processed);
        m_cancel.throwIfCancelled();
    }

    m_progress->finish();
    return tracks;
}

}  // namespace seabass::infrastructure::engine
