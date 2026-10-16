// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_edit_controller.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>
#include <vector>

#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"

namespace seabass::gui
{

namespace
{

QString displayNames(const QStringList &libraries)
{
    QStringList names;
    for (const QString &l : libraries) {
        names << (l == QLatin1String("onelibrary") ? QStringLiteral("OneLibrary")
                  : l == QLatin1String("engine")   ? QStringLiteral("Engine")
                                                   : QStringLiteral("rekordbox"));
    }
    return names.join(QStringLiteral(", "));
}

QString ambiguous(const QString &playlist, const QStringList &libraries)
{
    return QStringLiteral("More than one playlist is called \"%1\" (in %2), so Seabass cannot tell which "
                          "one you mean. Rename one of them first.")
        .arg(playlist, displayNames(libraries));
}

// PlaylistEditController::lastAddPlaylist, in the settings: Browse makes
// a controller per page, and the playlist being put together is the same
// one after a restart.
constexpr const char *LastAddKey = "lastAddPlaylist";

}  // namespace

PlaylistEditController::PlaylistEditController(QObject *parent) : QObject(parent)
{
    connect(&m_lookupWatcher, &QFutureWatcher<PlaylistWhereabouts>::finished, this,
            &PlaylistEditController::onLookupFinished);
}

PlaylistEditController::~PlaylistEditController()
{
    // What is queued is dropped; the one running finishes on its own and
    // is never heard of. The cursor goes back either way.
    m_lookups.clear();
    if (m_busy) {
        QGuiApplication::restoreOverrideCursor();
    }
}

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
                    forgetUnstagedNewPlaylists();
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
                    m_additions.clear();
                    m_removals.clear();
                    m_newPlaylists.clear();
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

bool PlaylistEditController::createPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &name)
{
    setErrorMessage({});
    const QString playlist = name.trimmed();
    if (playlist.isEmpty()) {
        setErrorMessage(QStringLiteral("A playlist needs a name."));
        return false;
    }
    if (playlist.contains(QLatin1Char('/')) || playlist.contains(QLatin1Char(';'))) {
        setErrorMessage(QStringLiteral("A playlist name cannot hold \"/\" or \";\"."));
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string path = playlist.toStdString();
    if (m_staged.count(CreatePlaylistChange::idFor(path)) > 0) {
        setErrorMessage(QStringLiteral("\"%1\" is already waiting to be created.").arg(playlist));
        return false;
    }
    lookUp(rekordboxPath, enginePath, path, [=, this](const PlaylistWhereabouts &where) {
        if (m_staged.count(CreatePlaylistChange::idFor(path)) > 0) {
            setErrorMessage(QStringLiteral("\"%1\" is already waiting to be created.").arg(playlist));
            return;
        }
        if (!where.having.isEmpty()) {
            setErrorMessage(
                QStringLiteral("There is already a playlist \"%1\" (in %2).").arg(playlist, displayNames(where.having)));
            return;
        }
        if (where.onStick.isEmpty()) {
            setErrorMessage(QStringLiteral("This stick has no library to add a playlist to."));
            return;
        }
        if (!m_session->stage(std::make_unique<CreatePlaylistChange>(rekordboxPath, enginePath, path, where.onStick))) {
            return;  // the session reported why (another page's edits)
        }
        m_staged.insert(CreatePlaylistChange::idFor(path));
        m_newPlaylists << playlist;
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

QString PlaylistEditController::lastAddPlaylist() const
{
    return QSettings().value(QLatin1String(LastAddKey)).toString();
}

bool PlaylistEditController::addToPlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                           const QString &filePath, const QString &title)
{
    setErrorMessage({});
    if (playlist.isEmpty() || filePath.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    // Remembered at once, so the menu offers it again straight away.
    if (lastAddPlaylist() != playlist) {
        QSettings().setValue(QLatin1String(LastAddKey), playlist);
        emit lastAddPlaylistChanged();
    }
    const std::string name = playlist.toStdString();
    lookUp(rekordboxPath, enginePath, name, [=, this](const PlaylistWhereabouts &where) {
        // Asked when its turn comes: a create queued before it is staged by then.
        const QStringList libraries = isNewPlaylistStaged(playlist) ? where.onStick : where.having;
        if (libraries.isEmpty()) {
            setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
            return;
        }
        if (!where.several.isEmpty()) {
            setErrorMessage(ambiguous(playlist, where.several));
            return;
        }
        const std::string file = filePath.toStdString();
        if (!m_session->stage(std::make_unique<AddToPlaylistChange>(rekordboxPath, enginePath, name, file, title, libraries))) {
            return;
        }
        const QString id = AddToPlaylistChange::idFor(name, file);
        if (m_staged.insert(id).second) {
            std::erase_if(m_additions, [&](const auto &a) { return std::get<0>(a) == id; });
            m_additions.emplace_back(id, playlist, filePath);
        }
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

void PlaylistEditController::keepOutOfPlaylist(const QString &playlist, const QString &filePath)
{
    unstage(AddToPlaylistChange::idFor(playlist.toStdString(), filePath.toStdString()));
}

QVariantMap PlaylistEditController::pendingAdditions() const
{
    QVariantMap additions;
    for (const auto &[id, playlist, file] : m_additions) {
        if (m_staged.count(id) > 0) {
            QStringList files = additions.value(playlist).toStringList();
            files << file;
            additions[playlist] = files;
        }
    }
    return additions;
}

int PlaylistEditController::trackCountChange(const QString &playlist) const
{
    int change = 0;
    for (const auto &[id, name, file] : m_additions) {
        change += name == playlist && m_staged.count(id) > 0 ? 1 : 0;
    }
    for (const auto &[id, name, file] : m_removals) {
        change -= name == playlist && m_staged.count(id) > 0 ? 1 : 0;
    }
    return change;
}

bool PlaylistEditController::isPlaylistEdited(const QString &playlist) const
{
    const auto staged = [&](const auto &c) { return std::get<1>(c) == playlist && m_staged.count(std::get<0>(c)) > 0; };
    return std::any_of(m_additions.begin(), m_additions.end(), staged)
        || std::any_of(m_removals.begin(), m_removals.end(), staged);
}

bool PlaylistEditController::isAdditionStaged(const QString &playlist, const QString &filePath) const
{
    return m_staged.count(AddToPlaylistChange::idFor(playlist.toStdString(), filePath.toStdString())) > 0;
}

void PlaylistEditController::dropNewPlaylist(const QString &name)
{
    // The tracks staged into it go with it: without the playlist there
    // is nothing to add them to.
    const QString prefix = AddToPlaylistChange::idFor(name.toStdString(), std::string());
    std::vector<QString> adds;
    for (const QString &id : m_staged) {
        if (id.startsWith(prefix)) {
            adds.push_back(id);
        }
    }
    for (const QString &id : adds) {
        unstage(id);
    }
    unstage(CreatePlaylistChange::idFor(name.toStdString()));
}

bool PlaylistEditController::isNewPlaylistStaged(const QString &name) const
{
    return m_staged.count(CreatePlaylistChange::idFor(name.toStdString())) > 0;
}

void PlaylistEditController::forgetUnstagedNewPlaylists()
{
    m_newPlaylists.removeIf([this](const QString &name) { return !isNewPlaylistStaged(name); });
    std::erase_if(m_additions, [this](const auto &a) { return m_staged.count(std::get<0>(a)) == 0; });
    std::erase_if(m_removals, [this](const auto &r) { return m_staged.count(std::get<0>(r)) == 0; });
}

bool PlaylistEditController::deletePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist)
{
    setErrorMessage({});
    if (playlist.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string name = playlist.toStdString();
    lookUp(rekordboxPath, enginePath, name, [=, this](const PlaylistWhereabouts &where) {
        if (where.having.isEmpty()) {
            setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
            return;
        }
        if (!where.several.isEmpty()) {
            setErrorMessage(ambiguous(playlist, where.several));
            return;
        }
        if (!m_session->stage(std::make_unique<DeletePlaylistChange>(rekordboxPath, enginePath, name, where.having))) {
            return;  // the session reported why (another page's edits)
        }
        m_staged.insert(DeletePlaylistChange::idFor(name));
        ++m_pendingRevision;
        emit pendingChanged();
    });
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
    lookUp(rekordboxPath, enginePath, name, [=, this](const PlaylistWhereabouts &where) {
        if (where.having.isEmpty()) {
            setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
            return;
        }
        if (!where.several.isEmpty()) {
            setErrorMessage(ambiguous(playlist, where.several));
            return;
        }
        const std::string file = filePath.toStdString();
        if (!m_session->stage(
                std::make_unique<RemoveFromPlaylistChange>(rekordboxPath, enginePath, name, file, title, where.having))) {
            return;
        }
        const QString id = RemoveFromPlaylistChange::idFor(name, file);
        if (m_staged.insert(id).second) {
            std::erase_if(m_removals, [&](const auto &r) { return std::get<0>(r) == id; });
            m_removals.emplace_back(id, playlist, filePath);
        }
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

void PlaylistEditController::lookUp(const QString &rekordboxPath, const QString &enginePath, const std::string &playlist,
                                    std::function<void(const PlaylistWhereabouts &)> then)
{
    m_lookups.push_back({rekordboxPath, enginePath, playlist, std::move(then)});
    runNextLookup();
}

void PlaylistEditController::runNextLookup()
{
    if (m_lookupWatcher.isRunning()) {
        return;
    }
    if (m_lookups.empty()) {
        setBusy(false);
        return;
    }
    setBusy(true);
    const Lookup &next = m_lookups.front();
    m_lookupWatcher.setFuture(QtConcurrent::run(
        [rekordboxPath = next.rekordboxPath, enginePath = next.enginePath, playlist = next.playlist]() {
            return playlistWhereabouts(rekordboxPath, enginePath, playlist);
        }));
}

void PlaylistEditController::onLookupFinished()
{
    Lookup done = std::move(m_lookups.front());
    m_lookups.pop_front();
    // playlistWhereabouts swallows what the readers throw, so this is for
    // whatever else a worker can raise: said, not rethrown on the GUI
    // thread (see future_result.hpp).
    QString thrown;
    const PlaylistWhereabouts where = takeResult(m_lookupWatcher, &thrown);
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not read the stick's playlists: %1").arg(thrown));
    } else if (attachSession(done.rekordboxPath, done.enginePath)) {
        // The stick may have started a save meanwhile; nothing is staged
        // into a save that is writing.
        done.then(where);
    }
    runNextLookup();
}

void PlaylistEditController::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    if (busy) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::BusyCursor));
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
    emit busyChanged();
}

void PlaylistEditController::unstage(const QString &changeId)
{
    if (m_staged.erase(changeId) == 0) {
        return;
    }
    if (m_session) {
        m_session->unstage(changeId);
    }
    forgetUnstagedNewPlaylists();
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
