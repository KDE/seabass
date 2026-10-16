// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <set>

namespace seabass::gui
{

class LibraryEditSession;

// Browse's playlist editing (Experimental): creating a playlist, deleting
// one and taking a track out of one, staged into the stick's edit session
// like an added cue, so the page's Save writes them into every library on
// the stick (CreatePlaylistChange; DeletePlaylistChange and
// RemoveFromPlaylistChange where the playlist is).
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
    // The playlist a track was last added to, for the row menu's
    // shortcut; empty until one was. Kept for the run, across pages and
    // sticks (the menu leaves it out where the stick has no such playlist).
    Q_PROPERTY(QString lastAddPlaylist READ lastAddPlaylist NOTIFY lastAddPlaylistChanged)

public:
    explicit PlaylistEditController(QObject *parent = nullptr);

    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }
    QStringList newPlaylists() const { return m_newPlaylists; }
    QString lastAddPlaylist() const;

    // rekordboxPath is the stick's PIONEER folder and enginePath its
    // Engine Library folder; either may be empty. Return whether the
    // change was staged; a refusal sets errorMessage.
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
    Q_INVOKABLE void keepPlaylist(const QString &playlist);
    Q_INVOKABLE void keepInPlaylist(const QString &playlist, const QString &filePath);
    Q_INVOKABLE bool isPlaylistStaged(const QString &playlist) const;
    Q_INVOKABLE bool isRemovalStaged(const QString &playlist, const QString &filePath) const;

signals:
    void errorMessageChanged();
    void lastAddPlaylistChanged();
    void pendingChanged();
    // A save wrote at least one of these edits: the page should rescan.
    void playlistsSaved();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    void setErrorMessage(const QString &message);
    void unstage(const QString &changeId);
    // Drops from m_newPlaylists every name whose create is no longer staged.
    void forgetUnstagedNewPlaylists();

    QPointer<LibraryEditSession> m_session;
    std::set<QString> m_staged;  // change ids
    QStringList m_newPlaylists;
    int m_pendingRevision = 0;
    QString m_errorMessage;
};

}  // namespace seabass::gui
