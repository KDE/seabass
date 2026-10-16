// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QImage>
#include <QList>
#include <QQuickAsyncImageProvider>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace seabass::gui
{

class CoverCache;

// "image://playlist-thumb/<id>": a playlist's small cover for its row in
// the sidebar, one image made from its tracks' covers the way
// PlaylistCoverMosaic.qml lays them out (one fills the square, two split
// it, three put one on the left and two stacked on the right, four make
// a 2 x 2 grid), at the side the Image asks for. One image per row, not
// four: a long list of playlists decodes and draws a quarter of what
// four tiles each would.
//
// The id is the playlist's first four distinct cover URLs, each written
// out in hex (its UTF-8 bytes) and joined by ".": nothing but the covers
// decides the image, so a playlist whose leading covers change asks for
// a new one by itself. Hex and dots, because the id passes through the
// QML engine as part of a URL, which re-encodes what it does not allow:
// it was percent-encoding joined by "|", and the engine turned every "|"
// into "%7C", so a playlist with more than one cover named one path that
// does not exist and came back clear (Sebastian's stick, 2026-10-16).
// Only digits, letters a to f and dots go through unchanged both ways. Made once and kept in the
// cover cache under "playlist:<id>:<side>", read back before anything is
// composed again, in this run or the next; old ones go with the cache's
// own eviction. The tiles come from CoverCache::cover() too, so a cover
// the track list has decoded is not decoded again.
//
// A playlist with no covers has no id at all (ScanController gives it no
// source): the row draws its neutral tile in the Theme's colours, which
// this side does not know. Asked for anyway, an empty id is a clear
// image. A cover that cannot be read leaves its tile clear, so whatever
// the row has behind the image shows there, as in the mosaic.
//
// Composed off the GUI thread, on a pool of its own, as CoverImageProvider
// reads its covers.
class PlaylistThumbnailProvider : public QQuickAsyncImageProvider
{
public:
    explicit PlaylistThumbnailProvider(std::shared_ptr<CoverCache> cache);
    ~PlaylistThumbnailProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // The id for these cover URLs (file URLs, as ArtworkImage is given
    // them), the first four that are distinct and not empty; "" for none.
    static QString idForCovers(const QStringList &coverUrls);
    // The local cover paths an id names, in order: the id as the engine
    // hands it over, or as made.
    static QStringList coverPathsFromId(const QString &id);

    // The thumbnail for an id at `side` device pixels: the kept one when
    // there is one, else composed and kept. What a response delivers.
    QImage thumbnail(const QString &id, int side);

    // How many thumbnails have been composed rather than read back.
    int compositions() const { return m_compositions.load(); }

    // The mosaic itself: `tiles` (up to four, null where a cover could not
    // be read) laid out in a side x side image, the gaps clear and the
    // corners rounded at a twelfth of the side (the row's
    // Theme.cornerRadius / 2 at its 24 px).
    static QImage compose(const QList<QImage> &tiles, int side);

private:
    std::shared_ptr<CoverCache> m_cache;
    QThreadPool m_pool;
    std::atomic<int> m_compositions{0};
};

}  // namespace seabass::gui
