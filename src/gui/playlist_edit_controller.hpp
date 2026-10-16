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
#include <QVariantMap>

#include <deque>
#include <map>
#include <functional>
#include <set>
#include <tuple>
#include <vector>

#include "gui/edit/changes/playlist_edit_changes.hpp"

namespace seabass::gui
{

class LibraryEditSession;

// Browse's playlist editing (Experimental): creating a playlist, deleting
// one, adding a track to one, taking it out or moving it within one,
// staged into the stick's edit session like an added cue, so the page's
// Save writes them into every library on the stick (CreatePlaylistChange;
// the others where the playlist is).
class PlaylistEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Bumps whenever what is staged changes, so QML re-reads the queries.
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)
    // The playlists staged to be created, in the order asked for: the
    // list shows them until Save makes them.
    Q_PROPERTY(QStringList newPlaylists READ newPlaylists NOTIFY pendingChanged)
    // The tracks staged to be added, as {playlist: [file paths]}, each
    // list in the order they were added: Browse shows them in their
    // playlist until Save writes them.
    Q_PROPERTY(QVariantMap pendingAdditions READ pendingAdditions NOTIFY pendingChanged)
    // The tracks staged to move within a playlist, as {playlist:
    // [{filePath, toIndex}]}, each list in the order they were moved (a
    // track moved again counts as moved last): Browse shows them at their
    // new place until Save writes them.
    Q_PROPERTY(QVariantMap pendingMoves READ pendingMoves NOTIFY pendingChanged)
    // The playlist a track was last added to, for the row menu's
    // shortcut; empty until one was. Kept in the settings, across pages,
    // sticks and restarts (the menu leaves it out where the stick has no
    // such playlist).
    Q_PROPERTY(QString lastAddPlaylist READ lastAddPlaylist NOTIFY lastAddPlaylistChanged)
    // An edit asked for is being looked up on the stick (each catalog read
    // whole, off the GUI thread); the busy cursor shows meanwhile.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // What the last save did with this page's playlist edits, in a line
    // ("\"50th\" saved: 3 tracks added"), for the page to show; empty when
    // it wrote none of them.
    Q_PROPERTY(QString lastSavedSummary READ lastSavedSummary NOTIFY lastSavedSummaryChanged)

public:
    explicit PlaylistEditController(QObject *parent = nullptr);
    ~PlaylistEditController() override;

    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }
    QStringList newPlaylists() const { return m_newPlaylists; }
    QVariantMap pendingAdditions() const;
    QVariantMap pendingMoves() const;
    QString lastAddPlaylist() const;
    bool busy() const { return m_busy; }
    QString lastSavedSummary() const { return m_lastSavedSummary; }

    // rekordboxPath is the stick's PIONEER folder and enginePath its
    // Engine Library folder; either may be empty. Each looks the playlist
    // up on the stick off the GUI thread, in the order asked, and stages
    // the change when that is done (busy meanwhile); a refusal, then or
    // at once, sets errorMessage. Return false for one refused at once.
    // A new, empty playlist called `name` (trimmed) at the top level of
    // every library on the stick. Refuses a name that is empty, holds a
    // "/" (a folder path) or a ";" (Engine takes none), or that a library
    // on the stick or an earlier create already has.
    Q_INVOKABLE bool createPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &name);
    Q_INVOKABLE void dropNewPlaylist(const QString &name);
    Q_INVOKABLE bool isNewPlaylistStaged(const QString &name) const;
    Q_INVOKABLE bool deletePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist);
    Q_INVOKABLE bool removeFromPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                        const QString &filePath, const QString &title);
    // The file at the end of `playlist` in every library on the stick
    // that has it (or will: one staged to be created). Remembers the
    // playlist as lastAddPlaylist.
    Q_INVOKABLE bool addToPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                   const QString &filePath, const QString &title);
    Q_INVOKABLE void keepOutOfPlaylist(const QString &playlist, const QString &filePath);
    Q_INVOKABLE bool isAdditionStaged(const QString &playlist, const QString &filePath) const;
    // The file to `toIndex` (from 0) in `playlist`'s order as Browse shows
    // it, in every library on the stick that has the playlist. Moving it
    // again replaces the move staged before.
    Q_INVOKABLE bool moveInPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                    const QString &filePath, const QString &title, int toIndex);
    Q_INVOKABLE void keepPositionInPlaylist(const QString &playlist, const QString &filePath);
    Q_INVOKABLE bool isMoveStaged(const QString &playlist, const QString &filePath) const;
    // Tracks staged into `playlist` less those staged out of it: what its
    // count becomes at Save.
    Q_INVOKABLE int trackCountChange(const QString &playlist) const;
    // A track is staged into, out of or within `playlist`.
    Q_INVOKABLE bool isPlaylistEdited(const QString &playlist) const;
    Q_INVOKABLE void keepPlaylist(const QString &playlist);
    Q_INVOKABLE void keepInPlaylist(const QString &playlist, const QString &filePath);
    Q_INVOKABLE bool isPlaylistStaged(const QString &playlist) const;
    Q_INVOKABLE bool isRemovalStaged(const QString &playlist, const QString &filePath) const;

signals:
    void errorMessageChanged();
    void lastAddPlaylistChanged();
    void busyChanged();
    void lastSavedSummaryChanged();
    void pendingChanged();
    // A save wrote at least one of these edits: the page should rescan.
    void playlistsSaved();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    // Queues a lookup of `playlist` on the stick; `then` runs on the GUI
    // thread with what it found, unless a save is writing by then.
    void lookUp(const QString &rekordboxPath, const QString &enginePath, const std::string &playlist,
                std::function<void(const PlaylistWhereabouts &)> then);
    void runNextLookup();
    void onLookupFinished();
    void setBusy(bool busy);
    // Tallies one change the running save applied (by its id), and words
    // the tally once the save is done.
    void noteApplied(const QString &changeId);
    QString describeApplied() const;
    void setErrorMessage(const QString &message);
    void unstage(const QString &changeId);
    // Drops from m_newPlaylists every name whose create is no longer
    // staged, and from m_additions, m_removals and m_moves every change
    // that is not.
    void forgetUnstagedNewPlaylists();

    QPointer<LibraryEditSession> m_session;
    struct Lookup
    {
        QString rekordboxPath;
        QString enginePath;
        std::string playlist;
        std::function<void(const PlaylistWhereabouts &)> then;
    };
    std::deque<Lookup> m_lookups;
    QFutureWatcher<PlaylistWhereabouts> m_lookupWatcher;
    bool m_busy = false;
    // The running save's applied changes: how many of each kind, and the
    // playlists they were in.
    std::map<QString, int> m_appliedKinds;
    std::set<QString> m_appliedPlaylists;
    QString m_lastSavedSummary;
    std::set<QString> m_staged;  // change ids
    QStringList m_newPlaylists;
    // Every add staged, in order, as (change id, playlist, file); the
    // ones no longer in m_staged are left out when read.
    std::vector<std::tuple<QString, QString, QString>> m_additions;
    // The same for every removal staged.
    std::vector<std::tuple<QString, QString, QString>> m_removals;
    // Every move staged, in order, as (change id, playlist, file, index).
    std::vector<std::tuple<QString, QString, QString, int>> m_moves;
    int m_pendingRevision = 0;
    QString m_errorMessage;
};

}  // namespace seabass::gui
