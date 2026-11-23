// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playback_controller.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <QAudioBuffer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QMediaPlayer>
#endif
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QVariantMap>

#include "beat_grid_display.hpp"
#include "domain/beat_grid.hpp"
#if defined(Q_OS_LINUX)
#include "mpris_service.hpp"
#endif
#include "infrastructure/audio/playback_timeline.hpp"
#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"
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
// The cues as a page hands them, every position moved by `shiftMs` (see
// infrastructure/audio/playback_timeline.hpp): 0 leaves the list as it
// came, so takeCues() still sees an unchanged list as unchanged. A loop's
// end moves with it, and only a loop's: another cue's loopEndMs means
// nothing and is left as it came. Nothing moves before the start of the
// file: a cue inside an MP3's lead-in plays from 0.
QVariantList shiftedCues(const QVariantList &cues, double shiftMs)
{
    if (shiftMs == 0.0) {
        return cues;
    }
    QVariantList shifted;
    shifted.reserve(cues.size());
    for (const QVariant &value : cues) {
        QVariantMap cue = value.toMap();
        const auto move = [&](const QString &key) {
            const auto it = cue.find(key);
            if (it != cue.end() && it->canConvert<double>()) {
                *it = std::max(0.0, it->toDouble() + shiftMs);
            }
        };
        move(QStringLiteral("positionMs"));
        if (cue.value(QStringLiteral("isLoop")).toBool()) {
            move(QStringLiteral("loopEndMs"));
        }
        shifted.append(cue);
    }
    return shifted;
}

// A detail waveform as detailWaveform hands it to QML: three bytes a
// column, low, mid and high, 0 to 255. Made in the worker that read it.
QByteArray columnBytes(const domain::DetailWaveform &detail)
{
    QByteArray columns;
    columns.reserve(static_cast<qsizetype>(detail.columns.size() * 3));
    const auto toByte = [](double v) {
        return static_cast<char>(static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
    };
    for (const domain::WaveformColumn &column : detail.columns) {
        columns.append(toByte(column.low));
        columns.append(toByte(column.mid));
        columns.append(toByte(column.high));
    }
    return columns;
}

domain::TrackAnalysis readAnalysis(const QString &format, const QString &libraryPath, const QString &sourceId,
                                   const infrastructure::rekordbox::AnlzPathIndex *pathIndex = nullptr,
                                   std::size_t detailUpTo = 0)
{
    try {
        if (format == "rekordbox") {
            return infrastructure::rekordbox::readTrackAnalysis(libraryPath.toStdString(), sourceId.toStdString(),
                                                                nullptr, pathIndex);
        }
        if (format == "engine") {
            return infrastructure::engine::readTrackAnalysis(libraryPath.toStdString(), sourceId.toStdString(),
                                                             detailUpTo);
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

}  // namespace

PlaybackController::PlaybackController(QObject *parent) : QObject(parent) {}

QMediaPlayer *PlaybackController::player()
{
    if (m_player) {
        return m_player;
    }
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(static_cast<float>(m_volume));
    m_player->setAudioOutput(m_audioOutput);

    connect(m_player, &QMediaPlayer::durationChanged, this, &PlaybackController::durationChanged);
    // The loaded track's length has been reported: a length, or a file
    // that will not give one.
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (duration > 0) {
            markDurationKnown();
        }
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::LoadedMedia) {
            markDurationKnown();
        }
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, &PlaybackController::positionChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &PlaybackController::playingChanged);
    // Paused or stopped, no more audio arrives to bring the levels down:
    // without this a display would freeze on the last kick.
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state != QMediaPlayer::PlayingState) {
            clearLevels();
        }
    });
    // On to the next track by itself when one ends.
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::EndOfMedia) {
            return;
        }
        if (m_libraryBusy) {
            m_advanceWhenLibraryFree = true;   // see libraryBusy
        } else if (hasNext()) {
            next();
        }
    });
#if QT_VERSION < QT_VERSION_CHECK(6, 11, 0)
    // Qt 6.8.3's FFmpeg player loads some files and then never starts
    // them: play() reports playing, the position stays at 0, and only a
    // seek sets it going. Every MP3 on a real stick that carries cover art
    // did it, which on the macOS 12 build (Qt 6.8, see
    // tools/macos-monterey-dmg.sh) made Play do nothing. 6.11 plays them
    // as they are. One seek to where the player already is, once the file
    // has loaded, is the same seek a click in the waveform made by hand.
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        // Not only while playing: when a file finishes loading the player
        // has not reported play() back yet, and a seek in place changes
        // nothing for a paused one.
        if (status == QMediaPlayer::LoadedMedia) {
            m_player->setPosition(m_player->position());
        }
    });
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    m_bufferOutput = new QAudioBufferOutput(this);
    m_player->setAudioBufferOutput(m_bufferOutput);
    // Queued onto this thread by the context object: the player hands
    // buffers over from its own.
    connect(m_bufferOutput, &QAudioBufferOutput::audioBufferReceived, this, &PlaybackController::meterBuffer);
#endif
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        setErrorMessage(message);
    });
    return m_player;
}

qint64 PlaybackController::duration() const
{
    return m_player ? m_player->duration() : 0;
}

qint64 PlaybackController::position() const
{
    return m_player ? m_player->position() : 0;
}

bool PlaybackController::playing() const
{
    return m_player && m_player->playbackState() == QMediaPlayer::PlayingState;
}

void PlaybackController::setVolume(qreal volume)
{
    const qreal bounded = qBound(0.0, volume, 1.0);
    if (bounded == m_volume) {
        return;
    }
    m_volume = bounded;
    if (m_audioOutput) {
        m_audioOutput->setVolume(static_cast<float>(m_volume));
    }
    emit volumeChanged();
}

void PlaybackController::load(const QString &format, const QString &libraryPath, const QString &sourceId,
                               const QString &filePath, const QString &title, const QString &artist,
                               const QString &artworkPath, const QVariantList &cues,
                               const QString &fallbackArtworkPath, bool startPlaying)
{
    emit trackAboutToChange();
    if (m_player) {
        m_player->stop();
    }
    setErrorMessage({});
    // The queue belongs to one library; a track from another ends it.
    if (m_queue && (format != m_queueFormat || libraryPath != m_queueLibraryPath)) {
        dropQueue();
    }
    m_advanceWhenLibraryFree = false;
    m_waveform.clear();
    m_beatTimesMs.clear();
    m_beatNumbers.clear();
    m_drawnBeats.clear();
    ++m_trackSerial;
    dropDetailWaveform();

    m_currentFormat = format;
    m_currentSourceId = sourceId;
    m_currentLibraryPath = libraryPath;
    m_currentFilePath = filePath;
    m_title = title;
    m_artist = artist;
    m_artworkPath = artworkPath;
    m_cueShiftMs = 0.0;
    m_beatShiftMs = 0.0;
    m_cues = cues;
    m_hasTrack = true;
    m_fallbackArtworkPath = fallbackArtworkPath.isEmpty()
        ? queueValue(currentQueueRow(), "fallbackArtworkPath").toString() : fallbackArtworkPath;
    // A catalog names its artwork without checking it is on the stick
    // (that check is a stage of the catalog cache no page asks for just
    // to play a track). Settled here, once, with one stat: the player
    // bar, the ring and the watermark fall back on their own, but the
    // MPRIS art URL and anything else that takes artworkPath as a fact
    // must never be handed a file that is not there.
    const auto onDisk = [](const QString &artwork) {
        if (artwork.isEmpty()) {
            return false;
        }
        const QUrl url(artwork);
        const QString local = url.isLocalFile() ? url.toLocalFile() : artwork;
        return QFile::exists(local);
    };
    if (!onDisk(m_artworkPath)) {
        m_artworkPath = onDisk(m_fallbackArtworkPath) ? m_fallbackArtworkPath : QString();
        m_fallbackArtworkPath.clear();
    }

    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        setErrorMessage("audio file not found" + (filePath.isEmpty() ? QString() : (": " + filePath)));
        // Nothing left of the old track: stopped is not enough, its
        // source would still give the old length, and play() would play
        // the old file under the new title.
        if (m_player) {
            m_player->setSource(QUrl());
        }
        // No length will come: that is this track's answer.
        markDurationKnown();
    } else {
        // No library path, no analysis to read: a stored backup row plays
        // a file no catalog row stands behind, and an empty path would
        // resolve against the working directory.
        const auto index = format == QLatin1String("rekordbox") && !libraryPath.isEmpty()
            ? anlzIndexFor(libraryPath) : nullptr;
        // Not const: its detail is moved out below.
        domain::TrackAnalysis analysis = libraryPath.isEmpty()
            ? domain::TrackAnalysis{} : readAnalysis(format, libraryPath, sourceId, index.get(), MaximumKeptDetailColumns);
        m_waveform = toVariantList(analysis.waveform);
        // Kept for the popup only where it is overview-sized (Engine 2.x
        // and 3.x, about a thousand entries); a 1.x high-resolution
        // waveform (ten megabytes for an hour) is read again off the GUI
        // thread if the popup asks.
        // Built by the read only when it is that small: a larger one is not
        // built here at all.
        m_loadedDetailKept = !analysis.detail.columns.empty();
        m_loadedDetail = std::move(analysis.detail);
        // An MP3 is played on Engine's timeline; the cues and a rekordbox
        // grid are not on it (#50).
        const auto shift = infrastructure::audio::playbackShiftFor(
            filePath.toStdString(), format == QLatin1String("rekordbox")
                ? infrastructure::audio::GridTimeline::Rekordbox : infrastructure::audio::GridTimeline::Engine);
        m_cueShiftMs = shift.cuesMs;
        m_beatShiftMs = shift.beatsMs;
        m_cues = shiftedCues(cues, m_cueShiftMs);
        const std::vector<domain::Beat> &beats = analysis.beats;
        m_beatTimesMs.reserve(static_cast<qsizetype>(beats.size()));
        m_beatNumbers.reserve(static_cast<qsizetype>(beats.size()));
        for (const domain::Beat &beat : beats) {
            m_beatTimesMs.append(beat.timeMs + shift.beatsMs);
            m_beatNumbers.append(beat.beatInBar);
        }
        m_drawnBeats.reserve(beats.size());
        for (const domain::Beat &beat : beats) {
            m_drawnBeats.push_back({beat.timeMs + shift.beatsMs, beat.beatInBar});
        }
        m_drawnBeatsRateRecorded = analysis.beatRateRecorded;
        const QUrl source = QUrl::fromLocalFile(filePath);
        const bool sameFile = player()->source() == source;
        player()->setSource(source);
        // The same file again: setSource() changes nothing and no length
        // is reported again; the one it has is this track's. Only then:
        // for another file the status read here can still be the old
        // one's (stop() left it LoadedMedia, and a backend that loads
        // asynchronously has not moved it to LoadingMedia yet), and the
        // length with it.
        const auto status = m_player->mediaStatus();
        if (sameFile
            && (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia
                || status == QMediaPlayer::BufferingMedia || status == QMediaPlayer::EndOfMedia
                || status == QMediaPlayer::InvalidMedia)) {
            markDurationKnown();
        }
        if (startPlaying) {
            m_player->play();
        } else {
            m_player->pause();
        }
    }

    emit trackChanged();
    emit cuesChanged();
    emit queueChanged();
}

void PlaybackController::loadFile(const QString &format, const QString &libraryPath, const QString &filePath,
                                  const QString &title, const QString &artist)
{
    emit trackAboutToChange();
    if (m_player) {
        m_player->stop();
    }
    setErrorMessage({});
    dropQueue();
    m_advanceWhenLibraryFree = false;
    m_waveform.clear();
    m_beatTimesMs.clear();
    m_beatNumbers.clear();
    m_drawnBeats.clear();
    ++m_trackSerial;
    dropDetailWaveform();

    m_currentFormat = format;
    m_currentSourceId = QStringLiteral("recording:") + filePath;
    m_currentLibraryPath = libraryPath;
    m_currentFilePath = filePath;
    m_title = title;
    m_artist = artist;
    m_artworkPath.clear();
    m_fallbackArtworkPath.clear();
    m_cues.clear();
    m_cueShiftMs = 0.0;
    m_beatShiftMs = 0.0;
    m_hasTrack = true;

    // Never readAnalysis(): no catalog knows this file.
    if (filePath.isEmpty() || !QFile::exists(filePath)) {
        setErrorMessage("audio file not found" + (filePath.isEmpty() ? QString() : (": " + filePath)));
        if (m_player) {
            m_player->setSource(QUrl());
        }
        markDurationKnown();
    } else {
        player()->setSource(QUrl::fromLocalFile(filePath));
        m_player->play();
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
    const QVariantList shifted = shiftedCues(cues, m_cueShiftMs);
    if (shifted != m_cues) {
        m_cues = shifted;
        emit cuesChanged();
    }
    return true;
}

PlaybackController::CachedAnalysis PlaybackController::cachedAnalysis(const QString &format,
                                                                     const QString &libraryPath,
                                                                     const QString &sourceId) const
{
    // No library, no analysis: a stored backup row names none (see load()).
    if (libraryPath.isEmpty()) {
        return {};
    }
    const QString key = format + QLatin1Char('\x1f') + libraryPath + QLatin1Char('\x1f') + sourceId;
    if (const CachedAnalysis *cached = m_analysisCache.object(key)) {
        return *cached;
    }
    // Not while a library is being written, the rule the popup's detail
    // read keeps. Engine's analysis is read through libdjinterop, which
    // opens m.db: a save's own connection then meets this one's lock and
    // fails at once with "database is locked", and the whole save rolls
    // back. A list's rows ask here as they are built, which a save's
    // changes to the list make happen in the middle of it. Nothing is
    // kept, so the row asks again when the write ends (analysisRevision).
    if (m_libraryBusy) {
        return {};
    }
    const auto index = format == QLatin1String("rekordbox") ? anlzIndexFor(libraryPath) : nullptr;
    domain::TrackAnalysis analysis = readAnalysis(format, libraryPath, sourceId, index.get());
    CachedAnalysis read{toVariantList(analysis.waveform), std::move(analysis.beats), analysis.beatRateRecorded};
    m_analysisCache.insert(key, new CachedAnalysis(read));
    return read;
}

void PlaybackController::forgetAnalyses()
{
    m_analysisCache.clear();
    ++m_analysisRevision;
    emit analysisRevisionChanged();
}

QVariantList PlaybackController::waveformFor(const QString &format, const QString &libraryPath,
                                              const QString &sourceId) const
{
    return cachedAnalysis(format, libraryPath, sourceId).waveform;
}

QVariantList PlaybackController::beatGridFor(const QString &format, const QString &libraryPath,
                                              const QString &sourceId) const
{
    const CachedAnalysis analysis = cachedAnalysis(format, libraryPath, sourceId);
    return beatGridForDisplay(analysis.beats, analysis.beatRateRecorded);
}

QVariantList PlaybackController::beatTicksFor(const QString &format, const QString &libraryPath,
                                               const QString &sourceId, double durationMs, double widthPx) const
{
    if (!(durationMs > 0.0) || !(widthPx > 0.0)) {
        return {};
    }
    const CachedAnalysis analysis = cachedAnalysis(format, libraryPath, sourceId);
    return beatTicksForDisplay(analysis.beats, analysis.beatRateRecorded, durationMs, widthPx);
}

QVariantList PlaybackController::beatTicksAt(double durationMs, double widthPx) const
{
    return beatTicksForDisplay(m_drawnBeats, m_drawnBeatsRateRecorded, durationMs, widthPx);
}

void PlaybackController::markDurationKnown()
{
    if (m_durationSerial != m_trackSerial) {
        m_durationSerial = m_trackSerial;
        emit durationSerialChanged();
    }
}

QVariantMap PlaybackController::detailWaveform() const
{
    if (m_detailSerial == m_trackSerial) {
        return m_detailWaveform;
    }
    return {{QStringLiteral("columns"), QByteArray()},
            {QStringLiteral("msPerColumn"), 0.0},
            {QStringLiteral("startMs"), m_beatShiftMs}};
}

void PlaybackController::dropDetailWaveform()
{
    m_detailRequest.cancel();
    m_detailWanted = false;
    m_loadedDetail = {};
    m_loadedDetailKept = false;
    const bool had = m_detailSerial >= 0;
    m_detailSerial = -1;
    m_detailWaveform.clear();
    if (had) {
        emit detailWaveformChanged();
    }
}

void PlaybackController::setDetailWaveform(const QByteArray &columns, double msPerColumn, int serial)
{
    if (serial != m_trackSerial) {
        return;   // another track has been loaded since it was asked for
    }
    m_detailWaveform = {{QStringLiteral("columns"), columns},
                        {QStringLiteral("msPerColumn"), msPerColumn},
                        {QStringLiteral("startMs"), m_beatShiftMs}};
    m_detailSerial = serial;
    emit detailWaveformChanged();
}

void PlaybackController::requestDetailWaveform()
{
    if (!m_hasTrack) {
        return;
    }
    // Wanted even when it is already here: a popup closed and opened
    // again finds the detail delivered, and a write ending after that
    // must still ask for it again (setLibraryBusy(false)).
    m_detailWanted = true;
    if (m_detailSerial == m_trackSerial) {
        return;
    }
    const int serial = m_trackSerial;
    const bool engine = m_currentFormat == QLatin1String("engine");
    if ((!engine && m_currentFormat != QLatin1String("rekordbox")) || m_currentLibraryPath.isEmpty()
        || m_currentSourceId.startsWith(QLatin1String("recording:"))) {
        setDetailWaveform({}, 0.0, serial);
        return;
    }
    // Not while a library is being written: an analysis file, the
    // database or an audio file's tags can be half rewritten.
    // setLibraryBusy(false) asks again. An Engine 2.x or 3.x overview
    // load() kept is handed over meanwhile.
    if (m_libraryBusy) {
        if (engine && m_loadedDetailKept) {
            setDetailWaveform(columnBytes(m_loadedDetail), m_loadedDetail.msPerColumn, serial);
        }
        return;
    }
    // Everything else is read off the GUI thread (#104). export.pdb's
    // analysis paths: the index load() keeps for this library, as it is,
    // without looking at the file again here; the worker parses export.pdb
    // itself only where there is none.
    std::shared_ptr<const infrastructure::rekordbox::AnlzPathIndex> index;
    if (!engine) {
        if (const auto kept = m_anlzIndexes.find(m_currentLibraryPath); kept != m_anlzIndexes.end()) {
            index = kept->second.index;
        }
    }
    const std::string path = m_currentLibraryPath.toStdString();
    const std::string sourceId = m_currentSourceId.toStdString();
    const QString audioPath = engine ? m_currentFilePath : QString();
    // What this session made of the file before, checked by the worker.
    const std::shared_ptr<const AudioDetailEntry> known = engine ? audioDetailFor(audioPath) : nullptr;
    // The overview load() kept (Engine 2.x and 3.x), for when the audio
    // cannot be decoded: no second opening of the database for it.
    std::optional<domain::DetailWaveform> overview;
    if (engine && m_loadedDetailKept) {
        overview = m_loadedDetail;
    }
    // The library's stick, and the audio file's where it is elsewhere:
    // either going away ends the read.
    QStringList sticks{QFileInfo(m_currentLibraryPath).absolutePath()};
    if (!audioPath.isEmpty()) {
        sticks.append(QFileInfo(audioPath).absolutePath());
    }
    m_detailRequest.startOnSticks(
        QString::number(serial), sticks,
        [path, sourceId, engine, index, audioPath, known, overview](application::CancellationToken cancel) {
            DetailRead read;
            if (cancel.cancelled()) {
                return read;
            }
            if (!engine) {
                const domain::DetailWaveform detail
                    = infrastructure::rekordbox::readDetailWaveform(path, sourceId, nullptr, index.get());
                read.columns = columnBytes(detail);
                read.msPerColumn = detail.msPerColumn;
                return cancel.cancelled() ? DetailRead{} : read;
            }
            bool fromAudio = false;
#ifdef SEABASS_HAVE_QT_AUDIO
            const QFileInfo info(audioPath);
            if (!audioPath.isEmpty() && info.isFile()) {
                const qint64 size = info.size();
                const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
                if (known && known->size == size && known->modifiedMs == modifiedMs) {
                    // Made before from the file as it is now: no decode,
                    // and none for a file that could not be decoded.
                    if (!known->failed) {
                        read.columns = known->columns;
                        read.msPerColumn = known->msPerColumn;
                        fromAudio = true;
                    }
                } else {
                    auto entry = std::make_shared<AudioDetailEntry>();
                    entry->path = audioPath;
                    entry->size = size;
                    entry->modifiedMs = modifiedMs;
                    {
                        const domain::DetailWaveform detail = infrastructure::audio::audioDetailWaveform(
                            audioPath.toStdString(), [&cancel] { return cancel.cancelled(); });
                        entry->columns = columnBytes(detail);
                        entry->msPerColumn = detail.msPerColumn;
                    }
                    if (cancel.cancelled()) {
                        return DetailRead{};
                    }
                    entry->failed = entry->columns.isEmpty();
                    fromAudio = !entry->failed;
                    read.columns = entry->columns;
                    read.msPerColumn = entry->msPerColumn;
                    read.decoded = std::move(entry);
                }
            }
#else
            (void)audioPath;
            (void)known;
#endif
            if (cancel.cancelled()) {
                return DetailRead{};
            }
            if (!fromAudio) {
                const domain::DetailWaveform detail = overview ? *overview
                    : infrastructure::engine::readTrackAnalysis(path, sourceId, std::numeric_limits<std::size_t>::max()).detail;
                read.columns = columnBytes(detail);
                read.msPerColumn = detail.msPerColumn;
            }
            return cancel.cancelled() ? DetailRead{} : read;
        },
        {[this, serial](DetailRead &&read) {
             if (read.decoded) {
                 ++m_audioDetailDecodes;
                 keepAudioDetail(std::move(read.decoded));
             }
             setDetailWaveform(read.columns, read.msPerColumn, serial);
         },
         [](const QString &) {}, [] {}});
}

std::shared_ptr<const PlaybackController::AudioDetailEntry> PlaybackController::audioDetailFor(const QString &filePath)
{
    for (auto it = m_audioDetails.begin(); it != m_audioDetails.end(); ++it) {
        if ((*it)->path == filePath) {
            m_audioDetails.splice(m_audioDetails.begin(), m_audioDetails, it);
            return m_audioDetails.front();
        }
    }
    return nullptr;
}

void PlaybackController::keepAudioDetail(std::shared_ptr<const AudioDetailEntry> entry)
{
    std::erase_if(m_audioDetails, [&](const auto &kept) { return kept->path == entry->path; });
    m_audioDetails.push_front(std::move(entry));
    while (m_audioDetails.size() > MaximumAudioDetails) {
        m_audioDetails.pop_back();
    }
}

QVariantList PlaybackController::cuesOnGridTimeline(const QString &format, const QString &filePath,
                                                    const QVariantList &cues) const
{
    if (format != QLatin1String("engine") || filePath.isEmpty() || cues.isEmpty()) {
        return cues;
    }
    // The player's own shift for this file and an Engine grid: the cues
    // go where load() puts them, and the grid stays where it is.
    const auto shift = infrastructure::audio::playbackShiftFor(filePath.toStdString(),
                                                               infrastructure::audio::GridTimeline::Engine);
    return shiftedCues(cues, shift.cuesMs - shift.beatsMs);
}

void PlaybackController::releaseDetailWaveform()
{
    m_detailWanted = false;
    m_detailRequest.cancel();
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
    if (busy) {
        // A read under way may meet a file half rewritten; asked again
        // once the write is over.
        m_detailRequest.cancel();
    }
    if (!busy) {
        // A write has just finished, and any library may now say something
        // else about any track: a sample rate recorded, a grid replaced.
        forgetAnalyses();
        // What was delivered may be what the write replaced: dropped, and
        // read again if the popup still wants it.
        if (m_detailSerial >= 0) {
            m_detailSerial = -1;
            m_detailWaveform.clear();
            emit detailWaveformChanged();
        }
        // The Engine detail load() kept was read before the write: read it
        // again rather than hand that back.
        m_loadedDetailKept = false;
        m_loadedDetail = {};
        if (m_detailWanted) {
            requestDetailWaveform();
        }
    }
    if (!busy && m_advanceWhenLibraryFree) {
        m_advanceWhenLibraryFree = false;
        if (m_player && m_player->mediaStatus() == QMediaPlayer::EndOfMedia) {
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
        grid, static_cast<double>(position()), beats, static_cast<double>(duration()))));
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
    if (m_hasTrack && m_player) {
        m_player->play();
    }
}

void PlaybackController::pause()
{
    if (m_hasTrack && m_player) {
        m_player->pause();
    }
}

void PlaybackController::togglePlay()
{
    if (!m_hasTrack || !m_player) {
        return;
    }
    if (playing()) {
        m_player->pause();
    } else {
        m_player->play();
    }
}

void PlaybackController::seek(qint64 positionMs)
{
    if (m_player) {
        m_player->setPosition(positionMs);
    }
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
            // The nearest millisecond, not the one below: a rekordbox cue
            // moved onto the playback timeline rarely lands on a whole one.
            seek(std::llround(cue.value(QStringLiteral("positionMs")).toDouble()));
            return true;
        }
    }
    return false;
}

void PlaybackController::stop()
{
    emit trackAboutToChange();
    if (m_player) {
        m_player->stop();
        // Clearing the source, not just stopping playback, is what actually
        // releases the underlying file handle -- QMediaPlayer's backend (on
        // Linux, GStreamer/FFmpeg) keeps a track's file open as long as a
        // source is loaded, playing or not. Without this, ejecting a stick
        // right after playing a track from it consistently failed with
        // "target is busy": stop() left hasTrack false (so the UI correctly
        // showed no track loaded) while the real file descriptor stayed open
        // underneath, which the UI had no way to reveal.
        m_player->setSource(QUrl());
    }
    m_hasTrack = false;
    m_currentFormat.clear();
    m_currentSourceId.clear();
    m_currentFilePath.clear();
    m_waveform.clear();
    m_beatTimesMs.clear();
    m_beatNumbers.clear();
    m_drawnBeats.clear();
    ++m_trackSerial;
    dropDetailWaveform();
    m_cues.clear();
    m_cueShiftMs = 0.0;
    m_beatShiftMs = 0.0;
    emit trackChanged();
    markDurationKnown();
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
