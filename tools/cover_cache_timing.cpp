// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Times what a list of tracks pays for its covers (#76), split the way
// the cache splits it: reading the files off the stick, decoding them at
// the drawn size, and the same covers from the cover cache, in the next
// run (the database) and in this one (memory). Read-only on the stick;
// the cache it fills is a fresh one in a temporary directory, never the
// app's.
//
//   cover_cache_timing /media/sebas/STICK/PIONEER/Artwork
//   cover_cache_timing "/media/sebas/STICK/Engine Library/Artwork" 160
//
// The second argument is the decoded side in device pixels (default 80,
// a row's 40 px at 2x). On Linux every file is dropped from the page
// cache before each cold pass (posix_fadvise), so "cold" means read from
// the device; elsewhere the first pass is as cold as the system has it,
// so run it right after plugging the stick in.

#include <QBuffer>
#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>

#include <cstdio>
#include <iostream>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "gui/cover_cache.hpp"

using seabass::gui::CoverCache;

namespace
{

void dropFromPageCache(const QStringList &files)
{
#if defined(__linux__)
    for (const QString &file : files) {
        const int fd = ::open(QFile::encodeName(file).constData(), O_RDONLY);
        if (fd >= 0) {
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            ::close(fd);
        }
    }
#else
    (void)files;
#endif
}

void report(const char *what, qint64 nanoseconds, int count)
{
    std::printf("  %-34s %9.1f ms  %7.3f ms/cover\n", what, nanoseconds / 1e6, count ? nanoseconds / 1e6 / count : 0.0);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: cover_cache_timing <folder of covers> [side in device pixels, default 80]\n";
        return 2;
    }
    const QString folder = QCoreApplication::arguments().at(1);
    const int side = argc == 3 ? std::atoi(argv[2]) : 80;

    QStringList files;
    QDirIterator it(folder, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files << it.next();
    }
    files.sort();
    if (files.isEmpty()) {
        std::cerr << "no covers under " << argv[1] << "\n";
        return 1;
    }
    QTemporaryDir cacheDir;
    const QString database = cacheDir.filePath(QStringLiteral("covers.db"));

    // 1. The stick: every file read whole, nothing decoded.
    dropFromPageCache(files);
    std::vector<QByteArray> bytes;
    bytes.reserve(files.size());
    qint64 totalBytes = 0;
    QElapsedTimer timer;
    timer.start();
    for (const QString &file : files) {
        QFile f(file);
        bytes.push_back(f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
        totalBytes += bytes.back().size();
    }
    const qint64 readNs = timer.nsecsElapsed();

    // 2. Decoding those bytes at the drawn size, as the app does.
    timer.restart();
    int decoded = 0;
    for (const QByteArray &data : bytes) {
        QBuffer buffer;
        buffer.setData(data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        if (size.isValid() && std::min(size.width(), size.height()) > side) {
            reader.setScaledSize(size.scaled(side, side, Qt::KeepAspectRatioByExpanding));
        }
        decoded += reader.read().isNull() ? 0 : 1;
    }
    const qint64 decodeNs = timer.nsecsElapsed();

    // 3. Through the cache, empty: read, decode and store.
    dropFromPageCache(files);
    qint64 coldNs = 0;
    {
        CoverCache cache(database);
        timer.restart();
        for (const QString &file : files) {
            cache.cover(file, side);
        }
        coldNs = timer.nsecsElapsed();
    }

    // 4. The next run: a new cache on the same file, nothing from the stick.
    dropFromPageCache(files);
    CoverCache cache(database);
    timer.restart();
    for (const QString &file : files) {
        cache.cover(file, side);
    }
    const qint64 nextRunNs = timer.nsecsElapsed();
    const CoverCache::Counters nextRun = cache.counters();

    // 5. The same page again in this run: memory.
    timer.restart();
    for (const QString &file : files) {
        cache.cover(file, side);
    }
    const qint64 memoryNs = timer.nsecsElapsed();

    const int count = static_cast<int>(files.size());
    std::printf("%d covers, %.1f MB, decoded at %d px (%d decodable)\n", count, totalBytes / 1e6, side, decoded);
    report("read from the stick", readNs, count);
    report("decode at the drawn size", decodeNs, count);
    report("cache empty (read+decode+store)", coldNs, count);
    report("next run (from the cache file)", nextRunNs, count);
    report("same run again (memory)", memoryNs, count);
    std::printf("  next run: %d from the cache file, %d read from the stick; cache file %.1f MB\n", nextRun.diskHits,
                nextRun.fileReads, QFileInfo(database).size() / 1e6);
    return 0;
}
