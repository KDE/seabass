// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAudioOutput>
#include <QCache>
#include <QAbstractItemModel>
#include <QMediaPlayer>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>
#include <QtGlobal>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

#include "domain/audio_levels.hpp"

namespace seabass::infrastructure::rekordbox
{
class AnlzPathIndex;
}

class QAudioBuffer;
class QAudioBufferOutput;

namespace seabass::gui
{

// Plays a single track's audio file (via QMediaPlayer) and exposes its
// best-effort waveform preview for display, read on demand from whichever
// format the track came from -- never during a bulk library scan, since
// waveform decoding needs its own file I/O per track.
class PlaybackController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY trackChanged)
    Q_PROPERTY(QString currentFormat READ currentFormat NOTIFY trackChanged)
    Q_PROPERTY(QString currentSourceId READ currentSourceId NOTIFY trackChanged)
    // The library the loaded track came from. An id names a track only
    // within one: two sticks of one format both have a track 42.
    Q_PROPERTY(QString currentLibraryPath READ currentLibraryPath NOTIFY trackChanged)
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY trackChanged)
    Q_PROPERTY(QString artworkPath READ artworkPath NOTIFY trackChanged)
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY trackChanged)
    Q_PROPERTY(QVariantList cues READ cues NOTIFY trackChanged)
    // The loaded track's beat grid as rekordbox or Engine analysed it:
    // when each beat falls, in milliseconds, and its place in the bar (1
    // to 4, 1 the downbeat, 0 unknown), index for index. Empty for a
    // track with no grid. This is what a display keeps time by -- it is
    // what the DJ's own decks keep time by. The live levels below say how
    // loud the music is, not when the beat is.
    Q_PROPERTY(QList<qreal> beatTimesMs READ beatTimesMs NOTIFY trackChanged)
    Q_PROPERTY(QList<int> beatNumbers READ beatNumbers NOTIFY trackChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // How loud the playing audio is right now, in three bands of 0..1,
    // and a count that goes up by one on every beat -- a count rather
    // than a signal so that QML can simply bind to it. Measured from the
    // decoded audio as it plays (see AudioLevelMeter), so it is there for
    // every format, waveform or no waveform. All zero while nothing
    // plays. liveLevels says whether audio has actually been delivered --
    // not whether it could be: Qt has the means from 6.8 on, but only its
    // FFmpeg backend uses them, and on GStreamer not one buffer arrives.
    // Until one has, a display has the stored waveform to fall back on.
    // Whether there is a track before or after the loaded one in the
    // queue -- see setQueue().
    // Set on the app's one player, and only there: Seabass then appears
    // on the desktop as a media player, which is how the keyboard's media
    // keys, the desktop's media applet and the lock screen reach it (see
    // MprisService; Linux only, a no-op elsewhere). Off by default so
    // that the many controllers tests and previews make stay invisible.
    Q_PROPERTY(bool desktopMediaControls READ desktopMediaControls WRITE setDesktopMediaControls NOTIFY desktopMediaControlsChanged)
    Q_PROPERTY(bool hasNext READ hasNext NOTIFY queueChanged)
    Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY queueChanged)
    // Set while anything is writing a library (Main.qml binds it to
    // EditSessionRegistry.anyWriting). Moving through the queue opens
    // the library -- Engine's through libdjinterop, which opens it for
    // writing -- and with a queue that happens with nobody at the
    // keyboard: a track ends in the middle of a save. While this is set
    // next(), previous() and the end of a track load nothing; a track
    // that ended meanwhile is followed by the next one when it clears.
    Q_PROPERTY(bool libraryBusy READ libraryBusy WRITE setLibraryBusy NOTIFY libraryBusyChanged)
    Q_PROPERTY(bool liveLevels READ liveLevels NOTIFY liveLevelsChanged)
    Q_PROPERTY(qreal levelLow READ levelLow NOTIFY levelsChanged)
    Q_PROPERTY(qreal levelMid READ levelMid NOTIFY levelsChanged)
    Q_PROPERTY(qreal levelHigh READ levelHigh NOTIFY levelsChanged)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY beatCountChanged)

public:
    explicit PlaybackController(QObject *parent = nullptr);

    bool hasTrack() const { return m_hasTrack; }
    // Which track is loaded, for a page's own track list to compare
    // against its own rows (e.g. ScanPage.qml highlighting the currently
    // playing row) -- sourceId alone isn't unique across formats, so
    // both are exposed and a caller should compare both.
    QString currentFormat() const { return m_currentFormat; }
    QString currentSourceId() const { return m_currentSourceId; }
    QString currentLibraryPath() const { return m_currentLibraryPath; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString artworkPath() const { return m_artworkPath; }
    QVariantList waveform() const { return m_waveform; }
    QVariantList cues() const { return m_cues; }
    QList<qreal> beatTimesMs() const { return m_beatTimesMs; }
    QList<int> beatNumbers() const { return m_beatNumbers; }
    qint64 duration() const { return m_player.duration(); }
    qint64 position() const { return m_player.position(); }
    bool playing() const { return m_player.playbackState() == QMediaPlayer::PlayingState; }
    bool liveLevels() const { return m_buffersArrive; }
    bool libraryBusy() const { return m_libraryBusy; }
    void setLibraryBusy(bool busy);
    bool desktopMediaControls() const { return m_desktopMediaControls; }
    void setDesktopMediaControls(bool enabled);
    qreal levelLow() const { return m_levels.low; }
    qreal levelMid() const { return m_levels.mid; }
    qreal levelHigh() const { return m_levels.high; }
    int beatCount() const { return m_beatCount; }
    // Linear gain, 0.0 (silent) to 1.0 (unattenuated) -- matches
    // QAudioOutput::volume()'s own scale directly, no remapping.
    qreal volume() const { return m_audioOutput.volume(); }
    void setVolume(qreal volume);
    QString errorMessage() const { return m_errorMessage; }

    // format is "rekordbox" or "engine"; libraryPath is the corresponding
    // DetectedStick.rekordboxPath / .enginePath; sourceId/filePath/title/
    // artist come straight from the track row being played.
    Q_INVOKABLE void load(const QString &format, const QString &libraryPath, const QString &sourceId,
                           const QString &filePath, const QString &title, const QString &artist,
                           const QString &artworkPath, const QVariantList &cues);

    // Reads a track's waveform preview without touching playback state --
    // for a read-only preview (e.g. the Library page's per-track info
    // popup, or a TrackWaveformCard delegate in a conflict/duplicate
    // list) that shouldn't interrupt whatever's currently loaded/playing.
    // Same on-demand, synchronous-on-the-UI-thread read as load() itself
    // uses; a single track's preview blob is small enough that this
    // hasn't needed backgrounding so far (see load()'s own doc comment).
    // Cached (see m_waveformCache) since list views re-decode the same
    // few tracks' waveforms repeatedly as delegates scroll in and out.
    Q_INVOKABLE QVariantList waveformFor(const QString &format, const QString &libraryPath,
                                          const QString &sourceId) const;

    // The list the player walks: next(), previous(), and on to the next
    // track by itself when one ends. It is a track list model -- any
    // model with sourceId, filePath, title, artist, artworkPath, cues and
    // streamingSource roles -- looked at live, not copied: the queue IS
    // the list on screen, in the order and with the filter it has now.
    // The loaded track is found in it by its id each time, so re-sorting
    // the list under a playing track moves "next" with it; a track that
    // is no longer in the list has no next. Rows that cannot be played (a
    // streaming track, a missing file) are stepped over.
    // A plain load() keeps the queue as long as it stays in the same
    // library.
    Q_INVOKABLE void setQueue(QAbstractItemModel *model, const QString &format, const QString &libraryPath);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // The queue row the loaded track is on, or -1.
    Q_INVOKABLE int currentQueueRow() const;
    bool hasNext() const { return playableRowFrom(currentQueueRow(), +1) >= 0; }
    bool hasPrevious() const { return playableRowFrom(currentQueueRow(), -1) >= 0; }

    // Jumps by whole beats of the track's grid, forward or back, landing
    // on the same place in the beat it left -- a skip that stays in time.
    // Where there is no grid a beat is taken as 625 ms, which makes four
    // of them two and a half seconds.
    Q_INVOKABLE void skipBeats(int beats);
    // Seeks to hot cue `number` of the loaded track, numbered as the
    // players and every page here number them: 1 to 8, Engine's 0-based
    // slots already shifted by its reader. Playing or paused stays as it
    // was, like pressing a hot cue pad. Returns whether that cue exists;
    // a track without it, or no track, is left where it is.
    Q_INVOKABLE bool jumpToHotCue(int number);

    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void stop();

signals:
    void trackChanged();
    void durationChanged();
    void positionChanged();
    void playingChanged();
    void volumeChanged();
    void errorMessageChanged();
    void levelsChanged();
    void queueChanged();
    void libraryBusyChanged();
    void liveLevelsChanged();
    // The position jumped -- a seek, a skip -- rather than played on.
    void seeked(qint64 positionMs);
    void desktopMediaControlsChanged();
    // next(), previous() or the end of a track moved the player on from
    // `previousSourceId`: whoever was showing that track may want to follow.
    void advanced(const QString &previousSourceId);
    void beatCountChanged();

private:
    void setErrorMessage(const QString &message);
    void meterBuffer(const QAudioBuffer &buffer);
    // Levels back to zero and the meter's memory gone: nothing is
    // playing, or something else is about to.
    void clearLevels();
    int playableRowFrom(int row, int step) const;
    void dropQueue();
    void loadQueueRow(int row);
    QVariant queueValue(int row, const QByteArray &role) const;

    QMediaPlayer m_player;
    QAudioOutput m_audioOutput;
    // Owned through QObject parentage; null where Qt cannot deliver the
    // decoded audio (before 6.8).
    QAudioBufferOutput *m_bufferOutput = nullptr;
    domain::AudioLevelMeter m_meter;
    domain::AudioLevels m_levels;
    int m_beatCount = 0;
    bool m_hasTrack = false;
    QString m_currentFormat;
    QString m_currentSourceId;
    QString m_currentLibraryPath;
    QString m_title;
    QString m_artist;
    QString m_artworkPath;
    QVariantList m_waveform;
    QVariantList m_cues;
    QPointer<QAbstractItemModel> m_queue;
    bool m_desktopMediaControls = false;
    bool m_libraryBusy = false;
    bool m_advanceWhenLibraryFree = false;
    bool m_buffersArrive = false;
    QObject *m_mpris = nullptr;
    QString m_queueFormat;
    QString m_queueLibraryPath;
    QList<qreal> m_beatTimesMs;
    QList<int> m_beatNumbers;
    QString m_errorMessage;
    // Keyed on format+libraryPath+sourceId (see waveformFor()'s own doc
    // comment) -- QCache owns the heap-allocated values it stores; 300
    // is comfortably more than a scrollable list ever has on screen or
    // recently scrolled past at once.
    mutable QCache<QString, QVariantList> m_waveformCache{300};

    // Every track's analysis-file path on a stick, from one pass over its
    // export.pdb, for as long as that export.pdb is unchanged. Without it
    // each waveform parsed the whole database to find one path: about 20
    // ms a track on a 1,400-track stick, on the UI thread, so opening a
    // list of waveform cards (Library Health's Cues at 0:00) froze for as
    // long as it took to fill the screen, and scrolling stuttered row by
    // row. Keyed on the PIONEER folder; see anlzIndexFor() for when an
    // entry is rebuilt.
    struct AnlzIndexEntry
    {
        std::filesystem::file_time_type modified;
        std::uintmax_t size = 0;
        std::shared_ptr<const infrastructure::rekordbox::AnlzPathIndex> index;
    };
    std::shared_ptr<const infrastructure::rekordbox::AnlzPathIndex> anlzIndexFor(const QString &pioneerRoot) const;
    mutable std::map<QString, AnlzIndexEntry> m_anlzIndexes;
};

}  // namespace seabass::gui
