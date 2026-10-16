// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/playlist_thumbnail_provider.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QQuickTextureFactory>
#include <QRectF>
#include <QRunnable>
#include <QUrl>

#include <algorithm>
#include <cmath>

#include "gui/cover_cache.hpp"

namespace seabass::gui
{

namespace
{

constexpr int MaxTiles = 4;
// The corners' radius, as a share of the side: the row's
// Theme.cornerRadius / 2 at its 24 px.
constexpr double CornerFraction = 1.0 / 12.0;
const QLatin1Char Separator('|');

// Where tile `i` of `n` goes, in fractions of the square: as
// PlaylistCoverMosaic.qml's tileRect().
QRectF tileFraction(int i, int n)
{
    if (n <= 1) {
        return {0, 0, 1, 1};
    }
    if (n == 2) {
        return {i * 0.5, 0, 0.5, 1};
    }
    if (n == 3) {
        return i == 0 ? QRectF(0, 0, 0.5, 1) : QRectF(0.5, (i - 1) * 0.5, 0.5, 0.5);
    }
    return {(i % 2) * 0.5, (i / 2) * 0.5, 0.5, 0.5};
}

// The gap between tiles at `side` pixels: the mosaic's 2 px
// (Theme.chartSliceGap) at its 96, in proportion, never less than one.
double gapFor(int side)
{
    return std::max(1.0, std::round(side * 2.0 / 96.0));
}

// The tile's place in pixels, half the gap off each inner edge, as the
// mosaic places its tiles.
QRect tileRect(int i, int n, int side)
{
    const QRectF f = tileFraction(i, n);
    const double half = gapFor(side) / 2.0;
    const double x = f.x() * side + (f.x() > 0 ? half : 0);
    const double y = f.y() * side + (f.y() > 0 ? half : 0);
    const double w = f.width() * side - (f.width() < 1 ? half : 0);
    const double h = f.height() * side - (f.height() < 1 ? half : 0);
    const int left = static_cast<int>(std::round(x));
    const int top = static_cast<int>(std::round(y));
    return {left, top, static_cast<int>(std::round(x + w)) - left, static_cast<int>(std::round(y + h)) - top};
}

class ThumbnailResponse : public QQuickImageResponse, public QRunnable
{
public:
    ThumbnailResponse(PlaylistThumbnailProvider *provider, QString id, int side)
        : m_provider(provider), m_id(std::move(id)), m_side(side)
    {
        // The engine deletes it once it has its finished().
        setAutoDelete(false);
    }

    void run() override
    {
        if (!m_cancelled.load()) {
            m_image = m_provider->thumbnail(m_id, m_side);
        }
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    void cancel() override { m_cancelled.store(true); }

private:
    PlaylistThumbnailProvider *m_provider;
    QString m_id;
    int m_side;
    QImage m_image;
    std::atomic<bool> m_cancelled{false};
};

}  // namespace

PlaylistThumbnailProvider::PlaylistThumbnailProvider(std::shared_ptr<CoverCache> cache) : m_cache(std::move(cache))
{
    m_pool.setMaxThreadCount(2);
}

PlaylistThumbnailProvider::~PlaylistThumbnailProvider()
{
    m_pool.clear();
    m_pool.waitForDone();
}

QString PlaylistThumbnailProvider::idForCovers(const QStringList &coverUrls)
{
    QStringList parts;
    for (const QString &url : coverUrls) {
        if (url.isEmpty()) {
            continue;
        }
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(url));
        if (!parts.contains(encoded)) {
            parts << encoded;
        }
        if (parts.size() == MaxTiles) {
            break;
        }
    }
    return parts.join(Separator);
}

QStringList PlaylistThumbnailProvider::coverPathsFromId(const QString &id)
{
    QStringList paths;
    // The engine hands the id over partly decoded already (as it does the
    // cover provider's); "|" is never left encoded, and decoding each part
    // once more gives back the URL that was encoded.
    for (const QString &part : id.split(Separator, Qt::SkipEmptyParts)) {
        const QUrl url(QUrl::fromPercentEncoding(part.toUtf8()));
        paths << (url.isLocalFile() ? url.toLocalFile() : QString());
    }
    return paths;
}

QImage PlaylistThumbnailProvider::compose(const QList<QImage> &tiles, int side)
{
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const int n = std::min(static_cast<int>(tiles.size()), MaxTiles);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::Antialiasing);
    // Rounded here, not by the row: a mask or an effect on every row's
    // image would cost each row a layer of its own.
    QPainterPath rounded;
    const double radius = side * CornerFraction;
    rounded.addRoundedRect(QRectF(0, 0, side, side), radius, radius);
    painter.setClipPath(rounded);
    for (int i = 0; i < n; ++i) {
        const QImage &cover = tiles[i];
        if (cover.isNull()) {
            continue;
        }
        const QRect target = tileRect(i, n, side);
        // Cropped to fill the tile, as the mosaic's covers are
        // (PreserveAspectCrop): scaled to cover it, the middle kept.
        const QImage scaled = cover.scaled(target.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const QRect source((scaled.width() - target.width()) / 2, (scaled.height() - target.height()) / 2,
                           target.width(), target.height());
        painter.drawImage(target, scaled, source);
    }
    return image;
}

QImage PlaylistThumbnailProvider::thumbnail(const QString &id, int side)
{
    side = std::max(side, 1);
    const QStringList paths = coverPathsFromId(id);
    if (paths.isEmpty()) {
        QImage clear(side, side, QImage::Format_ARGB32_Premultiplied);
        clear.fill(Qt::transparent);
        return clear;
    }
    const QString key = QStringLiteral("playlist:") + id + QLatin1Char(':') + QString::number(side);
    if (QImage kept = m_cache->storedImage(key); !kept.isNull()) {
        return kept;
    }
    QList<QImage> tiles;
    const int n = std::min(static_cast<int>(paths.size()), MaxTiles);
    for (int i = 0; i < n; ++i) {
        const QRect target = tileRect(i, n, side);
        tiles << (paths[i].isEmpty() ? QImage() : m_cache->cover(paths[i], std::max(target.width(), target.height())));
    }
    const QImage image = compose(tiles, side);
    m_compositions.fetch_add(1);
    m_cache->keepImage(key, image);
    return image;
}

QQuickImageResponse *PlaylistThumbnailProvider::requestImageResponse(const QString &id, const QSize &requestedSize)
{
    const int side = requestedSize.isValid() ? std::max(requestedSize.width(), requestedSize.height()) : 48;
    auto *response = new ThumbnailResponse(this, id, side);
    m_pool.start(response);
    return response;
}

}  // namespace seabass::gui
