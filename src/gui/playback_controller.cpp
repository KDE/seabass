// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playback_controller.hpp"

#include <QAudioBuffer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#endif
#include <QFile>
#include <QUrl>
#include <QVariantMap>

#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"

namespace seabass::gui
{

namespace
{

QVariantList readWaveform(const QString &format, const QString &libraryPath, const QString &sourceId)
{
    std::vector<domain::WaveformColumn> points;
    try {
        if (format == "rekordbox") {
            points = infrastructure::rekordbox::readWaveformPreview(libraryPath.toStdString(), sourceId.toStdString());
        } else if (format == "engine") {
            points = infrastructure::engine::readWaveformPreview(libraryPath.toStdString(), sourceId.toStdString());
        }
        // Any other format (currently just "onelibrary") has no waveform
        // reader of its own -- stays empty rather than falling into
        // either branch above by default, which used to silently treat
        // an unrecognized format as Engine and could throw trying to
        // open a differently-shaped database as one.
    } catch (const std::exception &) {
        // Called directly on the UI thread (see load()/waveformFor()),
        // not through a background task's own try/catch -- an exception
        // escaping here would crash the app outright instead of just
        // leaving the waveform blank for this one track.
    }
    QVariantList waveform;
    for (const auto &col : points) {
        QVariantMap m;
        m["low"] = col.low;
        m["mid"] = col.mid;
        m["high"] = col.high;
        waveform.append(m);
    }
    return waveform;
}

}  // namespace

PlaybackController::PlaybackController(QObject *parent) : QObject(parent)
{
    m_player.setAudioOutput(&m_audioOutput);

    connect(&m_player, &QMediaPlayer::durationChanged, this, &PlaybackController::durationChanged);
    connect(&m_player, &QMediaPlayer::positionChanged, this, &PlaybackController::positionChanged);
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, &PlaybackController::playingChanged);
    // Paused or stopped, no more audio arrives to bring the levels down:
    // without this a display would freeze on the last kick.
    connect(&m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state != QMediaPlayer::PlayingState) {
            clearLevels();
        }
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    m_bufferOutput = new QAudioBufferOutput(this);
    m_player.setAudioBufferOutput(m_bufferOutput);
    // Queued onto this thread by the context object: the player hands
    // buffers over from its own.
    connect(m_bufferOutput, &QAudioBufferOutput::audioBufferReceived, this, &PlaybackController::meterBuffer);
#endif
    connect(&m_audioOutput, &QAudioOutput::volumeChanged, this, &PlaybackController::volumeChanged);
    connect(&m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        setErrorMessage(message);
    });
}

void PlaybackController::setVolume(qreal volume)
{
    m_audioOutput.setVolume(static_cast<float>(qBound(0.0, volume, 1.0)));
}

void PlaybackController::load(const QString &format, const QString &libraryPath, const QString &sourceId,
                               const QString &filePath, const QString &title, const QString &artist,
                               const QString &artworkPath, const QVariantList &cues)
{
    m_player.stop();
    setErrorMessage({});
    m_waveform.clear();

    m_currentFormat = format;
    m_currentSourceId = sourceId;
    m_title = title;
    m_artist = artist;
    m_artworkPath = artworkPath;
    m_cues = cues;
    m_hasTrack = true;

    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        setErrorMessage("audio file not found" + (filePath.isEmpty() ? QString() : (": " + filePath)));
    } else {
        m_waveform = readWaveform(format, libraryPath, sourceId);
        m_player.setSource(QUrl::fromLocalFile(filePath));
        m_player.play();
    }

    emit trackChanged();
}

QVariantList PlaybackController::waveformFor(const QString &format, const QString &libraryPath,
                                              const QString &sourceId) const
{
    const QString key = format + QLatin1Char('\x1f') + libraryPath + QLatin1Char('\x1f') + sourceId;
    if (const QVariantList *cached = m_waveformCache.object(key)) {
        return *cached;
    }
    QVariantList waveform = readWaveform(format, libraryPath, sourceId);
    m_waveformCache.insert(key, new QVariantList(waveform));
    return waveform;
}

void PlaybackController::togglePlay()
{
    if (!m_hasTrack) {
        return;
    }
    if (playing()) {
        m_player.pause();
    } else {
        m_player.play();
    }
}

void PlaybackController::seek(qint64 positionMs)
{
    m_player.setPosition(positionMs);
}

void PlaybackController::stop()
{
    m_player.stop();
    // Clearing the source, not just stopping playback, is what actually
    // releases the underlying file handle -- QMediaPlayer's backend (on
    // Linux, GStreamer/FFmpeg) keeps a track's file open as long as a
    // source is loaded, playing or not. Without this, ejecting a stick
    // right after playing a track from it consistently failed with
    // "target is busy": stop() left hasTrack false (so the UI correctly
    // showed no track loaded) while the real file descriptor stayed open
    // underneath, which the UI had no way to reveal.
    m_player.setSource(QUrl());
    m_hasTrack = false;
    m_currentFormat.clear();
    m_currentSourceId.clear();
    m_waveform.clear();
    m_cues.clear();
    emit trackChanged();
}

void PlaybackController::meterBuffer(const QAudioBuffer &buffer)
{
    // A buffer still in the queue from before a pause or a stop.
    if (!playing() || !buffer.isValid()) {
        return;
    }
    const QAudioFormat format = buffer.format();
    domain::SampleFormat sampleFormat;
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8: sampleFormat = domain::SampleFormat::UInt8; break;
    case QAudioFormat::Int16: sampleFormat = domain::SampleFormat::Int16; break;
    case QAudioFormat::Int32: sampleFormat = domain::SampleFormat::Int32; break;
    case QAudioFormat::Float: sampleFormat = domain::SampleFormat::Float; break;
    default: return;
    }
    const domain::AudioLevels levels = m_meter.feed(buffer.constData<void>(),
        static_cast<std::size_t>(buffer.frameCount()), format.channelCount(), sampleFormat, format.sampleRate());
    m_levels = levels;
    emit levelsChanged();
    if (levels.beat) {
        ++m_beatCount;
        emit beatCountChanged();
    }
}

void PlaybackController::clearLevels()
{
    m_meter.reset();
    if (m_levels.low == 0.0 && m_levels.mid == 0.0 && m_levels.high == 0.0) {
        return;
    }
    m_levels = {};
    emit levelsChanged();
}

void PlaybackController::setErrorMessage(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit errorMessageChanged();
}

}  // namespace seabass::gui
