// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/repoint_playlist_entries_change.hpp"

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{
constexpr const char *Label = "playlist-sync";
}

RepointPlaylistEntriesChange::RepointPlaylistEntriesChange(QString enginePath, int entries)
    : m_enginePath(std::move(enginePath)), m_entries(entries)
{
}

QString RepointPlaylistEntriesChange::idFor()
{
    return QStringLiteral("playlist-sync:engine-unresolved");
}

QString RepointPlaylistEntriesChange::id() const
{
    return idFor();
}

QString RepointPlaylistEntriesChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RepointPlaylistEntriesChange::description() const
{
    return QStringLiteral("Make %1 Engine playlist %2 the player cannot see visible again")
        .arg(m_entries)
        .arg(m_entries == 1 ? QStringLiteral("entry") : QStringLiteral("entries"));
}

QString RepointPlaylistEntriesChange::unit() const
{
    return QStringLiteral("entries");
}

QString RepointPlaylistEntriesChange::verb() const
{
    return QStringLiteral("repaired");
}

QStringList RepointPlaylistEntriesChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RepointPlaylistEntriesChange::filesToBackup(SaveContext &) const
{
    return {{pathToUtf8(pathFromUtf8(m_enginePath.toStdString()) / "Database2" / "m.db"), Label}};
}

ChangeOutcome RepointPlaylistEntriesChange::apply(SaveContext &ctx)
{
    try {
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), 1, Label);
        const auto done = infrastructure::engine::repointUnresolvedPlaylistEntries(session.writeRoot());
        if (done.repointed + done.removed > 0) {
            session.noteItemApplied();
        }
        ctx.log().record(std::string(Label) + ": repointed " + std::to_string(done.repointed)
                         + " Engine playlist entr(ies) the player could not see to their track's origin uuid, removed "
                         + std::to_string(done.removed) + " unseen second cop(ies), left "
                         + std::to_string(done.leftAlone) + " alone (no uuid would make them resolve)");
        if (done.repointed + done.removed == 0) {
            return ChangeOutcome::skip();
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
