// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/remove_dangling_playlist_entries_change.hpp"

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

RemoveDanglingPlaylistEntriesChange::RemoveDanglingPlaylistEntriesChange(QString enginePath, int entries)
    : m_enginePath(std::move(enginePath)), m_entries(entries)
{
}

QString RemoveDanglingPlaylistEntriesChange::idFor()
{
    return QStringLiteral("playlist-sync:engine-dangling");
}

QString RemoveDanglingPlaylistEntriesChange::id() const
{
    return idFor();
}

QString RemoveDanglingPlaylistEntriesChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RemoveDanglingPlaylistEntriesChange::description() const
{
    return QStringLiteral("Remove %1 Engine playlist %2 that point at no track")
        .arg(m_entries)
        .arg(m_entries == 1 ? QStringLiteral("entry") : QStringLiteral("entries"));
}

QString RemoveDanglingPlaylistEntriesChange::unit() const
{
    return QStringLiteral("entries");
}

QString RemoveDanglingPlaylistEntriesChange::verb() const
{
    return QStringLiteral("removed");
}

QStringList RemoveDanglingPlaylistEntriesChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RemoveDanglingPlaylistEntriesChange::filesToBackup(SaveContext &) const
{
    return {{pathToUtf8(pathFromUtf8(m_enginePath.toStdString()) / "Database2" / "m.db"), Label}};
}

ChangeOutcome RemoveDanglingPlaylistEntriesChange::apply(SaveContext &ctx)
{
    try {
        FormatWriteSession &session = sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), 1, Label);
        const int removed = infrastructure::engine::removeDanglingPlaylistEntries(
            pathToUtf8(pathFromUtf8(session.writeRoot()) / "Database2" / "m.db"));
        session.noteItemApplied();
        ctx.log().record(std::string(Label) + ": removed " + std::to_string(removed)
                         + " Engine playlist entr(ies) that pointed at no track");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
