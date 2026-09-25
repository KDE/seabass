// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/hashing/sha256.hpp"

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

constexpr std::string_view ImportedPrefix = "image://";
// The part of an imported reference that is the same on every machine --
// in both spellings, because the reference is the path as the *importing*
// computer wrote it, and Engine DJ on Windows writes
// "...\PIONEER\Artwork\00001\a5_m.jpg". Looking for the forward-slash
// form alone would have found nothing on a stick imported there: every
// track unrepairable, and the page telling the user "none of their images
// are on this stick" about a stick where all of them are.
constexpr std::string_view StickTail = "PIONEER/Artwork";
constexpr std::string_view StickTailWindows = "PIONEER\\Artwork";

fs::path databaseFile(const std::string &engineLibraryPath)
{
    return pathFromUtf8(engineLibraryPath) / "Database2" / "m.db";
}

fs::path artworkDirectory(const std::string &engineLibraryPath)
{
    return pathFromUtf8(engineLibraryPath) / "Artwork";
}

// The first bytes only: the audit asks this of every imported entry, and
// a full read of a thousand JPEGs to answer it would be a scan of its own.
bool isImageARepairCanName(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::array<char, 8> head{};
    in.read(head.data(), head.size());
    return !extensionForImage(std::string_view(head.data(), static_cast<size_t>(in.gcount()))).empty();
}

std::string readWholeFile(const fs::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

std::string artworkSourceKey(const std::string &trackFile)
{
    std::error_code ec;
    const fs::path file = pathFromUtf8(trackFile);
    const fs::path resolved = fs::weakly_canonical(file, ec);
    std::string key = pathToUtf8(ec ? file : resolved);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

int ArtworkAudit::repairable() const
{
    return static_cast<int>(std::count_if(unreadable.begin(), unreadable.end(), [](const ArtworkEntry &entry) {
        return !entry.imageOnStick.empty() || entry.otherSource;
    }));
}

std::string artworkFileName(std::span<const std::uint8_t> hash)
{
    static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((hash.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < hash.size(); i += 3) {
        const std::uint32_t triple = (hash[i] << 16) | (hash[i + 1] << 8) | hash[i + 2];
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
        out += Alphabet[(triple >> 6) & 0x3F];
        out += Alphabet[triple & 0x3F];
    }
    if (i + 1 == hash.size()) {
        const std::uint32_t triple = hash[i] << 16;
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
    } else if (i + 2 == hash.size()) {
        const std::uint32_t triple = (hash[i] << 16) | (hash[i + 1] << 8);
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
        out += Alphabet[(triple >> 6) & 0x3F];
    }
    return out;
}

ArtworkStorage classifyArtworkReference(std::string_view reference)
{
    if (reference.empty()) {
        return ArtworkStorage::None;
    }
    if (reference.starts_with(ImportedPrefix)) {
        return ArtworkStorage::ImportedPath;
    }
    // A hash. Whether its file is there is the caller's to check.
    return ArtworkStorage::Cached;
}

std::string imageOnStickFor(std::string_view reference, const std::string &stickRoot)
{
    bool windowsSpelling = false;
    auto at = reference.find(StickTail);
    if (at == std::string_view::npos) {
        at = reference.find(StickTailWindows);
        windowsSpelling = at != std::string_view::npos;
    }
    if (at == std::string_view::npos) {
        return {};
    }
    std::string tail(reference.substr(at));
    if (windowsSpelling) {
        // Only for the spelling that matched with backslashes. A backslash
        // inside a forward-slash reference is part of a file name -- legal
        // on Linux and on exFAT mounted there -- and turning it into a
        // separator would look up a file that does not exist and report the
        // track unrepairable.
        std::replace(tail.begin(), tail.end(), '\\', '/');
    }
    // One appended component, so the forward slashes inside `tail` stay
    // literal rather than being re-split -- the same care the Engine
    // reader's own artwork resolution takes, for the same Windows reason.
    //
    // And caught, for that same reason: `tail` is raw bytes out of a
    // database column, and on Windows pathFromUtf8() decodes them to
    // wide characters and can throw when they are not valid UTF-8.
    // libdjinterop_engine_reader.cpp catches the same throw per row,
    // after a real Windows run did exactly that. Uncaught here it would
    // leave runScanTask's outer handler to turn one unreadable artwork
    // path into "the Engine scan failed", costing the user every missing
    // file, junk cue and playlist tally for the format.
    try {
        return pathToUtf8((pathFromUtf8(stickRoot) / pathFromUtf8(tail)).make_preferred());
    } catch (const std::exception &) {
        return {};
    }
}

ArtworkAudit auditArtwork(const std::string &engineLibraryPath, const ArtworkSourceByTrackFile &sources,
                          const ArtworkSourceProbe &hasOtherSource, const application::CancellationToken &cancel)
{
    ArtworkAudit audit;
    const fs::path db = databaseFile(engineLibraryPath);
    const fs::path artwork = artworkDirectory(engineLibraryPath);
    const std::string stickRoot = pathToUtf8(pathFromUtf8(engineLibraryPath).parent_path());

    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        audit.error = "could not read " + pathToUtf8(db);
        if (handle) {
            sqlite3_close(handle);
        }
        return audit;
    }

    // Every Engine library this ships against has Track.path, but an
    // older or hand-made schema without it must still get its art
    // checked: without the column there is simply no rekordbox track to
    // match, not a failed scan.
    bool hasTrackPath = false;
    {
        sqlite3_stmt *columns = nullptr;
        if (sqlite3_prepare_v2(handle, "PRAGMA table_info(Track);", -1, &columns, nullptr) == SQLITE_OK) {
            while (sqlite3_step(columns) == SQLITE_ROW) {
                const unsigned char *column = sqlite3_column_text(columns, 1);
                if (column != nullptr && std::string(reinterpret_cast<const char *>(column)) == "path") {
                    hasTrackPath = true;
                    break;
                }
            }
        }
        sqlite3_finalize(columns);
    }

    sqlite3_stmt *stmt = nullptr;
    const std::string sqlText =
        std::string("SELECT t.id, t.title, t.artist, a.hash, t.albumArtId, ")
        + (hasTrackPath ? "t.path" : "NULL")
        + " FROM Track t LEFT JOIN AlbumArt a ON a.id = t.albumArtId ORDER BY t.id";
    const char *sql = sqlText.c_str();
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        audit.error = std::string("could not read the Track table: ") + sqlite3_errmsg(handle);
        sqlite3_close(handle);
        return audit;
    }

    // Where a fault's image can come back from, in the order of how
    // close the copy is to the track: the rekordbox catalog on this same
    // stick first, then the audio file's own tags -- which is where both
    // libraries took their copies from in the first place.
    const auto findASourceFor = [&sources, &hasOtherSource](ArtworkEntry &entry) {
        if (!sources.empty() && !entry.trackFile.empty()) {
            const auto found = sources.find(artworkSourceKey(entry.trackFile));
            if (found != sources.end() && isImageARepairCanName(pathFromUtf8(found->second))) {
                entry.imageOnStick = found->second;
                return;
            }
        }
        if (hasOtherSource) {
            entry.otherSource = hasOtherSource(entry);
        }
    };

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        // Per row: the page this runs for may be gone, and whoever left it
        // waits for this to stop before the stick is anyone else's.
        if (cancel.cancelled()) {
            sqlite3_finalize(stmt);
            sqlite3_close(handle);
            throw application::OperationCancelled();
        }
        ArtworkEntry entry;
        entry.trackId = sqlite3_column_int64(stmt, 0);
        if (const unsigned char *title = sqlite3_column_text(stmt, 1)) {
            entry.title = reinterpret_cast<const char *>(title);
        }
        if (const unsigned char *artist = sqlite3_column_text(stmt, 2)) {
            entry.artist = reinterpret_cast<const char *>(artist);
        }
        if (const unsigned char *trackPath = sqlite3_column_text(stmt, 5)) {
            // Engine stores it relative to the library directory
            // ("../Contents/..."), which is where the rekordbox catalog's
            // own absolute paths meet it.
            std::error_code pathEc;
            const fs::path absolute = fs::weakly_canonical(
                pathFromUtf8(engineLibraryPath) / pathFromUtf8(reinterpret_cast<const char *>(trackPath)), pathEc);
            entry.trackFile = pathEc ? std::string() : pathToUtf8(absolute);
        }
        const void *blob = sqlite3_column_blob(stmt, 3);
        const int size = sqlite3_column_bytes(stmt, 3);
        if (blob == nullptr || size <= 0) {
            // Nothing to find the art by, which is two different things.
            //
            // Engine's way of saying "this track has no cover" is not a
            // missing albumArtId. It is a row: libdjinterop seeds
            // AlbumArt (1, '', NULL) in every schema it writes
            // (schema_1_18_0_os.cpp and its siblings) and points art-less
            // tracks at it -- ALBUM_ART_ID_NONE in
            // djinterop/engine/v3/track_table.hpp. 0 is the other
            // spelling, which this project's own Engine reader already
            // excludes. Both are "no art asked for", and reporting them
            // would put a permanent, unfixable warning on every healthy
            // library: most tracks on most sticks have no cover.
            //
            // What is left -- an albumArtId of its own, pointing at a row
            // whose hash is NULL or empty -- is art asked for that nothing
            // can resolve. The committed fixture has two of those (two
            // tracks at AlbumArt 469, hash NULL, on a library whose row 1
            // carries a real 65-byte reference, so it has no sentinel),
            // and counting them as "no art asked for" left them out of
            // every figure the page shows.
            constexpr std::int64_t AlbumArtIdNone = 1;
            const std::int64_t albumArtId = sqlite3_column_int64(stmt, 4);
            if (sqlite3_column_type(stmt, 4) == SQLITE_NULL || albumArtId == 0 || albumArtId == AlbumArtIdNone) {
                continue;
            }
            entry.storage = ArtworkStorage::RowWithoutHash;
            audit.tracksWithArt++;
            // There is nothing to look the image up by, but there is
            // still a track, and a track has a file: the art can be
            // rebuilt from the same places as any other fault, and the
            // repair writes the row it never had.
            findASourceFor(entry);
            audit.unreadable.push_back(std::move(entry));
            continue;
        }
        const std::string reference(static_cast<const char *>(blob), static_cast<size_t>(size));
        entry.storage = classifyArtworkReference(reference);
        audit.tracksWithArt++;

        if (entry.storage == ArtworkStorage::ImportedPath) {
            entry.reference = reference;
            entry.imageOnStick = imageOnStickFor(reference, stickRoot);
            std::error_code ec;
            if (!entry.imageOnStick.empty() && !fs::is_regular_file(pathFromUtf8(entry.imageOnStick), ec)) {
                entry.imageOnStick.clear();
            }
            // And a file that is there but is not an image a repair can
            // name (the written file's extension says JPEG or PNG, which
            // is all a player has to go on) is not repairable either. Told
            // here rather than at save time, so the count the page shows
            // and the button it offers are what a repair will actually do,
            // and so one odd file cannot stop a save of a thousand others.
            if (!entry.imageOnStick.empty() && !isImageARepairCanName(pathFromUtf8(entry.imageOnStick))) {
                entry.imageOnStick.clear();
            }
            audit.unreadable.push_back(std::move(entry));
            continue;
        }

        const std::span<const std::uint8_t> hash(static_cast<const std::uint8_t *>(blob), static_cast<size_t>(size));
        const std::string name = artworkFileName(hash);
        std::error_code ec;
        bool readable = false;
        bool anyFile = false;
        for (const char *extension : {".jpg", ".jpeg", ".png"}) {
            const fs::path cached = artwork / pathFromUtf8(name + extension);
            if (!fs::is_regular_file(cached, ec)) {
                continue;
            }
            anyFile = true;
            // There being a file is not the question a player asks. An
            // unclean unplug leaves directory entries whose data is gone:
            // the name is right, the size is zero, and Engine draws its
            // grey placeholder. Counting those as "a player can read this"
            // is how a stick reports every cover art fixed while the
            // player shows blanks -- so the bytes have to say JPEG or PNG,
            // which is all isImageARepairCanName asks.
            if (isImageARepairCanName(cached)) {
                readable = true;
                break;
            }
        }
        if (readable) {
            audit.readableByAPlayer++;
        } else {
            entry.storage = anyFile ? ArtworkStorage::CachedFileUnreadable : ArtworkStorage::CachedFileMissing;
            entry.reference = name;
            findASourceFor(entry);
            audit.unreadable.push_back(std::move(entry));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);

    std::stable_partition(audit.unreadable.begin(), audit.unreadable.end(),
                          [](const ArtworkEntry &entry) { return !entry.imageOnStick.empty(); });
    return audit;
}

ArtworkRepair repairArtwork(const std::string &engineLibraryPath, const std::vector<ArtworkEntry> &entries,
                            const std::function<void(const std::string &)> &beforeWrite,
                            const std::string &databaseFileOverride,
                            const ArtworkSourceReader &readOtherSource)
{
    ArtworkRepair result;
    const fs::path db = databaseFileOverride.empty() ? databaseFile(engineLibraryPath) : pathFromUtf8(databaseFileOverride);
    const fs::path artwork = artworkDirectory(engineLibraryPath);

    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        result.error = "could not open " + pathToUtf8(db);
        if (handle) {
            sqlite3_close(handle);
        }
        return result;
    }
    // BEGIN is deferred, so the lock is taken at the first INSERT. Without
    // this, another connection holding the database for a moment (the
    // reader the same scan just used, a libdjinterop writer in the same
    // save) comes back as "database is locked" at once and fails the whole
    // save, rather than waiting the moment out.
    sqlite3_busy_timeout(handle, 5000);
    std::error_code ec;
    fs::create_directories(artwork, ec);
    // Checked, like the COMMIT below: without a transaction every INSERT
    // and UPDATE autocommits, ROLLBACK does nothing, and a repair that
    // fails halfway leaves rows behind while reporting that it wrote
    // none -- the opposite of what this function promises.
    if (sqlite3_exec(handle, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        result.error = std::string("could not begin the art repair: ") + sqlite3_errmsg(handle);
        sqlite3_close(handle);
        return result;
    }

    auto fail = [&](const std::string &message) {
        sqlite3_exec(handle, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(handle);
        result.error = message;
        result.repaired = 0;
        return result;
    };

    // beforeWrite is SaveContext::protectForThisChange, which throws when
    // it cannot copy a file aside (no temporary space, say). Uncaught it
    // would unwind past an open write transaction and an open handle,
    // leaving a hot journal beside the database that the save's own
    // rollback then restores underneath a connection still holding it.
    try {
        for (const ArtworkEntry &entry : entries) {
            std::string bytes;
            if (!entry.imageOnStick.empty()) {
                bytes = readWholeFile(entry.imageOnStick);
            } else if (entry.otherSource && readOtherSource) {
                bytes = readOtherSource(entry);
            }
            if (bytes.empty()) {
                continue;  // no source, or an unreadable one: a cover is not worth failing the save
            }
            const std::string extension = extensionForImage(bytes);
            if (extension.empty()) {
                result.notAnImage++;
                continue;  // neither JPEG nor PNG: nothing a player is promised to read
            }
            const auto full = hashing::Sha256::of(std::as_bytes(std::span(bytes)));
            // 20 bytes, the width Engine's own rows use.
            const std::span<const std::uint8_t> hash(full.data(), 20);
            const std::string name = artworkFileName(hash);
            const fs::path destination = artwork / pathFromUtf8(name + extension);
            // Not "is it there" but "is it an image": an empty file at
            // the right name is exactly what this repair exists to fix,
            // and skipping it because something is there would write the
            // row, report success, and leave the player showing nothing.
            if (!fs::is_regular_file(destination, ec) || !isImageARepairCanName(destination)) {
                if (beforeWrite) {
                    beforeWrite(pathToUtf8(destination));
                }
                // Durably, like every other write onto a stick: a bare
                // ofstream leaves the bytes in the write-back cache, and a
                // stick pulled after the save said Done would leave a
                // truncated file sitting at exactly the name the audit
                // looks for. The track would then count as readable from
                // then on, the page would call the library healthy, the
                // player would show a broken cover, and no rescan could
                // ever surface it -- worse than not having copied it.
                const std::string destinationUtf8 = pathToUtf8(destination);
                if (!writeFileDurablyAtomic(destinationUtf8, bytes)) {
                    return fail("could not write " + destinationUtf8);
                }
                result.filesWritten.push_back(destinationUtf8);
            }

            std::int64_t albumArtId = 0;
            sqlite3_stmt *find = nullptr;
            if (sqlite3_prepare_v2(handle, "SELECT id FROM AlbumArt WHERE hash = ?;", -1, &find, nullptr) != SQLITE_OK) {
                return fail(std::string("could not look up the art row: ") + sqlite3_errmsg(handle));
            }
            sqlite3_bind_blob(find, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
            if (sqlite3_step(find) == SQLITE_ROW) {
                albumArtId = sqlite3_column_int64(find, 0);
            }
            sqlite3_finalize(find);

            if (albumArtId == 0) {
                sqlite3_stmt *insert = nullptr;
                if (sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (hash, albumArt) VALUES (?, NULL);", -1, &insert,
                                       nullptr) != SQLITE_OK) {
                    return fail(std::string("could not add the art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_blob(insert, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(insert) != SQLITE_DONE) {
                    sqlite3_finalize(insert);
                    return fail(std::string("could not add the art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_finalize(insert);
                albumArtId = sqlite3_last_insert_rowid(handle);
            }

            sqlite3_stmt *point = nullptr;
            if (sqlite3_prepare_v2(handle, "UPDATE Track SET albumArtId = ? WHERE id = ?;", -1, &point, nullptr)
                != SQLITE_OK) {
                return fail(std::string("could not point the track at its art: ") + sqlite3_errmsg(handle));
            }
            sqlite3_bind_int64(point, 1, albumArtId);
            sqlite3_bind_int64(point, 2, entry.trackId);
            if (sqlite3_step(point) != SQLITE_DONE) {
                sqlite3_finalize(point);
                return fail(std::string("could not point the track at its art: ") + sqlite3_errmsg(handle));
            }
            sqlite3_finalize(point);
            if (sqlite3_changes(handle) > 0) {
                result.repaired++;
            } else {
                // The image is in the library and the AlbumArt row is
                // written, but the track itself went between the audit and
                // the save. Counted apart, because gating `repaired` on
                // this alone let a repair that had done all its work report
                // that none of the images could be read.
                result.tracksNoLongerThere++;
            }
        }
    } catch (const std::exception &e) {
        return fail(std::string("could not set aside a file before writing it: ") + e.what());
    }

    if (sqlite3_exec(handle, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(std::string("could not commit the art repair: ") + sqlite3_errmsg(handle));
    }
    sqlite3_close(handle);
    return result;
}

}  // namespace seabass::infrastructure::engine
