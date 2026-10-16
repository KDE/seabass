// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/playlist_edit_changes.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <set>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *CreateLabel = "playlist-create";
constexpr const char *DeleteLabel = "playlist-delete";
constexpr const char *RemoveLabel = "playlist-remove";
constexpr const char *AddLabel = "playlist-add";
constexpr const char *MoveLabel = "playlist-move";
constexpr const char *RenameLabel = "playlist-rename";

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

// Each node of a playlist tree (export.pdb's or OneLibrary's) with its
// full path, spelled by walking up its parents.
template<typename Node>
std::vector<std::pair<const Node *, std::string>> spelledPaths(const std::vector<Node> &tree)
{
    std::map<decltype(Node::id), const Node *> byId;
    for (const auto &n : tree) {
        byId[n.id] = &n;
    }
    std::vector<std::pair<const Node *, std::string>> spelled;
    for (const auto &n : tree) {
        std::string path = n.name;
        int guard = 0;
        for (auto p = n.parentId; p != 0 && byId.count(p) && guard < 64; p = byId.at(p)->parentId, ++guard) {
            path = byId.at(p)->name + "/" + path;
        }
        spelled.emplace_back(&n, std::move(path));
    }
    return spelled;
}

// export.pdb's playlists (and folders) spelling a full path, by walking
// its tree. Two when a folder holds two of one name, or a name holds a
// "/" that reads as a folder.
std::vector<uint32_t> pdbPlaylistIds(const infrastructure::rekordbox::PdbRowWriter &rows, const std::string &path)
{
    std::vector<uint32_t> ids;
    for (const auto &[node, spelled] : spelledPaths(rows.playlistTree())) {
        if (spelled == path) {
            ids.push_back(node->id);
        }
    }
    return ids;
}

// The one playlist at `path`, or nothing when there is none; throws when
// there are several, since editing by path would have to guess.
std::optional<uint32_t> pdbPlaylistId(const infrastructure::rekordbox::PdbRowWriter &rows, const std::string &path)
{
    const auto ids = pdbPlaylistIds(rows, path);
    if (ids.size() > 1) {
        throw std::runtime_error("rekordbox has " + std::to_string(ids.size()) + " playlists named \"" + path + "\"");
    }
    return ids.empty() ? std::nullopt : std::optional<uint32_t>(ids.front());
}

void requireOneEnginePlaylist(const std::string &library, const std::string &path)
{
    const int n = infrastructure::engine::enginePlaylistCountAtPath(library, path);
    if (n > 1) {
        throw std::runtime_error("Engine has " + std::to_string(n) + " playlists named \"" + path + "\"");
    }
}

void requireOneOneLibraryPlaylist(infrastructure::onelibrary::OneLibraryCueWriter &w, const std::string &path)
{
    const int n = w.playlistCountAtPath(path);
    if (n > 1) {
        throw std::runtime_error("OneLibrary has " + std::to_string(n) + " playlists named \"" + path + "\"");
    }
}

// "/Contents/..." as export.pdb spells a file on the stick.
std::string pathOnStick(const std::string &pioneerRoot, const std::string &filePath)
{
    std::error_code ec;
    const auto relative = std::filesystem::relative(pathFromUtf8(filePath), pathFromUtf8(pioneerRoot).parent_path(), ec);
    return ec ? filePath : "/" + pathToGenericUtf8(relative);
}

QStringList touched(const QStringList &libraries)
{
    return libraries;
}

std::vector<BackupTarget> backupsFor(const QStringList &libraries, const QString &pioneerRoot, const QString &enginePath,
                                     const char *label)
{
    std::vector<BackupTarget> targets;
    for (const auto &library : libraries) {
        if (library == QLatin1String("rekordbox")) {
            targets.push_back({exportPdbOf(pioneerRoot.toStdString()), label});
        } else if (library == QLatin1String("onelibrary")) {
            targets.push_back({infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneerRoot.toStdString()), label});
        } else if (library == QLatin1String("engine")) {
            targets.push_back({engineDbOf(enginePath.toStdString()), label});
        }
    }
    return targets;
}

// `order` (one library's copy of a playlist, by that library's ids) with
// the one entry whose id is in `file` moved to `toIndex`, clamped to the
// end. Nothing, with `why` saying so, when the copy holds the file not
// exactly once.
template<typename Id>
std::optional<std::vector<Id>> movedOrder(const std::vector<Id> &order, const std::set<Id> &file, int toIndex,
                                          const std::string &library, std::string &why)
{
    const auto held = std::count_if(order.begin(), order.end(), [&](Id id) { return file.count(id) > 0; });
    if (held != 1) {
        why = held == 0 ? library + "'s playlist does not hold the file"
                        : library + "'s playlist holds the file " + std::to_string(held) + " times";
        return std::nullopt;
    }
    std::vector<Id> moved = order;
    const auto at = std::find_if(moved.begin(), moved.end(), [&](Id id) { return file.count(id) > 0; });
    const Id id = *at;
    moved.erase(at);
    const size_t to = std::min(static_cast<size_t>(std::max(toIndex, 0)), moved.size());
    moved.insert(moved.begin() + static_cast<std::ptrdiff_t>(to), id);
    return moved;
}

}  // namespace

PlaylistWhereabouts playlistWhereabouts(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist)
{
    PlaylistWhereabouts where;
    const auto note = [&](const char *library, int count) {
        where.onStick << QString::fromLatin1(library);
        if (count > 0) {
            where.having << QString::fromLatin1(library);
        }
        if (count > 1) {
            where.several << QString::fromLatin1(library);
        }
    };
    const std::string pioneer = pioneerRoot.toStdString();
    if (!pioneer.empty() && std::filesystem::exists(pathFromUtf8(exportPdbOf(pioneer)))) {
        int count = 0;
        try {
            count = static_cast<int>(pdbPlaylistIds(infrastructure::rekordbox::PdbRowWriter(exportPdbOf(pioneer)), playlist).size());
        } catch (const std::exception &) {
        }
        note("rekordbox", count);
    }
    if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        int count = 0;
        try {
            count = infrastructure::onelibrary::OneLibraryCueWriter(pioneer).playlistCountAtPath(playlist);
        } catch (const std::exception &) {
        }
        note("onelibrary", count);
    }
    const std::string engine = enginePath.toStdString();
    if (!engine.empty() && std::filesystem::exists(pathFromUtf8(engineDbOf(engine)))) {
        int count = 0;
        try {
            count = infrastructure::engine::enginePlaylistCountAtPath(engine, playlist);
        } catch (const std::exception &) {
        }
        note("engine", count);
    }
    return where;
}

std::vector<std::string> playlistPathsIn(const std::string &catalog, const std::string &path)
{
    std::vector<std::string> paths;
    if (path.empty()) {
        return paths;
    }
    try {
        if (catalog == "rekordbox") {
            if (!std::filesystem::exists(pathFromUtf8(exportPdbOf(path)))) {
                return paths;
            }
            const infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(path));
            for (const auto &[node, spelled] : spelledPaths(rows.playlistTree())) {
                if (!node->isFolder) {
                    paths.push_back(spelled);
                }
            }
        } else if (catalog == "onelibrary") {
            if (!infrastructure::onelibrary::OneLibraryCueWriter::existsFor(path)) {
                return paths;
            }
            infrastructure::onelibrary::OneLibraryCueWriter w(path);
            for (const auto &[node, spelled] : spelledPaths(w.playlistTree())) {
                if (!node->isFolder) {
                    paths.push_back(spelled);
                }
            }
        } else if (catalog == "engine") {
            if (!std::filesystem::exists(pathFromUtf8(engineDbOf(path)))) {
                return paths;
            }
            for (const auto &info : infrastructure::engine::listEnginePlaylists(path)) {
                if (!info.folder) {
                    paths.push_back(info.path);
                }
            }
        }
    } catch (const std::exception &) {
        paths.clear();
    }
    return paths;
}

CreatePlaylistChange::CreatePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_libraries(std::move(libraries))
{
}

QString CreatePlaylistChange::idFor(const std::string &playlist)
{
    return QStringLiteral("addcue:create-playlist:") + QString::fromStdString(playlist);
}

QString CreatePlaylistChange::id() const
{
    return idFor(m_playlist);
}

QString CreatePlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString CreatePlaylistChange::description() const
{
    return QStringLiteral("Create the playlist \"%1\"").arg(QString::fromStdString(m_playlist));
}

QString CreatePlaylistChange::playlistEdited() const
{
    return QString::fromStdString(m_playlist);
}

QString CreatePlaylistChange::subject() const
{
    return QString::fromStdString(m_playlist);
}

QString CreatePlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString CreatePlaylistChange::verb() const
{
    return QStringLiteral("created");
}

QStringList CreatePlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> CreatePlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, CreateLabel);
}

ChangeOutcome CreatePlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    int created = 0;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, CreateLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            if (!pdbPlaylistId(rows, m_playlist)) {
                rows.createPlaylist(0, m_playlist, false);
                if (!rows.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                }
                session.noteItemApplied();
                ++created;
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), CreateLabel);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            requireOneOneLibraryPlaylist(writer, m_playlist);
            if (writer.playlistCountAtPath(m_playlist) == 0) {
                writer.createPlaylist("", m_playlist, false);
                ++created;
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            FormatWriteSession &session =
                sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), 1, CreateLabel);
            requireOneEnginePlaylist(session.writeRoot(), m_playlist);
            if (infrastructure::engine::enginePlaylistCountAtPath(session.writeRoot(), m_playlist) == 0) {
                infrastructure::engine::createEnginePlaylist(session.writeRoot(), m_playlist);
                session.noteItemApplied();
                ++created;
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    if (created == 0) {
        ctx.log().record("playlist-create: \"" + m_playlist + "\" is in every library already, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-create: created \"" + m_playlist + "\" in " + std::to_string(created) + " libraries");
    return ChangeOutcome::success();
}

DeletePlaylistChange::DeletePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_libraries(std::move(libraries))
{
}

QString DeletePlaylistChange::idFor(const std::string &playlist)
{
    return QStringLiteral("addcue:delete-playlist:") + QString::fromStdString(playlist);
}

QString DeletePlaylistChange::id() const
{
    return idFor(m_playlist);
}

QString DeletePlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString DeletePlaylistChange::description() const
{
    return QStringLiteral("Delete the playlist \"%1\"").arg(QString::fromStdString(m_playlist));
}

QString DeletePlaylistChange::playlistEdited() const
{
    return QString::fromStdString(m_playlist);
}

QString DeletePlaylistChange::subject() const
{
    return QString::fromStdString(m_playlist);
}

QString DeletePlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString DeletePlaylistChange::verb() const
{
    return QStringLiteral("deleted");
}

QStringList DeletePlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> DeletePlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, DeleteLabel);
}

ChangeOutcome DeletePlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    int deleted = 0;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, DeleteLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            if (const auto id = pdbPlaylistId(rows, m_playlist)) {
                if (rows.deletePlaylist(*id) > 0) {
                    if (!rows.commit()) {
                        return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                    }
                    session.noteItemApplied();
                    ++deleted;
                }
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), DeleteLabel);
            requireOneOneLibraryPlaylist(sharedOneLibraryWriter(ctx, pioneer), m_playlist);
            if (sharedOneLibraryWriter(ctx, pioneer).deletePlaylist(m_playlist) > 0) {
                ++deleted;
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            FormatWriteSession &session =
                sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), 1, DeleteLabel);
            requireOneEnginePlaylist(session.writeRoot(), m_playlist);
            if (infrastructure::engine::deleteEnginePlaylist(session.writeRoot(), m_playlist) > 0) {
                session.noteItemApplied();
                ++deleted;
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    if (deleted == 0) {
        ctx.log().record("playlist-delete: \"" + m_playlist + "\" is in no library any more, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-delete: deleted \"" + m_playlist + "\" from " + std::to_string(deleted) + " libraries");
    return ChangeOutcome::success();
}

AddToPlaylistChange::AddToPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string filePath,
                                         QString title, QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_filePath(std::move(filePath)), m_title(std::move(title)), m_libraries(std::move(libraries))
{
}

QString AddToPlaylistChange::idFor(const std::string &playlist, const std::string &filePath)
{
    return QStringLiteral("addcue:add-to-playlist:") + QString::fromStdString(playlist) + QLatin1Char('|')
           + QString::fromStdString(filePath);
}

QString AddToPlaylistChange::id() const
{
    return idFor(m_playlist, m_filePath);
}

QString AddToPlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString AddToPlaylistChange::description() const
{
    return QStringLiteral("Add \"%1\" to \"%2\"").arg(m_title, QString::fromStdString(m_playlist));
}

QString AddToPlaylistChange::playlistEdited() const
{
    return QString::fromStdString(m_playlist);
}

QString AddToPlaylistChange::subject() const
{
    return m_title;
}

QString AddToPlaylistChange::unit() const
{
    return QStringLiteral("playlist entries");
}

QString AddToPlaylistChange::verb() const
{
    return QStringLiteral("added");
}

QStringList AddToPlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> AddToPlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, AddLabel);
}

ChangeOutcome AddToPlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    int added = 0;
    std::vector<std::string> leftAlone;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, AddLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            const auto playlist = pdbPlaylistId(rows, m_playlist);
            const auto ids = rows.trackIdsWithFilePath(pathOnStick(pioneer, m_filePath));
            if (!playlist || ids.empty()) {
                leftAlone.push_back(playlist ? "rekordbox has no row for the file" : "rekordbox has no such playlist");
            } else if (rows.appendPlaylistEntries(*playlist, {ids.front()}) > 0) {
                if (!rows.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                }
                session.noteItemApplied();
                ++added;
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), AddLabel);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            requireOneOneLibraryPlaylist(writer, m_playlist);
            if (writer.playlistCountAtPath(m_playlist) == 0) {
                leftAlone.push_back("OneLibrary has no such playlist");
            } else {
                try {
                    added += writer.addToPlaylist(m_playlist, m_filePath) ? 1 : 0;
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    leftAlone.push_back("OneLibrary has no row for the file");
                }
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            const std::string engine = m_enginePath.toStdString();
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, AddLabel);
            requireOneEnginePlaylist(session.writeRoot(), m_playlist);
            const auto ids = infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_filePath);
            if (infrastructure::engine::enginePlaylistCountAtPath(session.writeRoot(), m_playlist) == 0) {
                leftAlone.push_back("Engine has no such playlist");
            } else if (ids.size() != 1) {
                // None: Engine does not have the track. Two: which one the
                // playlist should hold is a guess (Clean Up first).
                leftAlone.push_back(ids.empty() ? "Engine has no row for the file"
                                                : "Engine has " + std::to_string(ids.size()) + " rows for the file");
            } else if (infrastructure::engine::insertIntoEnginePlaylist(session.writeRoot(), m_playlist, ids.front(),
                                                                        std::nullopt)) {
                session.noteItemApplied();
                ++added;
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    for (const auto &why : leftAlone) {
        ctx.log().record("playlist-add: \"" + m_filePath + "\" not added to \"" + m_playlist + "\" there: " + why);
    }
    if (added == 0) {
        ctx.log().record("playlist-add: \"" + m_filePath + "\" added to \"" + m_playlist + "\" nowhere, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-add: added \"" + m_filePath + "\" to \"" + m_playlist + "\" in " + std::to_string(added)
                     + " libraries");
    return ChangeOutcome::success();
}

RemoveFromPlaylistChange::RemoveFromPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist,
                                                   std::string filePath, QString title, QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_filePath(std::move(filePath)), m_title(std::move(title)), m_libraries(std::move(libraries))
{
}

QString RemoveFromPlaylistChange::idFor(const std::string &playlist, const std::string &filePath)
{
    return QStringLiteral("addcue:remove-from-playlist:") + QString::fromStdString(playlist) + QLatin1Char('|')
           + QString::fromStdString(filePath);
}

QString RemoveFromPlaylistChange::id() const
{
    return idFor(m_playlist, m_filePath);
}

QString RemoveFromPlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString RemoveFromPlaylistChange::description() const
{
    return QStringLiteral("Remove \"%1\" from \"%2\"").arg(m_title, QString::fromStdString(m_playlist));
}

QString RemoveFromPlaylistChange::playlistEdited() const
{
    return QString::fromStdString(m_playlist);
}

QString RemoveFromPlaylistChange::subject() const
{
    return m_title;
}

QString RemoveFromPlaylistChange::unit() const
{
    return QStringLiteral("playlist entries");
}

QString RemoveFromPlaylistChange::verb() const
{
    return QStringLiteral("removed");
}

QStringList RemoveFromPlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> RemoveFromPlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, RemoveLabel);
}

ChangeOutcome RemoveFromPlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    size_t removed = 0;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, RemoveLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            const auto playlist = pdbPlaylistId(rows, m_playlist);
            const auto ids = rows.trackIdsWithFilePath(pathOnStick(pioneer, m_filePath));
            if (playlist && !ids.empty()) {
                const size_t n = rows.removePlaylistEntries(*playlist, std::set<uint32_t>(ids.begin(), ids.end()));
                if (n > 0) {
                    if (!rows.commit()) {
                        return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                    }
                    session.noteItemApplied();
                    removed += n;
                }
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), RemoveLabel);
            requireOneOneLibraryPlaylist(sharedOneLibraryWriter(ctx, pioneer), m_playlist);
            try {
                removed += sharedOneLibraryWriter(ctx, pioneer).removeFromPlaylist(m_playlist, m_filePath) ? 1 : 0;
            } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                // OneLibrary does not list the file: nothing to take out.
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            const std::string engine = m_enginePath.toStdString();
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, RemoveLabel);
            requireOneEnginePlaylist(session.writeRoot(), m_playlist);
            // Relative to the stick's library folder, not the write root:
            // a save may write Engine through a scratch copy elsewhere.
            for (const std::int64_t id :
                 infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_filePath)) {
                if (infrastructure::engine::removeFromEnginePlaylist(session.writeRoot(), m_playlist, id)) {
                    session.noteItemApplied();
                    ++removed;
                }
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    if (removed == 0) {
        ctx.log().record("playlist-remove: \"" + m_filePath + "\" is not in \"" + m_playlist + "\" any more, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-remove: took \"" + m_filePath + "\" out of \"" + m_playlist + "\"");
    return ChangeOutcome::success();
}

MoveInPlaylistChange::MoveInPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist,
                                           std::string filePath, QString title, int toIndex, QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_filePath(std::move(filePath)), m_title(std::move(title)), m_toIndex(toIndex), m_libraries(std::move(libraries))
{
}

QString MoveInPlaylistChange::idFor(const std::string &playlist, const std::string &filePath)
{
    return QStringLiteral("addcue:move-in-playlist:") + QString::fromStdString(playlist) + QLatin1Char('|')
           + QString::fromStdString(filePath);
}

QString MoveInPlaylistChange::id() const
{
    return idFor(m_playlist, m_filePath);
}

QString MoveInPlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString MoveInPlaylistChange::description() const
{
    return QStringLiteral("Move \"%1\" to position %2 in \"%3\"")
        .arg(m_title)
        .arg(m_toIndex + 1)
        .arg(QString::fromStdString(m_playlist));
}

QString MoveInPlaylistChange::playlistEdited() const
{
    return QString::fromStdString(m_playlist);
}

QString MoveInPlaylistChange::subject() const
{
    return m_title;
}

QString MoveInPlaylistChange::unit() const
{
    return QStringLiteral("playlist entries");
}

QString MoveInPlaylistChange::verb() const
{
    return QStringLiteral("moved");
}

QStringList MoveInPlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> MoveInPlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, MoveLabel);
}

ChangeOutcome MoveInPlaylistChange::apply(SaveContext &ctx)
{
    const std::string pioneer = m_pioneerRoot.toStdString();
    int moved = 0;
    std::vector<std::string> leftAlone;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, MoveLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            const auto playlist = pdbPlaylistId(rows, m_playlist);
            const auto ids = rows.trackIdsWithFilePath(pathOnStick(pioneer, m_filePath));
            std::string why;
            if (!playlist) {
                leftAlone.push_back("rekordbox has no such playlist");
            } else if (const auto order = movedOrder(rows.playlistTrackIds(*playlist),
                                                     std::set<uint32_t>(ids.begin(), ids.end()), m_toIndex, "rekordbox", why)) {
                if (rows.reorderPlaylist(*playlist, *order)) {
                    if (!rows.commit()) {
                        return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                    }
                    session.noteItemApplied();
                    ++moved;
                } else {
                    leftAlone.push_back("rekordbox has it there already");
                }
            } else {
                leftAlone.push_back(why);
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), MoveLabel);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            requireOneOneLibraryPlaylist(writer, m_playlist);
            if (writer.playlistCountAtPath(m_playlist) == 0) {
                leftAlone.push_back("OneLibrary has no such playlist");
            } else {
                std::vector<int64_t> file;
                try {
                    file = writer.contentIdsForFile(m_filePath);
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    // No row for the file: the playlist cannot hold it,
                    // which movedOrder says.
                }
                std::vector<int64_t> order;
                for (const auto &[content, sequence] : writer.playlistContent(m_playlist)) {
                    order.push_back(content);
                }
                std::string why;
                if (const auto wanted =
                        movedOrder(order, std::set<int64_t>(file.begin(), file.end()), m_toIndex, "OneLibrary", why)) {
                    if (writer.reorderPlaylistContent(m_playlist, *wanted)) {
                        ++moved;
                    } else {
                        leftAlone.push_back("OneLibrary has it there already");
                    }
                } else {
                    leftAlone.push_back(why);
                }
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            const std::string engine = m_enginePath.toStdString();
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", engine, 1, MoveLabel);
            requireOneEnginePlaylist(session.writeRoot(), m_playlist);
            if (infrastructure::engine::enginePlaylistCountAtPath(session.writeRoot(), m_playlist) == 0) {
                leftAlone.push_back("Engine has no such playlist");
            } else {
                const auto ids =
                    infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_filePath);
                const auto order = infrastructure::engine::engineTrackIdsInPlaylist(session.writeRoot(), m_playlist);
                std::string why;
                if (const auto wanted =
                        movedOrder(order, std::set<std::int64_t>(ids.begin(), ids.end()), m_toIndex, "Engine", why)) {
                    // Engine orders by links, not numbers: the entry goes
                    // after the one before its new place, or first.
                    const auto at = std::find_if(wanted->begin(), wanted->end(), [&](std::int64_t id) {
                        return std::find(ids.begin(), ids.end(), id) != ids.end();
                    });
                    const std::optional<std::int64_t> after =
                        at == wanted->begin() ? std::nullopt : std::optional<std::int64_t>(*(at - 1));
                    if (infrastructure::engine::moveInEnginePlaylist(session.writeRoot(), m_playlist, *at, after)) {
                        session.noteItemApplied();
                        ++moved;
                    } else {
                        leftAlone.push_back("Engine has it there already");
                    }
                } else {
                    leftAlone.push_back(why);
                }
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    for (const auto &why : leftAlone) {
        ctx.log().record("playlist-move: \"" + m_filePath + "\" not moved in \"" + m_playlist + "\" there: " + why);
    }
    if (moved == 0) {
        ctx.log().record("playlist-move: \"" + m_filePath + "\" moved in \"" + m_playlist + "\" nowhere, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-move: moved \"" + m_filePath + "\" to position " + std::to_string(m_toIndex + 1) + " in \""
                     + m_playlist + "\" in " + std::to_string(moved) + " libraries");
    return ChangeOutcome::success();
}

RenamePlaylistChange::RenamePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string newName,
                                           QStringList libraries)
    : m_pioneerRoot(std::move(pioneerRoot)), m_enginePath(std::move(enginePath)), m_playlist(std::move(playlist)),
      m_newName(std::move(newName)), m_libraries(std::move(libraries))
{
}

QString RenamePlaylistChange::idFor(const std::string &playlist)
{
    return QStringLiteral("addcue:rename-playlist:") + QString::fromStdString(playlist);
}

std::string RenamePlaylistChange::renamedPath(const std::string &playlist, const std::string &newName)
{
    const size_t slash = playlist.rfind('/');
    return slash == std::string::npos ? newName : playlist.substr(0, slash + 1) + newName;
}

QString RenamePlaylistChange::id() const
{
    return idFor(m_playlist);
}

QString RenamePlaylistChange::owner() const
{
    return QStringLiteral("addcue");
}

QString RenamePlaylistChange::description() const
{
    return QStringLiteral("Rename the playlist \"%1\" to \"%2\"")
        .arg(QString::fromStdString(m_playlist), QString::fromStdString(m_newName));
}

QString RenamePlaylistChange::subject() const
{
    return QString::fromStdString(m_playlist);
}

QString RenamePlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString RenamePlaylistChange::verb() const
{
    return QStringLiteral("renamed");
}

QStringList RenamePlaylistChange::formatsTouched() const
{
    return touched(m_libraries);
}

std::vector<BackupTarget> RenamePlaylistChange::filesToBackup(SaveContext &) const
{
    return backupsFor(m_libraries, m_pioneerRoot, m_enginePath, RenameLabel);
}

ChangeOutcome RenamePlaylistChange::apply(SaveContext &ctx)
{
    if (m_newName.empty() || QString::fromStdString(m_newName).trimmed().toStdString() != m_newName
        || m_newName.find_first_of("/;") != std::string::npos) {
        return ChangeOutcome::failure(QStringLiteral("\"%1\" is no playlist name: it must be non-empty, trimmed, "
                                                     "and hold no \"/\" or \";\".")
                                          .arg(QString::fromStdString(m_newName)));
    }
    const std::string pioneer = m_pioneerRoot.toStdString();
    const std::string renamed = renamedPath(m_playlist, m_newName);
    if (renamed == m_playlist) {
        ctx.log().record("playlist-rename: \"" + m_playlist + "\" has that name already, skipped");
        return ChangeOutcome::skip();
    }
    // Why a library with `atOld` playlists at the old path and `atNew` at
    // the new one is left alone, or nothing when it is renamed.
    const auto refusal = [&](const std::string &library, int atOld, int atNew) -> std::optional<std::string> {
        if (atOld == 0) {
            return library + " has no such playlist";
        }
        if (atOld > 1) {
            return library + " has " + std::to_string(atOld) + " playlists named \"" + m_playlist + "\"";
        }
        if (atNew > 0) {
            return library + " has a playlist \"" + renamed + "\" already";
        }
        return std::nullopt;
    };
    int done = 0;
    std::vector<std::string> leftAlone;
    try {
        if (m_libraries.contains(QStringLiteral("rekordbox"))) {
            FormatWriteSession &session = sharedFormatWriteSession(ctx, "rekordbox", pioneer, 1, RenameLabel);
            infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(session.writeRoot()));
            const auto ids = pdbPlaylistIds(rows, m_playlist);
            if (const auto why = refusal("rekordbox", static_cast<int>(ids.size()),
                                         static_cast<int>(pdbPlaylistIds(rows, renamed).size()))) {
                leftAlone.push_back(*why);
            } else if (rows.renamePlaylist(ids.front(), m_newName)) {
                if (!rows.commit()) {
                    return ChangeOutcome::failure(QStringLiteral("could not write export.pdb"));
                }
                session.noteItemApplied();
                ++done;
            }
        }
        if (m_libraries.contains(QStringLiteral("onelibrary"))) {
            ctx.backupOnce(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pioneer), RenameLabel);
            auto &writer = sharedOneLibraryWriter(ctx, pioneer);
            if (const auto why = refusal("OneLibrary", writer.playlistCountAtPath(m_playlist),
                                         writer.playlistCountAtPath(renamed))) {
                leftAlone.push_back(*why);
            } else if (writer.renamePlaylist(m_playlist, m_newName)) {
                ++done;
            }
        }
        if (m_libraries.contains(QStringLiteral("engine"))) {
            FormatWriteSession &session =
                sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), 1, RenameLabel);
            const std::string root = session.writeRoot();
            if (const auto why = refusal("Engine", infrastructure::engine::enginePlaylistCountAtPath(root, m_playlist),
                                         infrastructure::engine::enginePlaylistCountAtPath(root, renamed))) {
                leftAlone.push_back(*why);
            } else if (const int smart = infrastructure::engine::engineSmartlistsFiledUnder(root, m_playlist); smart > 0) {
                leftAlone.push_back("Engine has " + std::to_string(smart) + " smart playlists filed by its name");
            } else if (infrastructure::engine::renameEnginePlaylist(root, m_playlist, renamed)) {
                session.noteItemApplied();
                ++done;
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    for (const auto &why : leftAlone) {
        ctx.log().record("playlist-rename: not renamed in \"" + m_playlist + "\" there: " + why);
    }
    if (done == 0) {
        ctx.log().record("playlist-rename: \"" + m_playlist + "\" renamed nowhere, skipped");
        return ChangeOutcome::skip();
    }
    ctx.log().record("playlist-rename: renamed \"" + m_playlist + "\" to \"" + renamed + "\" in " + std::to_string(done)
                     + " libraries");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
