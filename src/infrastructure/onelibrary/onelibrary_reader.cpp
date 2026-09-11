#include "infrastructure/onelibrary/onelibrary_reader.hpp"

#include <filesystem>
#include <stdexcept>
#include <unordered_map>

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

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

std::vector<Track> OneLibraryReader::readAll()
{
    if (!OneLibraryCueWriter::existsFor(m_pioneerRoot)) {
        throw std::runtime_error("no OneLibrary (exportLibrary.db) present for this stick");
    }
    std::string dbPath = OneLibraryCueWriter::dbPathFor(m_pioneerRoot);
    fs::path stickRoot = fs::path(m_pioneerRoot).parent_path();

    SqlCipherLibrary lib;
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
            int64_t contentId = stmt.columnInt64(0);
            std::string path = playlistPath(stmt.columnInt64(1), playlistTree);
            if (path.empty()) {
                continue;
            }
            playlistsByContentId[contentId].push_back(
                PlaylistMembership{path, static_cast<int>(stmt.columnInt64(2))});
        }
    }

    // content_id -> cues. kind=0 is a memory cue, kind=1..8 a hot cue in
    // that slot -- same encoding OneLibraryCueWriter writes (see its own
    // doc comment on the confidence level of that mapping).
    std::unordered_map<int64_t, std::vector<CuePoint>> cuesByContentId;
    {
        SqlCipherStatement stmt(db, "SELECT content_id, kind, inUsec, cueComment, isActiveLoop, outUsec FROM cue "
                                    "ORDER BY content_id");
        while (stmt.step()) {
            int64_t contentId = stmt.columnInt64(0);
            int64_t kind = stmt.columnInt64(1);
            CuePoint cue;
            cue.kind = kind == 0 ? CuePoint::Kind::Memory : CuePoint::Kind::Hot;
            cue.hotCueNumber = kind == 0 ? 0 : static_cast<int>(kind);
            cue.positionMs = static_cast<double>(stmt.columnInt64(2)) / 1000.0;
            cue.comment = stmt.columnText(3);
            // A loop is flagged and carries its out point; the writer
            // mirrors this, so loops survive a OneLibrary round trip.
            if (stmt.columnInt64(4) != 0 && stmt.columnInt64(5) > stmt.columnInt64(2)) {
                cue.isLoop = true;
                cue.loopEndMs = static_cast<double>(stmt.columnInt64(5)) / 1000.0;
            }
            // No color-lookup table exists anywhere in this schema (see
            // OneLibraryCueWriter's own doc comment) -- left empty rather
            // than fabricated, same honesty the writer already has.
            cuesByContentId[contentId].push_back(std::move(cue));
        }
    }

    size_t total = 0;
    {
        SqlCipherStatement count(db, "SELECT count(*) FROM content");
        if (count.step()) {
            total = static_cast<size_t>(count.columnInt64(0));
        }
    }
    m_progress->start("Reading OneLibrary", total);

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
    {
        SqlCipherStatement table(db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='album'");
        if (table.step() && table.columnInt64(0) > 0) {
            SqlCipherStatement columns(db, "PRAGMA table_info(content)");
            while (columns.step()) {
                if (columns.columnText(1) == "album_id") {
                    hasAlbums = true;
                    break;
                }
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
    SqlCipherStatement stmt(db,
                             "SELECT c.content_id, c.title, a.name, c.bpmx100, c.length, c.path, c.fileName, "
                             "c.bitrate, c.fileSize, k.name, c.djPlayCount, i.path, " + albumColumn
                                 + " FROM content c "
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
            // documented never to trust: on Windows, operator/ re-encodes
            // its argument through the current locale's narrow-to-wide
            // codecvt and throws std::filesystem::filesystem_error on
            // bytes that are not valid UTF-8. One track with an
            // undecodable path must not take down the whole scan.
            try {
                track.filePath = (stickRoot / relPath.substr(1)).make_preferred().string();
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
            try {
                fs::path candidate = (stickRoot / imageRelPath.substr(1)).make_preferred();
                std::error_code ec;
                if (fs::exists(candidate, ec)) {
                    track.artworkPath = candidate.string();
                }
            } catch (const std::exception &e) {
                m_progress->warn("content_id=" + track.sourceId + ": artwork path unreadable (" + e.what() + ")");
            }
        }

        auto cuesIt = cuesByContentId.find(contentId);
        if (cuesIt != cuesByContentId.end()) {
            track.cues = cuesIt->second;
        }
        auto playlistsIt = playlistsByContentId.find(contentId);
        if (playlistsIt != playlistsByContentId.end()) {
            track.playlists = playlistsIt->second;
        }

        tracks.push_back(std::move(track));
        m_progress->tick(++done);
        m_cancel.throwIfCancelled();
    }
    m_progress->finish();

    return tracks;
}

}  // namespace seabass::infrastructure::onelibrary
