// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/cover_image_provider.hpp"

#include <QQmlEngine>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QUrl>

#include <atomic>

#include "gui/cover_cache.hpp"

namespace seabass::gui
{

namespace
{

class CoverResponse : public QQuickImageResponse, public QRunnable
{
public:
    CoverResponse(std::shared_ptr<CoverCache> cache, QString path, int side)
        : m_cache(std::move(cache)), m_path(std::move(path)), m_side(side)
    {
        // The engine deletes the response once it has its finished(); the
        // pool must not delete it as well.
        setAutoDelete(false);
    }

    void run() override
    {
        if (!m_cancelled.load()) {
            m_image = m_path.isEmpty() ? QImage() : m_cache->cover(m_path, m_side);
            if (m_image.isNull()) {
                // Worded as Qt words a file it cannot load, which is what
                // this is, and what the storm already knows a missing
                // cover by (tests/qml-storm/StormRunner.qml).
                m_error = QStringLiteral("Cannot open: ") + m_path;
            }
        }
        // Also when cancelled: the engine waits for it to let go.
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override { m_cancelled.store(true); }

private:
    std::shared_ptr<CoverCache> m_cache;
    QString m_path;
    int m_side;
    QImage m_image;
    QString m_error;
    std::atomic<bool> m_cancelled{false};
};

}  // namespace

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
    auto *response = new CoverResponse(m_cache, localPathFromId(id), side);
    m_pool.start(response);
    return response;
}

void registerCoverImageProvider(QQmlEngine *engine)
{
    engine->addImageProvider(QStringLiteral("cover"),
                             new CoverImageProvider(std::make_shared<CoverCache>(CoverCache::defaultDatabaseFile())));
}

}  // namespace seabass::gui
