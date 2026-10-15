// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The cover cache (#76): a hit reads nothing from the cover's file, a
// cover whose bytes changed under the same name is read again, eviction
// keeps the file under its cap, two processes share it, and a damaged or
// unusable file never costs a cover.

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QString>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "gui/cover_cache.hpp"
#include "gui/qt_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::gui::CoverCache;
using seabass::gui::pathToQString;

namespace
{

fs::path g_scratch;

QString writeImage(const fs::path &file, QSize size, QColor colour, const char *format = "PNG")
{
    fs::create_directories(file.parent_path());
    QImage image(size, QImage::Format_RGB32);
    image.fill(colour);
    const QString path = pathToQString(file);
    const bool saved = image.save(path, format);
    assert(saved);
    return path;
}

void setModified(const QString &path, const QDateTime &when)
{
    QFile file(path);
    const bool opened = file.open(QIODevice::ReadWrite);
    assert(opened);
    const bool set = file.setFileTime(when, QFileDevice::FileModificationTime);
    assert(set);
}

QString database(const std::string &name)
{
    return pathToQString(g_scratch / name / "covers.db");
}

// The child of twoProcessesShareTheFile(): fills the cache with its own
// covers while the parent fills it with others. Every write has to land:
// one turned away because the other process held the file is a failure.
int runChild(const QString &db, const QString &folder, int count)
{
    CoverCache cache(db);
    if (!cache.persistent()) {
        return 2;
    }
    for (int i = 0; i < count; ++i) {
        const QString path = folder + QStringLiteral("/child-%1.png").arg(i);
        if (cache.cover(path, 48).isNull()) {
            return 3;
        }
    }
    return cache.counters().stores == count ? 0 : 4;
}

void aHitReadsNothingFromTheFile()
{
    const QString cover = writeImage(g_scratch / "hit" / "a.png", QSize(400, 300), Qt::red);
    const QString db = database("hit");
    QImage first;
    {
        CoverCache cache(db);
        assert(cache.persistent());
        first = cache.cover(cover, 64);
        assert(!first.isNull());
        // Covers the 64 px square, the way the art is cropped into one.
        assert(first.height() == 64 && first.width() == 85);
        assert(cache.counters().fileReads == 1 && cache.counters().stores == 1);

        assert(cache.cover(cover, 64) == first);
        assert(cache.counters().memoryHits == 1);
        cache.dropMemory();
        assert(cache.cover(cover, 64) == first);
        assert(cache.counters().diskHits == 1);
        assert(cache.counters().fileReads == 1);
    }
    // The next run: from the file, not from the cover.
    CoverCache again(db);
    assert(again.cover(cover, 64) == first);
    assert(again.counters().diskHits == 1 && again.counters().fileReads == 0);
    // Another size is another entry.
    assert(again.cover(cover, 32).height() == 32);
    assert(again.counters().fileReads == 1);
}

// A pane-sized cover is kept encoded and comes back the same size.
void aLargeCoverIsKeptEncoded()
{
    const QString cover = writeImage(g_scratch / "large" / "a.png", QSize(900, 900), Qt::blue);
    CoverCache cache(database("large"));
    const QImage first = cache.cover(cover, 600);
    assert(first.size() == QSize(600, 600));
    cache.dropMemory();
    const QImage hit = cache.cover(cover, 600);
    assert(cache.counters().diskHits == 1);
    assert(hit.size() == QSize(600, 600));
    const QColor centre = hit.pixelColor(300, 300);
    assert(centre.blue() > 240 && centre.red() < 15 && centre.green() < 15);
}

// A pane asking for more than the cover has (rekordbox's 240 px _m.jpg
// at 600): kept as the file's own bytes, not as 230 KB of pixels.
void aCoverAtItsOwnSizeIsKeptAsItsFile()
{
    const fs::path file = g_scratch / "own-size" / "a1_m.jpg";
    const QString cover = writeImage(file, QSize(240, 240), Qt::magenta, "JPG");
    CoverCache cache(database("own-size"));
    const QImage first = cache.cover(cover, 600);
    assert(first.size() == QSize(240, 240));
    const auto fileBytes = static_cast<std::int64_t>(fs::file_size(file));
    assert(cache.storedBytes() >= fileBytes && cache.storedBytes() < fileBytes + 1024);
    cache.dropMemory();
    assert(cache.cover(cover, 600) == first);
    assert(cache.counters().diskHits == 1);
}

// What a rekordbox re-export does: another picture under the same name.
// The same size, too (two uncompressed BMPs of one size), so only the time
// tells.
void changedBytesUnderTheSameNameAreReadAgain()
{
    const fs::path file = g_scratch / "changed" / "a5.bmp";
    const QString cover = writeImage(file, QSize(200, 200), Qt::red, "BMP");
    const QDateTime then = QDateTime::currentDateTimeUtc().addSecs(-3600);
    setModified(cover, then);
    const auto redSize = fs::file_size(file);
    CoverCache cache(database("changed"));
    assert(cache.cover(cover, 50).pixelColor(25, 25) == QColor(Qt::red));

    writeImage(file, QSize(200, 200), Qt::green, "BMP");
    assert(fs::file_size(file) == redSize);
    setModified(cover, then.addSecs(5));
    const QImage now = cache.cover(cover, 50);
    assert(now.pixelColor(25, 25) == QColor(Qt::green));
    assert(cache.counters().fileReads == 2);
    cache.dropMemory();
    assert(cache.cover(cover, 50).pixelColor(25, 25) == QColor(Qt::green));
}

void evictionKeepsTheFileUnderItsCap()
{
    const std::int64_t cap = 400 * 1024;
    const QString db = database("evict");
    CoverCache cache(db, cap);
    QString firstCover;
    QString lastCover;
    // 100 x 100 pixels are 40 KB each: 30 of them are three times the cap.
    for (int i = 0; i < 30; ++i) {
        const QString cover = writeImage(g_scratch / "evict" / ("c" + std::to_string(i) + ".png"), QSize(100, 100),
                                         QColor(i * 8, 0, 0));
        assert(!cache.cover(cover, 100).isNull());
        if (i == 0) {
            firstCover = cover;
        }
        lastCover = cover;
        assert(cache.storedBytes() <= cap);
    }
    assert(cache.counters().stores == 30);
    // The file, not just the count: freed pages are reused, so it stays at
    // about the cap however many covers went through it.
    const auto fileSize = static_cast<std::int64_t>(fs::file_size(seabass::gui::pathFromQString(db)));
    assert(fileSize <= cap + 64 * 1024);

    cache.dropMemory();
    const int reads = cache.counters().fileReads;
    cache.cover(lastCover, 100);
    assert(cache.counters().fileReads == reads && "the newest is kept");
    cache.cover(firstCover, 100);
    assert(cache.counters().fileReads == reads + 1 && "the oldest went first");
}

void twoProcessesShareTheFile()
{
    const QString db = database("shared");
    const QString folder = pathToQString(g_scratch / "shared" / "covers");
    const int count = 200;
    // Every cover written first, so the two then do nothing but use the
    // cache, at the same time.
    for (const char *who : {"parent-", "child-"}) {
        for (int i = 0; i < count; ++i) {
            writeImage(g_scratch / "shared" / "covers" / (who + std::to_string(i) + ".png"), QSize(120, 120),
                       QColor(0, i % 256, 200));
        }
    }
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--child"), db, folder, QString::number(count)});
    const bool started = child.waitForStarted(10000);
    assert(started);
    {
        CoverCache cache(db);
        for (int i = 0; i < count; ++i) {
            assert(!cache.cover(folder + QStringLiteral("/parent-%1.png").arg(i), 48).isNull());
        }
        assert(cache.counters().stores == count);
    }
    const bool finished = child.waitForFinished(60000);
    assert(finished);
    if (child.exitCode() != 0) {
        std::cerr << "child exited " << child.exitCode() << ": " << child.readAllStandardError().toStdString() << "\n";
    }
    assert(child.exitCode() == 0);

    // Both sets are there for a third.
    CoverCache reader(db);
    for (const char *who : {"parent-", "child-"}) {
        for (int i = 0; i < count; ++i) {
            reader.cover(pathToQString(g_scratch / "shared" / "covers" / (who + std::to_string(i) + ".png")), 48);
        }
    }
    assert(reader.counters().diskHits == 2 * count);
    assert(reader.counters().fileReads == 0);
}

void aDamagedFileIsMadeAgain()
{
    const QString db = database("damaged");
    fs::create_directories(seabass::gui::pathFromQString(db).parent_path());
    {
        std::ofstream garbage(seabass::gui::pathFromQString(db), std::ios::binary);
        garbage << std::string(8192, 'x');
    }
    const QString cover = writeImage(g_scratch / "damaged" / "a.png", QSize(100, 100), Qt::yellow);
    CoverCache cache(db);
    assert(cache.persistent());
    assert(cache.counters().rebuilds == 1);
    assert(!cache.cover(cover, 40).isNull());
    assert(cache.counters().stores == 1);
}

// No database at all (its path is a directory): covers still load, from
// their files, every time.
void anUnusableFileCostsNoCover()
{
    const fs::path blocked = g_scratch / "unusable" / "covers.db";
    fs::create_directories(blocked);
    const QString cover = writeImage(g_scratch / "unusable" / "a.png", QSize(100, 100), Qt::cyan);
    CoverCache cache(pathToQString(blocked));
    assert(!cache.persistent());
    assert(!cache.cover(cover, 40).isNull());
    assert(!cache.cover(cover, 40).isNull());
    assert(cache.counters().memoryHits == 1);
}

void missingAndBrokenCoversAreNull()
{
    CoverCache cache(database("missing"));
    assert(cache.cover(pathToQString(g_scratch / "missing" / "nothing.jpg"), 40).isNull());
    const fs::path broken = g_scratch / "missing" / "broken.jpg";
    {
        std::ofstream out(broken, std::ios::binary);
        out << "not an image";
    }
    assert(cache.cover(pathToQString(broken), 40).isNull());
    assert(cache.counters().stores == 0);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 5 && std::strcmp(argv[1], "--child") == 0) {
        return runChild(QString::fromUtf8(argv[2]), QString::fromUtf8(argv[3]), std::atoi(argv[4]));
    }
    g_scratch = seabass::testing::scratchRoot() / "cover-cache-test";
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);

    aHitReadsNothingFromTheFile();
    aLargeCoverIsKeptEncoded();
    aCoverAtItsOwnSizeIsKeptAsItsFile();
    changedBytesUnderTheSameNameAreReadAgain();
    evictionKeepsTheFileUnderItsCap();
    twoProcessesShareTheFile();
    aDamagedFileIsMadeAgain();
    anUnusableFileCostsNoCover();
    missingAndBrokenCoversAreNull();

    std::cout << "cover_cache_test: all cases passed\n";
    return 0;
}
