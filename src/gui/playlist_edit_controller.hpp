// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>

#include <set>

namespace seabass::gui
{

class LibraryEditSession;

// Browse's playlist editing (Experimental): deleting a playlist and taking
// a track out of one, staged into the stick's edit session like an added
// cue, so the page's Save writes them into every library on the stick
// that has the playlist (DeletePlaylistChange, RemoveFromPlaylistChange).
class PlaylistEditController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // Bumps whenever what is staged changes, so QML re-reads the queries.
    Q_PROPERTY(int pendingRevision READ pendingRevision NOTIFY pendingChanged)

public:
    explicit PlaylistEditController(QObject *parent = nullptr);

    QString errorMessage() const { return m_errorMessage; }
    int pendingRevision() const { return m_pendingRevision; }

    // rekordboxPath is the stick's PIONEER folder and enginePath its
    // Engine Library folder; either may be empty. Return whether the
    // change was staged; a refusal sets errorMessage.
    Q_INVOKABLE bool deletePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist);
    Q_INVOKABLE bool removeFromPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                        const QString &filePath, const QString &title);
    Q_INVOKABLE void keepPlaylist(const QString &playlist);
    Q_INVOKABLE void keepInPlaylist(const QString &playlist, const QString &filePath);
    Q_INVOKABLE bool isPlaylistStaged(const QString &playlist) const;
    Q_INVOKABLE bool isRemovalStaged(const QString &playlist, const QString &filePath) const;

signals:
    void errorMessageChanged();
    void pendingChanged();
    // A save wrote at least one of these edits: the page should rescan.
    void playlistsSaved();

private:
    bool attachSession(const QString &rekordboxPath, const QString &enginePath);
    void setErrorMessage(const QString &message);
    void unstage(const QString &changeId);

    QPointer<LibraryEditSession> m_session;
    std::set<QString> m_staged;  // change ids
    int m_pendingRevision = 0;
    QString m_errorMessage;
};

}  // namespace seabass::gui
