// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "playlist_edit_controller.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
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

// A playlist name as a create or a rename takes it: trimmed; empty when
// it is none, `why` saying so.
QString playlistName(const QString &name, QString &why)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        why = QStringLiteral("A playlist needs a name.");
        return {};
    }
    if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char(';'))) {
        why = QStringLiteral("A playlist name cannot hold \"/\" or \";\".");
        return {};
    }
    return trimmed;
}

}  // namespace

PlaylistEditController::PlaylistEditController(QObject *parent) : QObject(parent)
{
    connect(&m_lookupWatcher, &QFutureWatcher<std::pair<PlaylistWhereabouts, PlaylistWhereabouts>>::finished, this,
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
                    noteApplied(changeId);
                    forgetUnstagedNewPlaylists();
                    ++m_pendingRevision;
                    emit pendingChanged();
                }
            });
            connect(m_session, &LibraryEditSession::saveFinished, this, [this](const QVariantMap &summary) {
                // Worded first: the page shows it when it hears of the save.
                const QString described = describeApplied();
                m_appliedKinds.clear();
                m_appliedPlaylists.clear();
                if (m_lastSavedSummary != described) {
                    m_lastSavedSummary = described;
                    emit lastSavedSummaryChanged();
                }
                if (summary.value("written").toInt() > 0) {
                    emit playlistsSaved();
                }
            });
            connect(m_session, &LibraryEditSession::changesDiscarded, this, [this]() {
                if (!m_staged.empty()) {
                    m_staged.clear();
                    m_additions.clear();
                    m_removals.clear();
                    m_moves.clear();
                    m_renames.clear();
                    m_creates.clear();
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
    QString why;
    const QString playlist = playlistName(name, why);
    if (playlist.isEmpty()) {
        setErrorMessage(why);
        return false;
    }
    if (!attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string path = playlist.toStdString();
    // By value: the lookup's turn comes after this returns.
    const auto taken = [=, this]() -> QString {
        if (m_staged.count(CreatePlaylistChange::idFor(path)) > 0) {
            return QStringLiteral("\"%1\" is already waiting to be created.").arg(playlist);
        }
        for (const Rename &r : m_renames) {
            if (r.to == playlist) {
                return QStringLiteral("\"%1\" is the new name of \"%2\" already.").arg(playlist, r.from);
            }
        }
        return {};
    };
    if (const QString refusal = taken(); !refusal.isEmpty()) {
        setErrorMessage(refusal);
        return false;
    }
    lookUp(rekordboxPath, enginePath, path, [=, this](const PlaylistWhereabouts &where) {
        if (const QString refusal = taken(); !refusal.isEmpty()) {
            setErrorMessage(refusal);
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
        const QStringList libraries = where.onStick;
        const auto remake = [=](const std::string &named) -> std::unique_ptr<PendingChange> {
            return std::make_unique<CreatePlaylistChange>(rekordboxPath, enginePath, named, libraries);
        };
        if (!m_session->stage(remake(path))) {
            return;  // the session reported why (another page's edits)
        }
        m_staged.insert(CreatePlaylistChange::idFor(path));
        m_creates[playlist] = remake;
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
    // Looked up by the path the stick has now, staged under the name shown
    // (see renamePlaylist).
    const std::string name = playlist.toStdString();
    lookUp(rekordboxPath, enginePath, stickPathOf(playlist).toStdString(), [=, this](const PlaylistWhereabouts &where) {
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
        const auto remake = [=](const std::string &named) -> std::unique_ptr<PendingChange> {
            return std::make_unique<AddToPlaylistChange>(rekordboxPath, enginePath, named, file, title, libraries);
        };
        if (!m_session->stage(remake(name))) {
            return;
        }
        const QString id = AddToPlaylistChange::idFor(name, file);
        if (m_staged.insert(id).second) {
            std::erase_if(m_additions, [&](const Edit &a) { return a.id == id; });
            m_additions.push_back({id, playlist, filePath, -1, ++m_sequence, remake});
        }
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

void PlaylistEditController::keepOutOfPlaylist(const QString &playlist, const QString &filePath)
{
    for (const QString &name : namesOf(playlist)) {
        unstage(AddToPlaylistChange::idFor(name.toStdString(), filePath.toStdString()));
    }
}

QVariantMap PlaylistEditController::pendingAdditions() const
{
    QVariantMap additions;
    for (const Edit &a : m_additions) {
        if (m_staged.count(a.id) > 0) {
            QStringList files = additions.value(a.playlist).toStringList();
            files << a.file;
            additions[a.playlist] = files;
        }
    }
    return additions;
}

int PlaylistEditController::trackCountChange(const QString &playlist) const
{
    const QStringList names = namesOf(playlist);
    int change = 0;
    for (const Edit &a : m_additions) {
        change += names.contains(a.playlist) && m_staged.count(a.id) > 0 ? 1 : 0;
    }
    for (const Edit &r : m_removals) {
        change -= names.contains(r.playlist) && m_staged.count(r.id) > 0 ? 1 : 0;
    }
    return change;
}

bool PlaylistEditController::isPlaylistEdited(const QString &playlist) const
{
    const QStringList names = namesOf(playlist);
    const auto staged = [&](const Edit &c) { return names.contains(c.playlist) && m_staged.count(c.id) > 0; };
    return std::any_of(m_additions.begin(), m_additions.end(), staged)
        || std::any_of(m_removals.begin(), m_removals.end(), staged)
        || std::any_of(m_moves.begin(), m_moves.end(), staged) || isRenameStaged(playlist);
}

bool PlaylistEditController::moveInPlaylist(const QString &rekordboxPath, const QString &enginePath,
                                            const QString &playlist, const QString &filePath, const QString &title,
                                            int toIndex)
{
    setErrorMessage({});
    if (playlist.isEmpty() || filePath.isEmpty() || toIndex < 0 || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string name = playlist.toStdString();
    lookUp(rekordboxPath, enginePath, stickPathOf(playlist).toStdString(), [=, this](const PlaylistWhereabouts &where) {
        // A playlist staged to be created holds only what is staged into
        // it, and is written in every library on the stick.
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
        const auto remake = [=](const std::string &named) -> std::unique_ptr<PendingChange> {
            return std::make_unique<MoveInPlaylistChange>(rekordboxPath, enginePath, named, file, title, toIndex, libraries);
        };
        if (!m_session->stage(remake(name))) {
            return;
        }
        // The same id: the session replaced the move staged before, and
        // the preview takes this one, last.
        const QString id = MoveInPlaylistChange::idFor(name, file);
        m_staged.insert(id);
        std::erase_if(m_moves, [&](const Edit &m) { return m.id == id; });
        m_moves.push_back({id, playlist, filePath, toIndex, ++m_sequence, remake});
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

void PlaylistEditController::keepPositionInPlaylist(const QString &playlist, const QString &filePath)
{
    for (const QString &name : namesOf(playlist)) {
        unstage(MoveInPlaylistChange::idFor(name.toStdString(), filePath.toStdString()));
    }
}

bool PlaylistEditController::isMoveStaged(const QString &playlist, const QString &filePath) const
{
    const QStringList names = namesOf(playlist);
    return std::any_of(names.begin(), names.end(), [&](const QString &name) {
        return m_staged.count(MoveInPlaylistChange::idFor(name.toStdString(), filePath.toStdString())) > 0;
    });
}

QVariantMap PlaylistEditController::pendingMoves() const
{
    QVariantMap moves;
    for (const Edit &m : m_moves) {
        if (m_staged.count(m.id) > 0) {
            QVariantList list = moves.value(m.playlist).toList();
            list << QVariantMap{{QStringLiteral("filePath"), m.file}, {QStringLiteral("toIndex"), m.toIndex}};
            moves[m.playlist] = list;
        }
    }
    return moves;
}

bool PlaylistEditController::isAdditionStaged(const QString &playlist, const QString &filePath) const
{
    const QStringList names = namesOf(playlist);
    return std::any_of(names.begin(), names.end(), [&](const QString &name) {
        return m_staged.count(AddToPlaylistChange::idFor(name.toStdString(), filePath.toStdString())) > 0;
    });
}

void PlaylistEditController::dropNewPlaylist(const QString &name)
{
    // The tracks staged into it go with it, and their moves: without the
    // playlist there is nothing to add them to.
    for (const QString &id : editsAddressing(name)) {
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
    std::erase_if(m_creates, [this](const auto &c) { return !isNewPlaylistStaged(c.first); });
    std::erase_if(m_additions, [this](const Edit &a) { return m_staged.count(a.id) == 0; });
    std::erase_if(m_removals, [this](const Edit &r) { return m_staged.count(r.id) == 0; });
    std::erase_if(m_moves, [this](const Edit &m) { return m_staged.count(m.id) == 0; });
    std::erase_if(m_renames, [this](const Rename &r) { return m_staged.count(r.id) == 0; });
}

bool PlaylistEditController::deletePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist)
{
    setErrorMessage({});
    if (playlist.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const std::string name = playlist.toStdString();
    lookUp(rekordboxPath, enginePath, stickPathOf(playlist).toStdString(), [=, this](const PlaylistWhereabouts &where) {
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
    lookUp(rekordboxPath, enginePath, stickPathOf(playlist).toStdString(), [=, this](const PlaylistWhereabouts &where) {
        if (where.having.isEmpty()) {
            setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
            return;
        }
        if (!where.several.isEmpty()) {
            setErrorMessage(ambiguous(playlist, where.several));
            return;
        }
        const std::string file = filePath.toStdString();
        const QStringList libraries = where.having;
        const auto remake = [=](const std::string &named) -> std::unique_ptr<PendingChange> {
            return std::make_unique<RemoveFromPlaylistChange>(rekordboxPath, enginePath, named, file, title, libraries);
        };
        if (!m_session->stage(remake(name))) {
            return;
        }
        const QString id = RemoveFromPlaylistChange::idFor(name, file);
        if (m_staged.insert(id).second) {
            std::erase_if(m_removals, [&](const Edit &r) { return r.id == id; });
            m_removals.push_back({id, playlist, filePath, -1, ++m_sequence, remake});
        }
        ++m_pendingRevision;
        emit pendingChanged();
    });
    return true;
}

bool PlaylistEditController::renamePlaylist(const QString &rekordboxPath, const QString &enginePath, const QString &playlist,
                                            const QString &newName)
{
    setErrorMessage({});
    QString why;
    const QString name = playlistName(newName, why);
    if (name.isEmpty()) {
        setErrorMessage(why);
        return false;
    }
    if (playlist.isEmpty() || !attachSession(rekordboxPath, enginePath)) {
        return false;
    }
    const QString renamed = QString::fromStdString(RenamePlaylistChange::renamedPath(playlist.toStdString(), name.toStdString()));
    if (renamed == playlist) {
        return true;  // the name it has: nothing to stage
    }
    // What already takes the new path in what is staged; checked when
    // asked and again when the lookup's turn comes.
    const auto taken = [=, this]() -> QString {
        if (isNewPlaylistStaged(renamed)) {
            return QStringLiteral("\"%1\" is already waiting to be created.").arg(renamed);
        }
        for (const Rename &r : m_renames) {
            if (r.to == renamed) {
                return QStringLiteral("\"%1\" is the new name of \"%2\" already.").arg(renamed, r.from);
            }
        }
        return {};
    };
    if (const QString refusal = taken(); !refusal.isEmpty()) {
        setErrorMessage(refusal);
        return false;
    }
    if (isPlaylistStaged(playlist)) {
        setErrorMessage(QStringLiteral("\"%1\" is staged to be deleted. Keep it first to rename it.").arg(playlist));
        return false;
    }
    const auto renameOf = [this](const QString &shown) {
        return std::find_if(m_renames.begin(), m_renames.end(), [&](const Rename &r) { return r.to == shown; });
    };
    // A second rename replaces the first (the same change id, at the
    // first's place in the order), and edits staged under the first's new
    // name would then address a name nothing gives it.
    if (const auto earlier = renameOf(playlist); earlier != m_renames.end()) {
        if (!editsAddressing(playlist).empty() || m_staged.count(DeletePlaylistChange::idFor(playlist.toStdString())) > 0) {
            setErrorMessage(QStringLiteral("\"%1\" has edits staged under its new name. Save them first, or take them "
                                           "back, to rename it again.")
                                .arg(playlist));
            return false;
        }
        if (renamed == earlier->from) {
            keepPlaylistName(playlist);  // back to the name the stick has
            return true;
        }
    }
    // Nested renames would each have to know the other's order.
    for (const Rename &r : m_renames) {
        if (r.to != playlist && (within(playlist, r.to) || within(r.to, playlist) || within(r.from, playlist))) {
            setErrorMessage(QStringLiteral("\"%1\" is being renamed to \"%2\". Save that first to rename \"%3\".")
                                .arg(r.from, r.to, playlist));
            return false;
        }
    }
    const QString from = stickPathOf(playlist);
    lookUp(rekordboxPath, enginePath, from.toStdString(), renamed.toStdString(),
           [=, this](const PlaylistWhereabouts &where, const PlaylistWhereabouts &there) {
               if (const QString refusal = taken(); !refusal.isEmpty()) {
                   setErrorMessage(refusal);
                   return;
               }
               if (!there.having.isEmpty()) {
                   setErrorMessage(QStringLiteral("There is already a playlist \"%1\" (in %2).")
                                       .arg(renamed, displayNames(there.having)));
                   return;
               }
               if (isNewPlaylistStaged(playlist)) {
                   renameNewPlaylist(playlist, renamed);
                   return;
               }
               if (where.having.isEmpty()) {
                   setErrorMessage(QStringLiteral("No library on the stick has a playlist \"%1\".").arg(playlist));
                   return;
               }
               if (!where.several.isEmpty()) {
                   setErrorMessage(ambiguous(playlist, where.several));
                   return;
               }
               if (!m_session->stage(std::make_unique<RenamePlaylistChange>(rekordboxPath, enginePath, from.toStdString(),
                                                                            name.toStdString(), where.having))) {
                   return;
               }
               const QString id = RenamePlaylistChange::idFor(from.toStdString());
               m_staged.insert(id);
               std::erase_if(m_renames, [&](const Rename &r) { return r.id == id; });
               m_renames.push_back({id, from, renamed});
               ++m_pendingRevision;
               emit pendingChanged();
           });
    return true;
}

void PlaylistEditController::renameNewPlaylist(const QString &playlist, const QString &renamed)
{
    const auto create = m_creates.find(playlist);
    if (create == m_creates.end()) {
        return;
    }
    // Its edits in the order they were staged, across the three lists:
    // staged again after the create, as they were.
    std::vector<Edit> edits;
    for (const auto *list : {&m_additions, &m_removals, &m_moves}) {
        for (const Edit &e : *list) {
            if (e.playlist == playlist && m_staged.count(e.id) > 0) {
                edits.push_back(e);
            }
        }
    }
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.sequence < b.sequence; });
    const auto remakeCreate = create->second;
    for (const Edit &e : edits) {
        m_staged.erase(e.id);
        m_session->unstage(e.id);
    }
    m_staged.erase(CreatePlaylistChange::idFor(playlist.toStdString()));
    m_session->unstage(CreatePlaylistChange::idFor(playlist.toStdString()));
    forgetUnstagedNewPlaylists();

    const std::string name = renamed.toStdString();
    if (m_session->stage(remakeCreate(name))) {
        m_staged.insert(CreatePlaylistChange::idFor(name));
        m_creates[renamed] = remakeCreate;
        m_newPlaylists << renamed;
        for (Edit e : edits) {
            auto change = e.remake(name);
            const QString id = change->id();
            if (!m_session->stage(std::move(change))) {
                continue;
            }
            m_staged.insert(id);
            e.id = id;
            e.playlist = renamed;
            auto &list = e.toIndex >= 0 ? m_moves
                         : id.startsWith(QStringLiteral("addcue:add-to-playlist:")) ? m_additions : m_removals;
            list.push_back(e);
        }
    }
    ++m_pendingRevision;
    emit pendingChanged();
}

void PlaylistEditController::keepPlaylistName(const QString &playlist)
{
    const auto rename = std::find_if(m_renames.begin(), m_renames.end(), [&](const Rename &r) {
        return (r.from == playlist || r.to == playlist) && m_staged.count(r.id) > 0;
    });
    if (rename == m_renames.end()) {
        return;
    }
    const Rename taken = *rename;
    // The edits asked for under the new name would find nothing at Save.
    for (const QString &id : editsAddressing(taken.to)) {
        unstage(id);
    }
    unstage(taken.id);
}

bool PlaylistEditController::isRenameStaged(const QString &playlist) const
{
    return std::any_of(m_renames.begin(), m_renames.end(), [&](const Rename &r) {
        return (r.from == playlist || r.to == playlist) && m_staged.count(r.id) > 0;
    });
}

QVariantMap PlaylistEditController::pendingRenames() const
{
    QVariantMap renames;
    for (const Rename &r : m_renames) {
        if (m_staged.count(r.id) > 0) {
            renames[r.from] = r.to;
        }
    }
    return renames;
}

bool PlaylistEditController::within(const QString &path, const QString &folder)
{
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

QString PlaylistEditController::stickPathOf(const QString &shown) const
{
    for (const Rename &r : m_renames) {
        if (m_staged.count(r.id) > 0 && within(shown, r.to)) {
            return r.from + shown.mid(r.to.size());
        }
    }
    return shown;
}

QStringList PlaylistEditController::namesOf(const QString &shown) const
{
    QStringList names{shown};
    if (const QString stick = stickPathOf(shown); stick != shown) {
        names << stick;
    }
    return names;
}

std::vector<QString> PlaylistEditController::editsAddressing(const QString &path) const
{
    std::vector<QString> ids;
    for (const auto *list : {&m_additions, &m_removals, &m_moves}) {
        for (const Edit &e : *list) {
            if (within(e.playlist, path) && m_staged.count(e.id) > 0) {
                ids.push_back(e.id);
            }
        }
    }
    for (const QString &id : m_staged) {
        const QString prefix = DeletePlaylistChange::idFor(std::string());
        if (id.startsWith(prefix) && within(id.mid(prefix.size()), path)) {
            ids.push_back(id);
        }
    }
    return ids;
}

void PlaylistEditController::lookUp(const QString &rekordboxPath, const QString &enginePath, const std::string &playlist,
                                    std::function<void(const PlaylistWhereabouts &)> then)
{
    lookUp(rekordboxPath, enginePath, playlist, std::string(),
           [then = std::move(then)](const PlaylistWhereabouts &where, const PlaylistWhereabouts &) { then(where); });
}

void PlaylistEditController::lookUp(const QString &rekordboxPath, const QString &enginePath, const std::string &playlist,
                                    const std::string &also,
                                    std::function<void(const PlaylistWhereabouts &, const PlaylistWhereabouts &)> then)
{
    m_lookups.push_back({rekordboxPath, enginePath, playlist, also, std::move(then)});
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
        [rekordboxPath = next.rekordboxPath, enginePath = next.enginePath, playlist = next.playlist, also = next.also]() {
            return std::make_pair(playlistWhereabouts(rekordboxPath, enginePath, playlist),
                                  also.empty() ? PlaylistWhereabouts{} : playlistWhereabouts(rekordboxPath, enginePath, also));
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
    const auto [where, also] = takeResult(m_lookupWatcher, &thrown);
    if (!thrown.isEmpty()) {
        setErrorMessage(QStringLiteral("Could not read the stick's playlists: %1").arg(thrown));
    } else if (attachSession(done.rekordboxPath, done.enginePath)) {
        // The stick may have started a save meanwhile; nothing is staged
        // into a save that is writing.
        done.then(where, also);
    }
    runNextLookup();
}

void PlaylistEditController::noteApplied(const QString &changeId)
{
    // "addcue:<kind>:<playlist>" or "addcue:<kind>:<playlist>|<file>".
    const QStringList parts = changeId.split(QLatin1Char(':'));
    if (parts.size() < 3) {
        return;
    }
    const QString kind = parts.at(1);
    const QString rest = parts.mid(2).join(QLatin1Char(':'));
    ++m_appliedKinds[kind];
    m_appliedPlaylists.insert(rest.section(QLatin1Char('|'), 0, 0));
}

QString PlaylistEditController::describeApplied() const
{
    if (m_appliedPlaylists.empty()) {
        return {};
    }
    const auto tracks = [](int n, const char *verb) {
        return QStringLiteral("%1 %2 %3").arg(n).arg(n == 1 ? QStringLiteral("track") : QStringLiteral("tracks"),
                                                    QLatin1String(verb));
    };
    const bool one = m_appliedPlaylists.size() == 1;
    const auto count = [&](const char *kind) {
        const auto it = m_appliedKinds.find(QLatin1String(kind));
        return it == m_appliedKinds.end() ? 0 : it->second;
    };
    // In the order a playlist's life runs, whatever order the save had.
    QStringList parts;
    if (const int n = count("create-playlist")) {
        parts << (one ? QStringLiteral("created") : QStringLiteral("%1 created").arg(n));
    }
    if (const int n = count("add-to-playlist")) {
        parts << tracks(n, "added");
    }
    if (const int n = count("move-in-playlist")) {
        parts << tracks(n, "moved");
    }
    if (const int n = count("remove-from-playlist")) {
        parts << tracks(n, "removed");
    }
    if (const int n = count("delete-playlist")) {
        parts << (one ? QStringLiteral("deleted") : QStringLiteral("%1 deleted").arg(n));
    }
    int other = 0;
    for (const auto &[kind, n] : m_appliedKinds) {
        static const QStringList known{QStringLiteral("create-playlist"), QStringLiteral("add-to-playlist"),
                                       QStringLiteral("move-in-playlist"), QStringLiteral("remove-from-playlist"),
                                       QStringLiteral("delete-playlist")};
        other += known.contains(kind) ? 0 : n;
    }
    if (other > 0) {
        parts << QStringLiteral("%1 %2").arg(other).arg(other == 1 ? QStringLiteral("change") : QStringLiteral("changes"));
    }
    const QString head = one ? QStringLiteral("\"%1\" saved").arg(*m_appliedPlaylists.begin())
                             : QStringLiteral("%1 playlists saved").arg(m_appliedPlaylists.size());
    return head + QStringLiteral(": ") + parts.join(QStringLiteral(", "));
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
    for (const QString &name : namesOf(playlist)) {
        unstage(DeletePlaylistChange::idFor(name.toStdString()));
    }
}

void PlaylistEditController::keepInPlaylist(const QString &playlist, const QString &filePath)
{
    for (const QString &name : namesOf(playlist)) {
        unstage(RemoveFromPlaylistChange::idFor(name.toStdString(), filePath.toStdString()));
    }
}

bool PlaylistEditController::isPlaylistStaged(const QString &playlist) const
{
    const QStringList names = namesOf(playlist);
    return std::any_of(names.begin(), names.end(), [&](const QString &name) {
        return m_staged.count(DeletePlaylistChange::idFor(name.toStdString())) > 0;
    });
}

bool PlaylistEditController::isRemovalStaged(const QString &playlist, const QString &filePath) const
{
    const QStringList names = namesOf(playlist);
    return std::any_of(names.begin(), names.end(), [&](const QString &name) {
        return m_staged.count(RemoveFromPlaylistChange::idFor(name.toStdString(), filePath.toStdString())) > 0;
    });
}

void PlaylistEditController::setErrorMessage(const QString &message)
{
    if (m_errorMessage != message) {
        m_errorMessage = message;
        emit errorMessageChanged();
    }
}

}  // namespace seabass::gui
