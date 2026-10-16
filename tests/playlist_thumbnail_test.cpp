// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A playlist's thumbnail (PlaylistThumbnailProvider) through a real cover
// cache: each cover in the tile the mosaic gives it, for one to four
// covers; no covers, a clear image; the id the covers themselves, read
// back whatever their paths hold; a thumbnail composed once and read back
// after, in the same run and the next; and the cache's cap trimming the
// oldest entries first, thumbnails among them.

#include <QCoreApplication>
#include <QImage>
#include <QString>
#include <QUrl>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "gui/cover_cache.hpp"
#include "gui/playlist_thumbnail_provider.hpp"
#include "gui/qt_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::gui::CoverCache;
using seabass::gui::pathToQString;
using seabass::gui::PlaylistThumbnailProvider;

namespace
{

fs::path g_scratch;
int g_failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++g_failures;
    }
}

QString writeCover(const std::string &name, QColor colour)
{
    const fs::path file = g_scratch / "covers" / name;
    fs::create_directories(file.parent_path());
    QImage image(80, 60, QImage::Format_RGB32);
    image.fill(colour);
    const QString path = pathToQString(file);
    image.save(path, "PNG");
    return path;
}

QString urlOf(const QString &path)
{
    return QUrl::fromLocalFile(path).toString();
}

QString database(const std::string &name)
{
    return pathToQString(g_scratch / name / "covers.db");
}

bool near(QColor got, QColor want)
{
    return std::abs(got.red() - want.red()) < 12 && std::abs(got.green() - want.green()) < 12
        && std::abs(got.blue() - want.blue()) < 12 && got.alpha() > 240;
}

std::string describe(QColor c)
{
    return std::to_string(c.red()) + "," + std::to_string(c.green()) + "," + std::to_string(c.blue()) + " a"
        + std::to_string(c.alpha());
}

const QColor Colours[] = {QColor(220, 30, 30), QColor(30, 180, 60), QColor(40, 70, 220), QColor(230, 200, 30)};

// Each cover in its tile: sampled in the middle of where the mosaic puts
// it, at 48 px. One fills; two split down the middle; three, the first
// on the left half and the other two stacked on the right; four, a grid.
void eachCoverInItsTile()
{
    PlaylistThumbnailProvider provider(std::make_shared<CoverCache>(database("tiles-provider")));
    QStringList urls;
    for (int i = 0; i < 4; ++i) {
        urls << urlOf(writeCover("tile-" + std::to_string(i) + ".png", Colours[i]));
    }
    const QList<QList<QPoint>> centres = {
        {{24, 24}},
        {{12, 24}, {36, 24}},
        {{12, 24}, {36, 12}, {36, 36}},
        {{12, 12}, {36, 12}, {12, 36}, {36, 36}},
    };
    for (int n = 1; n <= 4; ++n) {
        const QString id = PlaylistThumbnailProvider::idForCovers(urls.mid(0, n));
        const QImage image = provider.thumbnail(id, 48);
        check(image.size() == QSize(48, 48), std::to_string(n) + " covers: 48 x 48");
        for (int i = 0; i < n; ++i) {
            const QPoint at = centres[n - 1][i];
            const QColor got = image.pixelColor(at);
            check(near(got, Colours[i]), std::to_string(n) + " covers: cover " + std::to_string(i) + " at "
                                             + std::to_string(at.x()) + "," + std::to_string(at.y()) + " is "
                                             + describe(got));
        }
    }
    // The gap between tiles and the rounded corner are clear.
    const QImage four = provider.thumbnail(PlaylistThumbnailProvider::idForCovers(urls), 48);
    check(four.pixelColor(24, 12).alpha() < 40, "the gap between two tiles is clear: " + describe(four.pixelColor(24, 12)));
    check(four.pixelColor(0, 0).alpha() < 40, "the corner is rounded off: " + describe(four.pixelColor(0, 0)));
    // A cover that cannot be read leaves its tile clear, the rest drawn.
    const QString missing = urlOf(pathToQString(g_scratch / "covers" / "not-there.png"));
    const QImage withGap = provider.thumbnail(PlaylistThumbnailProvider::idForCovers({urls[0], missing}), 48);
    check(near(withGap.pixelColor(12, 24), Colours[0]), "the readable cover drawn");
    check(withGap.pixelColor(36, 24).alpha() < 40, "the missing one's tile clear");
}

// No covers: no id, and asked anyway, a clear image of the side asked.
void noCoversIsAClearImage()
{
    PlaylistThumbnailProvider provider(std::make_shared<CoverCache>(database("none")));
    check(PlaylistThumbnailProvider::idForCovers({}).isEmpty(), "no covers, no id");
    check(PlaylistThumbnailProvider::idForCovers({QString(), QString()}).isEmpty(), "empty sources, no id");
    const QImage clear = provider.thumbnail(QString(), 40);
    check(clear.size() == QSize(40, 40), "a clear image of the side asked");
    check(clear.pixelColor(20, 20).alpha() == 0, "and clear");
    check(provider.compositions() == 0, "nothing composed for it");
}

// The id is the first four distinct covers, in order, and gives them back
// whatever their paths hold, the separator included.
void theIdIsTheCovers()
{
    const QString odd = urlOf(QStringLiteral("/music/AC|DC 100% Björk #1/a b.jpg"));
    const QString plain = urlOf(QStringLiteral("/music/plain.jpg"));
    const QStringList urls = {odd, plain, odd, urlOf("/c.jpg"), urlOf("/d.jpg"), urlOf("/e.jpg")};
    const QString id = PlaylistThumbnailProvider::idForCovers(urls);
    const QStringList back = PlaylistThumbnailProvider::coverPathsFromId(id);
    check(back.size() == 4, "four covers, the repeat dropped: " + std::to_string(back.size()));
    check(back.value(0) == QStringLiteral("/music/AC|DC 100% Björk #1/a b.jpg"), "an odd path read back: "
                                                                                     + back.value(0).toStdString());
    check(back.value(1) == QStringLiteral("/music/plain.jpg") && back.value(2) == QStringLiteral("/c.jpg")
              && back.value(3) == QStringLiteral("/d.jpg"),
          "in order");
    check(id.count(QLatin1Char('.')) == 3, "three separators, none left from the paths");

    // And as the QML engine hands it over: the id taken back out of the
    // image URL it was put in (QQuickPixmap's own way). A separator the
    // URL re-encodes ("|" became "%7C") ran every path into one.
    const QUrl source(QStringLiteral("image://playlist-thumb/") + id);
    const QString handed = source.toString(QUrl::RemoveScheme | QUrl::RemoveAuthority).mid(1);
    const QStringList fromEngine = PlaylistThumbnailProvider::coverPathsFromId(handed);
    check(fromEngine == back, "the same four covers through the engine's URL: "
                                  + QString::number(fromEngine.size()).toStdString() + " "
                                  + fromEngine.value(0).toStdString());
}

// Composed once: the second request reads the kept one, and so does the
// next run, from the file.
void composedOnceAndReadBack()
{
    const QString db = database("kept");
    const QStringList urls = {urlOf(writeCover("kept-0.png", Colours[0])), urlOf(writeCover("kept-1.png", Colours[1]))};
    const QString id = PlaylistThumbnailProvider::idForCovers(urls);
    QImage first;
    {
        auto cache = std::make_shared<CoverCache>(db);
        PlaylistThumbnailProvider provider(cache);
        first = provider.thumbnail(id, 48);
        check(provider.compositions() == 1, "composed the first time");
        check(provider.thumbnail(id, 48) == first, "the same image the second time");
        check(provider.compositions() == 1, "not composed again: " + std::to_string(provider.compositions()));
        cache->dropMemory();
        check(provider.thumbnail(id, 48) == first, "from the database once the memory is gone");
        check(provider.compositions() == 1, "still not composed again");
        provider.thumbnail(id, 96);
        check(provider.compositions() == 2, "another side is another image");
    }
    auto cache = std::make_shared<CoverCache>(db);
    PlaylistThumbnailProvider provider(cache);
    check(provider.thumbnail(id, 48) == first, "the next run reads it back");
    check(provider.compositions() == 0, "and composes nothing");
    check(cache->counters().fileReads == 0, "nor reads a cover");
}

// Over the cap, the oldest go first, thumbnails among them.
void theCapTrimsTheOldestFirst()
{
    const QString db = database("cap");
    QImage image(48, 48, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    // 48 x 48 pixels are 9216 bytes; room for two, not three.
    {
        CoverCache cache(db, 25000);
        cache.keepImage(QStringLiteral("playlist:a:48"), image);
        cache.keepImage(QStringLiteral("playlist:b:48"), image);
        check(cache.storedBytes() > 18432, "two kept: " + std::to_string(cache.storedBytes()));
        cache.keepImage(QStringLiteral("playlist:c:48"), image);
        check(cache.storedBytes() <= 25000, "under the cap: " + std::to_string(cache.storedBytes()));
    }
    CoverCache cache(db, 25000);
    check(cache.storedImage(QStringLiteral("playlist:a:48")).isNull(), "the oldest trimmed");
    check(!cache.storedImage(QStringLiteral("playlist:b:48")).isNull(), "the next kept");
    check(!cache.storedImage(QStringLiteral("playlist:c:48")).isNull(), "the newest kept");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    g_scratch = seabass::testing::scratchRoot() / "playlist-thumbnail-test";
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);

    eachCoverInItsTile();
    noCoversIsAClearImage();
    theIdIsTheCovers();
    composedOnceAndReadBack();
    theCapTrimsTheOldestFirst();

    if (g_failures > 0) {
        std::cerr << "playlist_thumbnail_test: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "playlist_thumbnail_test: all cases passed\n";
    return 0;
}
