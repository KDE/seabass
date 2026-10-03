// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/onelibrary/onelibrary_reader.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_source_for_root.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"

namespace seabass::infrastructure::onelibrary
{

namespace fs = std::filesystem;
using domain::CuePoint;
using domain::PlaylistMembership;
using domain::Track;

namespace
{

struct PlaylistInfo
{
    std::string name;
    int64_t parentId = 0;
};

// Mirrors KaitaiRekordboxReader's playlistPath() exactly -- same "walk
// parent links up to the root, guard against a cyclic chain" shape, just
// against exportLibrary.db's own playlist/playlist_id_parent columns
// instead of export.pdb's PLAYLIST_TREE.
std::string playlistPath(int64_t id, const std::unordered_map<int64_t, PlaylistInfo> &tree)
{
    std::vector<std::string> parts;
    int64_t current = id;
    for (int guard = 0; current != 0 && guard < 64; ++guard) {
        auto it = tree.find(current);
        if (it == tree.end()) {
            break;
        }
        parts.push_back(it->second.name);
        current = it->second.parentId;
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) {
            path += "/";
        }
        path += *it;
    }
    return path;
}

}  // namespace

OneLibraryReader::OneLibraryReader(std::string pioneerRoot) : m_pioneerRoot(std::move(pioneerRoot)) {}

std::optional<size_t> OneLibraryReader::countTracks()
{
    if (!OneLibraryCueWriter::existsFor(m_pioneerRoot)) {
        return std::nullopt;
    }
    try {
        SqlCipherLibrary lib;
        SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(m_pioneerRoot), /*readOnly=*/true);
        db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
        SqlCipherStatement count(db, "SELECT count(*) FROM content");
        if (count.step()) {
            return static_cast<size_t>(count.columnInt64(0));
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

std::vector<Track> OneLibraryReader::readAll()
{
    // The catalog pass is a fraction of the analysis-file pass, so the
    // one bar a caller of readAll() sees follows the files, under the
    // label this reader always had. The catalog pass still checks the
    // cancellation token per track.
    auto tracks = readCatalog(application::NullProgressReporter::instance());
    readAnalysis(tracks, *m_progress, "Reading OneLibrary");
    return tracks;
}

std::vector<Track> OneLibraryReader::readTracks()
{
    return readCatalog(*m_progress);
}

void OneLibraryReader::fillCues(std::vector<Track> &tracks)
{
    readAnalysis(tracks, *m_progress, "Reading OneLibrary cues");
}

void OneLibraryReader::readAnalysis(std::vector<Track> &tracks, application::ProgressReporter &progress,
                                    const std::string &label)
{
    // A browsed stick backup serves its analysis files out of the
    // archive; the same resolution the DeviceLibrary reader makes.
    const auto source = rekordbox::anlzSourceForPioneerRoot(m_pioneerRoot);
    progress.start(label, tracks.size());
    size_t done = 0;
    for (auto &track : tracks) {
        if (track.format == "onelibrary" && !track.analysisFile.empty()) {
            // A track's cues are its analysis file's, never the cue table's:
            // see the class comment. A file that is not there (nothing
            // analysed the track) means no cues, which is also what a
            // player has to go on.
            try {
                auto cues = rekordbox::readAnalysisFileCues(*source, track.analysisFile, &track.cueLists);
                if (!cues) {
                    // Said, not passed over: the row names a file that is
                    // not there, so no player has its cues either.
                    m_progress->warn("content_id=" + track.sourceId + ": analysis file " + track.analysisFile
                                     + " is not on the stick, so the track has no cues a player reads");
                }
                track.cues = cues ? std::move(*cues) : std::vector<CuePoint>{};
            } catch (const rekordbox::AnalysisFileUnreadable &e) {
                track.cues.clear();
                m_progress->warn("content_id=" + track.sourceId + ": analysis file " + track.analysisFile
                              + " unreadable, its cues were not read (" + e.what() + ")");
            }
            if (auto modified = rekordbox::analysisFileModifiedAt(m_pioneerRoot, track.analysisFile)) {
                track.metadataModifiedAt = *modified;
            }
        }
        progress.tick(++done);
        m_cancel.throwIfCancelled();
    }
    progress.finish();
}

std::vector<Track> OneLibraryReader::readCatalog(application::ProgressReporter &progress)
{
    if (!OneLibraryCueWriter::existsFor(m_pioneerRoot)) {
        throw std::runtime_error("no OneLibrary (exportLibrary.db) present for this stick");
    }
    std::string dbPath = OneLibraryCueWriter::dbPathFor(m_pioneerRoot);
    fs::path stickRoot = pathFromUtf8(m_pioneerRoot).parent_path();

    SqlCipherLibrary lib;
    // A stick pulled mid-save leaves OneLibrary with a pending journal,
    // which a read-only open cannot get past. Roll it back first, keeping
    // a copy on this computer; see sqlite_pending_journal.hpp (#48). One
    // still being written after the wait is left to the read-only open
    // below, which reports the lock itself.
    const auto readWith = [&lib, &dbPath](bool readOnly) {
        SqliteReadOutcome outcome;
        try {
            SqlCipherDb db(lib, dbPath, readOnly);
            try {
                db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
                SqlCipherStatement read(db, "SELECT count(*) FROM sqlite_master");
                read.step();
            } catch (const std::exception &e) {
                outcome.code = lib.extendedErrcode(db.handle());
                outcome.message = e.what();
            }
        } catch (const std::exception &e) {
            outcome.code = 1;  // SQLITE_ERROR: could not open
            outcome.message = e.what();
        }
        return outcome;
    };
    const PendingJournalRecovery recovery = recoverPendingJournal(
        pathFromUtf8(dbPath), paths::localRoot() / "recovered", [&readWith]() { return readWith(true); },
        [&readWith]() { return readWith(false); }, std::chrono::seconds(10));
    if (recovery.found && !recovery.recovered) {
        throw std::runtime_error("OneLibrary on this stick was left mid-save (was the stick pulled while saving?) "
                                 "and could not be put back: " + recovery.error
                                 + ". If the stick is mounted read-only, repair it in Library Health first.");
    }
    if (recovery.recovered) {
        std::cerr << "onelibrary: rolled back an unfinished save in " << dbPath << "; a copy of how it was is in "
                  << pathToUtf8(recovery.keptCopy) << "\n";
    }
    SqlCipherDb db(lib, dbPath, /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");

    // Playlist tree, built once up front -- same shape as every other
    // reader in this codebase (id -> {name, parent}, then a second pass
    // resolves each track's memberships to full paths).
    std::unordered_map<int64_t, PlaylistInfo> playlistTree;
    {
        SqlCipherStatement stmt(db, "SELECT playlist_id, name, playlist_id_parent FROM playlist");
        while (stmt.step()) {
            playlistTree[stmt.columnInt64(0)] = PlaylistInfo{stmt.columnText(1), stmt.columnInt64(2)};
        }
    }

    // content_id -> playlist memberships (name resolved via playlistTree,
    // position from playlist_content's own sequenceNo).
    std::unordered_map<int64_t, std::vector<PlaylistMembership>> playlistsByContentId;
    {
        SqlCipherStatement stmt(db, "SELECT content_id, playlist_id, sequenceNo FROM playlist_content");
        while (stmt.step()) {
            // The two passes before the track loop check the token too:
            // together they are as long as the loop on a library with
            // many cues, and a stop must not wait them out.
            m_cancel.throwIfCancelled();
            int64_t contentId = stmt.columnInt64(0);
            std::string path = playlistPath(stmt.columnInt64(1), playlistTree);
            if (path.empty()) {
                continue;
            }
            playlistsByContentId[contentId].push_back(
                PlaylistMembership{path, static_cast<int>(stmt.columnInt64(2))});
        }
    }

    size_t total = 0;
    {
        SqlCipherStatement count(db, "SELECT count(*) FROM content");
        if (count.step()) {
            total = static_cast<size_t>(count.columnInt64(0));
        }
    }
    progress.start("Reading OneLibrary", total);

    // The album table is optional. rekordbox has shipped several
    // exportLibrary.db schema versions (see the OneLibrary issues), and
    // refusing to read a stick at all because one nice-to-have field's
    // table is absent is a bad trade: the album is one line in a details
    // panel, the catalog is the whole library. So the join is added only
    // when the table is there, and the field stays empty when it is not.
    //
    // Checked rather than caught: letting the prepare fail and retrying
    // would also swallow a genuine SQL error in the rest of the query.
    // Both halves, because they can differ: a schema was found with the
    // album table present and content.album_id absent, which passed a
    // table-only check and then failed to prepare the query.
    bool hasAlbums = false;
    bool hasAnalysisPath = false;
    {
        bool hasAlbumTable = false;
        SqlCipherStatement table(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='album'");
        if (table.step() && table.columnInt64(0) > 0) {
            hasAlbumTable = true;
        }
        SqlCipherStatement columns(db, "PRAGMA table_info(content)");
        while (columns.step()) {
            const std::string column = columns.columnText(1);
            if (column == "album_id") {
                hasAlbums = hasAlbumTable;
            } else if (column == "analysisDataFilePath") {
                hasAnalysisPath = true;
            }
        }
    }

    std::vector<Track> tracks;
    size_t done = 0;
    // content.album_id, NOT album_id_album: the artist join uses the
    // doubled form, so this column name was worth checking against a real
    // database rather than inferring it. PRAGMA table_info(content) on a
    // real stick says album_id.
    const std::string albumColumn = hasAlbums ? "al.name" : "NULL";
    const std::string albumJoin = hasAlbums ? " LEFT JOIN album al ON al.album_id = c.album_id" : "";
    // Every real exportLibrary.db seen has the column; a database without
    // it names no analysis files, so its tracks read with no cues.
    const std::string analysisColumn = hasAnalysisPath ? "c.analysisDataFilePath" : "NULL";
    SqlCipherStatement stmt(db,
                             "SELECT c.content_id, c.title, a.name, c.bpmx100, c.length, c.path, c.fileName, "
                             "c.bitrate, c.fileSize, k.name, c.djPlayCount, i.path, " + albumColumn
                                 + ", " + analysisColumn + " FROM content c "
                             "LEFT JOIN artist a ON a.artist_id = c.artist_id_artist "
                             "LEFT JOIN key k ON k.key_id = c.key_id "
                             "LEFT JOIN image i ON i.image_id = c.image_id" + albumJoin);
    while (stmt.step()) {
        int64_t contentId = stmt.columnInt64(0);

        Track track;
        track.sourceId = std::to_string(contentId);
        track.format = "onelibrary";
        track.title = stmt.columnText(1);
        track.artist = stmt.columnText(2);
        // Column 12, not 11: i.path is 11. Counting the SELECT list by
        // eye put this one place early, and it read the artwork path as
        // the album -- caught by the reader test asserting the value
        // rather than merely that the query ran.
        track.album = stmt.columnText(12);
        track.bpm = static_cast<double>(stmt.columnInt64(3)) / 100.0;
        track.durationSeconds = static_cast<double>(stmt.columnInt64(4));
        std::string relPath = stmt.columnText(5);
        if (!relPath.empty()) {
            // relPath is already "/Contents/..." (leading slash) --
            // fs::path's own "/" operator would treat a leading-slash RHS
            // as an absolute replacement, not a join, so strip it first.
            //
            // make_preferred() matters on Windows specifically: relPath's
            // *own* forward slashes (e.g. "Contents/Artist/Track.mp3")
            // are appended as one path component in a single operator/
            // call, so fs::path preserves them as literal '/' characters
            // rather than re-splitting them into separate components --
            // .string() would otherwise return a path mixing Windows'
            // native backslash (from the stickRoot join) with these
            // untouched forward slashes. Found via a real Windows test
            // failure comparing this against a path built the "normal"
            // way (separate operator/ calls per component, which do get
            // the native separator throughout).
            //
            // relPath is a raw OneLibrary column, so it is exactly the
            // kind of foreign data normalizedPathKey() (path_key.cpp) is
            // documented never to trust: pathFromUtf8() decodes it as the
            // UTF-8 it is meant to be, and on Windows that decoding can
            // throw for bytes that are not valid UTF-8. One track with an
            // undecodable path must not take down the whole scan.
            try {
                track.filePath = pathToUtf8((stickRoot / pathFromUtf8(relPath.substr(1))).make_preferred());
            } catch (const std::exception &e) {
                m_progress->warn("content_id=" + track.sourceId + ": file path unreadable (" + e.what() + ")");
            }
        }
        track.filename = stmt.columnText(6);
        track.bitrate = static_cast<int>(stmt.columnInt64(7));
        track.fileSizeBytes = static_cast<std::uint64_t>(stmt.columnInt64(8));
        track.key = stmt.columnText(9);
        if (!stmt.columnIsNull(10)) {
            track.playCount = static_cast<int>(stmt.columnInt64(10));
        }
        std::string imageRelPath = stmt.columnText(11);
        if (!imageRelPath.empty()) {
            // Same stick-root-relative convention as content.path (e.g.
            // "/PIONEER/Artwork/00001/b1.jpg") -- confirmed against real
            // hardware-written data, and unlike Engine's own art (see
            // libdjinterop_engine_reader.cpp's much longer version of this
            // same fix), no URI-unwrapping needed here at all.
            // make_preferred(): same mixed-separator fix as filePath
            // above, same reason -- imageRelPath.substr(1)'s own forward
            // slashes are appended as one path component and survive
            // .string() as literal characters on Windows otherwise.
            //
            // Same undecodable-bytes risk as filePath above, and the same
            // fix: one bad artwork path must not lose the whole track.
            //
            // Not checked for existence, as in the Engine reader: whether
            // the file is still there is for the consumer that draws or
            // audits it to find out, not a stat per track in every read.
            try {
                track.artworkPath = pathToUtf8((stickRoot / pathFromUtf8(imageRelPath.substr(1))).make_preferred());
            } catch (const std::exception &e) {
                m_progress->warn("content_id=" + track.sourceId + ": artwork path unreadable (" + e.what() + ")");
            }
        }

        // Column 13. Where the track's cues are: see readAnalysis().
        track.analysisFile = stmt.columnText(13);
        auto playlistsIt = playlistsByContentId.find(contentId);
        if (playlistsIt != playlistsByContentId.end()) {
            track.playlists = playlistsIt->second;
        }

        tracks.push_back(std::move(track));
        progress.tick(++done);
        m_cancel.throwIfCancelled();
    }
    progress.finish();

    return tracks;
}

}  // namespace seabass::infrastructure::onelibrary
