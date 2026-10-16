// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/cover_cache.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>

#include <cstring>
#include <stdexcept>

#include <sqlite3.h>

#include "gui/qt_path.hpp"
#include "infrastructure/local/sqlite_statement.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

namespace seabass::gui
{

using infrastructure::local::exec;
using infrastructure::local::Statement;

namespace
{

constexpr const char *Context = "cover cache";
// The layout below. Any other number is a file from another build: it is
// emptied, not read.
constexpr int SchemaVersion = 1;
// How stale a cover's last use may get before a hit writes it again: one
// write per cover per minute at most, not one per row drawn.
constexpr qint64 TouchAfterSeconds = 60;

enum Kind : int
{
    Pixels = 0,   // QImage::Format_ARGB32_Premultiplied, width * 4 bytes a line
    Encoded = 1,  // JPEG or PNG
};

bool damaged(sqlite3 *db)
{
    const int code = sqlite3_errcode(db) & 0xff;
    return code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
}

// `scaled` says whether it came out smaller than the file's own image.
QImage decode(const QByteArray &bytes, int side, bool &scaled)
{
    scaled = false;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    // Scaled while decoding (a JPEG scales in its DCT), to cover the
    // square the art is cropped into: the shorter side becomes `side`.
    // Never enlarged.
    const QSize size = reader.size();
    if (side > 0 && size.isValid() && std::min(size.width(), size.height()) > side) {
        reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatioByExpanding));
        scaled = true;
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

}  // namespace

CoverCache::CoverCache(QString databaseFile, std::int64_t capBytes, std::int64_t memoryCapBytes)
    : m_file(std::move(databaseFile)), m_cap(capBytes)
{
    // QCache counts cost in whatever unit it is given; kilobytes keep a
    // large image's cost inside an int.
    m_memory.setMaxCost(static_cast<qsizetype>(memoryCapBytes / 1024));
    const std::lock_guard<std::mutex> guard(m_lock);
    open();
}

CoverCache::~CoverCache()
{
    const std::lock_guard<std::mutex> guard(m_lock);
    closeDatabase();
}

QString CoverCache::defaultDatabaseFile()
{
    if (qEnvironmentVariableIsSet("SEABASS_HOME")) {
        return pathToQString(infrastructure::paths::localRoot() / "cache" / "covers.db");
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/seabass/covers.db");
}

void CoverCache::open()
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
        // Another window may be writing: wait a moment rather than fail,
        // and a write that still cannot get in is a cover not kept.
        sqlite3_busy_timeout(m_db, 1000);
        exec(m_db, "PRAGMA journal_mode = WAL;", Context);
        exec(m_db, "PRAGMA synchronous = NORMAL;", Context);
        exec(m_db, "PRAGMA mmap_size = 268435456;", Context);
        // The check and the making in one transaction: two windows starting
        // together on a new file must not both find it empty, and the one
        // second drop the table the first has just filled.
        exec(m_db, "BEGIN IMMEDIATE;", Context);
        int version = 0;
        {
            Statement statement(m_db, "PRAGMA user_version;", Context);
            if (statement.step()) {
                version = statement.columnInt(0);
            }
        }
        if (version != SchemaVersion) {
            exec(m_db, "DROP TABLE IF EXISTS covers;", Context);
        }
        // The image last, so reading the other columns (the size sum,
        // eviction) stops before the blob's overflow pages.
        exec(m_db,
             "CREATE TABLE IF NOT EXISTS covers (key TEXT PRIMARY KEY, last_used INTEGER NOT NULL, "
             "bytes INTEGER NOT NULL, kind INTEGER NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL, "
             "data BLOB NOT NULL);",
             Context);
        if (version != SchemaVersion) {
            exec(m_db, ("PRAGMA user_version = " + std::to_string(SchemaVersion) + ";").c_str(), Context);
        }
        exec(m_db, "COMMIT;", Context);
        Statement sum(m_db, "SELECT coalesce(sum(bytes), 0) FROM covers;", Context);
        m_storedBytes = sum.step() ? sum.columnInt64(0) : 0;
    } catch (const std::exception &) {
        const bool rebuildIt = damaged(m_db);
        closeDatabase();
        if (rebuildIt && m_counters.rebuilds == 0) {
            rebuild();
        }
    }
}

void CoverCache::closeDatabase()
{
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

void CoverCache::rebuild()
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
bool CoverCache::withDatabase(Work &&work)
{
    if (m_db == nullptr) {
        return false;
    }
    try {
        work();
        return true;
    } catch (const std::exception &) {
        // Busy is a cover not kept this time; damage is a new file, once
        // per run, so a disk that keeps damaging it does not loop.
        if (damaged(m_db) && m_counters.rebuilds == 0) {
            rebuild();
        }
        return false;
    }
}

bool CoverCache::lookup(const QString &key, QImage &image)
{
    bool found = false;
    withDatabase([&] {
        const std::string utf8Key = key.toStdString();
        qint64 lastUsed = 0;
        {
            Statement statement(m_db, "SELECT kind, width, height, data, last_used FROM covers WHERE key = ?;",
                                Context);
            statement.bind(1, utf8Key);
            if (!statement.step()) {
                return;
            }
            const int kind = statement.columnInt(0);
            const int width = statement.columnInt(1);
            const int height = statement.columnInt(2);
            const std::string_view data = statement.columnBlobView(3);
            lastUsed = statement.columnInt64(4);
            if (kind == Pixels && width > 0 && height > 0
                && data.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
                QImage pixels(width, height, QImage::Format_ARGB32_Premultiplied);
                if (pixels.isNull()) {
                    return;
                }
                for (int y = 0; y < height; ++y) {
                    std::memcpy(pixels.scanLine(y), data.data() + static_cast<size_t>(y) * width * 4,
                                static_cast<size_t>(width) * 4);
                }
                image = std::move(pixels);
            } else if (kind == Encoded) {
                QImage encoded;
                if (!encoded.loadFromData(reinterpret_cast<const uchar *>(data.data()), static_cast<int>(data.size()))) {
                    return;
                }
                image = encoded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            } else {
                return;
            }
        }
        found = true;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - lastUsed >= TouchAfterSeconds) {
            Statement touch(m_db, "UPDATE covers SET last_used = ? WHERE key = ?;", Context);
            touch.bindInt64(1, now);
            touch.bind(2, utf8Key);
            touch.run();
        }
    });
    return found;
}

void CoverCache::store(const QString &key, const QImage &image, const QByteArray &original, bool scaled)
{
    QByteArray encoded;
    int kind = Pixels;
    if (std::max(image.width(), image.height()) > MaxPixelSide) {
        kind = Encoded;
        if (!scaled) {
            // Drawn at its own size: the file's bytes are the smallest
            // copy there is, and lose nothing. Pixels would not be: a
            // rekordbox 240 px cover is 9 KB as a JPEG and 230 KB as
            // pixels, and at pane size WHALESHARK2's 3,161 of them came to
            // 730 MB, evicting each other in a single pass.
            encoded = original;
        } else {
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, image.hasAlphaChannel() ? "PNG" : "JPG", 90)) {
                return;
            }
        }
    }
    const std::string utf8Key = key.toStdString();
    const qint64 bytes = (kind == Pixels ? static_cast<qint64>(image.width()) * image.height() * 4 : encoded.size())
        + static_cast<qint64>(utf8Key.size());
    const bool stored = withDatabase([&] {
        Statement statement(m_db,
                            "INSERT OR REPLACE INTO covers (key, last_used, bytes, kind, width, height, data) "
                            "VALUES (?, ?, ?, ?, ?, ?, ?);",
                            Context);
        statement.bind(1, utf8Key);
        statement.bindInt64(2, QDateTime::currentSecsSinceEpoch());
        statement.bindInt64(3, bytes);
        statement.bind(4, kind);
        statement.bind(5, image.width());
        statement.bind(6, image.height());
        if (kind == Pixels) {
            // Format_ARGB32_Premultiplied lines are width * 4 bytes, already
            // 4-byte aligned, so the image's bits are the rows end to end.
            statement.bindBlob(7, image.constBits(), static_cast<int>(image.sizeInBytes()));
        } else {
            statement.bindBlob(7, encoded.constData(), static_cast<int>(encoded.size()));
        }
        statement.run();
    });
    if (stored) {
        m_counters.stores++;
        m_storedBytes += bytes;
        evictIfNeeded();
    }
}

void CoverCache::evictIfNeeded()
{
    if (m_storedBytes <= m_cap) {
        return;
    }
    withDatabase([&] {
        // Down to nine tenths of the cap, so the next few covers do not each
        // evict again; the most recently used kept, newest first among
        // covers used in the same second. The pages freed are reused by the
        // next covers, so the file stays at about the cap.
        Statement evict(m_db,
                        "DELETE FROM covers WHERE rowid IN (SELECT rowid FROM (SELECT rowid, sum(bytes) OVER "
                        "(ORDER BY last_used DESC, rowid DESC) AS kept FROM covers) WHERE kept > ?);",
                        Context);
        evict.bindInt64(1, m_cap / 10 * 9);
        evict.run();
        Statement sum(m_db, "SELECT coalesce(sum(bytes), 0) FROM covers;", Context);
        m_storedBytes = sum.step() ? sum.columnInt64(0) : 0;
    });
}

QImage CoverCache::cover(const QString &localPath, int side)
{
    const QFileInfo info(localPath);
    if (!info.isFile()) {
        return {};
    }
    side = std::max(side, 0);
    const QString key = localPath + QLatin1Char('|') + QString::number(info.size()) + QLatin1Char('|')
        + QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|')
        + QString::number(side);
    if (side > 0) {
        const std::lock_guard<std::mutex> guard(m_lock);
        if (const QImage *kept = m_memory.object(key)) {
            m_counters.memoryHits++;
            return *kept;
        }
        QImage stored;
        if (lookup(key, stored)) {
            m_counters.diskHits++;
            m_memory.insert(key, new QImage(stored), static_cast<qsizetype>(stored.sizeInBytes() / 1024 + 1));
            return stored;
        }
    }

    // The read and the decode outside the lock: they are the slow part,
    // and other covers can be served from the cache meanwhile.
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray bytes = file.readAll();
    file.close();
    bool scaled = false;
    const QImage image = decode(bytes, side, scaled);
    {
        const std::lock_guard<std::mutex> guard(m_lock);
        m_counters.fileReads++;
        if (!image.isNull() && side > 0) {
            m_memory.insert(key, new QImage(image), static_cast<qsizetype>(image.sizeInBytes() / 1024 + 1));
            store(key, image, bytes, scaled);
        }
    }
    return image;
}

QImage CoverCache::storedImage(const QString &key)
{
    const std::lock_guard<std::mutex> guard(m_lock);
    if (const QImage *kept = m_memory.object(key)) {
        m_counters.memoryHits++;
        return *kept;
    }
    QImage stored;
    if (lookup(key, stored)) {
        m_counters.diskHits++;
        m_memory.insert(key, new QImage(stored), static_cast<qsizetype>(stored.sizeInBytes() / 1024 + 1));
        return stored;
    }
    return {};
}

void CoverCache::keepImage(const QString &key, const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const std::lock_guard<std::mutex> guard(m_lock);
    m_memory.insert(key, new QImage(pixels), static_cast<qsizetype>(pixels.sizeInBytes() / 1024 + 1));
    // No file behind it: a large one is encoded (scaled says so), a small
    // one kept as pixels like a row's cover.
    store(key, pixels, QByteArray(), true);
}

CoverCache::Counters CoverCache::counters() const
{
    const std::lock_guard<std::mutex> guard(m_lock);
    return m_counters;
}

bool CoverCache::persistent() const
{
    const std::lock_guard<std::mutex> guard(m_lock);
    return m_db != nullptr;
}

std::int64_t CoverCache::storedBytes()
{
    const std::lock_guard<std::mutex> guard(m_lock);
    return m_storedBytes;
}

void CoverCache::dropMemory()
{
    const std::lock_guard<std::mutex> guard(m_lock);
    m_memory.clear();
}

}  // namespace seabass::gui
