// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/engine_playlist_changes.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"

namespace seabass::gui
{

namespace
{

std::vector<BackupTarget> engineDatabaseTarget(const QString &enginePath)
{
    if (!engineRowWriteRefusal(enginePath).isEmpty()) {
        return {};
    }
    return {{FormatWriteSession::databaseFileFor("engine", enginePath.toStdString()),
             rekordboxExportSyncOwner().toStdString()}};
}

QString quotedPath(const std::string &path)
{
    return QStringLiteral("\"%1\"").arg(QString::fromStdString(path));
}

}  // namespace

CreateEnginePlaylistChange::CreateEnginePlaylistChange(QString enginePath, std::string path, int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_path(std::move(path)), m_itemCountHint(itemCountHint)
{
}

QString CreateEnginePlaylistChange::idFor(const std::string &path)
{
    return rekordboxExportSyncOwner() + QStringLiteral(":engine-playlist-create:") + QString::fromStdString(path);
}

QString CreateEnginePlaylistChange::id() const
{
    return idFor(m_path);
}

QString CreateEnginePlaylistChange::owner() const
{
    return rekordboxExportSyncOwner();
}

QString CreateEnginePlaylistChange::description() const
{
    return QStringLiteral("Create the playlist %1 in Engine").arg(quotedPath(m_path));
}

QString CreateEnginePlaylistChange::subject() const
{
    return QString::fromStdString(m_path);
}

QString CreateEnginePlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString CreateEnginePlaylistChange::verb() const
{
    return QStringLiteral("created");
}

QStringList CreateEnginePlaylistChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> CreateEnginePlaylistChange::filesToBackup(SaveContext &) const
{
    return engineDatabaseTarget(m_enginePath);
}

ChangeOutcome CreateEnginePlaylistChange::apply(SaveContext &ctx)
{
    if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
        return ChangeOutcome::failure(refusal);
    }
    const std::string label = rekordboxExportSyncOwner().toStdString();
    try {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, label);
        const std::string root = session.writeRoot();
        const int existing = infrastructure::engine::enginePlaylistCountAtPath(root, m_path);
        if (existing == 1) {
            ctx.log().record(label + ": Engine has the playlist \"" + m_path + "\" already; nothing to create");
            return ChangeOutcome::skip();
        }
        if (existing > 1) {
            return ChangeOutcome::failure(QStringLiteral("Engine has %1 playlists named %2; not creating another.")
                                              .arg(existing)
                                              .arg(quotedPath(m_path)));
        }
        const std::int64_t id = infrastructure::engine::createEnginePlaylist(root, m_path);
        if (infrastructure::engine::enginePlaylistCountAtPath(root, m_path) != 1) {
            return ChangeOutcome::failure(
                QStringLiteral("Engine does not name exactly one playlist %1 after creating it.").arg(quotedPath(m_path)));
        }
        session.noteItemApplied();
        ctx.log().record(label + ": created the Engine playlist \"" + m_path + "\" (id=" + std::to_string(id) + ")");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

RenameEnginePlaylistChange::RenameEnginePlaylistChange(QString enginePath, std::string from, std::string to,
                                                       int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_from(std::move(from)), m_to(std::move(to)), m_itemCountHint(itemCountHint)
{
}

QString RenameEnginePlaylistChange::idFor(const std::string &from)
{
    return rekordboxExportSyncOwner() + QStringLiteral(":engine-playlist-rename:") + QString::fromStdString(from);
}

QString RenameEnginePlaylistChange::id() const
{
    return idFor(m_from);
}

QString RenameEnginePlaylistChange::owner() const
{
    return rekordboxExportSyncOwner();
}

QString RenameEnginePlaylistChange::description() const
{
    return QStringLiteral("Rename the playlist %1 to %2 in Engine").arg(quotedPath(m_from), quotedPath(m_to));
}

QString RenameEnginePlaylistChange::subject() const
{
    return QString::fromStdString(m_from);
}

QString RenameEnginePlaylistChange::unit() const
{
    return QStringLiteral("playlists");
}

QString RenameEnginePlaylistChange::verb() const
{
    return QStringLiteral("renamed");
}

QStringList RenameEnginePlaylistChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RenameEnginePlaylistChange::filesToBackup(SaveContext &) const
{
    return engineDatabaseTarget(m_enginePath);
}

ChangeOutcome RenameEnginePlaylistChange::apply(SaveContext &ctx)
{
    if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
        return ChangeOutcome::failure(refusal);
    }
    const std::string label = rekordboxExportSyncOwner().toStdString();
    try {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, label);
        const std::string root = session.writeRoot();
        if (infrastructure::engine::enginePlaylistCountAtPath(root, m_from) == 0
            && infrastructure::engine::enginePlaylistCountAtPath(root, m_to) == 1) {
            ctx.log().record(label + ": Engine has \"" + m_to + "\" and no \"" + m_from + "\"; already renamed");
            return ChangeOutcome::skip();
        }
        if (!infrastructure::engine::renameEnginePlaylist(root, m_from, m_to)) {
            ctx.log().record(label + ": \"" + m_from + "\" is its own new name; nothing to rename");
            return ChangeOutcome::skip();
        }
        if (infrastructure::engine::enginePlaylistCountAtPath(root, m_from) != 0
            || infrastructure::engine::enginePlaylistCountAtPath(root, m_to) != 1) {
            return ChangeOutcome::failure(QStringLiteral("Engine does not read %1 as renamed to %2 after renaming it.")
                                              .arg(quotedPath(m_from), quotedPath(m_to)));
        }
        session.noteItemApplied();
        ctx.log().record(label + ": renamed the Engine playlist \"" + m_from + "\" to \"" + m_to + "\"");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

std::unique_ptr<PendingChange> deleteEnginePlaylistChange(QString enginePath, std::string path)
{
    return ownedByRekordboxExportSync(std::make_unique<DeletePlaylistChange>(
        QString(), std::move(enginePath), std::move(path), QStringList{QStringLiteral("engine")}));
}

}  // namespace seabass::gui
