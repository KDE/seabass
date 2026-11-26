// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <map>
#include <optional>
#include <set>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/track_import_prepare.hpp"
#include "gui/edit/track_reanalysis_prepare.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Browse's track import (experimental): audio files and folders from the
// computer, dropped on a playlist or picked from its menu, staged into
// the stick's edit session as one batch. The page's Save copies each
// file to the stick and gives it a row in every library the stick has,
// and its place in the playlist.
//
// What is offered is read, planned and rehearsed off the GUI thread
// (prepareTrackImport); `busy` meanwhile. Then `lastResult` says what is
// staged and what was left out and why, and resultReady() is emitted.
//
// A second offer for the playlist of a batch that waits joins it: both
// are prepared again as one batch, which takes the place of the one
// that waits when it is ready (a cancel leaves that one as it was). An
// offer for another playlist waits for the save.
//
// One batch at a time, into a session with nothing else staged: the
// batch is checked against the stick as it is (which playlist exists,
// what export.pdb still has room for), and edits staged before it would
// make that another stick by the time the save reaches the imports.
//
// Analyse again (analyseAgain()): the tracks of a playlist analysed by
// Seabass from their audio on the stick, one ReanalyseTrackChange staged
// per track that took it. It runs where the import does (`busy`, the same
// progress, the same cancel; never both at once) and stages beside what
// waits, since it plans nothing against the stick's rows: a track staged
// again replaces its earlier change.
class TrackImportController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // While busy: the step in hand ("stick", "files", "check", "analysis"; "" when
    // idle), how many of its files are done of how many (total 0: the
    // step cannot count ahead), and the file in hand by its name.
    Q_PROPERTY(QString progressStep READ progressStep NOTIFY progressChanged)
    Q_PROPERTY(int progressDone READ progressDone NOTIFY progressChanged)
    Q_PROPERTY(int progressTotal READ progressTotal NOTIFY progressChanged)
    Q_PROPERTY(QString progressFile READ progressFile NOTIFY progressChanged)
    // Every file in hand now, by name: several while tracks are analysed.
    Q_PROPERTY(QStringList progressFiles READ progressFiles NOTIFY progressChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Bumps whenever what is staged changes, so QML re-reads the queries.
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)
    // How many tracks are staged to be imported.
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY pendingChanged)
    // The playlist the staged batch goes into ("" for none), and whether
    // the save makes it.
    Q_PROPERTY(QString stagedPlaylist READ stagedPlaylist NOTIFY pendingChanged)
    Q_PROPERTY(bool stagedPlaylistIsNew READ stagedPlaylistIsNew NOTIFY pendingChanged)
    // What the last import asked for came to, as {staged, addedToPlaylist
    // (tracks the stick already has, staged to go into the playlist),
    // analysed and withGrid (of the staged tracks: how many Seabass
    // analysed, and how many of those got a beat grid), followedGrids
    // (the names of those whose grid follows a moving tempo), megabytes,
    // playlist, playlistIsNew, note, notImported: [{name, why}],
    // possibleDuplicates}: `staged` tracks are in the session, the
    // `notImported` ones are not, `possibleDuplicates` of those may be
    // asked for all the same (importPossibleDuplicates).
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY resultReady)
    // While Analyse again runs: the playlist it analyses; "" otherwise,
    // and while an import is prepared.
    Q_PROPERTY(QString reanalysingPlaylist READ reanalysingPlaylist NOTIFY busyChanged)
    // What the last Analyse again came to: {playlist, analysed (tracks
    // staged with a new analysis), leftAlone (tracks left as they are),
    // cueTracks (tracks with cues on the old grid that would move),
    // cuesToMove (how many), gridChangedTooMuch (tracks whose grid
    // changed too much to carry cues), all of that run only}.
    Q_PROPERTY(QVariantMap lastReanalysis READ lastReanalysis NOTIFY reanalysisReady)
    // The review of the cues on the old grid: one entry per track whose
    // Analyse again change still waits and whose cues would move, of this
    // run and earlier ones: {id, title (with the artist), gridShiftMs,
    // oldBpm, newBpm, moveCues, offBeat,
    // rows: [{label, name, color, isLoop, oldText, newText, diffMs,
    // oldEndText, newEndText, strip: {spanMs, columns: [{low, mid, high}],
    // oldBeats, newBeats, oldCueMs, newCueMs, oldEndMs, newEndMs}}]}, the
    // strip's times in ms from its start, all on one timeline.
    Q_PROPERTY(QVariantList cueReview READ cueReview NOTIFY cueReviewChanged)
    // The titles of the tracks whose grid changed too much to carry cues.
    Q_PROPERTY(QStringList cueReviewTooDifferent READ cueReviewTooDifferent NOTIFY cueReviewChanged)

public:
    explicit TrackImportController(QObject *parent = nullptr);
    ~TrackImportController() override;

    bool busy() const { return m_busy; }
    QString progressStep() const { return m_progressStep; }
    int progressDone() const { return m_progressDone; }
    int progressTotal() const { return m_progressTotal; }
    QString progressFile() const { return m_progressFile; }
    QStringList progressFiles() const { return m_progressFiles; }
    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }
    int stagedCount() const { return m_stagedTracks; }
    QString stagedPlaylist() const { return !m_staged.empty() ? m_stagedPlaylist : QString(); }
    bool stagedPlaylistIsNew() const { return !m_staged.empty() && m_stagedPlaylistIsNew; }
    QVariantMap lastResult() const { return m_lastResult; }
    QString reanalysingPlaylist() const { return m_reanalysingPlaylist; }
    QVariantMap lastReanalysis() const { return m_lastReanalysis; }
    QVariantList cueReview() const;
    QStringList cueReviewTooDifferent() const;

    // Offers `files` (local files and folders, as urls or paths) for
    // import onto the stick whose PIONEER folder is rekordboxPath and
    // Engine Library folder enginePath (either may be empty). `playlist`:
    // the playlist they go into, "" for none; `create`: the save makes it
    // (a name, at the top level); `atIndex`: where the first goes in it,
    // from 0, below 0 for the end. False, with errorMessage, for an offer
    // refused at once.
    Q_INVOKABLE bool importFiles(const QString &rekordboxPath, const QString &enginePath, const QVariantList &files,
                                 const QString &playlist = QString(), bool create = false, int atIndex = -1);
    // The last offer again, its possible duplicates imported too. Takes
    // the batch staged from it back first.
    Q_INVOKABLE bool importPossibleDuplicates();
    // Stops the preparing of an offer; nothing of it is staged.
    Q_INVOKABLE void cancel();
    // Takes the staged batch back.
    Q_INVOKABLE void dropStaged();
    // By how many tracks `playlist` grows at Save.
    Q_INVOKABLE int trackCountChange(const QString &playlist) const;
    // A name to propose for a new playlist made of `files`: the folder's
    // own when one folder was offered, "" otherwise.
    Q_INVOKABLE QString suggestedPlaylistName(const QVariantList &files) const;
    // Analyse again: every track of `playlist` (its path, as the libraries
    // name it) analysed from its audio file on the stick, and a new
    // analysis staged for each that took one. False, with errorMessage,
    // for a request refused at once.
    Q_INVOKABLE bool analyseAgain(const QString &rekordboxPath, const QString &enginePath, const QString &playlist);
    // Takes back the new analyses staged from `playlist`.
    Q_INVOKABLE void dropReanalysis(const QString &playlist);
    // How many tracks of `playlist` wait for Save with a new analysis.
    Q_INVOKABLE int reanalysisCount(const QString &playlist) const;
    // Whether the save moves the cues of the track whose change is
    // `changeId` onto its new grid; all of the review's at once.
    Q_INVOKABLE void setMoveCues(const QString &changeId, bool move);
    Q_INVOKABLE void setMoveCuesForAll(bool move);
    // The audio file extensions a file dialog should offer, as
    // "*.mp3 *.flac ...".
    Q_INVOKABLE QString fileDialogFilter() const;

signals:
    void busyChanged();
    void progressChanged();
    void errorMessageChanged();
    void pendingChanged();
    void resultReady();
    // Analyse again has staged what it could (lastReanalysis).
    void reanalysisReady();
    void cueReviewChanged();
    // One track's choice changed (setMoveCues(), setMoveCuesForAll()): the
    // review updates that switch alone, not the whole list.
    void moveCuesChanged(const QString &changeId, bool move);
    // A save wrote at least one imported track: the page should rescan.
    void tracksImported();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    bool start(ImportRequest request);
    void onPrepared();
    void onReanalysed();
    // Passes progress from the worker to the properties, for this run only.
    ImportProgressSink progressSink();
    void setBusy(bool busy);
    void setProgress(const QString &step, int done, int total, const QString &file, const QStringList &inHand = {});
    void setErrorMessage(const QString &message);
    void forgetStaged();
    void forgetReanalysis();
    // Drops from the cue review the changes no longer staged.
    void pruneCueReview();

    QPointer<LibraryEditSession> m_session;
    QFutureWatcher<PreparedImport> m_watcher;
    QFutureWatcher<PreparedReanalysis> m_reanalysisWatcher;
    application::CancellationToken m_cancel;
    // The offer being prepared, and the last one that was prepared: the
    // one the staged batch is of.
    ImportRequest m_preparing;
    ImportRequest m_request;
    // The offer being prepared takes the place of the batch that waits.
    bool m_joining = false;
    // That batch's changes when the preparing started.
    std::set<QString> m_joined;
    std::set<QString> m_stagedExistingIds;
    std::vector<std::string> m_possibleDuplicates;
    bool m_busy = false;
    // Counts the offers, so progress of an earlier one is told apart.
    int m_offer = 0;
    QString m_progressStep;
    int m_progressDone = 0;
    int m_progressTotal = 0;
    QString m_progressFile;
    QStringList m_progressFiles;
    QString m_errorMessage;
    int m_pendingRevision = 0;
    std::set<QString> m_staged;  // change ids
    std::set<QString> m_stagedImports;  // those of them that are a track
    int m_stagedTracks = 0;
    // Tracks the stick already has, staged to go into the playlist.
    int m_stagedExisting = 0;
    int m_appliedTracks = 0;
    QString m_stagedPlaylist;
    bool m_stagedPlaylistIsNew = false;
    QVariantMap m_lastResult;
    // Analyse again: the request in hand, and the changes staged by it,
    // by id, with the playlist they were asked for from.
    ReanalysisRequest m_reanalysing;
    QString m_reanalysingPlaylist;
    std::map<QString, QString> m_reanalysed;
    // The last run's changes, for the cue review, in playlist order.
    std::vector<std::shared_ptr<ReanalyseTrackChange>> m_reviewed;
    QVariantMap m_lastReanalysis;
};

}  // namespace seabass::gui
