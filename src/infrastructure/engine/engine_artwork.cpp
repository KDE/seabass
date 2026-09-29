// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_sqlite.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <unordered_map>

#include "application/use_cases/fill_file_sizes.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

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

// SHA-1 (FIPS 180-4), for the one place it is needed: naming a cover the
// way Engine names the images it keeps in the database.
std::array<std::uint8_t, 20> sha1(std::string_view data)
{
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string message(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    }
    const auto rotl = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto byte = [&](int k) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(message[chunk + 4 * i + k]));
            };
            w[i] = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        for (int k = 0; k < 4; ++k) {
            digest[static_cast<size_t>(4 * i + k)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * k));
        }
    }
    return digest;
}

// The text hash Engine appears to give an image it keeps in the database:
// lowercase hex of a SHA-1 of its bytes, leading zeros dropped. Inferred,
// not documented: it matches the one untouched JPEG in a 4.5.0 library,
// while the others there were re-encoded after hashing.
std::string databaseImageHash(std::string_view image)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint8_t byte : sha1(image)) {
        hex += Hex[byte >> 4];
        hex += Hex[byte & 0x0F];
    }
    const auto first = hex.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : hex.substr(first);
}

// Whether the library keeps its covers in the database: schema 3.0.1 and
// earlier do (libdjinterop's maintainer, and the schemas themselves); from
// 3.0.2 on they are files under Artwork/. A library without the version, or
// without an image column, is taken for the file kind.
bool keepsCoversInDatabase(sqlite3 *handle, bool hasImageColumn)
{
    if (!hasImageColumn) {
        return false;
    }
    sqlite3_stmt *stmt = nullptr;
    bool older = false;
    if (sqlite3_prepare_v2(handle,
                           "SELECT schemaVersionMajor, schemaVersionMinor, schemaVersionPatch FROM Information "
                           "ORDER BY id LIMIT 1;",
                           -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        const std::array<std::int64_t, 3> version = {sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1),
                                                     sqlite3_column_int64(stmt, 2)};
        older = version < std::array<std::int64_t, 3>{3, 0, 2};
    }
    sqlite3_finalize(stmt);
    return older;
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

std::string cachedArtworkFile(const std::string &artworkDirectory, std::span<const std::uint8_t> hash, bool *anyFile)
{
    const std::string name = artworkFileName(hash);
    std::error_code ec;
    for (const char *extension : {".jpg", ".jpeg", ".png"}) {
        const fs::path cached = pathFromUtf8(artworkDirectory) / pathFromUtf8(name + extension);
        if (!fs::is_regular_file(cached, ec)) {
            continue;
        }
        if (anyFile != nullptr) {
            *anyFile = true;
        }
        // There being a file is not the question a player asks. An
        // unclean unplug leaves directory entries whose data is gone: the
        // name is right, the size is zero, and Engine draws its grey
        // placeholder. Counting those as "a player can read this" is how a
        // stick reports every cover art fixed while the player shows
        // blanks, so the bytes have to say JPEG or PNG.
        if (isImageARepairCanName(cached)) {
            return pathToUtf8(cached);
        }
    }
    return {};
}

AlbumArtImageHeads::~AlbumArtImageHeads()
{
    sqlite3_blob_close(m_blob);
}

std::string AlbumArtImageHeads::of(std::int64_t albumArtId)
{
    // One handle moved from row to row: opening one per row costs a
    // statement's worth of work each time.
    const bool positioned = m_blob != nullptr
        ? sqlite3_blob_reopen(m_blob, albumArtId) == SQLITE_OK
        : sqlite3_blob_open(m_handle, "main", "AlbumArt", "albumArt", albumArtId, 0, &m_blob) == SQLITE_OK;
    if (!positioned) {
        // A failed reopen leaves the handle unusable; the next row opens
        // a fresh one.
        sqlite3_blob_close(m_blob);
        m_blob = nullptr;
        return {};
    }
    std::string head(static_cast<size_t>(std::min(sqlite3_blob_bytes(m_blob), 12)), '\0');
    if (sqlite3_blob_read(m_blob, head.data(), static_cast<int>(head.size()), 0) != SQLITE_OK) {
        head.clear();
    }
    return head;
}

std::string albumArtImageHead(sqlite3 *handle, std::int64_t albumArtId)
{
    return AlbumArtImageHeads(handle).of(albumArtId);
}

ArtworkStorage classifyArtworkReference(std::string_view reference, ReferenceType type)
{
    if (reference.empty()) {
        return ArtworkStorage::None;
    }
    if (reference.starts_with(ImportedPrefix)) {
        return ArtworkStorage::ImportedPath;
    }
    // A hash. Whether its file, or its image in the row, is there is the
    // caller's to check.
    return type == ReferenceType::Text ? ArtworkStorage::InDatabase : ArtworkStorage::Cached;
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
    // A save committing holds the database for a moment: waited out, as in
    // the other connections to it, from the first statement on.
    sqlite3_busy_timeout(handle, 5000);

    // Every Engine library this ships against has Track.path, but an
    // older or hand-made schema without it must still get its art
    // checked: without the column there is simply no rekordbox track to
    // match, not a failed scan.
    bool hasTrackPath = false;
    // And a schema whose AlbumArt has no image column keeps no images in
    // the database: its art is all files.
    bool hasImageColumn = false;
    try {
        hasTrackPath = hasColumn(handle, "Track", "path");
        hasImageColumn = hasColumn(handle, "AlbumArt", "albumArt");
    } catch (const std::exception &e) {
        audit.error = e.what();
        sqlite3_close(handle);
        return audit;
    }

    // One read transaction for the row list and the tracks below, so both
    // see the same state of a database a save may be writing.
    if (sqlite3_exec(handle, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        audit.error = std::string("could not begin reading the database: ") + sqlite3_errmsg(handle);
        sqlite3_close(handle);
        return audit;
    }

    // The first bytes of every image kept in the database, once per row
    // rather than once per track pointing at it, through one blob handle.
    std::unordered_map<std::int64_t, std::string> imageHeadByRow;
    if (hasImageColumn) {
        sqlite3_stmt *images = nullptr;
        const std::string listSql = "SELECT id FROM AlbumArt WHERE " + byteLengthSql("albumArt") + " > 0;";
        if (sqlite3_prepare_v2(handle, listSql.c_str(), -1, &images, nullptr) != SQLITE_OK) {
            audit.error = std::string("could not read the AlbumArt table: ") + sqlite3_errmsg(handle);
            sqlite3_close(handle);
            return audit;
        }
        std::vector<std::int64_t> rows;
        int step;
        while ((step = sqlite3_step(images)) == SQLITE_ROW) {
            rows.push_back(sqlite3_column_int64(images, 0));
        }
        sqlite3_finalize(images);
        if (step != SQLITE_DONE) {
            audit.error = std::string("could not read the AlbumArt table: ") + sqlite3_errmsg(handle);
            sqlite3_close(handle);
            return audit;
        }
        AlbumArtImageHeads heads(handle);
        for (const std::int64_t row : rows) {
            imageHeadByRow[row] = heads.of(row);
        }
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
        // Asked before the value is read: reading it may convert it.
        const ReferenceType referenceType =
            sqlite3_column_type(stmt, 3) == SQLITE_TEXT ? ReferenceType::Text : ReferenceType::Blob;
        const void *blob = sqlite3_column_blob(stmt, 3);
        const int size = sqlite3_column_bytes(stmt, 3);
        const bool pointsAtArt = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
        const std::int64_t albumArtId = sqlite3_column_int64(stmt, 4);

        // A row holding an image is decided by that image alone: whatever
        // its hash says, this is what a player shows, and it is never
        // re-pointed. Bytes there that are no image decide nothing when
        // the hash names the art elsewhere (a file, an imported path); a
        // text hash names the image in the row itself, so there they make
        // the row a fault, repaired in place.
        if (const auto image = pointsAtArt ? imageHeadByRow.find(albumArtId) : imageHeadByRow.end();
            image != imageHeadByRow.end()) {
            if (!extensionForImage(image->second).empty()) {
                audit.tracksWithArt++;
                audit.readableByAPlayer++;
                continue;
            }
            const std::string_view hashBytes(static_cast<const char *>(blob), blob ? static_cast<size_t>(size) : 0);
            if (isOtherImageFormat(image->second)) {
                // A real image a player may not show: reported, and left
                // alone. No repair is offered, since any would write over
                // it or leave it behind.
                audit.tracksWithArt++;
                entry.storage = ArtworkStorage::InDatabaseOtherFormat;
                entry.reference = std::string(hashBytes);
                audit.unreadable.push_back(std::move(entry));
                continue;
            }
            if (classifyArtworkReference(hashBytes, referenceType) == ArtworkStorage::InDatabase) {
                audit.tracksWithArt++;
                entry.storage = ArtworkStorage::InDatabaseUnreadable;
                entry.reference = std::string(hashBytes);
                findASourceFor(entry);
                audit.unreadable.push_back(std::move(entry));
                continue;
            }
        }
        if (blob == nullptr || size <= 0) {
            // Nothing to find the art by, which is two different things.
            //
            // Engine's way of saying "this track has no cover" is not a
            // missing albumArtId. It is a seeded row at id 1 with nothing
            // in it: Engine 2.x and 3.x write AlbumArt (1, NULL, NULL),
            // libdjinterop (1, '', NULL) (its schema_*.cpp, and the
            // firmware reference dumps under testdata/ref), and art-less
            // tracks point at it. An empty text hash with no image is the
            // same row wherever it sits. 0 names no row at all. All are "no
            // art asked for", and reporting them would put a permanent,
            // unfixable warning on every healthy library: most tracks on
            // most sticks have no cover. The seed is told by its id
            // because nothing else sets it apart; a library whose row 1
            // holds a hash or an image is decided by that content above.
            //
            // What is left -- a row of its own whose hash is NULL or an
            // empty blob -- is art asked for that nothing can resolve. The
            // committed fixture has two of those (two tracks at AlbumArt
            // 469, hash NULL), and counting them as "no art asked for"
            // left them out of every figure the page shows.
            constexpr std::int64_t SeededNoCoverRow = 1;
            if (!pointsAtArt || albumArtId == 0 || albumArtId == SeededNoCoverRow
                || referenceType == ReferenceType::Text) {
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
        entry.storage = classifyArtworkReference(reference, referenceType);
        audit.tracksWithArt++;

        if (entry.storage == ArtworkStorage::InDatabase && !hasImageColumn) {
            // A text hash on a schema whose rows cannot hold an image
            // names nothing a player can find: a row without a usable
            // hash, rebuilt like one.
            entry.storage = ArtworkStorage::RowWithoutHash;
            findASourceFor(entry);
            audit.unreadable.push_back(std::move(entry));
            continue;
        }
        if (entry.storage == ArtworkStorage::InDatabase) {
            // A text hash names the image in its own row, and that row
            // holds none.
            entry.storage = ArtworkStorage::InDatabaseUnreadable;
            entry.reference = reference;
            findASourceFor(entry);
            audit.unreadable.push_back(std::move(entry));
            continue;
        }

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
        bool anyFile = false;
        const bool readable = !cachedArtworkFile(pathToUtf8(artwork), hash, &anyFile).empty();
        const std::string name = artworkFileName(hash);
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
    sqlite3_exec(handle, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(handle);

    std::stable_partition(audit.unreadable.begin(), audit.unreadable.end(),
                          [](const ArtworkEntry &entry) { return !entry.imageOnStick.empty(); });
    return audit;
}

std::uint64_t artworkBytesOnStick(const std::vector<domain::Track> &tracks, const std::string &engineLibraryPath)
{
    // The copies the reader writes out of the database are on this
    // computer, not the stick: the images they copy are counted below.
    const std::string localCopies = pathToUtf8(paths::localEngineArtworkDir());
    std::uint64_t bytes = 0;
    std::set<std::string> counted;
    for (const auto &track : tracks) {
        if (track.artworkPath.empty() || track.artworkPath.rfind(localCopies, 0) == 0
            || !counted.insert(track.artworkPath).second) {
            continue;
        }
        bytes += application::fileSizeOnDisk(track.artworkPath).value_or(0);
    }
    if (engineLibraryPath.empty()) {
        return bytes;
    }
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(databaseFile(engineLibraryPath)).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr)
        == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        const std::string sumSql = "SELECT coalesce(sum(" + byteLengthSql("albumArt") + "), 0) FROM AlbumArt;";
        if (sqlite3_prepare_v2(handle, sumSql.c_str(), -1, &stmt, nullptr)
                == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW) {
            bytes += static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(handle);
    return bytes;
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

    bool hasImageColumn = false;
    try {
        hasImageColumn = hasColumn(handle, "AlbumArt", "albumArt");
    } catch (const std::exception &e) {
        return fail(e.what());
    }
    const std::string imageLength = hasImageColumn ? byteLengthSql("a.albumArt") : "0";
    const bool coversInDatabase = keepsCoversInDatabase(handle, hasImageColumn);

    // beforeWrite is SaveContext::protectForThisChange, which throws when
    // it cannot copy a file aside (no temporary space, say). Uncaught it
    // would unwind past an open write transaction and an open handle,
    // leaving a hot journal beside the database that the save's own
    // rollback then restores underneath a connection still holding it.
    try {
        for (const ArtworkEntry &entry : entries) {
            // The image for a track whose row it shares with others: given
            // a row of its own below, rather than written into theirs.
            std::string ownRowBytes;
            if (entry.storage == ArtworkStorage::InDatabaseUnreadable && !hasImageColumn) {
                result.noLongerInDatabase++;  // rows here cannot hold an image
                continue;
            }
            if (entry.storage == ArtworkStorage::InDatabaseUnreadable) {
                // Repaired in place: the image goes into the row the track
                // already points at, whose hash and id stay, so every track
                // sharing the row keeps it. Asked of the row as it is now.
                sqlite3_stmt *current = nullptr;
                if (sqlite3_prepare_v2(handle,
                                       "SELECT a.id, typeof(a.hash) = 'text' AND a.hash != '' "
                                       "AND substr(a.hash, 1, 8) != 'image://' "
                                       "FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId WHERE t.id = ?;",
                                       -1, &current, nullptr)
                    != SQLITE_OK) {
                    return fail(std::string("could not read the track's art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_int64(current, 1, entry.trackId);
                if (sqlite3_step(current) != SQLITE_ROW) {
                    sqlite3_finalize(current);
                    result.tracksNoLongerThere++;
                    continue;
                }
                const std::int64_t row = sqlite3_column_int64(current, 0);
                const bool storedInDatabase = sqlite3_column_int(current, 1) != 0;
                sqlite3_finalize(current);
                const std::string headBytes = albumArtImageHead(handle, row);
                if (isOtherImageFormat(headBytes)) {
                    result.keptInDatabase++;  // a real image, in a format of its own
                    continue;
                }
                if (!extensionForImage(headBytes).empty()) {
                    // Never overwritten: whatever put it there, it reads.
                    result.alreadyReadable++;
                    continue;
                }
                if (!storedInDatabase) {
                    result.noLongerInDatabase++;
                    continue;
                }
                std::string bytes;
                if (!entry.imageOnStick.empty()) {
                    bytes = readWholeFile(entry.imageOnStick);
                } else if (entry.otherSource && readOtherSource) {
                    bytes = readOtherSource(entry);
                }
                if (bytes.empty()) {
                    continue;
                }
                if (extensionForImage(bytes).empty()) {
                    result.notAnImage++;
                    continue;
                }
                // In place only for a track alone on its row: written into
                // a row other tracks share, one track's cover would become
                // theirs, and nothing here knows theirs is the same.
                sqlite3_stmt *sharing = nullptr;
                if (sqlite3_prepare_v2(handle, "SELECT count(*) FROM Track WHERE albumArtId = ?;", -1, &sharing,
                                       nullptr)
                    != SQLITE_OK) {
                    return fail(std::string("could not count the tracks on an art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_int64(sharing, 1, row);
                const bool shared = sqlite3_step(sharing) == SQLITE_ROW && sqlite3_column_int64(sharing, 0) > 1;
                sqlite3_finalize(sharing);
                if (shared) {
                    ownRowBytes = std::move(bytes);
                } else {
                    sqlite3_stmt *write = nullptr;
                    if (sqlite3_prepare_v2(handle, "UPDATE AlbumArt SET albumArt = ? WHERE id = ?;", -1, &write,
                                           nullptr)
                        != SQLITE_OK) {
                        return fail(std::string("could not write the image into its row: ") + sqlite3_errmsg(handle));
                    }
                    sqlite3_bind_blob(write, 1, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
                    sqlite3_bind_int64(write, 2, row);
                    if (sqlite3_step(write) != SQLITE_DONE) {
                        sqlite3_finalize(write);
                        return fail(std::string("could not write the image into its row: ") + sqlite3_errmsg(handle));
                    }
                    sqlite3_finalize(write);
                    result.repaired++;
                    continue;
                }
            }
            // Asked of the database as it is now, not of the audit: a
            // track whose row holds a readable image, or names one by a
            // text hash, keeps its art in the database and is never
            // re-pointed. The new row would hold no image, and the old one
            // would be left to nobody. Bytes that are no image stay where
            // they are, in a row nothing points at any more.
            bool keepsImageInDatabase =
                entry.storage == ArtworkStorage::InDatabase || entry.storage == ArtworkStorage::InDatabaseOtherFormat;
            if (!keepsImageInDatabase && ownRowBytes.empty()) {
                sqlite3_stmt *current = nullptr;
                // Without the image column a text hash keeps nothing here.
                const std::string textHash = hasImageColumn
                    ? "typeof(a.hash) = 'text' AND a.hash != '' AND substr(a.hash, 1, 8) != 'image://'"
                    : "0";
                const std::string sql = "SELECT a.id, " + imageLength + " > 0, " + textHash
                    + " FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId WHERE t.id = ?;";
                if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &current, nullptr)
                    != SQLITE_OK) {
                    return fail(std::string("could not read the track's art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_int64(current, 1, entry.trackId);
                if (sqlite3_step(current) == SQLITE_ROW) {
                    const std::int64_t row = sqlite3_column_int64(current, 0);
                    const bool holdsBytes = sqlite3_column_int(current, 1) != 0;
                    keepsImageInDatabase = sqlite3_column_int(current, 2) != 0
                        || (holdsBytes && [&] {
                               const std::string head = albumArtImageHead(handle, row);
                               return !extensionForImage(head).empty() || isOtherImageFormat(head);
                           }());
                }
                sqlite3_finalize(current);
            }
            if (keepsImageInDatabase) {
                result.keptInDatabase++;
                continue;
            }
            std::string bytes = std::move(ownRowBytes);
            if (bytes.empty() && !entry.imageOnStick.empty()) {
                bytes = readWholeFile(entry.imageOnStick);
            } else if (bytes.empty() && entry.otherSource && readOtherSource) {
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
            std::int64_t albumArtId = 0;
            if (coversInDatabase) {
                // The library's own kind of row: a text hash and the image
                // in it, found again by that hash when a second track has
                // the same cover.
                const std::string textHash = databaseImageHash(bytes);
                sqlite3_stmt *find = nullptr;
                if (sqlite3_prepare_v2(handle,
                                       "SELECT id FROM AlbumArt WHERE typeof(hash) = 'text' AND hash = ? AND "
                                       "albumArt = ?;",
                                       -1, &find, nullptr)
                    != SQLITE_OK) {
                    return fail(std::string("could not look up the art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_text(find, 1, textHash.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_blob(find, 2, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(find) == SQLITE_ROW) {
                    albumArtId = sqlite3_column_int64(find, 0);
                }
                sqlite3_finalize(find);
                if (albumArtId == 0) {
                    sqlite3_stmt *insert = nullptr;
                    if (sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (hash, albumArt) VALUES (?, ?);", -1, &insert,
                                           nullptr)
                        != SQLITE_OK) {
                        return fail(std::string("could not add the art row: ") + sqlite3_errmsg(handle));
                    }
                    sqlite3_bind_text(insert, 1, textHash.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_blob(insert, 2, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT);
                    if (sqlite3_step(insert) != SQLITE_DONE) {
                        sqlite3_finalize(insert);
                        return fail(std::string("could not add the art row: ") + sqlite3_errmsg(handle));
                    }
                    sqlite3_finalize(insert);
                    albumArtId = sqlite3_last_insert_rowid(handle);
                }
            } else {
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

                sqlite3_stmt *find = nullptr;
                if (sqlite3_prepare_v2(handle, "SELECT id FROM AlbumArt WHERE hash = ?;", -1, &find, nullptr)
                    != SQLITE_OK) {
                    return fail(std::string("could not look up the art row: ") + sqlite3_errmsg(handle));
                }
                sqlite3_bind_blob(find, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
                if (sqlite3_step(find) == SQLITE_ROW) {
                    albumArtId = sqlite3_column_int64(find, 0);
                }
                sqlite3_finalize(find);

                if (albumArtId == 0) {
                    sqlite3_stmt *insert = nullptr;
                    if (sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (hash) VALUES (?);", -1, &insert,
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
