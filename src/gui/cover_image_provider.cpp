// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/cover_image_provider.hpp"

#include <QQmlEngine>
#include <QUrl>

#include <atomic>

#include "gui/cover_cache.hpp"
#include "gui/playlist_thumbnail_provider.hpp"
#include "gui/pooled_image_response.hpp"

namespace seabass::gui
{

CoverImageProvider::CoverImageProvider(std::shared_ptr<CoverCache> cache) : m_cache(std::move(cache))
{
    // A few at once: one stick read waiting does not hold up the hits
    // queued behind it, and a stick is not asked for more in parallel
    // than it answers.
    m_pool.setMaxThreadCount(3);
}

CoverImageProvider::~CoverImageProvider()
{
    m_pool.clear();
    m_pool.waitForDone();
}

QString CoverImageProvider::localPathFromId(const QString &id)
{
    // The id arrives partly decoded already (QUrl's pretty form keeps
    // only what would be ambiguous encoded, "%25" among it), so decoding
    // it once more yields the file URL ArtworkImage encoded.
    const QUrl url(QUrl::fromPercentEncoding(id.toUtf8()));
    return url.isLocalFile() ? url.toLocalFile() : QString();
}

QQuickImageResponse *CoverImageProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    // ArtworkImage asks for a square; the larger side, should anything ask
    // for another shape, so the crop still has the pixels it is cut from.
    const int side = requestedSize.isValid() ? std::max(requestedSize.width(), requestedSize.height()) : 0;
    // The job keeps its own copies: the response may be gone before it
    // runs (see PooledImageResponse).
    return PooledImageResponse::start(
        m_pool, [cache = m_cache, path = localPathFromId(id), side](const std::atomic<bool> &, QString &error) {
            QImage image = path.isEmpty() ? QImage() : cache->cover(path, side);
            if (image.isNull()) {
                // Worded as Qt words a file it cannot load, which is what
                // this is, and what the storm already knows a missing
                // cover by (tests/qml-storm/StormRunner.qml).
                error = QStringLiteral("Cannot open: ") + path;
            }
            return image;
        });
}

void registerCoverImageProvider(QQmlEngine *engine)
{
    // One cache under both: a playlist's thumbnail is made from the covers
    // the track list has already decoded.
    const auto cache = std::make_shared<CoverCache>(CoverCache::defaultDatabaseFile());
    engine->addImageProvider(QStringLiteral("cover"), new CoverImageProvider(cache));
    engine->addImageProvider(QStringLiteral("playlist-thumb"), new PlaylistThumbnailProvider(cache));
}

}  // namespace seabass::gui
