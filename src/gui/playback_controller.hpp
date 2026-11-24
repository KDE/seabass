// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QCache>
#include <QAbstractItemModel>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>
#include <QtGlobal>

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <set>

#include "domain/audio_levels.hpp"
#include "domain/beat_grid.hpp"
#include "domain/waveform.hpp"
#include "gui/async_request.hpp"

namespace seabass::infrastructure::rekordbox
{
class AnlzPathIndex;
}

class QAudioBuffer;
class QAudioBufferOutput;
class QAudioOutput;
class QMediaPlayer;

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
    // Art to show when artworkPath names a file that is not there: an
    // Engine row's rekordbox sibling's cover (see TrackListModel's
    // fallbackArtworkPath role). Empty when there is none.
    Q_PROPERTY(QString fallbackArtworkPath READ fallbackArtworkPath NOTIFY trackChanged)
    // waveform, beatTimesMs and beatNumbers come with the track, or after
    // it when it was loaded during a write (see analysisPending).
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY loadedAnalysisChanged)
    // Its own signal, not trackChanged: the cues of the loaded track can
    // arrive after it (see takeCues()), and that is not a new track.
    Q_PROPERTY(QVariantList cues READ cues NOTIFY cuesChanged)
    // The loaded track's beat grid as rekordbox or Engine analysed it:
    // when each beat falls, in milliseconds, and its place in the bar (1
    // to 4, 1 the downbeat, 0 unknown), index for index. Empty for a
    // track with no grid. This is what a display keeps time by -- it is
    // what the DJ's own decks keep time by. The live levels below say how
    // loud the music is, not when the beat is.
    Q_PROPERTY(QList<qreal> beatTimesMs READ beatTimesMs NOTIFY loadedAnalysisChanged)
    Q_PROPERTY(QList<int> beatNumbers READ beatNumbers NOTIFY loadedAnalysisChanged)
    // True while the loaded track's analysis (waveform, beats, the grid
    // beatTicksAt() draws) waits for a write to end: load() during
    // libraryBusy plays the audio but opens no library, and the analysis
    // is read when libraryBusy goes false. A binding on beatTicksAt()
    // reads this too, so the grid is laid out again then.
    Q_PROPERTY(bool analysisPending READ analysisPending NOTIFY loadedAnalysisChanged)
    // Goes up by one whenever the loaded track changes (load, loadFile,
    // stop): what a binding on beatTicksAt() depends on to lay out the
    // new track's grid, without reading a list of every beat to notice.
    Q_PROPERTY(int trackSerial READ trackSerial NOTIFY trackChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    // The trackSerial whose length has been reported: equal to
    // trackSerial once the loaded track's duration is known, or known
    // never to come (a missing file, one that does not decode, stop()).
    // Until then `duration` may still be the old track's: a display that
    // waits for the new length waits for this, not for duration > 0.
    Q_PROPERTY(int durationSerial READ durationSerial NOTIFY durationSerialChanged)
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
    // Goes up each time forgetAnalyses() drops what waveformFor(),
    // beatGridFor() and beatTicksFor() kept: when a write ends and when
    // the sticks change. A binding that calls one of those reads this
    // too, so it asks again then. Without it a row keeps what it was
    // given: an empty answer from during a write (see libraryBusy), or
    // the analysis the write has just replaced.
    // Never -1: a binding reads it as `analysisRevision !== -1`, true
    // for a stand-in player (tests) that has no such property too.
    Q_PROPERTY(int analysisRevision READ analysisRevision NOTIFY analysisRevisionChanged)
    Q_PROPERTY(bool liveLevels READ liveLevels NOTIFY liveLevelsChanged)
    Q_PROPERTY(qreal levelLow READ levelLow NOTIFY levelsChanged)
    Q_PROPERTY(qreal levelMid READ levelMid NOTIFY levelsChanged)
    Q_PROPERTY(qreal levelHigh READ levelHigh NOTIFY levelsChanged)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY beatCountChanged)
    // The loaded track's waveform at the finest resolution its library
    // holds and Seabass reads (domain::DetailWaveform), for the zoomed
    // waveform popup (#104): {columns, msPerColumn, startMs}. `columns`
    // is an ArrayBuffer in QML, three bytes a column (low, mid, high, 0
    // to 255): a list of maps would be some forty thousand of them.
    // msPerColumn 0 spreads the columns over the track's length; startMs
    // is where the first column falls on the timeline the player plays
    // on, moved as the beats are. Empty columns until requestDetailWaveform()
    // has an answer, and where there is nothing finer (a view then draws
    // `waveform`).
    Q_PROPERTY(QVariantMap detailWaveform READ detailWaveform NOTIFY detailWaveformChanged)

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
    QString fallbackArtworkPath() const { return m_fallbackArtworkPath; }
    QVariantList waveform() const { return m_waveform; }
    QVariantList cues() const { return m_cues; }
    QList<qreal> beatTimesMs() const { return m_beatTimesMs; }
    QList<int> beatNumbers() const { return m_beatNumbers; }
    int trackSerial() const { return m_trackSerial; }
    int durationSerial() const { return m_durationSerial; }
    qint64 duration() const;
    qint64 position() const;
    bool playing() const;
    bool liveLevels() const { return m_buffersArrive; }
    bool libraryBusy() const { return m_libraryBusy; }
    int analysisRevision() const { return m_analysisRevision; }
    bool analysisPending() const { return m_analysisPending; }
    void setLibraryBusy(bool busy);
    bool desktopMediaControls() const { return m_desktopMediaControls; }
    void setDesktopMediaControls(bool enabled);
    qreal levelLow() const { return m_levels.low; }
    qreal levelMid() const { return m_levels.mid; }
    qreal levelHigh() const { return m_levels.high; }
    int beatCount() const { return m_beatCount; }
    QVariantMap detailWaveform() const;
    // Linear gain, 0.0 (silent) to 1.0 (unattenuated) -- matches
    // QAudioOutput::volume()'s own scale directly, no remapping.
    qreal volume() const { return m_volume; }
    void setVolume(qreal volume);
    QString errorMessage() const { return m_errorMessage; }

    // format is "rekordbox" or "engine"; libraryPath is the corresponding
    // DetectedStick.rekordboxPath / .enginePath; sourceId/filePath/title/
    // artist come straight from the track row being played.
    // fallbackArtworkPath is the row's own; left out, it is looked up in
    // the queue when the track is in it (Browse offers its list as the
    // queue before it plays a row), and is empty otherwise.
    // startPlaying false loads the track paused at its start, the way
    // opening the waveform popup with nothing playing does (#104).
    Q_INVOKABLE void load(const QString &format, const QString &libraryPath, const QString &sourceId,
                           const QString &filePath, const QString &title, const QString &artist,
                           const QString &artworkPath, const QVariantList &cues,
                           const QString &fallbackArtworkPath = QString(), bool startPlaying = true);

    // A plain audio file that no catalog lists: a set recording on the
    // stick (Clean Up Recordings). What load() does minus every catalog
    // part: no analysis is read (the sourceId is not a catalog's, and a
    // rekordbox lookup would parse it as a number), no waveform, beats,
    // cues or artwork, and no queue, so next and previous do nothing.
    // format ("engine" or "rekordbox") only picks the form the play key
    // takes. The sourceId becomes "recording:" + filePath.
    Q_INVOKABLE void loadFile(const QString &format, const QString &libraryPath, const QString &filePath,
                              const QString &title, const QString &artist);

    // The loaded track's cues, read after it was loaded: Browse lists a
    // rekordbox stick before its cue pass is done, and a track played in
    // that time was loaded with none. Taken only when format, library
    // and sourceId name the loaded track; playback, position and
    // everything else about it stay as they are. Returns whether they
    // were taken.
    Q_INVOKABLE bool takeCues(const QString &format, const QString &libraryPath, const QString &sourceId,
                              const QVariantList &cues);

    // Reads a track's waveform preview without touching playback state --
    // for a read-only preview (e.g. the Library page's per-track info
    // popup, or a TrackWaveformCard delegate in a conflict/duplicate
    // list) that shouldn't interrupt whatever's currently loaded/playing.
    // Same on-demand, synchronous-on-the-UI-thread read as load() itself
    // uses; a single track's preview blob is small enough that this
    // hasn't needed backgrounding so far (see load()'s own doc comment).
    // Cached (see m_waveformCache) since list views re-decode the same
    // few tracks' waveforms repeatedly as delegates scroll in and out.
    // While libraryBusy, only what is kept: nothing is read then, and a
    // binding asks again through analysisRevision.
    Q_INVOKABLE QVariantList waveformFor(const QString &format, const QString &libraryPath,
                                          const QString &sourceId) const;
    // Drops every analysis waveformFor(), beatGridFor() and beatTicksFor()
    // have kept, so the next ask reads the library again. Called when a
    // write ends (libraryBusy going false) and, from Main.qml, when the
    // set of sticks changes: a kept entry is keyed on a path and a track
    // id, and another stick at the same path has other tracks under the
    // same small Engine ids. The loaded track's own grid is not touched.
    Q_INVOKABLE void forgetAnalyses();
    // The same track's beat grid, as WaveformView.beatData takes it
    // (beat_grid_display.hpp), from the same read and the same cache
    // entry as waveformFor(): a
    // preview that shows both reads the analysis once. On the library's
    // own timeline, not the player's.
    Q_INVOKABLE QVariantList beatGridFor(const QString &format, const QString &libraryPath,
                                          const QString &sourceId) const;
    // What every view that draws a grid binds to: only the beats of that
    // grid a waveform `widthPx` wide showing `durationMs` will draw
    // (layoutBeatTicks()), a few dozen where the whole grid is some 1300,
    // so that no view turns every beat into a map and lays them all out
    // again, on load or at each width of a resize. Laid out by the view
    // at that width they give the same ticks. Empty without a length or a
    // width.
    Q_INVOKABLE QVariantList beatTicksFor(const QString &format, const QString &libraryPath,
                                           const QString &sourceId, double durationMs, double widthPx) const;
    // The same for the loaded track's grid, on the timeline the player
    // plays on; nothing for an Engine row that records no sample rate
    // (the player keeps time by that grid, but nothing draws it as fact).
    // Bind with trackSerial and analysisPending so a new track, and one
    // whose analysis came after a write, is laid out again.
    Q_INVOKABLE QVariantList beatTicksAt(double durationMs, double widthPx) const;
    // Asks for the loaded track's detailWaveform, read off the GUI thread
    // (AsyncRequest), never while a library is being written (asked again
    // once the write is over), and dropped if another track has been
    // loaded by the time it arrives. rekordbox's is its scrolling waveform
    // (PWV7, else PWV3). Engine stores nothing finer than an overview, so
    // an Engine track's is made from its audio file on Engine's timeline,
    // as a Denon player makes its own (qt_multimedia_detail_waveform.hpp),
    // and kept for the session for the last few files (m_audioDetails);
    // where the file cannot be decoded it is Engine's stored waveform, as
    // it always was. While a library is being written the stored one
    // load() read is handed over at once, and the audio's follows once
    // the write is over.
    Q_INVOKABLE void requestDetailWaveform();
    // The popup has closed: what it asked for is no longer wanted, and a
    // write ending does not read it again for nobody.
    Q_INVOKABLE void releaseDetailWaveform();
    // How many audio files this controller has decoded (or tried to) for
    // an Engine track's detail: for the tests of its cache.
    Q_INVOKABLE int audioDetailDecodes() const { return m_audioDetailDecodes; }

    // A track's cues as a page holds them (on rekordbox's timeline, see
    // playback_timeline.hpp) moved onto the timeline of the beat grid
    // beatTicksFor() draws for that track: for an Engine MP3 Engine's,
    // by the same shift the player moves them by; for a rekordbox track
    // they are on its grid's timeline already and come back as given.
    // What the details pane draws, so a cue sits on the beat it marks.
    Q_INVOKABLE QVariantList cuesOnGridTimeline(const QString &format, const QString &filePath,
                                                const QVariantList &cues) const;
    // The other way: a position picked on that view (a click on the
    // details pane's waveform) put on the timeline the cues are held on
    // and a cue writer takes, rekordbox's, so the cue written sits where
    // the click was (#123). Unchanged for anything but an Engine MP3.
    Q_INVOKABLE double positionOnCueTimeline(const QString &format, const QString &filePath,
                                             double positionMs) const;

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
    // Emitted before load(), loadFile() or stop() changes anything: what
    // was shown of the old track can still be read (the player bar keeps
    // a picture of it to blend from, #104).
    void trackAboutToChange();
    void trackChanged();
    void cuesChanged();
    void durationChanged();
    void positionChanged();
    void playingChanged();
    void volumeChanged();
    void errorMessageChanged();
    void levelsChanged();
    void queueChanged();
    void libraryBusyChanged();
    void analysisRevisionChanged();
    void loadedAnalysisChanged();
    void liveLevelsChanged();
    // The position jumped -- a seek, a skip -- rather than played on.
    void seeked(qint64 positionMs);
    void desktopMediaControlsChanged();
    // next(), previous() or the end of a track moved the player on from
    // `previousSourceId`: whoever was showing that track may want to follow.
    void advanced(const QString &previousSourceId);
    void beatCountChanged();
    void detailWaveformChanged();
    void durationSerialChanged();

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

    // The player, made the first time a track is loaded (gh#96). Making
    // a QMediaPlayer loads Qt's media backend, and on Windows that is the
    // FFmpeg plugin with 19 DLLs and 69 MB behind it, every one of which
    // Defender scans the first time it is loaded after an install. Made
    // with the controller, that came before the first window. Until there
    // is a player nothing is loaded, so nothing plays: duration, position
    // and playing read 0, 0 and false, and the transport does nothing.
    QMediaPlayer *player();
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    // Kept here, so it can be set (MPRIS, a test) before there is an
    // output to hand it to. 1.0 is QAudioOutput's own default.
    qreal m_volume = 1.0;
    // Owned through QObject parentage; null where Qt cannot deliver the
    // decoded audio (before 6.8) and until there is a player.
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
    QString m_fallbackArtworkPath;
    QVariantList m_waveform;
    // On the playback timeline: the page's list moved by m_cueShiftMs
    // (infrastructure/audio/playback_timeline.hpp), as are the beat
    // times below. 0 for anything but an MP3 with a readable lead-in.
    QVariantList m_cues;
    double m_cueShiftMs = 0.0;
    QPointer<QAbstractItemModel> m_queue;
    bool m_desktopMediaControls = false;
    bool m_libraryBusy = false;
    int m_analysisRevision = 0;
    // load() ran while m_libraryBusy: the loaded track's analysis is read
    // when the write ends (setLibraryBusy(false)).
    bool m_analysisPending = false;
    bool m_advanceWhenLibraryFree = false;
    bool m_buffersArrive = false;
    QObject *m_mpris = nullptr;
    QString m_queueFormat;
    QString m_queueLibraryPath;
    QList<qreal> m_beatTimesMs;
    QList<int> m_beatNumbers;
    // The loaded track's grid as beatTicksAt() draws it: shifted onto the
    // playback timeline, and whether its rate is the library's.
    std::vector<domain::Beat> m_drawnBeats;
    bool m_drawnBeatsRateRecorded = true;
    // How far the loaded track's grid was moved onto the playback
    // timeline; its detail waveform moves with it.
    double m_beatShiftMs = 0.0;
    // detailWaveform for the track of m_detailSerial; m_loadedDetail is
    // what load() read of an Engine track's waveform, handed over as it
    // is when asked for. m_detailWanted: asked for, for this track.
    QVariantMap m_detailWaveform;
    int m_detailSerial = -1;
    bool m_detailWanted = false;
    domain::DetailWaveform m_loadedDetail;
    bool m_loadedDetailKept = false;
    static constexpr size_t MaximumKeptDetailColumns = 4096;
    int m_durationSerial = -1;
    void markDurationKnown();
    // Reads the loaded track's analysis (waveform, beats, the detail kept
    // for the popup) into the m_ members, on the playback timeline.
    void readLoadedAnalysis();
    // `columns` as detailWaveform hands them over: three bytes a column.
    void setDetailWaveform(const QByteArray &columns, double msPerColumn, int serial);
    void dropDetailWaveform();
    // An audio file's detail as made from it, or that it could not be
    // made, for the file as it was when it was decoded: its size and
    // modification time are read in the worker, before the decode, so
    // that a file rewritten during it is never kept under its new time.
    struct AudioDetailEntry
    {
        QString path;
        qint64 size = -1;
        qint64 modifiedMs = 0;
        bool failed = false;
        QByteArray columns;
        double msPerColumn = 0.0;
    };
    // What the worker hands back, the columns already as bytes.
    struct DetailRead
    {
        QByteArray columns;
        double msPerColumn = 0.0;
        // Set where the worker decoded the audio file (or tried to):
        // what to keep in m_audioDetails, success or failure.
        std::shared_ptr<const AudioDetailEntry> decoded;
    };
    AsyncRequest<DetailRead> m_detailRequest{this, nullptr};
    // The loaded track's audio file, for an Engine track's detail.
    QString m_currentFilePath;
    // Details made from audio files this session, most recently used
    // first, the last MaximumAudioDetails files: opening the popup on the
    // same track again does not decode it again, and a file that could
    // not be decoded is not tried (and waited for) again either. Three
    // bytes a column at 150 a second: about 160 KB for six minutes, 3.2
    // MB for a two-hour mix. The worker checks an entry against the file
    // (size and modification time) before using it; nothing is looked at
    // on the GUI thread.
    std::list<std::shared_ptr<const AudioDetailEntry>> m_audioDetails;
    static constexpr std::size_t MaximumAudioDetails = 4;
    std::shared_ptr<const AudioDetailEntry> audioDetailFor(const QString &filePath);
    void keepAudioDetail(std::shared_ptr<const AudioDetailEntry> entry);
    int m_audioDetailDecodes = 0;
    int m_trackSerial = 0;
    QString m_errorMessage;
    // One read of a track's analysis, for waveformFor() and
    // beatGridFor() alike. The beats are kept as they were read (16
    // bytes a beat) and made into a list only when asked for: a list of
    // maps is some 15 times that, and 300 tracks of 1300 beats each
    // would hold on to about 100 MB.
    struct CachedAnalysis
    {
        QVariantList waveform;
        std::vector<domain::Beat> beats;
        bool beatRateRecorded = true;
    };
    CachedAnalysis cachedAnalysis(const QString &format, const QString &libraryPath, const QString &sourceId) const;
    // Keyed on format+libraryPath+sourceId (see waveformFor()'s own doc
    // comment) -- QCache owns the heap-allocated values it stores; 300
    // is comfortably more than a scrollable list ever has on screen or
    // recently scrolled past at once.
    mutable QCache<QString, CachedAnalysis> m_analysisCache{300};

    // The read copy of an Engine Library's m.db made off the GUI thread
    // for the rows that wait on it (#110; see cachedAnalysis()), once per
    // library at a time. A library whose copy could not be made is not
    // tried again until the next write ends, so its rows do not ask in a
    // loop; they stay empty, as during a write.
    void prepareReadCopy(const QString &engineLibraryPath) const;
    mutable std::set<QString> m_readCopiesPreparing;
    mutable std::set<QString> m_readCopiesFailed;

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
