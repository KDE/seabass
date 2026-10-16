// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCache>
#include <QImage>
#include <QString>

#include <cstdint>
#include <mutex>

struct sqlite3;

namespace seabass::gui
{

// Cover art kept on the computer, decoded, so a list of tracks shows its
// covers without reading them off the stick again. Every visit to a page
// read every visible cover from the stick, many small reads over USB, and
// nothing was kept between visits or between runs; cover images do not
// change, so they are worth keeping (#76).
//
// One SQLite file (defaultDatabaseFile()), read through a memory map, in
// WAL mode so two windows can use it at once. Not one file per cover:
// Defender scans every file opened on Windows, and thousands of small
// files are thousands of scans. A small in-memory cache sits on top, so
// scrolling back and forth does not even reach the file.
//
// Keyed by content, never by the cover's name alone: a rekordbox
// re-export can put a different picture under the same aNNN.jpg, so the
// key is the path with the file's size and modification time, and the
// size it was decoded to. A changed image changes the key, and a stale
// entry is never found, only evicted. Engine's covers are files named by
// their own hash, so the same key holds for them.
//
// Row-sized covers are stored as the pixels Qt draws, so a hit is a copy
// with no decode; larger ones (the detail pane) as the file's own bytes
// when drawn at its own size, or else as JPEG, or PNG when they have
// alpha. The file is capped (about 200 MB) and the least
// recently used covers go first.
//
// A cache and never a source of truth: a database that cannot be opened,
// is not a database, or is damaged is deleted and made again, and if
// that fails too the covers are read from the stick as before. Nothing
// here ever fails a cover that could be read.
//
// Thread-safe: the image provider calls cover() from several threads.
class CoverCache
{
public:
    static constexpr std::int64_t DefaultCapBytes = 200LL * 1024 * 1024;
    static constexpr std::int64_t DefaultMemoryCapBytes = 32LL * 1024 * 1024;
    // Decoded covers up to this side are stored as pixels, larger ones
    // encoded: a row's 40 px at 2x is 25 KB of pixels, a pane's 600 px
    // would be 1.4 MB.
    static constexpr int MaxPixelSide = 128;

    explicit CoverCache(QString databaseFile, std::int64_t capBytes = DefaultCapBytes,
                        std::int64_t memoryCapBytes = DefaultMemoryCapBytes);
    ~CoverCache();

    CoverCache(const CoverCache &) = delete;
    CoverCache &operator=(const CoverCache &) = delete;

    // The image at localPath, scaled to cover a side x side square (the
    // way the art is cropped to fill one), never enlarged; side 0 is the
    // image's own size and is not kept. Null when the file is missing,
    // unreadable or no image.
    QImage cover(const QString &localPath, int side);

    // An image the app made itself and keeps here under a key of its own
    // (a playlist's composed thumbnail: PlaylistThumbnailProvider), so the
    // next request, in this run or the next, does not make it again. The
    // key is the caller's to make content-addressed; such entries share
    // the cap and its eviction with the covers. storedImage() is null when
    // nothing is kept under the key.
    QImage storedImage(const QString &key);
    void keepImage(const QString &key, const QImage &image);

    // Where the app keeps it: the platform's cache directory
    // (~/.cache/seabass/covers.db on Linux), or under SEABASS_HOME when
    // that is set, so a sandboxed run never touches the real one.
    static QString defaultDatabaseFile();

    // What cover() did, for tests and the timing tool.
    struct Counters
    {
        int fileReads = 0;    // covers read from their file
        int memoryHits = 0;
        int diskHits = 0;     // from the database
        int stores = 0;
        int rebuilds = 0;     // the database deleted and made again
    };
    Counters counters() const;

    // Whether covers are kept in the database at all (false when it could
    // not be opened or made).
    bool persistent() const;

    // The bytes of image data stored, as eviction counts them.
    std::int64_t storedBytes();

    // Empties the in-memory layer, so the next cover() goes to the
    // database: what a second run of the app would see.
    void dropMemory();

private:
    void open();
    void closeDatabase();
    void rebuild();
    // Runs `work` on the database under the lock; a damaged database is
    // rebuilt and the work counted as a miss.
    template <typename Work>
    bool withDatabase(Work &&work);
    bool lookup(const QString &key, QImage &image);
    // `original` is the file's bytes, kept as they are when the image
    // was not scaled.
    void store(const QString &key, const QImage &image, const QByteArray &original, bool scaled);
    void evictIfNeeded();

    QString m_file;
    std::int64_t m_cap;
    mutable std::mutex m_lock;
    sqlite3 *m_db = nullptr;
    std::int64_t m_storedBytes = 0;
    QCache<QString, QImage> m_memory;
    Counters m_counters;
};

}  // namespace seabass::gui
