// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/player_start_guard.hpp"

#include <QMediaPlayer>

#include <algorithm>

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"

namespace seabass::infrastructure::audio
{

qint64 playerStartMs(const std::string &filePath)
{
    if (!domain::hasMp3Extension(filePath)) {
        return 0;
    }
    const auto leadIn = readMp3LeadIn(filePath);
    if (!leadIn || leadIn->sampleRate <= 0) {
        return 0;
    }
    const int samples = domain::ffmpegPartialFirstFrameSamples(*leadIn);
    if (samples <= 0) {
        return 0;
    }
    // Strictly past the frame's end: Qt keeps a frame that ends exactly
    // on the seek position.
    return static_cast<qint64>(samples) * 1000 / leadIn->sampleRate + 1;
}

PlayerStartGuard::PlayerStartGuard(QMediaPlayer *player, QObject *parent) : QObject(parent), m_player(player)
{
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && m_startMs > 0 && m_player->position() < m_startMs) {
            m_player->setPosition(m_startMs);
        }
    });
}

void PlayerStartGuard::setFile(const QString &filePath)
{
    m_startMs = filePath.isEmpty() ? 0 : playerStartMs(filePath.toStdString());
}

qint64 PlayerStartGuard::seek(qint64 positionMs)
{
    const qint64 at = std::max(positionMs, m_startMs);
    m_player->setPosition(at);
    return at;
}

void PlayerStartGuard::play()
{
    // At its end the player starts over from 0, and stopped (the same
    // file loaded again does not load anew) it is at 0.
    if (m_startMs > 0
        && (m_player->mediaStatus() == QMediaPlayer::EndOfMedia || m_player->position() < m_startMs)) {
        m_player->setPosition(m_startMs);
    }
    m_player->play();
}

}  // namespace seabass::infrastructure::audio
