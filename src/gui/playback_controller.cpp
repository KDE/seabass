// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playback_controller.hpp"

#include <algorithm>

#include <QAudioBuffer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#endif
#include <QFile>
#include <QUrl>
#include <QVariantMap>

#include "domain/beat_grid.hpp"
#if defined(Q_OS_LINUX)
#include "mpris_service.hpp"
#endif
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include <utility>

namespace seabass::gui
{

namespace
{

// The track's analysis -- waveform and beat grid -- from one read of the
// library. See domain::TrackAnalysis for why one.
//
// `pathIndex` is export.pdb's analysis paths for a rekordbox library,
// or null to look the one track up in the database itself.
domain::TrackAnalysis readAnalysis(const QString &format, const QString &libraryPath, const QString &sourceId,
                                   const infrastructure::rekordbox::AnlzPathIndex *pathIndex = nullptr)
{
    try {
        if (format == "rekordbox") {
            return infrastructure::rekordbox::readTrackAnalysis(libraryPath.toStdString(), sourceId.toStdString(),
                                                                nullptr, pathIndex);
        }
        if (format == "engine") {
            return infrastructure::engine::readTrackAnalysis(libraryPath.toStdString(), sourceId.toStdString());
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
    return {};
}

QVariantList toVariantList(const std::vector<domain::WaveformColumn> &points)
{
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

QVariantList readWaveform(const QString &format, const QString &libraryPath, const QString &sourceId,
                          const infrastructure::rekordbox::AnlzPathIndex *pathIndex)
{
    return toVariantList(readAnalysis(format, libraryPath, sourceId, pathIndex).waveform);
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
    // On to the next track by itself when one ends.
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::EndOfMedia) {
            return;
        }
        if (m_libraryBusy) {
            m_advanceWhenLibraryFree = true;   // see libraryBusy
        } else if (hasNext()) {
            next();
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
                               const QString &artworkPath, const QVariantList &cues,
                               const QString &fallbackArtworkPath)
{
    m_player.stop();
    setErrorMessage({});
    // The queue belongs to one library; a track from another ends it.
    if (m_queue && (format != m_queueFormat || libraryPath != m_queueLibraryPath)) {
        dropQueue();
    }
    m_advanceWhenLibraryFree = false;
    m_waveform.clear();
    m_beatTimesMs.clear();
    m_beatNumbers.clear();

    m_currentFormat = format;
    m_currentSourceId = sourceId;
    m_currentLibraryPath = libraryPath;
    m_title = title;
    m_artist = artist;
    m_artworkPath = artworkPath;
    m_cues = cues;
    m_hasTrack = true;
    m_fallbackArtworkPath = fallbackArtworkPath.isEmpty()
        ? queueValue(currentQueueRow(), "fallbackArtworkPath").toString() : fallbackArtworkPath;

    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        setErrorMessage("audio file not found" + (filePath.isEmpty() ? QString() : (": " + filePath)));
    } else {
        const auto index = format == QLatin1String("rekordbox") ? anlzIndexFor(libraryPath) : nullptr;
        const domain::TrackAnalysis analysis = readAnalysis(format, libraryPath, sourceId, index.get());
        m_waveform = toVariantList(analysis.waveform);
        const std::vector<domain::Beat> &beats = analysis.beats;
        m_beatTimesMs.reserve(static_cast<qsizetype>(beats.size()));
        m_beatNumbers.reserve(static_cast<qsizetype>(beats.size()));
        for (const domain::Beat &beat : beats) {
            m_beatTimesMs.append(beat.timeMs);
            m_beatNumbers.append(beat.beatInBar);
        }
        m_player.setSource(QUrl::fromLocalFile(filePath));
        m_player.play();
    }

    emit trackChanged();
    emit cuesChanged();
    emit queueChanged();
}

bool PlaybackController::takeCues(const QString &format, const QString &libraryPath, const QString &sourceId,
                                  const QVariantList &cues)
{
    if (!m_hasTrack || format != m_currentFormat || libraryPath != m_currentLibraryPath ||
        sourceId != m_currentSourceId) {
        return false;
    }
    if (cues != m_cues) {
        m_cues = cues;
        emit cuesChanged();
    }
    return true;
}

QVariantList PlaybackController::waveformFor(const QString &format, const QString &libraryPath,
                                              const QString &sourceId) const
{
    const QString key = format + QLatin1Char('\x1f') + libraryPath + QLatin1Char('\x1f') + sourceId;
    if (const QVariantList *cached = m_waveformCache.object(key)) {
        return *cached;
    }
    const auto index = format == QLatin1String("rekordbox") ? anlzIndexFor(libraryPath) : nullptr;
    QVariantList waveform = readWaveform(format, libraryPath, sourceId, index.get());
    m_waveformCache.insert(key, new QVariantList(waveform));
    return waveform;
}

// Rebuilt whenever export.pdb's size or modification time differs from
// the one the index was built from, which covers every rewrite that
// changes either. What it cannot see is a rewrite in place that keeps
// the size inside one tick of the filesystem's clock (two seconds on
// FAT32). That is acceptable here and nowhere else: this only picks
// which analysis file a preview is drawn from, the waveform cache above
// keeps a drawn preview for longer than that anyway, and no write ever
// asks this index anything (writers build their own, see AnlzPathIndex).
//
// Null when export.pdb cannot be read or parsed: readAnalysis() then
// looks the one track up itself, which fails the same way and leaves
// that waveform empty, as it always did.
std::shared_ptr<const infrastructure::rekordbox::AnlzPathIndex>
PlaybackController::anlzIndexFor(const QString &pioneerRoot) const
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path pdb = pathFromUtf8(pioneerRoot.toStdString()) / "rekordbox" / "export.pdb";
    const auto modified = fs::last_write_time(pdb, ec);
    if (ec) {
        m_anlzIndexes.erase(pioneerRoot);
        return nullptr;
    }
    const auto size = fs::file_size(pdb, ec);
    if (ec) {
        m_anlzIndexes.erase(pioneerRoot);
        return nullptr;
    }
    if (auto found = m_anlzIndexes.find(pioneerRoot);
        found != m_anlzIndexes.end() && found->second.modified == modified && found->second.size == size) {
        return found->second.index;
    }
    try {
        auto index = std::make_shared<const infrastructure::rekordbox::AnlzPathIndex>(pioneerRoot.toStdString());
        m_anlzIndexes[pioneerRoot] = AnlzIndexEntry{modified, size, index};
        return index;
    } catch (const std::exception &) {
        m_anlzIndexes.erase(pioneerRoot);
        return nullptr;
    }
}

void PlaybackController::setQueue(QAbstractItemModel *model, const QString &format, const QString &libraryPath)
{
    dropQueue();
    m_queue = model;
    m_queueFormat = format;
    m_queueLibraryPath = libraryPath;
    if (m_queue) {
        // Whatever changes the list changes what comes next.
        connect(m_queue, &QAbstractItemModel::modelReset, this, &PlaybackController::queueChanged);
        connect(m_queue, &QAbstractItemModel::layoutChanged, this, &PlaybackController::queueChanged);
        connect(m_queue, &QAbstractItemModel::rowsInserted, this, &PlaybackController::queueChanged);
        connect(m_queue, &QAbstractItemModel::rowsRemoved, this, &PlaybackController::queueChanged);
        connect(m_queue, &QAbstractItemModel::rowsMoved, this, &PlaybackController::queueChanged);
        // A page that goes takes its list with it, and with it "next".
        connect(m_queue, &QObject::destroyed, this, &PlaybackController::queueChanged);
    }
    emit queueChanged();
}

// Lets go of the queue AND of its signals. Forgetting the second made
// every return to a list connect it once more, and every change to the
// list then announce itself that many times over.
void PlaybackController::dropQueue()
{
    if (m_queue) {
        disconnect(m_queue, nullptr, this, nullptr);
    }
    m_queue = nullptr;
}

void PlaybackController::setLibraryBusy(bool busy)
{
    if (busy == m_libraryBusy) {
        return;
    }
    m_libraryBusy = busy;
    emit libraryBusyChanged();
    if (!busy && m_advanceWhenLibraryFree) {
        m_advanceWhenLibraryFree = false;
        if (m_player.mediaStatus() == QMediaPlayer::EndOfMedia) {
            next();
        }
    }
}

QVariant PlaybackController::queueValue(int row, const QByteArray &role) const
{
    if (!m_queue || row < 0 || row >= m_queue->rowCount()) {
        return {};
    }
    // A model need not have every role, and must not be asked for one it
    // has not: QML's ListModel crashes on a role of -1.
    const int roleId = m_queue->roleNames().key(role, -1);
    if (roleId < 0) {
        return {};
    }
    return m_queue->data(m_queue->index(row, 0), roleId);
}

int PlaybackController::currentQueueRow() const
{
    // A queue set for another library than the loaded track's has an id
    // like it only by coincidence.
    if (!m_queue || !m_hasTrack || m_currentFormat != m_queueFormat || m_currentLibraryPath != m_queueLibraryPath) {
        return -1;
    }
    const int role = m_queue->roleNames().key("sourceId", -1);
    if (role < 0) {
        return -1;
    }
    const int rows = m_queue->rowCount();
    for (int row = 0; row < rows; ++row) {
        if (m_queue->data(m_queue->index(row, 0), role).toString() == m_currentSourceId) {
            return row;
        }
    }
    return -1;
}

int PlaybackController::playableRowFrom(int row, int step) const
{
    if (!m_queue || row < 0) {
        return -1;
    }
    const int rows = m_queue->rowCount();
    for (int candidate = row + step; candidate >= 0 && candidate < rows; candidate += step) {
        const QString filePath = queueValue(candidate, "filePath").toString();
        if (queueValue(candidate, "streamingSource").toString().isEmpty() && !filePath.isEmpty()
            && QFile::exists(filePath)) {
            return candidate;
        }
    }
    return -1;
}

void PlaybackController::loadQueueRow(int row)
{
    if (row < 0 || m_libraryBusy) {
        return;
    }
    const QString previousSourceId = m_currentSourceId;
    load(m_queueFormat, m_queueLibraryPath, queueValue(row, "sourceId").toString(),
         queueValue(row, "filePath").toString(), queueValue(row, "title").toString(),
         queueValue(row, "artist").toString(), queueValue(row, "artworkPath").toString(),
         queueValue(row, "cues").toList(), queueValue(row, "fallbackArtworkPath").toString());
    emit advanced(previousSourceId);
}

void PlaybackController::next()
{
    loadQueueRow(playableRowFrom(currentQueueRow(), +1));
}

void PlaybackController::previous()
{
    loadQueueRow(playableRowFrom(currentQueueRow(), -1));
}

void PlaybackController::skipBeats(int beats)
{
    if (!m_hasTrack || beats == 0) {
        return;
    }
    const std::vector<double> grid(m_beatTimesMs.cbegin(), m_beatTimesMs.cend());
    seek(static_cast<qint64>(domain::positionAfterSkippingBeats(
        grid, static_cast<double>(m_player.position()), beats, static_cast<double>(m_player.duration()))));
}

void PlaybackController::setDesktopMediaControls(bool enabled)
{
    if (enabled == m_desktopMediaControls) {
        return;
    }
    m_desktopMediaControls = enabled;
#if defined(Q_OS_LINUX)
    if (enabled) {
        auto *service = new MprisService(this);
        // No session bus, or no name to be had on it: then there is no
        // service, rather than one that looks alive and hears nothing.
        if (service->start()) {
            m_mpris = service;
        } else {
            delete service;
        }
    } else {
        delete m_mpris;
        m_mpris = nullptr;
    }
#endif
    emit desktopMediaControlsChanged();
}

void PlaybackController::play()
{
    if (m_hasTrack) {
        m_player.play();
    }
}

void PlaybackController::pause()
{
    if (m_hasTrack) {
        m_player.pause();
    }
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
    emit seeked(positionMs);
}

bool PlaybackController::jumpToHotCue(int number)
{
    if (!m_hasTrack) {
        return false;
    }
    for (const QVariant &value : std::as_const(m_cues)) {
        const QVariantMap cue = value.toMap();
        if (cue.value(QStringLiteral("kind")).toString() == QLatin1String("hot") &&
            cue.value(QStringLiteral("hotCueNumber")).toInt() == number) {
            seek(static_cast<qint64>(cue.value(QStringLiteral("positionMs")).toDouble()));
            return true;
        }
    }
    return false;
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
    m_beatTimesMs.clear();
    m_beatNumbers.clear();
    m_cues.clear();
    emit trackChanged();
    emit cuesChanged();
    emit queueChanged();
}

void PlaybackController::meterBuffer(const QAudioBuffer &buffer)
{
    // A buffer still in the queue from before a pause or a stop.
    if (!playing() || !buffer.isValid()) {
        return;
    }
    if (!m_buffersArrive) {
        m_buffersArrive = true;
        emit liveLevelsChanged();
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
