// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_duration_probe.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace seabass::infrastructure::audio
{

QtMultimediaDurationProbe::QtMultimediaDurationProbe(int timeoutMs) : m_timeoutMs(timeoutMs) {}

std::optional<double> QtMultimediaDurationProbe::durationSeconds(const std::string &absoluteFilePath)
{
    if (absoluteFilePath.empty()) {
        return std::nullopt;
    }
    // QMediaPlayer treats a missing file as "invalid media" and still
    // costs a backend round trip; a stale catalog row pointing at a
    // deleted file is common enough on a real stick to be worth the
    // cheaper check first.
    const QString path = QString::fromStdString(absoluteFilePath);
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
    if (QCoreApplication::instance() == nullptr) {
        // Without an application object there is no event loop to run,
        // so the media would never finish loading. Refusing here is far
        // easier to diagnose than a silent 5 s timeout per file.
        return std::nullopt;
    }

    QMediaPlayer player;
    QEventLoop loop;
    std::optional<double> result;

    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop,
                      [&](QMediaPlayer::MediaStatus status) {
                          switch (status) {
                          case QMediaPlayer::LoadedMedia:
                          case QMediaPlayer::BufferedMedia: {
                              const qint64 ms = player.duration();
                              if (ms > 0) {
                                  result = static_cast<double>(ms) / 1000.0;
                              }
                              loop.quit();
                              break;
                          }
                          case QMediaPlayer::InvalidMedia:
                          case QMediaPlayer::NoMedia:
                              loop.quit();
                              break;
                          default:
                              break;  // still loading
                          }
                      });

    QTimer::singleShot(m_timeoutMs, &loop, &QEventLoop::quit);
    player.setSource(QUrl::fromLocalFile(path));
    loop.exec();

    return result;
}

}  // namespace seabass::infrastructure::audio
