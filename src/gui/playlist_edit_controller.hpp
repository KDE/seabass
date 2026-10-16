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

#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
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
    // new place until Save writes them. A track added at a place is listed
    // here too, at the point it was added, with "addedAt": true.
    Q_PROPERTY(QVariantMap pendingMoves READ pendingMoves NOTIFY pendingChanged)
    // The playlists staged to be renamed, as {path on the stick: new
    // path}: Browse lists them under the new name until Save writes it.
    // A playlist staged to be created is renamed in place instead (see
    // renamePlaylist), so it is never here.
    Q_PROPERTY(QVariantMap pendingRenames READ pendingRenames NOTIFY pendingChanged)
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
    QVariantMap pendingRenames() const;
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
    // that has it (or will: one staged to be created), or at `toIndex`
    // (from 0, its place after the add) when one is given: one change
    // either way. Staging it again replaces the add staged before.
    // Remembers the playlist as lastAddPlaylist.
    Q_INVOKABLE bool addToPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                   const QString &filePath, const QString &title, int toIndex = -1);
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

    // Renames `playlist` (its last segment; a folder takes its children
    // along) to `newName`, trimmed, in every library on the stick that has
    // it. Refuses a name that is empty or holds "/" or ";", one the stick
    // has at the new path, a new playlist's or another rename's name, a
    // playlist inside a folder being renamed or a folder holding one, and
    // a second rename of a playlist edited under its new name since.
    //
    // How the edits after a rename find the playlist. Browse shows the new
    // name at once, so every later edit is asked for under it, while the
    // stick still has the old one until Save. Save applies the changes in
    // the order they were staged, so the rename is written before every
    // edit asked for after it, and those edits must address the playlist
    // by its new name. Each edit is therefore looked up on the stick by
    // the path it has there now (stickPathOf: the new name mapped back to
    // the old, a renamed folder's children too), and staged under the name
    // shown, which is the one the stick has by its turn. An edit staged
    // before the rename keeps the old name, and is written before it. The
    // queries take either name (namesOf). Taking the rename back
    // (keepPlaylistName) takes back the edits addressed to the new name,
    // which would find nothing at Save; a second rename would leave them
    // addressing a name the first never gives it, so it is refused while
    // there are any.
    //
    // A playlist staged to be created is renamed in place: its create and
    // every edit staged into it are staged again under the new name, in
    // their order. Nothing of it is on the stick to rename.
    Q_INVOKABLE bool renamePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                    const QString &newName);
    // Takes a rename back, the playlist named by either name.
    Q_INVOKABLE void keepPlaylistName(const QString &playlist);
    Q_INVOKABLE bool isRenameStaged(const QString &playlist) const;

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
    // The same for two paths read in one turn: a rename's old and new.
    void lookUp(const QString &rekordboxPath, const QString &enginePath, const std::string &playlist,
                const std::string &also, std::function<void(const PlaylistWhereabouts &, const PlaylistWhereabouts &)> then);
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
    // The path a playlist shown as `shown` has on the stick now: a staged
    // rename's new path mapped back to its old one, a renamed folder's
    // children too.
    QString stickPathOf(const QString &shown) const;
    // `shown` and, when a rename maps it, the stick's path: the names an
    // edit of it may have been staged under.
    QStringList namesOf(const QString &shown) const;
    // Whether `path` is `folder` or lies below it.
    static bool within(const QString &path, const QString &folder);
    // The edits staged under a name at or below `path`.
    std::vector<QString> editsAddressing(const QString &path) const;
    // A staged new playlist renamed in place (see renamePlaylist).
    void renameNewPlaylist(const QString &playlist, const QString &renamed);

    QPointer<LibraryEditSession> m_session;
    struct Lookup
    {
        QString rekordboxPath;
        QString enginePath;
        std::string playlist;
        std::string also;  // empty: one path
        std::function<void(const PlaylistWhereabouts &, const PlaylistWhereabouts &)> then;
    };
    std::deque<Lookup> m_lookups;
    QFutureWatcher<std::pair<PlaylistWhereabouts, PlaylistWhereabouts>> m_lookupWatcher;
    bool m_busy = false;
    // The running save's applied changes: how many of each kind, and the
    // playlists they were in.
    std::map<QString, int> m_appliedKinds;
    std::set<QString> m_appliedPlaylists;
    // The renames this save applied, new path to old: the edits written
    // after one address the playlist by its new name, and are counted
    // under the old one with the rename, as one playlist.
    std::map<QString, QString> m_appliedRenames;
    QString m_lastSavedSummary;
    std::set<QString> m_staged;  // change ids
    QStringList m_newPlaylists;
    // One edit of a track's place in a playlist, as staged: `remake`
    // makes its change again for the playlist under another name (a new
    // playlist renamed in place), `sequence` says when it was staged
    // across the three lists.
    struct Edit
    {
        QString id;
        QString playlist;
        QString file;
        int toIndex = -1;
        std::uint64_t sequence = 0;
        std::function<std::unique_ptr<PendingChange>(const std::string &playlist)> remake;
    };
    // Every add staged, in order; the ones no longer in m_staged are left
    // out when read.
    std::vector<Edit> m_additions;
    // The same for every removal and every move staged.
    std::vector<Edit> m_removals;
    std::vector<Edit> m_moves;
    // Every rename staged: its change id, the path on the stick, the new.
    struct Rename
    {
        QString id;
        QString from;
        QString to;
    };
    std::vector<Rename> m_renames;
    // How each staged new playlist's create is made again under another
    // name, by its name.
    std::map<QString, std::function<std::unique_ptr<PendingChange>(const std::string &playlist)>> m_creates;
    std::uint64_t m_sequence = 0;
    int m_pendingRevision = 0;
    QString m_errorMessage;
};

}  // namespace seabass::gui
