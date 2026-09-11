// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_anonymizer.hpp"

#include "infrastructure/anonymization_export_layout.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <sqlite3.h>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "infrastructure/anonymization_placeholder.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

// libdjinterop derives track::filename() from the relative path, so it
// never writes the Track.filename column and set_relative_path() leaves
// it holding the original real filename. Nothing in this app reads that
// column -- but Engine DJ does, and so does anyone who opens the file, so
// an export that left it alone was still shipping every real filename.
//
// Done in SQL, after libdjinterop has closed its own connection, because
// there is no setter to do it through.
// Empties the waveform blob and reclaims what that frees.
//
// PerformanceData holds the cues (quickCues, loops) and the waveform
// (overviewWaveFormData) in the same row, so this clears one column
// rather than dropping the table. Vacuuming afterwards is the point of
// doing it at all: without it SQLite keeps the pages as free space and
// the file does not shrink, which is the whole reason this exists.
int emptyEngineWaveformColumn(const std::string &dbPath)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    int changed = 0;
    char *error = nullptr;
    if (sqlite3_exec(db, "UPDATE PerformanceData SET overviewWaveFormData = NULL "
                         "WHERE overviewWaveFormData IS NOT NULL;",
                     nullptr, nullptr, &error)
        == SQLITE_OK) {
        changed = sqlite3_changes(db);
    }
    sqlite3_free(error);
    error = nullptr;
    sqlite3_exec(db, "VACUUM;", nullptr, nullptr, &error);
    sqlite3_free(error);
    sqlite3_close(db);
    return changed;
}

int scrubFilenameColumn(const std::string &destinationRoot)
{
    const std::string dbPath = (std::filesystem::path(destinationRoot) / "Database2" / "m.db").string();
    sqlite3 *db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    // The scrubbed path is "Contents/<hash>.<ext>", so the filename is
    // everything after the last separator. rtrim(path, <path without
    // slashes>) leaves just the leading "Contents/", which replace() then
    // strips -- one statement, and correct for a path with no separator.
    const char *sql =
        "UPDATE Track SET filename = "
        "  CASE WHEN instr(path, '/') > 0 "
        "       THEN replace(path, rtrim(path, replace(path, '/', '')), '') "
        "       ELSE path END "
        "WHERE path IS NOT NULL;";
    char *error = nullptr;
    int changed = 0;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) {
        changed = sqlite3_changes(db);
    }
    sqlite3_free(error);
    sqlite3_close(db);
    return changed;
}

}  // namespace

namespace fs = std::filesystem;

namespace
{

std::string placeholder(const std::string &kind, size_t index)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03zu", index);
    return kind + " " + buf;
}

// Removes `doomed` from `pl`'s track list if present (no survivor to
// repoint to -- unlike LibdjinteropEngineCleanupWriter's consolidation
// case, this is a plain deletion), then recurses into child playlists.
// Rebuilds via clear_tracks()/add_track_back() rather than
// playlist::remove_track(), for the same reason documented in
// libdjinterop_engine_cleanup_writer.cpp: that method deletes by
// PlaylistEntity id, not track id, in this vendored libdjinterop
// version.
void removeFromPlaylistTree(djinterop::playlist pl, const djinterop::track &doomed)
{
    auto tracks = pl.tracks();
    bool containsDoomed = std::any_of(tracks.begin(), tracks.end(),
                                       [&](const djinterop::track &t) { return t.id() == doomed.id(); });
    if (containsDoomed) {
        pl.clear_tracks();
        for (const auto &t : tracks) {
            if (t.id() != doomed.id()) {
                pl.add_track_back(t);
            }
        }
    }
    for (auto &child : pl.children()) {
        removeFromPlaylistTree(child, doomed);
    }
}

// Renames every playlist/folder in the tree rooted at `pl`, depth-first.
int renamePlaylistTree(djinterop::playlist pl, size_t &nextIndex)
{
    pl.set_name(placeholder("Playlist", nextIndex++));
    int renamed = 1;
    for (auto &child : pl.children()) {
        renamed += renamePlaylistTree(child, nextIndex);
    }
    return renamed;
}

void copyTreeIfPresent(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        return;
    }
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        throw std::runtime_error("failed to copy " + from.string() + " to " + to.string() + ": " + ec.message());
    }
}

}  // namespace

EngineAnonymizationResult anonymizeEngineLibrary(const std::string &sourceRoot, const std::string &destinationRoot,
                                                  std::optional<size_t> maxTracks, bool slimForTesting,
                                                  application::ProgressReporter &reporter)
{
    EngineAnonymizationResult result;

    std::error_code ec;
    if (fs::exists(destinationRoot, ec) && !fs::is_empty(destinationRoot, ec)) {
        result.errorMessage = destinationRoot + " already exists and isn't empty -- refusing to write into it";
        return result;
    }
    fs::create_directories(destinationRoot, ec);

    try {
        copyTreeIfPresent(fs::path(sourceRoot) / "Database2", fs::path(destinationRoot) / "Database2");
        // That copy takes the whole Database2 directory, which holds more
        // than the catalog. Only m.db is scrubbed below; everything else
        // at that level went out untouched, and hm.db is the play history
        // -- real titles, artists, albums and paths, plus which set each
        // track was played in.
        //
        // Allowlisted rather than removed by name, for the same reason
        // the rekordbox side is: a future Engine version adding a file
        // must not be able to ship it unexamined. Subdirectories are
        // kept: OverviewData holds the low-resolution waveform previews,
        // which are derived numbers with no text in them.
        {
            std::error_code listEc;
            const fs::path databaseDir = fs::path(destinationRoot) / "Database2";
            std::vector<fs::path> unknown;
            for (const auto &entry : fs::directory_iterator(databaseDir, listEc)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (!isKeptEngineDatabaseFile(entry.path().filename().string())) {
                    unknown.push_back(entry.path());
                }
            }
            for (const auto &path : unknown) {
                std::error_code removeEc;
                fs::remove(path, removeEc);
                result.removedUnanonymizableFiles.push_back(path.filename().string());
            }
        }

        if (slimForTesting) {
            // The waveform, and only the waveform. Engine keeps its cues
            // in PerformanceData.quickCues and .loops, in the same table
            // and right beside this column, so the two have to be told
            // apart rather than the table dropped: on a real export the
            // waveform is 0.89 MB against 0.32 MB for every cue and loop
            // in the library.
            //
            // OverviewData is the same data again as .rgb files, 2.8 MB
            // of it, and nothing but a picture.
            std::error_code slimEc;
            fs::remove_all(fs::path(destinationRoot) / "Database2" / "OverviewData", slimEc);
            result.waveformRowsEmptied = emptyEngineWaveformColumn(
                (fs::path(destinationRoot) / "Database2" / "m.db").string());
        }

        if (!djinterop::engine::database_exists(destinationRoot)) {
            result.errorMessage = "no Engine Library found at " + sourceRoot;
            return result;
        }
        auto db = djinterop::engine::load_database(destinationRoot);

        std::vector<djinterop::track> allTracks = db.tracks();
        reporter.start("Anonymizing Engine library", allTracks.size());

        std::vector<djinterop::track> kept = allTracks;
        std::vector<djinterop::track> dropped;
        if (maxTracks && kept.size() > *maxTracks) {
            auto splitPoint = kept.begin() + static_cast<std::vector<djinterop::track>::difference_type>(*maxTracks);
            dropped.assign(splitPoint, kept.end());
            // erase(), not resize(): djinterop::track has no default
            // constructor (it wraps a pimpl handle), which resize()
            // would need for any growth-shaped operation.
            kept.erase(splitPoint, kept.end());
        }

        for (const auto &doomed : dropped) {
            for (auto &root : db.root_playlists()) {
                removeFromPlaylistTree(root, doomed);
            }
            db.remove_track(doomed);
        }

        // Unlike rekordbox's normalized artist table, Engine stores
        // artist as a plain string per track -- map each distinct real
        // artist name to one placeholder (rather than one placeholder
        // per track) so tracks that really do share an artist still
        // group together after anonymization, matching real-world
        // "browse by artist" structure.
        std::unordered_map<std::string, std::string> artistPlaceholderByRealName;
        size_t nextArtistIndex = 0;
        size_t nextCueLabelIndex = 0;

        size_t trackIndex = 0;
        for (auto &t : kept) {
          // Per track, not per run. libdjinterop throws on performance
          // data it cannot decode (real libraries have plenty), and a
          // single top-level catch meant the first such track aborted the
          // loop and left every track after it holding its real metadata
          // in an export that still got written and zipped. Refusing one
          // track has to cost one track.
          try {
            std::string realArtist = t.artist().value_or("");
            auto artistIt = artistPlaceholderByRealName.find(realArtist);
            if (artistIt == artistPlaceholderByRealName.end()) {
                artistIt = artistPlaceholderByRealName
                               .emplace(realArtist, placeholder("Artist", nextArtistIndex++))
                               .first;
            }

            // Filename keyed off the real filename via
            // anonymizationFilenamePlaceholder() (not a per-run
            // sequential index) so the same real track gets the same
            // obfuscated filename here and in the independently-run
            // rekordbox anonymizer -- see that function's own comment
            // for why that's what domain::TrackMatcher's cross-catalog
            // sync matching actually needs to keep working against
            // anonymized data.
            std::string realFilename = t.filename();
            std::string obfuscatedFilename = anonymizationFilenamePlaceholder(realFilename);
            t.set_title(anonymizationPlaceholder("Track", realFilename));
            t.set_artist(artistIt->second);
            t.set_comment(anonymizationPlaceholder("Comment", realFilename));
            t.set_relative_path("Contents/" + obfuscatedFilename);
            // Album, genre and record label are free text a person or a
            // tagging tool typed, exactly as identifying as the title, and
            // they were never scrubbed at all. Only set them when the real
            // track had one, so "no album" stays "no album" and the shape
            // of the library survives.
            if (t.album()) {
                t.set_album(anonymizationPlaceholder("Album", realFilename));
            }
            if (t.genre()) {
                t.set_genre(anonymizationPlaceholder("Genre", realFilename));
            }
            if (t.publisher()) {
                t.set_publisher(anonymizationPlaceholder("Label", realFilename));
            }

            // Hot cue/loop *labels* are the Engine-side equivalent of
            // rekordbox's cue comments -- real free text a DJ typed per
            // cue point, not just structural position/color data. Only
            // the label changes; position()/loop bounds/color etc. stay
            // exactly as read back from each present entry.
            auto hotCues = t.hot_cues();
            for (auto &cue : hotCues) {
                if (cue) {
                    cue->label = placeholder("Cue", nextCueLabelIndex++);
                }
            }
            t.set_hot_cues(hotCues);

            auto loops = t.loops();
            for (auto &l : loops) {
                if (l) {
                    l->label = placeholder("Cue", nextCueLabelIndex++);
                }
            }
            t.set_loops(loops);

          } catch (const std::exception &e) {
            ++result.tracksRefused;
            if (result.firstRefusalReason.empty()) {
                result.firstRefusalReason = e.what();
            }
          }
            ++trackIndex;
            reporter.tick(trackIndex);
        }

        size_t playlistIndex = 0;
        for (auto &root : db.root_playlists()) {
            result.playlistsRenamed += renamePlaylistTree(root, playlistIndex);
        }

        reporter.finish();
        result.tracksKept = static_cast<int>(kept.size());
        result.tracksDropped = static_cast<int>(dropped.size());
    } catch (const std::exception &e) {
        result.errorMessage = e.what();
    }
    if (result.errorMessage.empty()) {
        result.filenameColumnRows = scrubFilenameColumn(destinationRoot);
    }
    return result;
}

}  // namespace seabass::infrastructure::engine
