// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_track_rows.hpp"

#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "domain/engine_cue_translation.hpp"
#include "domain/junk_cue.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

// One connection, closed on every way out.
class Connection
{
public:
    Connection(const std::string &file, int flags)
    {
        m_ok = sqlite3_open_v2(file.c_str(), &m_handle, flags, nullptr) == SQLITE_OK;
        if (m_ok) {
            sqlite3_busy_timeout(m_handle, 5000);
        } else {
            m_why = m_handle ? sqlite3_errmsg(m_handle) : "out of memory";
        }
    }
    ~Connection() { sqlite3_close(m_handle); }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    bool ok() const { return m_ok; }
    const std::string &why() const { return m_why; }
    sqlite3 *handle() const { return m_handle; }

    bool exec(const std::string &sql, std::string *error) const
    {
        char *message = nullptr;
        if (sqlite3_exec(m_handle, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK) {
            return true;
        }
        *error = sql + ": " + (message ? message : "unknown error");
        sqlite3_free(message);
        return false;
    }

    // Runs `sql` with each id bound to ?1 in turn and returns the first
    // column of its row (a count) or the number of rows it changed; -1 on
    // an error, with *error set.
    std::int64_t count(const std::string &sql, std::int64_t id, std::string *error) const
    {
        return run(sql, id, true, error);
    }
    std::int64_t change(const std::string &sql, std::int64_t id, std::string *error) const
    {
        return run(sql, id, false, error);
    }

private:
    std::int64_t run(const std::string &sql, std::int64_t id, bool readsRow, std::string *error) const
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            *error = sql + ": " + sqlite3_errmsg(m_handle);
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, id);
        const int step = sqlite3_step(stmt);
        std::int64_t result = -1;
        if (readsRow && step == SQLITE_ROW) {
            result = sqlite3_column_int64(stmt, 0);
        } else if (!readsRow && step == SQLITE_DONE) {
            result = sqlite3_changes(m_handle);
        } else {
            *error = sql + ": " + sqlite3_errmsg(m_handle);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    sqlite3 *m_handle = nullptr;
    bool m_ok = false;
    std::string m_why;
};

namespace fs = std::filesystem;

std::string databaseUnder(const std::string &engineLibraryRoot)
{
    return pathToUtf8(pathFromUtf8(engineLibraryRoot) / "Database2" / "m.db");
}

std::string hex(const void *data, int size)
{
    static constexpr char Digits[] = "0123456789abcdef";
    const auto *bytes = static_cast<const unsigned char *>(data);
    std::string out;
    out.reserve(static_cast<size_t>(size) * 2);
    for (int i = 0; i < size; ++i) {
        out += Digits[bytes[i] >> 4];
        out += Digits[bytes[i] & 0x0f];
    }
    return out;
}

// The Information rows as one comparable string, for the error message.
std::string describe(const EngineInformationRows &info)
{
    std::string out = std::to_string(info.ids.size()) + " row(s)";
    for (const std::int64_t id : info.ids) {
        out += ", id " + std::to_string(id);
    }
    return out;
}

bool sameInformation(const EngineInformationRows &a, const EngineInformationRows &b)
{
    return a.ids == b.ids && a.values == b.values;
}

// Every table (not view) with a trackId column, in the order sqlite_master
// lists them. Empty with *error set when the schema cannot be read.
std::vector<std::string> tablesWithTrackId(const Connection &db, std::string *error)
{
    std::vector<std::string> tables;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;", -1, &stmt,
                           nullptr)
        != SQLITE_OK) {
        *error = std::string("could not read the schema: ") + sqlite3_errmsg(db.handle());
        sqlite3_finalize(stmt);
        return {};
    }
    std::vector<std::string> all;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        all.emplace_back(name ? name : "");
    }
    sqlite3_finalize(stmt);
    for (const std::string &table : all) {
        // pragma_table_info with the name bound, rather than spliced into
        // PRAGMA text: the name comes from the file.
        if (sqlite3_prepare_v2(db.handle(), "SELECT count(*) FROM pragma_table_info(?1) WHERE name = 'trackId';", -1,
                               &stmt, nullptr)
            != SQLITE_OK) {
            *error = std::string("could not read the schema: ") + sqlite3_errmsg(db.handle());
            sqlite3_finalize(stmt);
            return {};
        }
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
            tables.push_back(table);
        }
        sqlite3_finalize(stmt);
    }
    return tables;
}

bool contains(const std::vector<std::string> &names, const std::string &name)
{
    for (const auto &n : names) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

std::string quoted(const std::string &identifier)
{
    std::string out = "\"";
    for (const char c : identifier) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

}  // namespace

int removeEngineTrackRows(const std::string &databaseFile, const std::vector<std::int64_t> &trackIds, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("removeEngineTrackRows: the error out-parameter is required");
    }
    error->clear();
    if (trackIds.empty()) {
        return 0;
    }
    std::set<std::int64_t> seen;
    for (const std::int64_t id : trackIds) {
        if (!seen.insert(id).second) {
            *error = "track id=" + std::to_string(id) + " is named twice";
            return -1;
        }
    }

    Connection db(databaseFile, SQLITE_OPEN_READWRITE);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return -1;
    }
    if (!db.exec("BEGIN IMMEDIATE;", error)) {
        return -1;
    }
    const auto fail = [&]() {
        std::string ignored;
        db.exec("ROLLBACK;", &ignored);
        return -1;
    };

    const std::vector<std::string> named = tablesWithTrackId(db, error);
    if (!error->empty()) {
        return fail();
    }
    if (!contains(named, "PlaylistEntity")) {
        // Every Engine 2.x and 3.x schema has it; without it this is not
        // a database this function knows how to edit.
        *error = databaseFile + " has no PlaylistEntity table with a trackId; not an Engine 2.x or 3.x database";
        return fail();
    }

    // The playlist entries first, so Engine's trigger relinks each chain
    // while the rest of the list is still intact; Track last.
    std::vector<std::string> statements;
    for (const char *table : {"PlaylistEntity", "PreparelistEntity", "PerformanceData"}) {
        if (contains(named, table)) {
            statements.push_back(std::string("DELETE FROM ") + table + " WHERE trackId = ?1;");
        }
    }
    if (contains(named, "ChangeLog")) {
        statements.emplace_back("UPDATE ChangeLog SET trackId = NULL WHERE trackId = ?1;");
    }

    int removed = 0;
    for (const std::int64_t id : trackIds) {
        const std::int64_t present = db.count("SELECT count(*) FROM Track WHERE id = ?1;", id, error);
        if (present < 0) {
            return fail();
        }
        if (present == 0) {
            *error = "no track id=" + std::to_string(id) + " in " + databaseFile;
            return fail();
        }
        for (const std::string &sql : statements) {
            if (db.change(sql, id, error) < 0) {
                return fail();
            }
        }
        const std::int64_t gone = db.change("DELETE FROM Track WHERE id = ?1;", id, error);
        if (gone < 0) {
            return fail();
        }
        if (gone != 1) {
            *error = "deleting track id=" + std::to_string(id) + " removed " + std::to_string(gone) + " rows";
            return fail();
        }
        ++removed;
    }

    // Nothing may still name a removed track. Every table Engine is known
    // to keep one in was handled above, so this fires only on a schema
    // with a table this function has not met: refused, not left dangling.
    for (const std::int64_t id : trackIds) {
        for (const std::string &table : named) {
            const std::int64_t left =
                db.count("SELECT count(*) FROM " + quoted(table) + " WHERE trackId = ?1;", id, error);
            if (left < 0) {
                return fail();
            }
            if (left > 0) {
                *error = std::to_string(left) + " row(s) in " + table + " still name track id=" + std::to_string(id)
                    + " after removing it; not removing anything";
                return fail();
            }
        }
    }

    if (!db.exec("COMMIT;", error)) {
        return fail();
    }
    return removed;
}

int engineRowsNamingTrack(const std::string &databaseFile, std::int64_t trackId, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("engineRowsNamingTrack: the error out-parameter is required");
    }
    error->clear();
    Connection db(databaseFile, SQLITE_OPEN_READONLY);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return -1;
    }
    const std::vector<std::string> named = tablesWithTrackId(db, error);
    if (!error->empty()) {
        return -1;
    }
    std::int64_t total = db.count("SELECT count(*) FROM Track WHERE id = ?1;", trackId, error);
    if (total < 0) {
        return -1;
    }
    for (const std::string &table : named) {
        const std::int64_t rows = db.count("SELECT count(*) FROM " + quoted(table) + " WHERE trackId = ?1;", trackId, error);
        if (rows < 0) {
            return -1;
        }
        total += rows;
    }
    return static_cast<int>(total);
}

std::string engineRelativePath(const std::string &trackFile, const std::string &realEngineLibraryPath)
{
    std::error_code relError;
    const fs::path relative = fs::relative(pathFromUtf8(trackFile), pathFromUtf8(realEngineLibraryPath), relError);
    // fs::relative() does NOT report "no relation possible" as an error:
    // per the standard it is lexically_relative() underneath, which
    // returns an empty path when the two paths share no root (different
    // Windows drives: the rig creates a library on C: from tracks on a
    // stick's own drive letter). relError stays clear either way, measured
    // on Windows (its message there: "De bewerking is voltooid", Dutch for
    // "the operation completed successfully"), so emptiness is the only
    // signal it gives. The track's own path then, absolute.
    //
    // The generic spelling either way: trackFile is platform-native, with
    // backslashes on Windows, and libdjinterop's get_filename() only
    // splits on '/', so a backslash path reads as one file name with no
    // folder, and if that "name" has no dot create_track() throws "cannot
    // auto-determine file type based on extension".
    return pathToGenericUtf8(relative.empty() ? pathFromUtf8(trackFile) : relative);
}

int markForDeviceAnalysis(const std::string &databaseFile, const std::vector<std::int64_t> &ids, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("markForDeviceAnalysis: the error out-parameter is required");
    }
    error->clear();
    if (ids.empty()) {
        return 0;
    }
    std::set<std::int64_t> seen;
    for (const std::int64_t id : ids) {
        if (!seen.insert(id).second) {
            *error = "track id=" + std::to_string(id) + " is named twice";
            return -1;
        }
    }
    Connection db(databaseFile, SQLITE_OPEN_READWRITE);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return -1;
    }
    // Engine 1.x keeps the flag, with the rest of the performance data, in
    // a second database beside this one, and no 1.x hardware has been
    // available to see what a track waiting for analysis looks like there.
    // The creator leaves such a library as libdjinterop wrote it; for a
    // single row in a library a player owns, refused.
    {
        sqlite3_stmt *shape = nullptr;
        bool hasFlag = false;
        if (sqlite3_prepare_v2(db.handle(),
                               "SELECT count(*) FROM pragma_table_info('Track') WHERE name = 'isAnalyzed';", -1, &shape,
                               nullptr)
                == SQLITE_OK
            && sqlite3_step(shape) == SQLITE_ROW) {
            hasFlag = sqlite3_column_int(shape, 0) == 1;
        }
        sqlite3_finalize(shape);
        if (!hasFlag) {
            *error = databaseFile + " has no Track.isAnalyzed; not an Engine 2.x or 3.x database";
            return -1;
        }
    }
    if (!db.exec("BEGIN IMMEDIATE;", error)) {
        return -1;
    }
    const auto fail = [&]() {
        std::string ignored;
        db.exec("ROLLBACK;", &ignored);
        return -1;
    };
    int marked = 0;
    for (const std::int64_t id : ids) {
        const std::int64_t present = db.count("SELECT count(*) FROM Track WHERE id = ?1;", id, error);
        if (present < 0) {
            return fail();
        }
        if (present == 0) {
            *error = "no track id=" + std::to_string(id) + " in " + databaseFile;
            return fail();
        }
        // The blobs first, as the creator does: a row already marked
        // unanalysed while still carrying analysis data is the one state
        // no player has been seen in. PerformanceData is a view over Track
        // before 2.20, updated through Engine's own trigger, so its change
        // count says nothing and is not asked.
        if (db.change("UPDATE PerformanceData SET trackData = NULL, overviewWaveFormData = NULL, beatData = NULL "
                      "WHERE trackId = ?1;",
                      id, error)
            < 0) {
            return fail();
        }
        const std::int64_t flagged = db.change("UPDATE Track SET isAnalyzed = 0 WHERE id = ?1;", id, error);
        if (flagged < 0) {
            return fail();
        }
        if (flagged != 1) {
            *error = "marking track id=" + std::to_string(id) + " changed " + std::to_string(flagged) + " rows";
            return fail();
        }
        ++marked;
    }
    if (!db.exec("COMMIT;", error)) {
        return fail();
    }
    return marked;
}

std::optional<EngineInformationRows> readEngineInformation(const std::string &databaseFile, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("readEngineInformation: the error out-parameter is required");
    }
    error->clear();
    // READONLY: nothing here writes, and a wrong path must fail rather than
    // leave an empty database behind.
    Connection db(databaseFile, SQLITE_OPEN_READONLY);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return std::nullopt;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "SELECT id, * FROM Information ORDER BY id;", -1, &stmt, nullptr) != SQLITE_OK) {
        *error = std::string("could not read the Information row: ") + sqlite3_errmsg(db.handle());
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    EngineInformationRows info;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        info.ids.push_back(sqlite3_column_int64(stmt, 0));
        std::vector<std::optional<std::string>> row;
        for (int c = 1; c < sqlite3_column_count(stmt); ++c) {
            switch (sqlite3_column_type(stmt, c)) {
            case SQLITE_NULL: row.emplace_back(std::nullopt); break;
            case SQLITE_BLOB:
                row.emplace_back("x'" + hex(sqlite3_column_blob(stmt, c), sqlite3_column_bytes(stmt, c)) + "'");
                break;
            default: {
                const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(stmt, c));
                row.emplace_back(std::string(text ? text : "", static_cast<size_t>(sqlite3_column_bytes(stmt, c))));
            }
            }
        }
        info.values.push_back(std::move(row));
    }
    if (step != SQLITE_DONE) {
        *error = std::string("could not read the Information row: ") + sqlite3_errmsg(db.handle());
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_finalize(stmt);
    return info;
}

std::int64_t createEngineTrack(const std::string &writeRoot, const NewEngineTrack &track, EngineTrackCover *cover,
                               std::string *error, int *junkCuesDropped)
{
    if (error == nullptr) {
        throw std::invalid_argument("createEngineTrack: the error out-parameter is required");
    }
    if (cover == nullptr) {
        throw std::invalid_argument("createEngineTrack: the cover out-parameter is required");
    }
    error->clear();
    if (junkCuesDropped != nullptr) {
        *junkCuesDropped = 0;
    }
    cover->written = false;
    cover->filesWritten.clear();
    cover->problem.clear();

    const domain::Track &source = track.source;
    const std::string databaseFile = databaseUnder(writeRoot);
    std::error_code ec;
    if (!fs::is_regular_file(pathFromUtf8(databaseFile), ec)) {
        *error = "no Engine 2.x or 3.x database at " + databaseFile;
        return -1;
    }
    if (!source.streamingSource.empty()) {
        *error = "\"" + source.title + "\" is a streaming track; an Engine row needs a file on the stick";
        return -1;
    }
    if (source.filePath.empty() || !fs::is_regular_file(pathFromUtf8(source.filePath), ec)) {
        *error = "the audio file " + (source.filePath.empty() ? std::string("(none)") : source.filePath)
            + " is not there; an Engine row for it would name nothing";
        return -1;
    }
    if (!(track.sampleRateHz > 0.0)) {
        *error = "no sample rate for " + source.filePath + "; the cues cannot be placed without the file's own";
        return -1;
    }
    if (track.realEngineLibraryPath.empty()) {
        *error = "no Engine Library folder to make " + source.filePath + " relative to";
        return -1;
    }
    const std::string relativePath = engineRelativePath(source.filePath, track.realEngineLibraryPath);
    {
        // Reachable on Windows only: a file on another drive than the
        // library comes back absolute, and a player cannot follow that.
        const fs::path asPath = pathFromUtf8(relativePath);
        if (asPath.is_absolute() || asPath.has_root_name() || asPath.has_root_directory()) {
            *error = source.filePath + " is not on the same drive as " + track.realEngineLibraryPath
                + "; the row would name a path no player can follow";
            return -1;
        }
    }

    const std::optional<EngineInformationRows> informationBefore = readEngineInformation(databaseFile, error);
    if (!informationBefore) {
        return -1;
    }
    if (informationBefore->ids.size() != 1) {
        // Not this function's to repair, and Engine's own trigger reads
        // "SELECT uuid FROM Information" for every new row's origin.
        *error = "the Information table of " + databaseFile + " has " + describe(*informationBefore)
            + "; Engine expects exactly one row. Not adding " + relativePath;
        return -1;
    }
    {
        Connection db(databaseFile, SQLITE_OPEN_READONLY);
        if (!db.ok()) {
            *error = "could not open " + databaseFile + ": " + db.why();
            return -1;
        }
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db.handle(), "SELECT id, path FROM Track WHERE path = ?1 COLLATE NOCASE LIMIT 1;", -1,
                               &stmt, nullptr)
            != SQLITE_OK) {
            *error = std::string("could not look for the path in Track: ") + sqlite3_errmsg(db.handle());
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_text(stmt, 1, relativePath.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            const auto *existing = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            *error = "track id=" + std::to_string(sqlite3_column_int64(stmt, 0)) + " already names "
                + (existing ? existing : "") + "; not adding a second row for " + relativePath;
        } else if (step != SQLITE_DONE) {
            *error = std::string("could not look for the path in Track: ") + sqlite3_errmsg(db.handle());
        }
        sqlite3_finalize(stmt);
        if (!error->empty()) {
            return -1;
        }
    }

    // The creator's field mapping. No beat grid, sample count or rate: the
    // first two live in beatData and trackData, which markForDeviceAnalysis
    // empties next, and the player recomputes all three when it analyses.
    djinterop::track_snapshot snapshot;
    snapshot.title = source.title.empty() ? std::nullopt : std::optional(source.title);
    snapshot.artist = source.artist.empty() ? std::nullopt : std::optional(source.artist);
    if (source.bpm > 0.0) {
        snapshot.bpm = source.bpm;
    }
    if (auto key = parseRekordboxKey(source.key)) {
        snapshot.key = *key;
    }
    if (source.durationSeconds > 0.0) {
        snapshot.duration = std::chrono::milliseconds(static_cast<std::int64_t>(source.durationSeconds * 1000.0));
    }
    if (source.bitrate > 0) {
        snapshot.bitrate = source.bitrate;
    }
    if (source.rating.has_value()) {
        // Track::rating is 0-5 stars; Engine's own scale is 0-100.
        snapshot.rating = *source.rating * 20;
    }
    if (!source.comment.empty()) {
        snapshot.comment = source.comment;
    }
    if (source.fileSizeBytes > 0) {
        snapshot.file_bytes = source.fileSizeBytes;
    }
    snapshot.relative_path = relativePath;

    std::int64_t id = -1;
    try {
        auto db = djinterop::engine::load_database(writeRoot);
        id = db.create_track(snapshot).id();
        // db closes here, before the plain-SQL connection below writes.
    } catch (const std::exception &e) {
        *error = "could not create the Engine row for " + relativePath + ": " + e.what();
        return -1;
    }

    // From here a failure takes the row back out, so -1 always means no row.
    const auto undo = [&](const std::string &why) -> std::int64_t {
        std::string removeError;
        if (removeEngineTrackRows(databaseFile, {id}, &removeError) != 1) {
            *error = why + "; and removing the half-made row id=" + std::to_string(id) + " failed: " + removeError;
        } else {
            *error = why;
        }
        return -1;
    };

    std::string markError;
    if (markForDeviceAnalysis(databaseFile, {id}, &markError) != 1) {
        return undo("could not leave track id=" + std::to_string(id) + " for the player to analyse: " + markError);
    }

    // Junk is never written onto a stick (domain/junk_cue.hpp): the
    // planner hands none, and a cue that still is junk is dropped here,
    // counted, rather than put on a pad or made the main cue.
    const std::vector<domain::CuePoint> cues = domain::withoutJunkCues(source.cues);
    if (junkCuesDropped != nullptr) {
        *junkCuesDropped = static_cast<int>(source.cues.size() - cues.size());
    }
    const domain::EngineCueTranslation translated = domain::translateCuesForEngine(cues, {});
    if (!translated.cues.empty()) {
        try {
            LibdjinteropEngineCueWriter writer(writeRoot);
            writer.writeHotCuesAtSampleRate(std::to_string(id), translated.cues, track.sampleRateHz);
        } catch (const std::exception &e) {
            return undo("could not write the cues of track id=" + std::to_string(id) + ": " + e.what());
        }
    }

    if (!source.artworkPath.empty()) {
        ArtworkEntry entry;
        entry.trackId = id;
        entry.title = source.title;
        entry.artist = source.artist;
        entry.storage = ArtworkStorage::None;
        entry.imageOnStick = source.artworkPath;
        entry.trackFile = source.filePath;
        const ArtworkRepair repaired =
            repairArtwork(track.realEngineLibraryPath, {entry}, cover->beforeWrite, databaseFile);
        cover->filesWritten = repaired.filesWritten;
        cover->written = repaired.error.empty() && repaired.repaired == 1;
        if (!cover->written) {
            if (!repaired.error.empty()) {
                cover->problem = repaired.error;
            } else if (repaired.notAnImage > 0) {
                cover->problem = source.artworkPath + " is neither a JPEG nor a PNG";
            } else if (!repaired.failureReasons.empty()) {
                cover->problem = repaired.failureReasons.front();
            } else {
                cover->problem = "the cover " + source.artworkPath + " could not be read";
            }
        }
    }

    // Last: the table every reader of the library looks at first. A
    // player given a library whose Information row moved called the stick
    // corrupt and replaced m.db with an empty one. Nothing above writes it;
    // this is the guard that says so on every row added.
    std::string infoError;
    const std::optional<EngineInformationRows> informationAfter = readEngineInformation(databaseFile, &infoError);
    if (!informationAfter) {
        return undo("could not read the Information row back after adding track id=" + std::to_string(id) + ": "
                    + infoError);
    }
    if (!sameInformation(*informationBefore, *informationAfter)) {
        return undo("adding track id=" + std::to_string(id) + " changed the Information table: it was "
                    + describe(*informationBefore) + " and is now " + describe(*informationAfter)
                    + " (or the same ids with other values)");
    }
    return id;
}

}  // namespace seabass::infrastructure::engine
