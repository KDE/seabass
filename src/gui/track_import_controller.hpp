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

#include <optional>
#include <set>

#include "application/ports/cancellation_token.hpp"
#include "gui/edit/track_import_prepare.hpp"

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
    // analysed, and how many of those got a beat grid), megabytes,
    // playlist, playlistIsNew, note, notImported: [{name, why}],
    // possibleDuplicates}: `staged` tracks are in the session, the
    // `notImported` ones are not, `possibleDuplicates` of those may be
    // asked for all the same (importPossibleDuplicates).
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY resultReady)

public:
    explicit TrackImportController(QObject *parent = nullptr);
    ~TrackImportController() override;

    bool busy() const { return m_busy; }
    QString progressStep() const { return m_progressStep; }
    int progressDone() const { return m_progressDone; }
    int progressTotal() const { return m_progressTotal; }
    QString progressFile() const { return m_progressFile; }
    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }
    int stagedCount() const { return m_stagedTracks; }
    QString stagedPlaylist() const { return !m_staged.empty() ? m_stagedPlaylist : QString(); }
    bool stagedPlaylistIsNew() const { return !m_staged.empty() && m_stagedPlaylistIsNew; }
    QVariantMap lastResult() const { return m_lastResult; }

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
    // The audio file extensions a file dialog should offer, as
    // "*.mp3 *.flac ...".
    Q_INVOKABLE QString fileDialogFilter() const;

signals:
    void busyChanged();
    void progressChanged();
    void errorMessageChanged();
    void pendingChanged();
    void resultReady();
    // A save wrote at least one imported track: the page should rescan.
    void tracksImported();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    bool start(ImportRequest request);
    void onPrepared();
    void setBusy(bool busy);
    void setProgress(const QString &step, int done, int total, const QString &file);
    void setErrorMessage(const QString &message);
    void forgetStaged();

    QPointer<LibraryEditSession> m_session;
    QFutureWatcher<PreparedImport> m_watcher;
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
};

}  // namespace seabass::gui
