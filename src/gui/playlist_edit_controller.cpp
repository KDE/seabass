// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_edit_controller.hpp"

#include <memory>

#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"

namespace seabass::gui
{

PlaylistEditController::PlaylistEditController(QObject *parent) : QObject(parent) {}

bool PlaylistEditController::attachSession(const QString &rekordboxPath, const QString &enginePath)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session =
        registry->sessionFor(registry->libraryIdForPath(rekordboxPath.isEmpty() ? enginePath : rekordboxPath));
    if (session != m_session) {
        if (m_session) {
            disconnect(m_session, nullptr, this, nullptr);
        }
        m_session = session;
        if (m_session) {
            connect(m_session, &LibraryEditSession::changeApplied, this, [this](const QString &changeId) {
                if (m_staged.erase(changeId) > 0) {
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &summary) {
                if (summary.value("written").toInt() > 0) {
                    emit playlistsSaved();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                if (!m_staged.empty()) {
                    m_staged.clear();
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
        }
    }
    if (!m_session) {
        setErrorMessage(QStringLiteral("This stick has no edit session open."));
        return false;
    }
    m_session->setLibraryPaths(rekordboxPath, enginePath);
    if (m_session->writing()) {
        setErrorMessage(QStringLiteral("A save is writing to the stick. Try again when it has finished."));
        return false;
    }
    return true;
}

bool PlaylistEditController::deletePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist)
{
    setErrorMessage({});
    if (playlist.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string name = playlist.toStdString();
    const QStringList libraries = librariesWithPlaylist(rekordboxPath, enginePath, name);
    if (libraries.isEmpty()) {
        setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
        return false;
    }
    if (!m_session->stage(std::make_unique<DeletePlaylistChange>(rekordboxPath, enginePath, name, libraries))) {
        return false;  // the session reported why (another page's edits)
    }
    m_staged.insert(DeletePlaylistChange::idFor(name));
    ++m_pendingRevision;
    emit pendingChanged();
    return true;
}

bool PlaylistEditController::removeFromPlaylist(const QString &rekordboxPath, const QString &enginePath,
                                                const QString &playlist, const QString &filePath, const QString &title)
{
    setErrorMessage({});
    if (playlist.isEmpty() || filePath.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string name = playlist.toStdString();
    const QStringList libraries = librariesWithPlaylist(rekordboxPath, enginePath, name);
    if (libraries.isEmpty()) {
        setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
        return false;
    }
    const std::string file = filePath.toStdString();
    if (!m_session->stage(std::make_unique<RemoveFromPlaylistChange>(rekordboxPath, enginePath, name, file, title, libraries))) {
        return false;
    }
    m_staged.insert(RemoveFromPlaylistChange::idFor(name, file));
    ++m_pendingRevision;
    emit pendingChanged();
    return true;
}

void PlaylistEditController::unstage(const QString &changeId)
{
    if (m_staged.erase(changeId) == 0) {
        return;
    }
    if (m_session) {
        m_session->unstage(changeId);
    }
    ++m_pendingRevision;
    emit pendingChanged();
}

void PlaylistEditController::keepPlaylist(const QString &playlist)
{
    unstage(DeletePlaylistChange::idFor(playlist.toStdString()));
}

void PlaylistEditController::keepInPlaylist(const QString &playlist, const QString &filePath)
{
    unstage(RemoveFromPlaylistChange::idFor(playlist.toStdString(), filePath.toStdString()));
}

bool PlaylistEditController::isPlaylistStaged(const QString &playlist) const
{
    return m_staged.count(DeletePlaylistChange::idFor(playlist.toStdString())) > 0;
}

bool PlaylistEditController::isRemovalStaged(const QString &playlist, const QString &filePath) const
{
    return m_staged.count(RemoveFromPlaylistChange::idFor(playlist.toStdString(), filePath.toStdString())) > 0;
}

void PlaylistEditController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

}  // namespace seabass::gui
