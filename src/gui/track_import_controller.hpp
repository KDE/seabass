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
// One batch at a time, into a session with nothing else staged: the
// batch is checked against the stick as it is (which playlist exists,
// what export.pdb still has room for), and edits staged before it would
// make that another stick by the time the save reaches the imports.
class TrackImportController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Bumps whenever what is staged changes, so QML re-reads the queries.
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)
    // How many tracks are staged to be imported.
    Q_PROPERTY(int stagedCount READ stagedCount NOTIFY pendingChanged)
    // The playlist the staged batch goes into ("" for none), and whether
    // the save makes it.
    Q_PROPERTY(QString stagedPlaylist READ stagedPlaylist NOTIFY pendingChanged)
    Q_PROPERTY(bool stagedPlaylistIsNew READ stagedPlaylistIsNew NOTIFY pendingChanged)
    // What the last import asked for came to, as {staged, megabytes,
    // playlist, playlistIsNew, note, notImported: [{name, why}],
    // possibleDuplicates}: `staged` tracks are in the session, the
    // `notImported` ones are not, `possibleDuplicates` of those may be
    // asked for all the same (importPossibleDuplicates).
    Q_PROPERTY(QVariantMap lastResult READ lastResult NOTIFY resultReady)

public:
    explicit TrackImportController(QObject *parent = nullptr);
    ~TrackImportController() override;

    bool busy() const { return m_busy; }
    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }
    int stagedCount() const { return m_stagedTracks; }
    QString stagedPlaylist() const { return m_stagedTracks > 0 ? m_stagedPlaylist : QString(); }
    bool stagedPlaylistIsNew() const { return m_stagedTracks > 0 && m_stagedPlaylistIsNew; }
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
    void setErrorMessage(const QString &message);
    void forgetStaged();

    QPointer<LibraryEditSession> m_session;
    QFutureWatcher<PreparedImport> m_watcher;
    application::CancellationToken m_cancel;
    // The offer being prepared, and the last one prepared.
    ImportRequest m_request;
    std::vector<std::string> m_possibleDuplicates;
    bool m_busy = false;
    QString m_errorMessage;
    int m_pendingRevision = 0;
    std::set<QString> m_staged;  // change ids
    std::set<QString> m_stagedImports;  // those of them that are a track
    int m_stagedTracks = 0;
    int m_appliedTracks = 0;
    QString m_stagedPlaylist;
    bool m_stagedPlaylistIsNew = false;
    QVariantMap m_lastResult;
};

}  // namespace seabass::gui
