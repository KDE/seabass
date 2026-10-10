// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/remove_engine_track_change.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"

namespace seabass::gui
{

RemoveEngineTrackChange::RemoveEngineTrackChange(QString enginePath, domain::Track engineRow, int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_row(std::move(engineRow)), m_itemCountHint(itemCountHint)
{
}

QString RemoveEngineTrackChange::idFor(const std::string &engineSourceId)
{
    return rekordboxExportSyncOwner() + QStringLiteral(":engine-remove:") + QString::fromStdString(engineSourceId);
}

QString RemoveEngineTrackChange::id() const
{
    return idFor(m_row.sourceId);
}

QString RemoveEngineTrackChange::owner() const
{
    return rekordboxExportSyncOwner();
}

QString RemoveEngineTrackChange::description() const
{
    return QStringLiteral("Remove \"%1\" from Engine").arg(subject());
}

QString RemoveEngineTrackChange::subject() const
{
    const QString title = QString::fromStdString(m_row.title.empty() ? m_row.filename : m_row.title);
    return m_row.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(m_row.artist);
}

QString RemoveEngineTrackChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RemoveEngineTrackChange::verb() const
{
    return QStringLiteral("removed");
}

QStringList RemoveEngineTrackChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> RemoveEngineTrackChange::filesToBackup(SaveContext &) const
{
    if (!engineRowWriteRefusal(m_enginePath).isEmpty()) {
        return {};
    }
    return {{FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()),
             rekordboxExportSyncOwner().toStdString()}};
}

ChangeOutcome RemoveEngineTrackChange::apply(SaveContext &ctx)
{
    if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
        return ChangeOutcome::failure(refusal);
    }
    const auto id = engineId(m_row.sourceId);
    if (!id) {
        return ChangeOutcome::failure(QStringLiteral("\"%1\" is not an Engine track id; not removing \"%2\".")
                                          .arg(QString::fromStdString(m_row.sourceId), subject()));
    }
    if (m_row.filePath.empty()) {
        return ChangeOutcome::failure(
            QStringLiteral("Engine track %1 names no file to confirm it by; not removing \"%2\".").arg(*id).arg(subject()));
    }
    const std::string label = rekordboxExportSyncOwner().toStdString();
    const QString file = QString::fromStdString(m_row.filePath);
    try {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, label);
        const std::string database = FormatWriteSession::databaseFileFor("engine", session.writeRoot());
        const auto rows =
            infrastructure::engine::engineTrackIdsForFile(session.writeRoot(), session.realRoot(), m_row.filePath);
        if (rows.empty()) {
            ctx.log().record(label + ": Engine has no row for " + m_row.filePath + " any more; nothing to remove");
            return ChangeOutcome::skip();
        }
        if (std::find(rows.begin(), rows.end(), *id) == rows.end()) {
            return ChangeOutcome::failure(
                QStringLiteral("Engine track %1 no longer names %2; the library changed since it was read. Nothing "
                               "was removed.")
                    .arg(*id)
                    .arg(file));
        }
        std::string error;
        const int removed = infrastructure::engine::removeEngineTrackRows(database, {*id}, &error);
        if (removed != 1) {
            return ChangeOutcome::failure(QStringLiteral("Engine did not remove track %1 (%2): %3")
                                              .arg(*id)
                                              .arg(file, QString::fromStdString(error)));
        }
        // What landed: no row of any table names the id now.
        const int left = infrastructure::engine::engineRowsNamingTrack(database, *id, &error);
        if (left != 0) {
            return ChangeOutcome::failure(
                QStringLiteral("%1 row(s) still name Engine track %2 after removing it%3")
                    .arg(left)
                    .arg(*id)
                    .arg(error.empty() ? QString() : QStringLiteral(": ") + QString::fromStdString(error)));
        }
        session.noteItemApplied();
        ctx.log().record(label + ": removed Engine track id=" + std::to_string(*id) + " \"" + m_row.title + "\" ("
                         + m_row.filePath + ") with its playlist entries and performance data; the file stays");
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
