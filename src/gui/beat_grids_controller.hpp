// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QCache>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/analysis_writer.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "gui/async_request.hpp"

namespace seabass::infrastructure::rekordbox
{
class AnlzPathIndex;
}

namespace seabass::gui
{

class AnalysisSaveTally;
class LibraryEditSession;

// The tracks of the playlist the Beat Grids & Waveforms page shows, in the
// playlist's order: what rekordbox says of each (title, artist, tempo,
// key, cover), whether it is ticked, and what copying would do to it, as
// the preview last worked it out. Rows whose preview has not arrived say
// so (pending) and carry no state.
class BeatGridTrackListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        RekordboxIdRole = Qt::UserRole + 1,
        TitleRole,
        ArtistRole,
        BpmRole,
        KeyNameRole,
        ArtworkUrlRole,
        FallbackArtworkUrlRole,
        MatchedRole,
        TickedRole,
        PendingRole,
        GridStateRole,
        GridToneRole,
        GridTipRole,
        WaveformStateRole,
        WaveformToneRole,
        WaveformTipRole,
        NoteRole,
        NoteToneRole,
    };

    struct Row
    {
        QString rekordboxId;
        QString title;
        QString artist;
        double bpm = 0.0;
        QString keyName;
        QString artworkUrl;
        QString fallbackArtworkUrl;
        // In the Engine library, as one row for one rekordbox track: only
        // such a track can be ticked.
        bool matched = false;
        // Below: refreshed by the controller from its ticks and states.
        bool ticked = false;
        bool pending = false;
        QString gridState;
        QString gridTone;
        QString gridTip;
        QString waveformState;
        QString waveformTone;
        QString waveformTip;
        QString note;
        QString noteTone;
    };

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(std::vector<Row> rows);
    // Every row through `update`; the rows it changed are announced.
    void refresh(const std::function<void(Row &)> &update);
    const std::vector<Row> &rows() const { return m_rows; }

    // For tests and the page: a row as the delegate sees it, by index or by
    // its rekordbox id ({} for none).
    Q_INVOKABLE QVariantMap get(int index) const;
    Q_INVOKABLE int indexOf(const QString &rekordboxId) const;

signals:
    void countChanged();

private:
    std::vector<Row> m_rows;
};

// What the page's worker reads once per stick: both catalogs, the tracks
// they share (planBeatGridCopy), rekordbox's playlists, and what keeps a
// copy from being offered at all.
struct BeatGridLibrary
{
    QString error;
    bool cancelled = false;
    // Engine 1.x, whose analysis Seabass does not write.
    bool engineTooOld = false;
    bool stickReadOnly = false;
    QString ledgerError;
    std::string stickRoot;
    QString rekordboxPath;
    QString enginePath;

    struct Track
    {
        std::string rekordboxId;
        std::string title;
        std::string artist;
        double bpm = 0.0;
        std::string key;
        QString artworkUrl;
        QString fallbackArtworkUrl;
        // What the zoomed waveform popup loads into the player: the audio
        // file (empty where there is none to play), the covers as paths,
        // the length and rekordbox's cues.
        std::string filePath;
        std::string artworkPath;
        std::string fallbackArtworkPath;
        double durationMs = 0.0;
        std::vector<domain::CuePoint> cues;
        // The same for Engine's row of the track, for the popup opened from
        // the comparison's Engine lane: its file and its cues (empty where
        // Engine has no row).
        std::string engineFilePath;
        std::vector<domain::CuePoint> engineCues;
        // Into plan.items, -1 for a track with no Engine row of its own.
        int item = -1;
    };
    // rekordbox's tracks in the catalog's order: "All tracks".
    std::vector<Track> tracks;
    std::map<std::string, std::size_t> trackIndex;
    application::BeatGridCopyPlan plan;
    // Each playlist (its full path, "Folder/List") with its tracks' ids in
    // the playlist's order, by name.
    std::map<std::string, std::vector<std::string>> playlists;
    QVariantMap playlistThumbnails;
    std::shared_ptr<const infrastructure::rekordbox::AnlzPathIndex> anlzIndex;
};

// What one track's preview found: importAnalysis() with previewOnly, the
// options it ran with, and, where something would be written, the digest of
// rekordbox's analysis it read (rekordboxAnalysisDigest()): the change that
// writes it reads the analysis again and writes only that one. The
// analysis itself is not held: a few hundred kB a track, for thousands.
struct BeatGridTrackState
{
    bool failed = false;
    QString failure;
    application::AnalysisImportResult result;
    application::AnalysisImportOptions options;
    std::string analysisDigest;
    // Whether the row is analysed and the player's own analysis is kept on
    // the stick, and for a kept one what restoring it would do (with the
    // overview file it would put back or take away).
    application::EngineAnalysisFacts facts;
    std::optional<application::EngineAnalysisRestoreResult> restore;

    bool wouldWrite() const;
    bool canRestore() const;
};

// The Beat Grids & Waveforms page (Sync): rekordbox's beat grids and
// waveforms copied into the stick's Engine library for the playlists and
// tracks the user ticks.
//
// Loads both catalogs once, off the UI thread, and plans which rekordbox
// track is which Engine row. What copying would do to a track is worked
// out by a preview (importAnalysis with previewOnly: every check, nothing
// written), only for the tracks of the playlist shown and the ticked ones,
// in batches off the UI thread: one progress bar while a batch runs, a
// newer batch cancelling an older one, and every row a batch finished kept
// even when it was cancelled. A track's state is kept until the options
// change or a save lands.
//
// Ticks are by rekordbox track, so a tick survives switching playlists and
// a track in two playlists is ticked in both. Nothing is ticked at first.
//
// The counts are a preview's and are named for what they are (gridsToCopy,
// never "copied"): what a save did is counted from the changes that landed
// (lastSave*).
class BeatGridsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    // Reading the libraries: the page's busy overlay.
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int loadCurrent READ loadCurrent NOTIFY loadProgressChanged)
    Q_PROPERTY(int loadTotal READ loadTotal NOTIFY loadProgressChanged)
    Q_PROPERTY(QString loadLabel READ loadLabel NOTIFY loadProgressChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY libraryChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool engineTooOld READ engineTooOld NOTIFY libraryChanged)
    Q_PROPERTY(bool stickReadOnly READ stickReadOnly NOTIFY libraryChanged)
    Q_PROPERTY(QString ledgerWarning READ ledgerWarning NOTIFY ledgerWarningChanged)
    // What PlaylistListView reads of a controller.
    Q_PROPERTY(QStringList playlistNames READ playlistNames NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap playlistTrackCounts READ playlistTrackCounts NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap playlistThumbnails READ playlistThumbnails NOTIFY libraryChanged)
    Q_PROPERTY(int totalTrackCount READ totalTrackCount NOTIFY libraryChanged)
    Q_PROPERTY(int matchedCount READ matchedCount NOTIFY libraryChanged)
    // The playlist whose tracks are listed; "" for every track.
    Q_PROPERTY(QString shownPlaylist READ shownPlaylist WRITE setShownPlaylist NOTIFY shownPlaylistChanged)
    Q_PROPERTY(QAbstractListModel *tracks READ tracks CONSTANT)

    // What to copy, and whether over a grid set on the player. Both parts
    // on by default; the last off.
    Q_PROPERTY(bool grids READ grids WRITE setGrids NOTIFY optionsChanged)
    Q_PROPERTY(bool waveforms READ waveforms WRITE setWaveforms NOTIFY optionsChanged)
    Q_PROPERTY(bool replaceEngineEdits READ replaceEngineEdits WRITE setReplaceEngineEdits NOTIFY optionsChanged)

    // Moves whenever a tick does, for bindings that ask playlistCheckState().
    Q_PROPERTY(int tickRevision READ tickRevision NOTIFY ticksChanged)

    // A preview batch: the page's one inline progress bar.
    Q_PROPERTY(bool previewBusy READ previewBusy NOTIFY previewBusyChanged)
    Q_PROPERTY(int previewCurrent READ previewCurrent NOTIFY previewProgressChanged)
    Q_PROPERTY(int previewTotal READ previewTotal NOTIFY previewProgressChanged)

    // What the ticked tracks would get, from their previews.
    Q_PROPERTY(int tickedCount READ tickedCount NOTIFY summaryChanged)
    Q_PROPERTY(int tickedPendingCount READ tickedPendingCount NOTIFY summaryChanged)
    Q_PROPERTY(int gridsToCopy READ gridsToCopy NOTIFY summaryChanged)
    Q_PROPERTY(int waveformsToCopy READ waveformsToCopy NOTIFY summaryChanged)
    Q_PROPERTY(int tracksToComplete READ tracksToComplete NOTIFY summaryChanged)
    Q_PROPERTY(int tracksToWrite READ tracksToWrite NOTIFY summaryChanged)
    Q_PROPERTY(int leftAloneCount READ leftAloneCount NOTIFY summaryChanged)
    // [{reason, count}], the largest group first.
    Q_PROPERTY(QVariantList leftAlone READ leftAlone NOTIFY summaryChanged)
    Q_PROPERTY(QString summaryText READ summaryText NOTIFY summaryChanged)
    Q_PROPERTY(QString leftAloneText READ leftAloneText NOTIFY summaryChanged)
    Q_PROPERTY(bool canStage READ canStage NOTIFY summaryChanged)

    // The two ways back, over the ticked tracks: the player's own analysis
    // kept on the stick (and how many of those can be put back now), and
    // the analysed rows the player can be asked to analyse again.
    Q_PROPERTY(int keptTickedCount READ keptTickedCount NOTIFY summaryChanged)
    Q_PROPERTY(int restorableCount READ restorableCount NOTIFY summaryChanged)
    Q_PROPERTY(int analysedTickedCount READ analysedTickedCount NOTIFY summaryChanged)
    Q_PROPERTY(QString engineAnalysisText READ engineAnalysisText NOTIFY summaryChanged)
    Q_PROPERTY(bool canRestore READ canRestore NOTIFY summaryChanged)
    Q_PROPERTY(bool canLetPlayerAnalyse READ canLetPlayerAnalyse NOTIFY summaryChanged)
    // rekordbox tracks of the list shown with no Engine row, and the
    // sentence that says so ("" for none), with a link ("sync") to Sync
    // after Rekordbox Export.
    Q_PROPERTY(int notInEngineShown READ notInEngineShown NOTIFY notInEngineChanged)
    Q_PROPERTY(QString notInEngineText READ notInEngineText NOTIFY notInEngineChanged)

    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY stagedChanged)
    // What is staged: "copy", "restore", "letPlayerAnalyse", or "".
    Q_PROPERTY(QString stagedAction READ stagedAction NOTIFY stagedChanged)
    Q_PROPERTY(bool writing READ writing NOTIFY writingChanged)

    // The last save, counted from the changes that landed.
    Q_PROPERTY(int lastSaveGrids READ lastSaveGrids NOTIFY lastSaveChanged)
    Q_PROPERTY(int lastSaveWaveforms READ lastSaveWaveforms NOTIFY lastSaveChanged)
    Q_PROPERTY(int lastSaveCompleted READ lastSaveCompleted NOTIFY lastSaveChanged)
    Q_PROPERTY(int lastSaveTracks READ lastSaveTracks NOTIFY lastSaveChanged)
    Q_PROPERTY(int lastSaveRestored READ lastSaveRestored NOTIFY lastSaveChanged)
    Q_PROPERTY(int lastSaveMarked READ lastSaveMarked NOTIFY lastSaveChanged)
    Q_PROPERTY(QString lastSaveText READ lastSaveText NOTIFY lastSaveChanged)

    // How many rows' analyses have been read for their waveforms: what a
    // test watches to see that nothing is read while a library is written.
    Q_PROPERTY(int rowAnalysisReads READ rowAnalysisReads NOTIFY rowAnalysisReadsChanged)

public:
    explicit BeatGridsController(QObject *parent = nullptr);
    ~BeatGridsController() override;

    bool loading() const { return m_load.busy(); }
    int loadCurrent() const { return m_loadCurrent; }
    int loadTotal() const { return m_loadTotal; }
    QString loadLabel() const { return m_loadLabel; }
    bool loaded() const { return m_library != nullptr; }
    QString errorMessage() const { return m_errorMessage; }
    bool engineTooOld() const { return m_library && m_library->engineTooOld; }
    bool stickReadOnly() const { return m_library && m_library->stickReadOnly; }
    QString ledgerWarning() const;
    QStringList playlistNames() const { return m_playlistNames; }
    QVariantMap playlistTrackCounts() const { return m_playlistTrackCounts; }
    QVariantMap playlistThumbnails() const;
    int totalTrackCount() const { return m_library ? static_cast<int>(m_library->tracks.size()) : 0; }
    int matchedCount() const { return m_library ? static_cast<int>(m_library->plan.items.size()) : 0; }
    QString shownPlaylist() const { return m_shownPlaylist; }
    void setShownPlaylist(const QString &name);
    QAbstractListModel *tracks() { return &m_tracks; }
    bool grids() const { return m_options.grids; }
    void setGrids(bool on);
    bool waveforms() const { return m_options.waveforms; }
    void setWaveforms(bool on);
    bool replaceEngineEdits() const { return m_options.overwriteEngineEdits; }
    void setReplaceEngineEdits(bool on);
    int tickRevision() const { return m_tickRevision; }
    bool previewBusy() const { return m_preview.busy(); }
    int previewCurrent() const { return m_previewCurrent; }
    int previewTotal() const { return m_previewTotal; }

    int tickedCount() const { return static_cast<int>(m_ticked.size()); }
    int tickedPendingCount() const { return m_summary.pending; }
    int gridsToCopy() const { return m_summary.grids; }
    int waveformsToCopy() const { return m_summary.waveforms; }
    int tracksToComplete() const { return m_summary.completed; }
    int tracksToWrite() const { return m_summary.rows; }
    int leftAloneCount() const { return m_summary.leftAlone; }
    QVariantList leftAlone() const { return m_summary.groups; }
    QString summaryText() const { return m_summary.text; }
    QString leftAloneText() const { return m_summary.leftAloneText; }
    bool canStage() const;
    int keptTickedCount() const { return m_summary.kept; }
    int restorableCount() const { return m_summary.restorable; }
    int analysedTickedCount() const { return m_summary.analysed; }
    QString engineAnalysisText() const { return m_summary.engineAnalysisText; }
    bool canRestore() const;
    bool canLetPlayerAnalyse() const;
    int notInEngineShown() const { return m_notInEngineShown; }
    QString notInEngineText() const;
    int stagedCount() const { return static_cast<int>(m_stagedIds.size()); }
    QString stagedAction() const { return m_stagedIds.isEmpty() ? QString() : m_stagedAction; }
    bool writing() const;
    int lastSaveGrids() const { return m_lastSave.grids; }
    int lastSaveWaveforms() const { return m_lastSave.waveforms; }
    int lastSaveCompleted() const { return m_lastSave.completed; }
    int lastSaveTracks() const { return m_lastSave.tracks; }
    int lastSaveRestored() const { return m_lastSave.restored; }
    int lastSaveMarked() const { return m_lastSave.marked; }
    QString lastSaveText() const { return m_lastSave.text; }
    int rowAnalysisReads() const { return m_rowAnalysisReads; }

    // A row's small waveform: rekordbox's analysis of the track (its
    // waveform preview and beat grid), what the page copies and what the
    // user judges before copying it.
    //
    // A row asks for its track's when its delegate is built and lets go
    // of it when the delegate goes, so only the rows in view (and the few
    // the list builds ahead of it) are read. One read at a time, off the
    // GUI thread, the oldest ask first; a row let go before its turn is
    // never read, and one already being read is kept when it lands.
    // None while any library is being written (the rule PlaybackController
    // keeps by libraryBusy): asked for then, it waits, and a read under way
    // is put back to be read again. What was read is kept (bounded), so a
    // row scrolled back to draws at once; after a write the kept ones stay
    // drawn until each row's track has been read again.
    Q_INVOKABLE void requestRowAnalysis(const QString &rekordboxId);
    Q_INVOKABLE void releaseRowAnalysis(const QString &rekordboxId);
    // What has been read of a track, never a read itself: {ready} false
    // until its analysis is in; then {ready, columns (an ArrayBuffer in
    // QML, three bytes a column, low, mid and high), durationMs, playable
    // (there is an audio file to play)}. No columns for a track rekordbox
    // has no analysis of.
    Q_INVOKABLE QVariantMap rowAnalysis(const QString &rekordboxId) const;
    // The beats of that track's grid a waveform `widthPx` wide draws over
    // the track's length (beatTicksForDisplay()); empty until read.
    Q_INVOKABLE QVariantList rowBeatTicks(const QString &rekordboxId, double widthPx) const;
    // An opened row's comparison: what the Engine library has for the
    // track now, beside rekordbox's (rowAnalysis()), so the two grids and
    // waveforms can be judged against each other before anything is
    // copied. Read the way a row's own is (off the GUI thread, one at a
    // time, held while any library is written, read again after one),
    // but only for rows that are open.
    Q_INVOKABLE void requestEngineAnalysis(const QString &rekordboxId);
    Q_INVOKABLE void releaseEngineAnalysis(const QString &rekordboxId);
    // {ready} false until read; then {ready, hasEngineTrack (the track has
    // an Engine row), columns (as rowAnalysis()'s; empty where Engine has
    // no waveform), msPerColumn (0: spread over durationMs), durationMs,
    // hasGrid, bpm, firstDownbeatMs, rateRecorded}.
    Q_INVOKABLE QVariantMap engineAnalysis(const QString &rekordboxId) const;
    // As rowBeatTicks(), for Engine's grid of that track.
    Q_INVOKABLE QVariantList engineBeatTicks(const QString &rekordboxId, double widthPx) const;
    // rekordbox's grid in the same figures, on Engine's timeline (see
    // engineAnalysis()'s rekordboxShiftMs): {ready, hasGrid, bpm,
    // firstDownbeatMs, shiftMs}. Not ready until both sides are read.
    Q_INVOKABLE QVariantMap rowGridFacts(const QString &rekordboxId) const;
    // rekordbox's grid as the comparison draws it: rowBeatTicks() moved
    // onto Engine's timeline.
    Q_INVOKABLE QVariantList comparisonBeatTicks(const QString &rekordboxId, double widthPx) const;

    // The track as WaveformZoomPopup.openFor() takes it: {format,
    // libraryPath, sourceId, filePath, title, artist, artworkPath, cues,
    // fallbackArtworkPath}. Empty for a track the library does not have.
    Q_INVOKABLE QVariantMap zoomTrack(const QString &rekordboxId) const;
    // The same track as Engine has it, for the popup opened from the
    // comparison's Engine lane: Engine's waveform, grid and cues. Empty
    // for a track with no Engine row.
    Q_INVOKABLE QVariantMap engineZoomTrack(const QString &rekordboxId) const;

    // Reads the stick holding these two catalogs. A request for the same
    // paths while one runs is served by it.
    Q_INVOKABLE void load(const QString &rekordboxPath, const QString &enginePath);
    Q_INVOKABLE void cancelLoad();
    // Reads the stick again when another page saved to it meanwhile (Sync
    // after Rekordbox Export, which the page links to, adds Engine rows):
    // for the page to call when it is in front again. The ticks of tracks
    // that can still be ticked stay. Nothing when no other save came.
    Q_INVOKABLE void reloadIfStale();

    // PlaylistListView asks for a playlist's hover summary; this page
    // shows none.
    Q_INVOKABLE QVariantMap playlistSummary(const QString &) const { return {}; }

    Q_INVOKABLE bool isTicked(const QString &rekordboxId) const { return m_ticked.contains(rekordboxId); }
    Q_INVOKABLE void setTicked(const QString &rekordboxId, bool ticked);
    // Every track of a playlist ("" for every track) that can be ticked.
    Q_INVOKABLE void setPlaylistTicked(const QString &name, bool ticked);
    // Qt::Unchecked, Qt::PartiallyChecked or Qt::Checked over the tracks of
    // a playlist ("" for all) that can be ticked; Unchecked for one with
    // none.
    Q_INVOKABLE int playlistCheckState(const QString &name) const;
    // How many of a playlist's tracks can be ticked.
    Q_INVOKABLE int tickablePlaylistTracks(const QString &name) const;

    // One change per ticked track that would change, into the library's
    // edit session, which Save writes. Refused unless canStage.
    Q_INVOKABLE void stage();
    // One change per ticked track whose kept analysis can be put back, or
    // per ticked track with an analysed row, into the same session.
    // Refused unless canRestore / canLetPlayerAnalyse.
    Q_INVOKABLE void stageRestore();
    Q_INVOKABLE void stageLetPlayerAnalyse();
    Q_INVOKABLE void unstage();

signals:
    void loadingChanged();
    void loadProgressChanged();
    void libraryChanged();
    void loadCancelled();
    void errorMessageChanged();
    void ledgerWarningChanged();
    void shownPlaylistChanged();
    void optionsChanged();
    void ticksChanged();
    void previewBusyChanged();
    void previewProgressChanged();
    void summaryChanged();
    void stagedChanged();
    void writingChanged();
    void lastSaveChanged();
    void notInEngineChanged();
    // A save of this page's changes finished; `whole` when every change
    // landed and nothing went wrong. The page's toast says lastSaveText.
    void saveCounted(bool whole);
    // A row's analysis is in (rowAnalysis()); for the row showing that
    // track only.
    void rowAnalysisReady(const QString &rekordboxId);
    // What was read may be out of date (a write has ended): every row asks
    // for its track's again.
    void rowAnalysesStale();
    void rowAnalysisReadsChanged();
    // An opened row's Engine side is in (engineAnalysis()).
    void engineAnalysisReady(const QString &rekordboxId);

private:
    // One track's analysis as a row draws it, read off the GUI thread.
    struct RowAnalysis
    {
        QByteArray columns;
        std::vector<domain::Beat> beats;
        bool beatRateRecorded = true;
        // The track's audio file is there to be played.
        bool fileThere = false;
        // m_rowGeneration when it was read.
        std::uint64_t generation = 0;
    };
    // What the Engine library has for one track, for an opened row.
    struct EngineAnalysis
    {
        bool hasEngineTrack = false;
        // What to add to rekordbox's beat times (and its waveform) to put
        // them on Engine's timeline, where the player hears them: an MP3's
        // lead-in, 0 for anything else (playbackShiftFor(), #50). The two
        // are compared and drawn on that one timeline.
        double rekordboxShiftMs = 0.0;
        // The MP3's lead-in could not be read (the file did not open, a
        // read failed): the shift is unknown, not 0, and where the beats
        // fall is not compared.
        bool shiftUnknown = false;
        // The read itself failed (the stick went away, the worker was
        // refused): nothing of Engine's is known.
        bool readFailed = false;
        // Engine's copy of the audio file is on the stick: its waveform can
        // be opened zoomed and played (engineZoomTrack()).
        bool fileThere = false;
        QByteArray columns;
        std::vector<domain::Beat> beats;
        bool beatRateRecorded = true;
        // m_rowGeneration when it was read.
        std::uint64_t generation = 0;
    };
    bool rowReadsHeld() const;
    void onWriteStateChanged();
    void pumpRowReads();
    void pumpEngineReads();
    void forgetRowAnalyses();
    const BeatGridLibrary::Track *libraryTrack(const QString &rekordboxId) const;

    struct Summary
    {
        int pending = 0;
        int grids = 0;
        int waveforms = 0;
        int completed = 0;
        int rows = 0;
        int leftAlone = 0;
        int kept = 0;
        int restorable = 0;
        int analysed = 0;
        QString engineAnalysisText;
        QVariantList groups;
        QString text;
        QString leftAloneText;
    };
    struct LastSave
    {
        int grids = 0;
        int waveforms = 0;
        int completed = 0;
        int tracks = 0;
        int restored = 0;
        int marked = 0;
        QString text;
    };

    void startLoad();
    void adoptLibrary(std::shared_ptr<BeatGridLibrary> library);
    void rebuildRows();
    void refreshRows();
    void schedulePreview();
    void runPreview();
    void takePreviewRows(std::uint64_t generation, std::vector<std::pair<std::string, BeatGridTrackState>> rows);
    void invalidateStates();
    void recount();
    void ticksMoved();
    void attachSession();
    void clearStaged();
    void onSaveFinished(const QVariantMap &summary);
    void setErrorMessage(const QString &message);
    void setLoadProgress(int current, int total);
    void setPreviewProgress(int current, int total);
    // The rekordbox ids of a playlist ("" for every track) that can be ticked.
    std::vector<std::string> tickableIn(const QString &name) const;
    const std::vector<std::string> *playlistIds(const QString &name) const;
    void describe(BeatGridTrackListModel::Row &row) const;
    bool canStageAnything() const;
    void stageEngineAnalysisChanges(bool restore);

    QString m_rekordboxPath;
    QString m_enginePath;
    int m_loadCurrent = 0;
    int m_loadTotal = 0;
    QString m_loadLabel;
    QString m_errorMessage;
    QString m_ledgerError;
    std::shared_ptr<const BeatGridLibrary> m_library;
    // Another page saved to this library since it was read.
    bool m_libraryStale = false;
    QStringList m_playlistNames;
    QVariantMap m_playlistTrackCounts;
    QString m_shownPlaylist;
    BeatGridTrackListModel m_tracks;
    application::AnalysisCopyOptions m_options;
    QSet<QString> m_ticked;
    int m_tickRevision = 0;
    // Bumped when the options change or a save lands: a state worked out
    // before belongs to another generation and is dropped on arrival.
    std::uint64_t m_generation = 1;
    std::map<std::string, BeatGridTrackState> m_states;
    bool m_previewScheduled = false;
    int m_previewCurrent = 0;
    int m_previewTotal = 0;
    Summary m_summary;
    QStringList m_stagedIds;
    QString m_stagedAction;
    int m_notInEngineShown = 0;
    std::shared_ptr<AnalysisSaveTally> m_tally;
    QStringList m_appliedThisSave;
    LastSave m_lastSave;
    QPointer<LibraryEditSession> m_session;
    // Where preview workers hand their rows (PreviewSink in the .cpp).
    std::shared_ptr<QObject> m_previewSink;

    // The rows' analyses: what has been read, keyed by rekordbox id, at
    // most RowAnalysisCacheKb of it; the ids rows want read, the oldest
    // ask first; and whether reads are held for a write.
    static constexpr int RowAnalysisCacheKb = 32 * 1024;
    QCache<QString, RowAnalysis> m_rowAnalyses{RowAnalysisCacheKb};
    QStringList m_rowWanted;
    std::uint64_t m_rowGeneration = 1;
    bool m_rowReadsHeld = false;
    int m_rowAnalysisReads = 0;
    // The opened rows' Engine sides, the same way; few rows are open.
    static constexpr int EngineAnalysisCacheKb = 8 * 1024;
    QCache<QString, EngineAnalysis> m_engineAnalyses{EngineAnalysisCacheKb};
    QStringList m_engineWanted;

    // Last, so they are destroyed first: their destructors cancel the work.
    AsyncRequest<std::shared_ptr<BeatGridLibrary>> m_load{this, [this]() { emit loadingChanged(); }};
    AsyncRequest<QString> m_preview{this, [this]() { emit previewBusyChanged(); }};
    AsyncRequest<std::shared_ptr<RowAnalysis>> m_rowRead{this, nullptr};
    AsyncRequest<std::shared_ptr<EngineAnalysis>> m_engineRead{this, nullptr};
};

}  // namespace seabass::gui
