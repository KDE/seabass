// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: the one catalog a clone onto a stick with another label is
// meant to change, and proof that it changed nothing else.
//
// A clone (or a restore) onto a stick called something else makes the
// Engine library's covers label-proof as its last write (#101): every
// track on a player's link to another stick's name gets its image copied
// into the library, and a link whose image is not on the stick is renamed
// to the target's name. So the target's m.db is not the archive's byte for
// byte, and must not be: rig round 12 failed C1 for comparing it as if it
// were. What it must be instead, checked here row by row on copies of
// both databases (never on the stick's own file):
//
// - the app itself vouches for it: the record the restore left on the
//   target names the archive's database, the target's label, and the
//   fingerprint and content hash the target's still has, which is what
//   makes the stick list call the target current;
// - the same schema, and every table but AlbumArt the same, row for row
//   (as multisets: a duplicate row lost is a difference).
//   Track too, but for albumArtId on the tracks whose cover was copied in,
//   and lastEditTime on exactly those rows (Engine's own trigger stamps it
//   when albumArtId changes); sqlite_sequence but for AlbumArt's counter;
// - every AlbumArt row of the archive still there under its id: unchanged,
//   or a link to another stick's name renamed to the target's, with the
//   rest of the link as it was; every new row a stored cover (a hash whose
//   image file is under Engine Library/Artwork, or the image in the row),
//   the image the one its hash names, not empty, and on some track;
// - every track whose cover changed was on a link to another stick's name
//   and is now on such a stored cover;
// - and no track's cover is a link naming any stick but the target.

#pragma once

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/hashing/sha1.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/restored_databases.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::rig
{

namespace clone_covers_detail
{

constexpr std::string_view LinkPrefix = "image://fileart//media/";

// The stick a player's link names, or nothing when `hash` is not one.
inline std::optional<std::string> linkLabel(std::string_view hash)
{
    if (hash.substr(0, LinkPrefix.size()) != LinkPrefix) {
        return std::nullopt;
    }
    const std::string_view rest = hash.substr(LinkPrefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) {
        return std::nullopt;
    }
    return std::string(rest.substr(0, slash));
}

inline bool namesAnotherStick(std::string_view hash, const std::string &label)
{
    const std::optional<std::string> named = linkLabel(hash);
    return named && !infrastructure::engine::sameStickLabel(*named, label);
}

// A column's bytes and storage class, so a blob and a text of the same
// bytes are told apart.
struct Value
{
    int type = SQLITE_NULL;
    std::string bytes;
    bool operator==(const Value &) const = default;
};

inline Value column(sqlite3_stmt *stmt, int i)
{
    Value v;
    v.type = sqlite3_column_type(stmt, i);
    if (v.type == SQLITE_BLOB || v.type == SQLITE_TEXT) {
        const void *bytes = sqlite3_column_blob(stmt, i);
        if (bytes != nullptr) {
            v.bytes.assign(static_cast<const char *>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(stmt, i)));
        }
    } else if (v.type != SQLITE_NULL) {
        const unsigned char *text = sqlite3_column_text(stmt, i);
        v.bytes = text ? reinterpret_cast<const char *>(text) : "";
    }
    return v;
}

class Db
{
public:
    explicit Db(const std::filesystem::path &file)
    {
        if (sqlite3_open_v2(pathToUtf8(file).c_str(), &m_handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            m_error = m_handle ? sqlite3_errmsg(m_handle) : "cannot open";
        }
    }
    ~Db() { sqlite3_close(m_handle); }
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;
    bool ok() const { return m_error.empty(); }
    const std::string &error() const { return m_error; }
    sqlite3 *handle() const { return m_handle; }

    // Each row of `sql`, its columns handed to `row`; false (and the error
    // kept) when the statement fails.
    template<typename Row>
    bool each(const std::string &sql, Row row)
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            m_error = std::string(sqlite3_errmsg(m_handle)) + " in: " + sql;
            return false;
        }
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            row(stmt);
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE) {
            m_error = std::string(sqlite3_errmsg(m_handle)) + " in: " + sql;
            return false;
        }
        return true;
    }

    std::int64_t count(const std::string &sql)
    {
        std::int64_t n = -1;
        if (!each(sql, [&n](sqlite3_stmt *stmt) { n = sqlite3_column_int64(stmt, 0); })) {
            return -1;
        }
        return n;
    }

private:
    sqlite3 *m_handle = nullptr;
    std::string m_error;
};

inline std::string quoted(const std::string &name)
{
    std::string out = "\"";
    for (char c : name) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

inline std::string readWhole(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return in.bad() ? std::string() : bytes;
}

// The text hash Engine gives an image it keeps in the database, as the
// clone's repair writes it (engine_artwork.cpp's databaseImageHash):
// lowercase hex of a SHA-1 of the bytes, leading zeros dropped.
inline std::string databaseImageHash(std::string_view image)
{
    const std::string hex = infrastructure::hashing::toHex(infrastructure::hashing::sha1(image));
    const auto first = hex.find_first_not_of('0');
    return first == std::string::npos ? std::string("0") : hex.substr(first);
}

// A stored cover: a 20-byte hash whose image is under the library's
// Artwork folder, a JPEG or PNG and not empty, or a text hash with the
// image in the row. `checkContent`, for a row the clone wrote: the hash
// is the image's, as the repair names it (the file's SHA-256, its first
// 20 bytes; the row's image's SHA-1 for a text hash), so a cover cut
// short or swapped for another image does not pass for one.
inline bool isStoredCover(const Value &hash, const Value &image, const std::filesystem::path &artworkDir,
                          bool checkContent)
{
    if (hash.type == SQLITE_BLOB && hash.bytes.size() == 20) {
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(hash.bytes.data());
        const std::string file = infrastructure::engine::cachedArtworkFile(pathToUtf8(artworkDir), std::span(bytes, 20));
        if (file.empty()) {
            return false;
        }
        const std::string content = readWhole(pathFromUtf8(file));
        if (content.empty()) {
            return false;
        }
        if (!checkContent) {
            return true;
        }
        const infrastructure::hashing::Sha256Digest digest =
            infrastructure::hashing::Sha256::of(std::as_bytes(std::span(content)));
        return std::equal(digest.begin(), digest.begin() + 20, bytes);
    }
    if (hash.type != SQLITE_TEXT || linkLabel(hash.bytes) || image.type != SQLITE_BLOB || image.bytes.empty()) {
        return false;
    }
    return !checkContent || databaseImageHash(image.bytes) == hash.bytes;
}

}  // namespace clone_covers_detail

// The check above for the database at `row.path` on the stick at `root`
// called `label`. Prints one line for it and returns whether it holds.
inline bool checkLabelProofEngineDatabase(const infrastructure::stick_backup::BackupManifest &manifest,
                                          const infrastructure::stick_backup::ManifestRow &row,
                                          const infrastructure::stick_backup::Zip64Reader &reader,
                                          const std::filesystem::path &root, const std::string &label)
{
    using namespace clone_covers_detail;
    namespace fs = std::filesystem;
    namespace sb = infrastructure::stick_backup;
    const auto fail = [&row](const std::string &why) {
        std::cout << "  DIFFERS " << row.path << ": " << why << "\n";
        return false;
    };
    if (row.extra.empty()) {
        return fail("the archive recorded no fingerprint for it");
    }
    const fs::path onStick = root / pathFromUtf8(row.path);
    const std::optional<sb::DbSetFingerprint> live = sb::fingerprintDbSet(onStick);
    if (!live) {
        return fail("not a database the app can fingerprint");
    }
    // The app's own judgement first: what the stick list and the restore
    // planner go by.
    if (!sb::holdsBackupDatabase(sb::vouchedRestoredDatabases(root, label), row.path, row.extra, live->toHex())) {
        return fail("the target's record of the clone does not vouch for it (the stick list reads it as changed)");
    }

    // Copies of both sets, the stick's and the archive's, in a scratch
    // folder: no SQLite connection on the stick's own files.
    const fs::path scratch =
        fs::temp_directory_path()
        / ("seabass-rig-clone-covers-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "archive", ec);
    fs::create_directories(scratch / "target", ec);
    struct Cleanup
    {
        fs::path dir;
        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(dir, ignored);
        }
    } cleanup{scratch};
    const std::string name = pathToUtf8(onStick.filename());
    for (const fs::path &member : sb::dbSetMembers(onStick)) {
        fs::copy_file(member, scratch / "target" / member.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return fail("could not copy " + pathToUtf8(member) + ": " + ec.message());
        }
    }
    for (const std::string suffix : {"", "-wal", "-journal"}) {
        const std::optional<std::size_t> index = reader.findEntry(row.path + suffix);
        if (!index) {
            continue;
        }
        std::ofstream out(scratch / "archive" / pathFromUtf8(name + suffix), std::ios::binary);
        const std::string bytes = reader.readEntryToString(*index);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            return fail("could not extract " + row.path + suffix + " from the archive");
        }
    }

    Db db(scratch / "target" / pathFromUtf8(name));
    if (!db.ok()) {
        return fail("cannot open a copy: " + db.error());
    }
    const std::string attach = "ATTACH DATABASE '" + pathToUtf8(scratch / "archive" / pathFromUtf8(name)) + "' AS a;";
    if (sqlite3_exec(db.handle(), attach.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) {
        return fail(std::string("cannot open the archive's copy: ") + sqlite3_errmsg(db.handle()));
    }

    const std::int64_t schemaDiffers =
        db.count("SELECT count(*) FROM (SELECT type, name, tbl_name, sql FROM main.sqlite_master EXCEPT "
                 "SELECT type, name, tbl_name, sql FROM a.sqlite_master) ")
        + db.count("SELECT count(*) FROM (SELECT type, name, tbl_name, sql FROM a.sqlite_master EXCEPT "
                   "SELECT type, name, tbl_name, sql FROM main.sqlite_master)");
    if (schemaDiffers != 0) {
        return fail(schemaDiffers < 0 ? db.error() : "the schema differs");
    }

    // Every table but AlbumArt, row for row, in both directions.
    std::vector<std::string> tables;
    if (!db.each("SELECT name FROM a.sqlite_master WHERE type = 'table' ORDER BY name;",
                 [&tables](sqlite3_stmt *stmt) { tables.push_back(column(stmt, 0).bytes); })) {
        return fail(db.error());
    }
    bool hasTrackAndArt = false;
    for (const std::string &table : tables) {
        if (table == "AlbumArt") {
            hasTrackAndArt = true;
            continue;
        }
        std::string columns;
        std::string where;
        if (!db.each("PRAGMA a.table_info(" + quoted(table) + ");", [&](sqlite3_stmt *stmt) {
                const std::string col = column(stmt, 1).bytes;
                if (table == "Track" && (col == "albumArtId" || col == "lastEditTime")) {
                    return;
                }
                columns += (columns.empty() ? "" : ", ") + quoted(col);
            })) {
            return fail(db.error());
        }
        if (table == "sqlite_sequence") {
            where = " WHERE name <> 'AlbumArt'";
        }
        // As multisets, not sets: each distinct row with how many times it
        // is there. EXCEPT alone folds duplicates, and a table that lost
        // one of two identical rows (no primary key: a playlist entry
        // listed twice) would compare equal.
        const auto counted = [&](const std::string &schema) {
            return "SELECT " + columns + ", count(*) FROM " + schema + "." + quoted(table) + where + " GROUP BY "
                   + columns;
        };
        const std::string mine = counted("main");
        const std::string theirs = counted("a");
        const std::int64_t differ = db.count("SELECT count(*) FROM (" + mine + " EXCEPT " + theirs + ")")
                                    + db.count("SELECT count(*) FROM (" + theirs + " EXCEPT " + mine + ")");
        if (differ != 0) {
            return fail(differ < 0 ? db.error() : "table " + table + " differs from the archive's");
        }
    }
    if (!hasTrackAndArt) {
        return fail("no AlbumArt table: not an Engine library this check knows");
    }
    const std::int64_t stampedAlone =
        db.count("SELECT count(*) FROM main.Track t JOIN a.Track o ON o.id = t.id "
                 "WHERE t.lastEditTime IS NOT o.lastEditTime AND t.albumArtId IS o.albumArtId;");
    if (stampedAlone != 0) {
        return fail(stampedAlone < 0 ? db.error() : "a track's lastEditTime moved without its cover changing");
    }

    const fs::path artworkDir = onStick.parent_path().parent_path() / "Artwork";
    // The archive's cover rows: still there, unchanged or a link renamed.
    int renamed = 0;
    std::string problem;
    if (!db.each("SELECT o.id, o.hash, o.albumArt, t.id, t.hash, t.albumArt FROM a.AlbumArt o "
                 "LEFT JOIN main.AlbumArt t ON t.id = o.id;",
                 [&](sqlite3_stmt *stmt) {
                     const Value hash = column(stmt, 1);
                     const Value image = column(stmt, 2);
                     const Value newHash = column(stmt, 4);
                     const Value newImage = column(stmt, 5);
                     if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) {
                         problem = "cover row " + column(stmt, 0).bytes + " is gone";
                     } else if (newHash == hash && newImage == image) {
                         return;
                     } else if (namesAnotherStick(hash.bytes, label) && newImage == image && newHash.type == hash.type
                                && linkLabel(newHash.bytes)
                                && infrastructure::engine::sameStickLabel(*linkLabel(newHash.bytes), label)
                                && newHash.bytes.substr(LinkPrefix.size() + linkLabel(newHash.bytes)->size())
                                       == hash.bytes.substr(LinkPrefix.size() + linkLabel(hash.bytes)->size())) {
                         ++renamed;
                     } else if (problem.empty()) {
                         problem = "cover row " + column(stmt, 0).bytes + " changed beyond a rename to " + label;
                     }
                 })) {
        return fail(db.error());
    }
    if (!problem.empty()) {
        return fail(problem);
    }
    // The rows the clone added: stored covers only, each the image its hash
    // names, and each one a track is on (the clone adds a row only for a
    // track it moves onto it).
    int added = 0;
    if (!db.each("SELECT id, hash, albumArt, "
                 "EXISTS (SELECT 1 FROM main.Track t WHERE t.albumArtId = main.AlbumArt.id) "
                 "FROM main.AlbumArt WHERE id NOT IN (SELECT id FROM a.AlbumArt);",
                 [&](sqlite3_stmt *stmt) {
                     ++added;
                     if (!problem.empty()) {
                         return;
                     }
                     if (!isStoredCover(column(stmt, 1), column(stmt, 2), artworkDir, true)) {
                         problem = "added cover row " + column(stmt, 0).bytes
                                   + " is not a stored cover (its image missing, empty, or not the one its hash names)";
                     } else if (sqlite3_column_int(stmt, 3) == 0) {
                         problem = "added cover row " + column(stmt, 0).bytes + " is on no track";
                     }
                 })) {
        return fail(db.error());
    }
    if (!problem.empty()) {
        return fail(problem);
    }
    // The tracks whose cover changed: from a link to another stick's name
    // to a stored cover.
    int copied = 0;
    // A cover row the archive already had is the library's own, hashed
    // however its writer hashed it: only its file is checked.
    if (!db.each("SELECT t.id, oa.hash, ta.hash, ta.albumArt, ta.id IN (SELECT id FROM a.AlbumArt) "
                 "FROM main.Track t JOIN a.Track o ON o.id = t.id "
                 "LEFT JOIN a.AlbumArt oa ON oa.id = o.albumArtId LEFT JOIN main.AlbumArt ta ON ta.id = t.albumArtId "
                 "WHERE t.albumArtId IS NOT o.albumArtId;",
                 [&](sqlite3_stmt *stmt) {
                     ++copied;
                     const bool archiveRow = sqlite3_column_int(stmt, 4) != 0;
                     if ((!namesAnotherStick(column(stmt, 1).bytes, label)
                          || !isStoredCover(column(stmt, 2), column(stmt, 3), artworkDir, !archiveRow))
                         && problem.empty()) {
                         problem = "track " + column(stmt, 0).bytes
                                   + "'s cover changed, but not from a link to another stick to a stored cover";
                     }
                 })) {
        return fail(db.error());
    }
    if (!problem.empty()) {
        return fail(problem);
    }
    // And no track left on a link naming any stick but this one.
    int stillLinked = 0;
    if (!db.each("SELECT a.hash FROM main.Track t JOIN main.AlbumArt a ON a.id = t.albumArtId;",
                 [&](sqlite3_stmt *stmt) { stillLinked += namesAnotherStick(column(stmt, 0).bytes, label) ? 1 : 0; })) {
        return fail(db.error());
    }
    if (stillLinked != 0) {
        return fail(std::to_string(stillLinked) + " track(s) still on a link naming another stick than " + label);
    }
    if (copied == 0 && renamed == 0) {
        return fail("differs from the archive, but no cover was made label-proof");
    }
    std::cout << "  same    " << row.path << " but for its covers, made label-proof for " << label << ": " << copied
              << " track(s) on a stored cover (" << added << " cover row(s) added), " << renamed
              << " link(s) renamed; vouched for by the target's record\n";
    return true;
}

}  // namespace seabass::rig
