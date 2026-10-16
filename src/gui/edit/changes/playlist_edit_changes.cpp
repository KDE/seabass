// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/playlist_edit_changes.hpp"

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

std::string exportPdbOf(const std::string &pioneerRoot)
{
    return pathToUtf8(pathFromUtf8(pioneerRoot) / "rekordbox" / "export.pdb");
}

std::string engineDbOf(const std::string &enginePath)
{
    return pathToUtf8(pathFromUtf8(enginePath) / "Database2" / "m.db");
}

// export.pdb's playlists (and folders) spelling a full path, by walking
// its tree. Two when a folder holds two of one name, or a name holds a
// "/" that reads as a folder.
std::vector<uint32_t> pdbPlaylistIds(const infrastructure::rekordbox::PdbRowWriter &rows, const std::string &path)
{
    const auto tree = rows.playlistTree();
    std::map<uint32_t, const infrastructure::rekordbox::PdbRowWriter::PlaylistTreeNode *> byId;
    for (const auto &n : tree) {
        byId[n.id] = &n;
    }
    std::vector<uint32_t> ids;
    for (const auto &n : tree) {
        std::string spelled = n.name;
        int guard = 0;
        for (uint32_t p = n.parentId; p != 0 && byId.count(p) && guard < 64; p = byId.at(p)->parentId, ++guard) {
            spelled = byId.at(p)->name + "/" + spelled;
        }
        if (spelled == path) {
            ids.push_back(n.id);
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

bool oneLibraryHas(const std::string &pioneer, const std::string &path)
{
    if (!infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        return false;
    }
    try {
        infrastructure::onelibrary::OneLibraryCueWriter w(pioneer);
        return w.playlistCountAtPath(path) > 0;
    } catch (const std::exception &) {
        return false;
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

}  // namespace

QStringList librariesWithPlaylist(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist)
{
    QStringList libraries;
    const std::string pioneer = pioneerRoot.toStdString();
    if (!pioneer.empty() && std::filesystem::exists(pathFromUtf8(exportPdbOf(pioneer)))) {
        try {
            const infrastructure::rekordbox::PdbRowWriter rows(exportPdbOf(pioneer));
            if (!pdbPlaylistIds(rows, playlist).empty()) {
                libraries << QStringLiteral("rekordbox");
            }
        } catch (const std::exception &) {
        }
    }
    if (!pioneer.empty() && oneLibraryHas(pioneer, playlist)) {
        libraries << QStringLiteral("onelibrary");
    }
    const std::string engine = enginePath.toStdString();
    if (!engine.empty() && std::filesystem::exists(pathFromUtf8(engineDbOf(engine)))
        && infrastructure::engine::enginePlaylistCountAtPath(engine, playlist) > 0) {
        libraries << QStringLiteral("engine");
    }
    return libraries;
}

QStringList librariesWithSeveralPlaylists(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist)
{
    QStringList libraries;
    const std::string pioneer = pioneerRoot.toStdString();
    try {
        if (!pioneer.empty() && std::filesystem::exists(pathFromUtf8(exportPdbOf(pioneer)))
            && pdbPlaylistIds(infrastructure::rekordbox::PdbRowWriter(exportPdbOf(pioneer)), playlist).size() > 1) {
            libraries << QStringLiteral("rekordbox");
        }
        if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)
            && infrastructure::onelibrary::OneLibraryCueWriter(pioneer).playlistCountAtPath(playlist) > 1) {
            libraries << QStringLiteral("onelibrary");
        }
        const std::string engine = enginePath.toStdString();
        if (!engine.empty() && std::filesystem::exists(pathFromUtf8(engineDbOf(engine)))
            && infrastructure::engine::enginePlaylistCountAtPath(engine, playlist) > 1) {
            libraries << QStringLiteral("engine");
        }
    } catch (const std::exception &) {
    }
    return libraries;
}

QStringList librariesOnStick(const QString &pioneerRoot, const QString &enginePath)
{
    QStringList libraries;
    const std::string pioneer = pioneerRoot.toStdString();
    if (!pioneer.empty() && std::filesystem::exists(pathFromUtf8(exportPdbOf(pioneer)))) {
        libraries << QStringLiteral("rekordbox");
    }
    if (!pioneer.empty() && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneer)) {
        libraries << QStringLiteral("onelibrary");
    }
    const std::string engine = enginePath.toStdString();
    if (!engine.empty() && std::filesystem::exists(pathFromUtf8(engineDbOf(engine)))) {
        libraries << QStringLiteral("engine");
    }
    return libraries;
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

}  // namespace seabass::gui
