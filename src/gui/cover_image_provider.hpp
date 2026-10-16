// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <memory>

class QQmlEngine;

namespace seabass::gui
{

class CoverCache;

// "image://cover/<file URL, percent-encoded>": cover art through
// CoverCache, decoded at the size the Image asks for (its sourceSize).
// ArtworkImage turns a file URL into one of these; anything else it
// loads as it is.
//
// Off the GUI thread, on a few threads of its own: a hit is a copy, and
// a miss waits on the stick, so one slow cover does not hold up the
// cached ones behind it. A request the view no longer wants (its row
// scrolled away) is dropped before it starts.
class CoverImageProvider : public QQuickAsyncImageProvider
{
public:
    explicit CoverImageProvider(std::shared_ptr<CoverCache> cache);
    ~CoverImageProvider() override;

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // The local path an id names, empty when it names no local file.
    static QString localPathFromId(const QString &id);

private:
    std::shared_ptr<CoverCache> m_cache;
    QThreadPool m_pool;
};

// Registers the provider on `engine` as "cover", and the playlist
// thumbnails' (PlaylistThumbnailProvider) as "playlist-thumb", both over
// the one cache in CoverCache::defaultDatabaseFile(); the app and the QML
// tests both call this, so what the tests load is what the app loads.
void registerCoverImageProvider(QQmlEngine *engine);

}  // namespace seabass::gui
