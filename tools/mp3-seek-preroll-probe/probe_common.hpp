// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the #92 probes share: finding the one output device they may play
// on, and a player clock that is not stepped.

#pragma once

#include <QAudioDevice>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QObject>

#include <cstdio>

namespace probe
{

// The output device whose id is exactly ID, or a null device. An empty ID
// matches nothing: the probes must never fall back to a real output.
inline QAudioDevice outputDevice(const char *id)
{
    const QByteArray wanted(id);
    if (wanted.isEmpty()) {
        return {};
    }
    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        if (device.id() == wanted) {
            return device;
        }
    }
    std::fprintf(stderr, "no output device has the id \"%s\"; there are:\n", id);
    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        std::fprintf(stderr, "  %s (%s)\n", device.id().constData(), qPrintable(device.description()));
    }
    return {};
}

// QMediaPlayer::position() on the FFmpeg backend is a cached value that
// a 50 ms timer refreshes (qffmpegmediaplayer.cpp, m_positionUpdateTimer),
// so polling it steps in 50 ms. This clock takes the last reported value
// and adds the steady time since it was reported, the way the app's
// popup interpolates the position for its smooth scrolling.
class PlayerClock
{
public:
    explicit PlayerClock(QMediaPlayer &player) : m_player(player)
    {
        QObject::connect(&player, &QMediaPlayer::positionChanged, &player, [this](qint64 position) {
            m_reported = position;
            m_since.start();
        });
    }

    // After setPosition(): the player reports the new position once, at
    // once, and that report must not be mistaken for a moving clock.
    void reset() { m_reported = m_player.position(); m_since.start(); }

    double positionMs() const
    {
        if (m_player.playbackState() != QMediaPlayer::PlayingState || !m_since.isValid()) {
            return static_cast<double>(m_reported);
        }
        return static_cast<double>(m_reported) + m_since.nsecsElapsed() / 1e6;
    }

private:
    QMediaPlayer &m_player;
    qint64 m_reported = 0;
    QElapsedTimer m_since;
};

}  // namespace probe
