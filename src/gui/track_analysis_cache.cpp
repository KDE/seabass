// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/track_analysis_cache.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cmath>
#include <cstring>
#include <set>

#include <sqlite3.h>

#include "gui/qt_path.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/local/sqlite_statement.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_byte_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace seabass::gui
{

using infrastructure::local::exec;
using infrastructure::local::Statement;

namespace
{

constexpr const char *Context = "analysis cache";
// The layout below, and the serialised form. Any other number is a file
// from another build: it is emptied, not read.
constexpr int SchemaVersion = 1;
// How stale an entry's last use may get before a hit writes it again.
constexpr qint64 TouchAfterSeconds = 60;

bool damaged(sqlite3 *db)
{
    const int code = sqlite3_errcode(db) & 0xff;
    return code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
}

void putU32(std::string &out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

std::uint32_t getU32(const std::string &in, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[at + i])) << (8 * i);
    }
    return v;
}

// A file's part of the key: its size and modification time, or that it
// is not there.
QString fileStamp(const QFileInfo &info)
{
    if (!info.isFile()) {
        return QStringLiteral("-");
    }
    return QString::number(info.size()) + QLatin1Char('@') + QString::number(info.lastModified().toMSecsSinceEpoch());
}

}  // namespace

// Version byte, look byte, rate-recorded byte; the overview as a count
// and three bytes a column (each band 0..255, as every view is handed it
// in the end); the beats as a count and, each, the time as a double's
// eight bytes and the beat in its bar.
std::string TrackAnalysisCache::serialise(const domain::TrackAnalysis &analysis)
{
    std::string out;
    out.push_back(static_cast<char>(SchemaVersion));
    out.push_back(static_cast<char>(analysis.waveformLook));
    out.push_back(static_cast<char>(analysis.beatRateRecorded ? 1 : 0));
    putU32(out, static_cast<std::uint32_t>(analysis.waveform.size()));
    const auto byte = [](double v) {
        return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
    };
    for (const auto &column : analysis.waveform) {
        out.push_back(byte(column.low));
        out.push_back(byte(column.mid));
        out.push_back(byte(column.high));
    }
    putU32(out, static_cast<std::uint32_t>(analysis.beats.size()));
    for (const auto &beat : analysis.beats) {
        char time[sizeof(double)];
        std::memcpy(time, &beat.timeMs, sizeof(double));
        out.append(time, sizeof(double));
        out.push_back(static_cast<char>(beat.beatInBar));
    }
    return out;
}

std::optional<domain::TrackAnalysis> TrackAnalysisCache::deserialise(const std::string &bytes)
{
    if (bytes.size() < 11 || static_cast<unsigned char>(bytes[0]) != SchemaVersion) {
        return std::nullopt;
    }
    domain::TrackAnalysis analysis;
    const auto look = static_cast<unsigned char>(bytes[1]);
    if (look > static_cast<unsigned char>(domain::WaveformLook::Engine)) {
        return std::nullopt;
    }
    analysis.waveformLook = static_cast<domain::WaveformLook>(look);
    analysis.beatRateRecorded = bytes[2] != 0;
    std::size_t at = 3;
    const std::uint32_t columns = getU32(bytes, at);
    at += 4;
    if (bytes.size() < at + static_cast<std::size_t>(columns) * 3 + 4) {
        return std::nullopt;
    }
    analysis.waveform.reserve(columns);
    for (std::uint32_t i = 0; i < columns; ++i, at += 3) {
        analysis.waveform.push_back({static_cast<unsigned char>(bytes[at]) / 255.0,
                                     static_cast<unsigned char>(bytes[at + 1]) / 255.0,
                                     static_cast<unsigned char>(bytes[at + 2]) / 255.0});
    }
    const std::uint32_t beats = getU32(bytes, at);
    at += 4;
    if (bytes.size() != at + static_cast<std::size_t>(beats) * (sizeof(double) + 1)) {
        return std::nullopt;
    }
    analysis.beats.reserve(beats);
    for (std::uint32_t i = 0; i < beats; ++i) {
        domain::Beat beat;
        std::memcpy(&beat.timeMs, bytes.data() + at, sizeof(double));
        beat.beatInBar = static_cast<signed char>(bytes[at + sizeof(double)]);
        at += sizeof(double) + 1;
        analysis.beats.push_back(beat);
    }
    return analysis;
}

TrackAnalysisCache::TrackAnalysisCache(QString databaseFile, std::int64_t capBytes)
    : m_file(std::move(databaseFile)), m_cap(capBytes)
{
    const std::lock_guard<std::mutex> guard(m_lock);
    open();
}

TrackAnalysisCache::~TrackAnalysisCache()
{
    const std::lock_guard<std::mutex> guard(m_lock);
    closeDatabase();
}

TrackAnalysisCache &TrackAnalysisCache::shared()
{
    static TrackAnalysisCache cache(defaultDatabaseFile());
    return cache;
}

QString TrackAnalysisCache::defaultDatabaseFile()
{
    if (qEnvironmentVariableIsSet("SEABASS_HOME")) {
        return pathToQString(infrastructure::paths::localRoot() / "cache" / "analysis.db");
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/seabass/analysis.db");
}

void TrackAnalysisCache::open()
{
    if (m_file.isEmpty()) {
        return;
    }
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    if (sqlite3_open_v2(m_file.toUtf8().constData(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
        != SQLITE_OK) {
        closeDatabase();
        return;
    }
    try {
        sqlite3_busy_timeout(m_db, 1000);
        exec(m_db, "PRAGMA journal_mode = WAL;", Context);
        exec(m_db, "PRAGMA synchronous = NORMAL;", Context);
        exec(m_db, "BEGIN IMMEDIATE;", Context);
        int version = 0;
        {
            Statement statement(m_db, "PRAGMA user_version;", Context);
            if (statement.step()) {
                version = statement.columnInt(0);
            }
        }
        if (version != SchemaVersion) {
            exec(m_db, "DROP TABLE IF EXISTS analyses;", Context);
        }
        exec(m_db,
             "CREATE TABLE IF NOT EXISTS analyses (key TEXT PRIMARY KEY, last_used INTEGER NOT NULL, "
             "bytes INTEGER NOT NULL, data BLOB NOT NULL);",
             Context);
        if (version != SchemaVersion) {
            exec(m_db, ("PRAGMA user_version = " + std::to_string(SchemaVersion) + ";").c_str(), Context);
        }
        exec(m_db, "COMMIT;", Context);
        Statement sum(m_db, "SELECT coalesce(sum(bytes), 0) FROM analyses;", Context);
        m_storedBytes = sum.step() ? sum.columnInt64(0) : 0;
    } catch (const std::exception &) {
        const bool rebuildIt = damaged(m_db);
        closeDatabase();
        if (rebuildIt && m_counters.rebuilds == 0) {
            rebuild();
        }
    }
}

void TrackAnalysisCache::closeDatabase()
{
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void TrackAnalysisCache::rebuild()
{
    closeDatabase();
    m_counters.rebuilds++;
    for (const char *suffix : {"", "-wal", "-shm"}) {
        QFile::remove(m_file + QLatin1String(suffix));
    }
    m_storedBytes = 0;
    open();
}

template <typename Work>
bool TrackAnalysisCache::withDatabase(Work &&work)
{
    if (m_db == nullptr) {
        return false;
    }
    try {
        work();
        return true;
    } catch (const std::exception &) {
        if (damaged(m_db) && m_counters.rebuilds == 0) {
            rebuild();
        }
        return false;
    }
}

std::optional<domain::TrackAnalysis> TrackAnalysisCache::lookup(const std::string &key)
{
    std::optional<domain::TrackAnalysis> found;
    withDatabase([&] {
        qint64 lastUsed = 0;
        {
            Statement statement(m_db, "SELECT data, last_used FROM analyses WHERE key = ?;", Context);
            statement.bind(1, key);
            if (!statement.step()) {
                return;
            }
            const std::string_view data = statement.columnBlobView(0);
            lastUsed = statement.columnInt64(1);
            found = deserialise(std::string(data));
        }
        if (!found) {
            return;
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - lastUsed >= TouchAfterSeconds) {
            Statement touch(m_db, "UPDATE analyses SET last_used = ? WHERE key = ?;", Context);
            touch.bindInt64(1, now);
            touch.bind(2, key);
            touch.run();
        }
    });
    return found;
}

void TrackAnalysisCache::store(const std::string &key, const std::string &value)
{
    const qint64 bytes = static_cast<qint64>(value.size() + key.size());
    const bool stored = withDatabase([&] {
        Statement statement(m_db, "INSERT OR REPLACE INTO analyses (key, last_used, bytes, data) VALUES (?, ?, ?, ?);",
                            Context);
        statement.bind(1, key);
        statement.bindInt64(2, QDateTime::currentSecsSinceEpoch());
        statement.bindInt64(3, bytes);
        statement.bindBlob(4, value);
        statement.run();
    });
    if (stored) {
        m_counters.stores++;
        m_storedBytes += bytes;
        evictIfNeeded();
    }
}

void TrackAnalysisCache::evictIfNeeded()
{
    if (m_storedBytes <= m_cap) {
        return;
    }
    withDatabase([&] {
        Statement evict(m_db,
                        "DELETE FROM analyses WHERE rowid IN (SELECT rowid FROM (SELECT rowid, sum(bytes) OVER "
                        "(ORDER BY last_used DESC, rowid DESC) AS kept FROM analyses) WHERE kept > ?);",
                        Context);
        evict.bindInt64(1, m_cap / 10 * 9);
        evict.run();
        Statement sum(m_db, "SELECT coalesce(sum(bytes), 0) FROM analyses;", Context);
        m_storedBytes = sum.step() ? sum.columnInt64(0) : 0;
    });
}

void TrackAnalysisCache::forgetWrittenFiles()
{
    if (infrastructure::analysisFileWriteSerial() == m_writesSeen) {
        return;
    }
    std::uint64_t now = 0;
    const std::vector<std::string> written = infrastructure::analysisFilesWrittenSince(m_writesSeen, &now);
    // A key starts with its .DAT's absolute path, and a track's .DAT, .EXT
    // and .2EX share one folder: an entry goes with any of them.
    std::set<QString> folders;
    for (const std::string &path : written) {
        folders.insert(QFileInfo(QDir::cleanPath(pathToQString(pathFromUtf8(path)))).absolutePath()
                       + QLatin1Char('/'));
    }
    const bool forgotten = withDatabase([&] {
        exec(m_db, "BEGIN IMMEDIATE;", Context);
        try {
            for (const QString &folder : folders) {
                Statement forget(m_db, "DELETE FROM analyses WHERE lower(substr(key, 1, length(?1))) = lower(?1);",
                                 Context);
                forget.bind(1, folder.toStdString());
                forget.run();
            }
            exec(m_db, "COMMIT;", Context);
        } catch (...) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw;
        }
        Statement sum(m_db, "SELECT coalesce(sum(bytes), 0) FROM analyses;", Context);
        m_storedBytes = sum.step() ? sum.columnInt64(0) : 0;
    });
    // Entries that could not be dropped are never served: the files are
    // read from the stick from now on.
    if (!forgotten) {
        closeDatabase();
    }
    m_writesSeen = now;
}

domain::TrackAnalysis TrackAnalysisCache::rekordboxAnalysis(const std::string &pioneerRoot,
                                                            const std::string &trackSourceId,
                                                            const infrastructure::rekordbox::AnlzPathIndex *pathIndex)
{
    namespace rb = infrastructure::rekordbox;
    const auto readFromStick = [&] {
        {
            const std::lock_guard<std::mutex> guard(m_lock);
            m_counters.reads++;
        }
        return rb::readTrackAnalysis(pioneerRoot, trackSourceId, nullptr, pathIndex, /*threeBandOverview=*/true);
    };
    // Taken before the files are looked at: an analysis read while one
    // of them was being written may be the old one under the new key.
    const std::uint64_t writesBefore = infrastructure::analysisFileWriteSerial();
    std::optional<std::string> analyzePath;
    try {
        const auto trackId = static_cast<std::uint32_t>(std::stoul(trackSourceId));
        analyzePath = pathIndex != nullptr ? pathIndex->pathFor(trackId) : rb::findAnlzPathForTrackId(pioneerRoot, trackId);
    } catch (const std::exception &) {
        return {};
    }
    if (!analyzePath) {
        return {};
    }
    // Only analysis files that are files on disk are kept: a browsed
    // backup serves them from its archive, already on the computer.
    const auto onDisk = [&](const std::string &extension) {
        return QFileInfo(pathToQString(pathFromUtf8(pioneerRoot)
                                       / pathFromUtf8(rb::anlzSiblingRelativePath(*analyzePath, extension))));
    };
    const QFileInfo dat = onDisk(".DAT");
    if (!dat.isFile()) {
        return readFromStick();
    }
    const QFileInfo twoEx = onDisk(".2EX");
    const std::string key = (dat.absoluteFilePath() + QLatin1Char('|') + fileStamp(dat) + QLatin1Char('|')
                             + fileStamp(twoEx))
                                .toStdString();
    {
        const std::lock_guard<std::mutex> guard(m_lock);
        forgetWrittenFiles();
        if (auto kept = lookup(key)) {
            m_counters.hits++;
            return std::move(*kept);
        }
    }
    domain::TrackAnalysis analysis = readFromStick();
    // Nothing read is not kept: the next ask reads again (a file the
    // stick has not finished writing, a read that failed).
    if ((!analysis.waveform.empty() || !analysis.beats.empty())
        && infrastructure::analysisFileWriteSerial() == writesBefore) {
        const std::string value = serialise(analysis);
        const std::lock_guard<std::mutex> guard(m_lock);
        store(key, value);
    }
    return analysis;
}

TrackAnalysisCache::Counters TrackAnalysisCache::counters() const
{
    const std::lock_guard<std::mutex> guard(m_lock);
    return m_counters;
}

bool TrackAnalysisCache::persistent() const
{
    const std::lock_guard<std::mutex> guard(m_lock);
    return m_db != nullptr;
}

}  // namespace seabass::gui
