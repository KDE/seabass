// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_blob_check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <zlib.h>

#include "infrastructure/engine/engine_sqlite.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

// SQLite's words for the last failure on `handle`, with what kind of
// failure it is when that is one engineBlobGuardFailureReadsUnchecked()
// refuses on whatever the words: a lock, or a read-only connection (a hot
// journal it cannot roll back is SQLITE_READONLY_ROLLBACK).
std::string failureKind(sqlite3 *handle)
{
    switch (sqlite3_extended_errcode(handle) & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return " (locked)";
    case SQLITE_READONLY:
        return " (readonly)";
    case SQLITE_CANTOPEN:
        return " (unable to open)";
    default:
        return {};
    }
}

std::string sqliteFailure(sqlite3 *handle)
{
    return sqlite3_errmsg(handle) + failureKind(handle);
}

constexpr std::uint32_t KiB = 1024u;
constexpr std::uint32_t MiB = 1024u * KiB;

// The most any column inflates to: a high-resolution waveform, 6 bytes a
// point, of a track hours long.
constexpr std::uint32_t MaxEngineBlobBytes = 64u * MiB;

// The player's pads: eight hot cues, eight saved loops.
constexpr std::int64_t MaxCueSlots = 8;
// The closest two beat grid markers can sensibly be: 50 ms, a marker on
// every beat at 1200 BPM.
constexpr double MinMarkerSpacingSeconds = 0.05;

std::uint32_t maxBytesOf(EngineBlob column)
{
    switch (column) {
    case EngineBlob::TrackData:
        return 4u * KiB;
    case EngineBlob::BeatData:
        // 17 bytes of header and two grids of 24 bytes a marker: a grid
        // with a marker on every beat of a track hours long fits many
        // times over (6 h at 120 BPM is 43200 markers, 1 MiB a grid).
        return 16u * MiB;
    case EngineBlob::OverviewWaveFormData:
        return 1u * MiB;
    case EngineBlob::HighResolutionWaveFormData:
        return MaxEngineBlobBytes;
    case EngineBlob::QuickCues:
    case EngineBlob::Loops:
        return 64u * KiB;
    }
    return MaxEngineBlobBytes;
}

const char *nameOf(EngineBlob column)
{
    switch (column) {
    case EngineBlob::TrackData:
        return "trackData";
    case EngineBlob::BeatData:
        return "beatData";
    case EngineBlob::OverviewWaveFormData:
        return "overviewWaveFormData";
    case EngineBlob::HighResolutionWaveFormData:
        return "highResolutionWaveFormData";
    case EngineBlob::QuickCues:
        return "quickCues";
    case EngineBlob::Loops:
        return "loops";
    }
    return "?";
}

constexpr std::array<EngineBlob, 6> AllBlobs = {EngineBlob::TrackData,
                                                EngineBlob::HighResolutionWaveFormData,
                                                EngineBlob::OverviewWaveFormData,
                                                EngineBlob::BeatData,
                                                EngineBlob::QuickCues,
                                                EngineBlob::Loops};

std::uint64_t be(const unsigned char *p, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) {
        value = (value << 8) | p[i];
    }
    return value;
}

std::uint64_t le64(const unsigned char *p)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

// A count as the decoders read it, signed, within [0, most].
bool countWithin(std::uint64_t raw, std::int64_t most)
{
    const auto count = static_cast<std::int64_t>(raw);
    return count >= 0 && count <= most;
}

double beDouble(const unsigned char *p)
{
    const std::uint64_t bits = be(p, 8);
    double value;
    static_assert(sizeof value == sizeof bits);
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The most markers one beat grid of a beat data record can have: one every
// MinMarkerSpacingSeconds over the samples the record says the track has,
// and two more for its ends. Nothing past the bytes there are when the
// record gives no usable rate and length. (libdjinterop's 1.x decoder sets
// its own bound on top, 32768, and refuses a grid past it itself.)
std::int64_t markerBoundOf(const unsigned char *record)
{
    const double rate = beDouble(record);
    const double samples = beDouble(record + 8);
    if (!(rate > 0.0) || !(samples > 0.0) || !std::isfinite(rate) || !std::isfinite(samples)) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const double markers = samples / (rate * MinMarkerSpacingSeconds) + 2.0;
    return markers >= 9.0e18 ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(markers);
}

// The declared length and the stream after it, inflated to at most
// `bound` bytes. nullopt when the header is short, the length is over the
// bound, or the stream does not end within the blob, or ends past the
// bound or at another length than declared. Empty blobs and a declared 0
// decode to nothing, and are handed back as empty.
std::optional<std::vector<unsigned char>> inflated(std::string_view bytes, std::uint32_t bound)
{
    if (bytes.empty()) {
        return std::vector<unsigned char>{};
    }
    if (bytes.size() < 4) {
        return std::nullopt;
    }
    const auto *raw = reinterpret_cast<const unsigned char *>(bytes.data());
    const auto declared = static_cast<std::uint32_t>(be(raw, 4));
    if (declared == 0) {
        return std::vector<unsigned char>{};
    }
    // Checked before anything is inflated: the decoder reserves this much.
    if (declared > bound || bytes.size() < 5) {
        return std::nullopt;
    }
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) {
        return std::nullopt;
    }
    strm.next_in = const_cast<Bytef *>(raw + 4);
    strm.avail_in = static_cast<uInt>(bytes.size() - 4);
    std::vector<unsigned char> out(declared);
    std::array<unsigned char, 16384> spill{};
    std::size_t have = 0;
    int ret = Z_OK;
    while (ret == Z_OK) {
        // Into the record while it has room, then into a scratch chunk
        // only to tell a stream that goes on past the declared length.
        if (have < out.size()) {
            strm.next_out = out.data() + have;
            strm.avail_out = static_cast<uInt>(out.size() - have);
        } else {
            strm.next_out = spill.data();
            strm.avail_out = static_cast<uInt>(spill.size());
        }
        const uInt room = strm.avail_out;
        ret = inflate(&strm, Z_NO_FLUSH);
        have += room - strm.avail_out;
        if (have > declared) {
            ret = Z_DATA_ERROR;
        }
    }
    inflateEnd(&strm);
    // Z_BUF_ERROR with no input left is a stream cut short: the one
    // libdjinterop loops on.
    if (ret != Z_STREAM_END || have != declared) {
        return std::nullopt;
    }
    return out;
}

// The counts a decoder sizes a vector by or loops by, against the record.
bool structureSound(EngineBlob column, const unsigned char *p, std::size_t size)
{
    switch (column) {
    case EngineBlob::TrackData:
        return true;  // fixed fields; the decoder checks the length
    case EngineBlob::BeatData: {
        if (size < 33) {
            return true;  // refused by the decoder before any count is read
        }
        // rate, length, flag; then two grids, each a count and its markers.
        const std::int64_t most = markerBoundOf(p);
        std::size_t at = 17;
        for (int grid = 0; grid < 2; ++grid) {
            if (size - at < 8) {
                return false;  // the decoder would read the count past the end
            }
            const std::uint64_t raw = be(p + at, 8);
            at += 8;
            if (!countWithin(raw, most) || (size - at) / 24 < raw) {
                return false;
            }
            at += static_cast<std::size_t>(raw) * 24;
        }
        return true;
    }
    case EngineBlob::OverviewWaveFormData:
    case EngineBlob::HighResolutionWaveFormData: {
        // Two counts and the samples a point, then the points and the
        // maximum point, 3 bytes each (6 in the high-resolution one).
        const std::size_t perPoint = column == EngineBlob::OverviewWaveFormData ? 3 : 6;
        const std::size_t header = 24;
        if (size < header + perPoint) {
            return true;
        }
        // The decoder compares the two counts, so the first decides: it
        // multiplies it out (overflowing, for a count past the record)
        // and sizes the waveform by it.
        const std::uint64_t raw = be(p, 8);
        return countWithin(raw, static_cast<std::int64_t>((size - header) / perPoint))
            && (raw + 1) * perPoint <= size - header;
    }
    case EngineBlob::QuickCues: {
        if (size < 25) {
            return true;
        }
        // A count, then each cue: a label length, the label, 12 bytes; then
        // 17 bytes of main cue.
        const std::uint64_t raw = be(p, 8);
        return countWithin(raw, MaxCueSlots) && 8 + raw * 13 + 17 <= size;
    }
    case EngineBlob::Loops: {
        if (size < 8) {
            return true;
        }
        // A little-endian count, then each loop: a label length, the label,
        // 22 bytes.
        const std::uint64_t raw = le64(p);
        return countWithin(raw, MaxCueSlots) && 8 + raw * 23 <= size;
    }
    }
    return false;
}

}  // namespace

std::string engineBlobNames(EngineBlobs blobs)
{
    std::string names;
    for (EngineBlob blob : AllBlobs) {
        if (contains(blobs, blob)) {
            names += (names.empty() ? "" : ", ") + std::string(nameOf(blob));
        }
    }
    return names;
}

namespace internal
{

bool engineBlobDecodable(std::string_view bytes)
{
    return inflated(bytes, MaxEngineBlobBytes).has_value();
}

bool engineBlobDecodable(EngineBlob column, std::string_view bytes)
{
    if (column == EngineBlob::Loops) {
        // Stored as it is: no length, no stream, checked where it lies.
        return bytes.size() <= maxBytesOf(column)
            && structureSound(column, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
    }
    const auto record = inflated(bytes, maxBytesOf(column));
    return record && structureSound(column, record->data(), record->size());
}

}  // namespace internal

bool engineBlobGuardFailureReadsUnchecked(const std::string &error)
{
    std::string lower(error);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // A lock (SQLITE_BUSY, SQLITE_LOCKED), and a journal a read-only
    // connection cannot roll back (SQLITE_READONLY_ROLLBACK, "attempt to
    // write a readonly database"; any read-only or journal failure): the
    // database is mid-write or was left mid-write, and its blobs may be
    // half written. And a read-only open that cannot open the file at all
    // (SQLITE_CANTOPEN, "unable to open database file"): a WAL database on
    // a read-only mount, whose write-ahead log, unread, may hold the rows'
    // newest blobs.
    for (const char *refuse : {"locked", "busy", "readonly", "read-only", "journal", "unable to open"}) {
        if (lower.find(refuse) != std::string::npos) {
            return false;
        }
    }
    return true;
}

// The read-only connection, the statements and the columns they read.
struct EngineBlobGuard::State
{
    ~State()
    {
        sqlite3_finalize(rowQuery);
        sqlite3_close(handle);
    }
    sqlite3 *handle = nullptr;
    sqlite3_stmt *rowQuery = nullptr;
    // The table and its row id column, and the blob columns it has, in the
    // order the statements select them after the id.
    std::string table;
    std::string idColumn;
    std::vector<EngineBlob> columns;
};

EngineBlobGuard::EngineBlobGuard(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
{
}

EngineBlobGuard::~EngineBlobGuard() = default;

namespace
{

// Opens the database the blobs are in and finds their table. False with
// *error when it cannot.
bool open(const std::string &engineLibraryPath, std::unique_ptr<EngineBlobGuard::State> &state, std::string *error)
{
    if (state) {
        return true;
    }
    namespace fs = std::filesystem;
    const fs::path root = pathFromUtf8(engineLibraryPath);
    std::error_code ec;
    const fs::path mainDb = root / "Database2" / "m.db";
    const fs::path perfDb = root / "p.db";
    const bool v1 = !fs::exists(mainDb, ec) && fs::exists(perfDb, ec);
    const fs::path file = v1 ? perfDb : mainDb;
    if (!fs::exists(file, ec)) {
        *error = "no Engine database at " + engineLibraryPath;
        return false;
    }
    auto opened = std::make_unique<EngineBlobGuard::State>();
    if (sqlite3_open_v2(pathToUtf8(file).c_str(), &opened->handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        *error = "could not open " + pathToUtf8(file) + ": "
            + (opened->handle ? sqliteFailure(opened->handle) : "out of memory");
        return false;
    }
    sqlite3_busy_timeout(opened->handle, 5000);
    try {
        if (v1) {
            opened->table = "PerformanceData";
            opened->idColumn = "id";
        } else if (hasColumn(opened->handle, "Track", "trackData")) {
            opened->table = "Track";  // 2.x (PerformanceData a view over it, before 2.20)
            opened->idColumn = "id";
        } else {
            opened->table = "PerformanceData";  // 3.x
            opened->idColumn = "trackId";
        }
        for (EngineBlob blob : AllBlobs) {
            if (hasColumn(opened->handle, opened->table, nameOf(blob))) {
                opened->columns.push_back(blob);
            }
        }
    } catch (const std::exception &e) {
        // The schema read is the first read: a hot journal or a lock
        // shows here. Its kind as sqliteFailure() says it.
        *error = e.what() + failureKind(opened->handle);
        return false;
    }
    if (opened->columns.empty()) {
        *error = "no blob columns in " + opened->table + " of " + pathToUtf8(file);
        return false;
    }
    state = std::move(opened);
    return true;
}

std::string selectList(const EngineBlobGuard::State &s)
{
    std::string sql = "SELECT " + s.idColumn;
    for (EngineBlob blob : s.columns) {
        sql += ", " + std::string(nameOf(blob));
    }
    return sql + " FROM " + s.table;
}

// The damaged blobs of the row `stmt` is on.
EngineBlobs damagedOf(sqlite3_stmt *stmt, const std::vector<EngineBlob> &columns)
{
    EngineBlobs damaged = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const int c = static_cast<int>(i) + 1;
        const int type = sqlite3_column_type(stmt, c);
        if (type == SQLITE_NULL) {
            continue;  // decoded as nothing
        }
        // A number or text is read as its bytes, as libdjinterop reads it.
        // Checked where SQLite holds it, not copied.
        const void *data = sqlite3_column_blob(stmt, c);
        const int length = sqlite3_column_bytes(stmt, c);
        const std::string_view bytes = data ? std::string_view(static_cast<const char *>(data), static_cast<std::size_t>(length))
                                            : std::string_view();
        if (!internal::engineBlobDecodable(columns[i], bytes)) {
            damaged = damaged | columns[i];
        }
    }
    return damaged;
}

}  // namespace

std::optional<EngineBlobs> EngineBlobGuard::damaged(std::int64_t trackId, std::string *error)
{
    error->clear();
    if (!open(m_engineLibraryPath, m_state, error)) {
        return std::nullopt;
    }
    State &s = *m_state;
    if (s.rowQuery == nullptr) {
        const std::string sql = selectList(s) + " WHERE " + s.idColumn + " = ?1;";
        if (sqlite3_prepare_v2(s.handle, sql.c_str(), -1, &s.rowQuery, nullptr) != SQLITE_OK) {
            *error = "could not read the blobs of track id=" + std::to_string(trackId) + ": " + sqliteFailure(s.handle);
            sqlite3_finalize(s.rowQuery);
            s.rowQuery = nullptr;
            return std::nullopt;
        }
    }
    sqlite3_reset(s.rowQuery);
    sqlite3_bind_int64(s.rowQuery, 1, trackId);
    const int step = sqlite3_step(s.rowQuery);
    EngineBlobs result = 0;
    if (step == SQLITE_ROW) {
        result = damagedOf(s.rowQuery, s.columns);
    } else if (step != SQLITE_DONE) {
        *error = "could not read the blobs of track id=" + std::to_string(trackId) + ": " + sqliteFailure(s.handle);
        sqlite3_reset(s.rowQuery);
        return std::nullopt;
    }
    // Reset, so no lock is held between calls.
    sqlite3_reset(s.rowQuery);
    return result;
}

std::optional<std::unordered_map<std::int64_t, EngineBlobs>> EngineBlobGuard::damagedRows(std::string *error)
{
    error->clear();
    if (!open(m_engineLibraryPath, m_state, error)) {
        return std::nullopt;
    }
    const State &s = *m_state;
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = selectList(s) + ";";
    if (sqlite3_prepare_v2(s.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        *error = std::string("could not read the blobs: ") + sqliteFailure(s.handle);
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    std::unordered_map<std::int64_t, EngineBlobs> rows;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (const EngineBlobs damaged = damagedOf(stmt, s.columns)) {
            rows[sqlite3_column_int64(stmt, 0)] = damaged;
        }
    }
    if (step != SQLITE_DONE) {
        *error = std::string("could not read the blobs: ") + sqliteFailure(s.handle);
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    sqlite3_finalize(stmt);
    return rows;
}

}  // namespace seabass::infrastructure::engine
