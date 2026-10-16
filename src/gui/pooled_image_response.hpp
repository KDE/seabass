// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QImage>
#include <QMetaObject>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace seabass::gui
{

// An image response whose work runs in a pool, kept apart from the job
// that does it.
//
// The response belongs to the engine's pixmap reader, which deletes it
// when it is done with it -- normally after finished(), but also when a
// page, a request or the engine itself goes away first. The cover and
// playlist-thumbnail providers were each one object that was both the
// response and the pool's runnable, so a response deleted that way was
// deleted under its own run(), which went on to read its freed path:
// the QML storm crashed in CoverResponse::run() on Windows while sticks
// were pulled and pages torn down.
//
// Now the job owns copies of what it needs and a link to the response
// that the response's destructor cuts. The job hands its result over
// through a queued call, made while it holds the link, so it never
// reaches a deleted response, and a queued call to a response deleted
// after it was made is dropped with the response. finished() always
// follows, a cancelled request's too, as the engine waits for it.
class PooledImageResponse : public QQuickImageResponse
{
public:
    // The work, run in the pool. It sees the cancellation flag, and
    // gives the image, or a null one with `error` saying why.
    using Work = std::function<QImage(const std::atomic<bool> &cancelled, QString &error)>;

    static PooledImageResponse *start(QThreadPool &pool, Work work)
    {
        auto *response = new PooledImageResponse;
        pool.start(new Job(response->m_link, std::move(work)));
        return response;
    }

    ~PooledImageResponse() override
    {
        const std::lock_guard<std::mutex> guard(m_link->mutex);
        m_link->response = nullptr;
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override { m_link->cancelled.store(true); }

private:
    struct Link
    {
        std::mutex mutex;
        PooledImageResponse *response = nullptr;
        std::atomic<bool> cancelled{false};
    };

    class Job : public QRunnable
    {
    public:
        Job(std::shared_ptr<Link> link, Work work) : m_link(std::move(link)), m_work(std::move(work))
        {
            setAutoDelete(true);
        }

        void run() override
        {
            QImage image;
            QString error;
            if (!m_link->cancelled.load()) {
                try {
                    image = m_work(m_link->cancelled, error);
                } catch (const std::exception &e) {
                    image = QImage();
                    error = QString::fromUtf8(e.what());
                }
            }
            const std::lock_guard<std::mutex> guard(m_link->mutex);
            if (PooledImageResponse *response = m_link->response) {
                QMetaObject::invokeMethod(
                    response,
                    [response, image, error]() {
                        response->m_image = image;
                        response->m_error = error;
                        emit response->finished();
                    },
                    Qt::QueuedConnection);
            }
        }

    private:
        std::shared_ptr<Link> m_link;
        Work m_work;
    };

    PooledImageResponse() : m_link(std::make_shared<Link>()) { m_link->response = this; }

    std::shared_ptr<Link> m_link;
    QImage m_image;
    QString m_error;
};

}  // namespace seabass::gui
