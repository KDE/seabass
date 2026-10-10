// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/engine_membership_change.hpp"

#include <algorithm>
#include <cstdint>

#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/engine/engine_playlists.hpp"

namespace seabass::gui
{

EngineMembershipChange::EngineMembershipChange(QString enginePath, std::string playlistPath, std::vector<Add> adds,
                                               std::vector<Remove> removes, int itemCountHint)
    : m_enginePath(std::move(enginePath)), m_playlist(std::move(playlistPath)), m_adds(std::move(adds)),
      m_removes(std::move(removes)), m_itemCountHint(itemCountHint)
{
}

QString EngineMembershipChange::idFor(const std::string &playlistPath)
{
    return rekordboxExportSyncOwner() + QStringLiteral(":engine-membership:") + QString::fromStdString(playlistPath);
}

QString EngineMembershipChange::id() const
{
    return idFor(m_playlist);
}

QString EngineMembershipChange::owner() const
{
    return rekordboxExportSyncOwner();
}

QString EngineMembershipChange::description() const
{
    QStringList parts;
    if (!m_adds.empty()) {
        parts << (m_adds.size() == 1 ? QStringLiteral("1 track to add")
                                     : QStringLiteral("%1 tracks to add").arg(m_adds.size()));
    }
    if (!m_removes.empty()) {
        parts << (m_removes.size() == 1 ? QStringLiteral("1 track to take out")
                                        : QStringLiteral("%1 tracks to take out").arg(m_removes.size()));
    }
    return QStringLiteral("Update the playlist %1 in Engine (%2)")
        .arg(quotedPath(m_playlist), parts.isEmpty() ? QStringLiteral("nothing to change") : parts.join(QStringLiteral(", ")));
}

QString EngineMembershipChange::subject() const
{
    return QString::fromStdString(m_playlist);
}

QString EngineMembershipChange::unit() const
{
    return QStringLiteral("playlists");
}

QString EngineMembershipChange::verb() const
{
    return QStringLiteral("updated");
}

QStringList EngineMembershipChange::formatsTouched() const
{
    return {QStringLiteral("engine")};
}

std::vector<BackupTarget> EngineMembershipChange::filesToBackup(SaveContext &) const
{
    if (!engineRowWriteRefusal(m_enginePath).isEmpty()) {
        return {};
    }
    return {{FormatWriteSession::databaseFileFor("engine", m_enginePath.toStdString()),
             rekordboxExportSyncOwner().toStdString()}};
}

ChangeOutcome EngineMembershipChange::apply(SaveContext &ctx)
{
    m_added = 0;
    m_removed = 0;
    m_alreadyDone = 0;
    if (const QString refusal = engineRowWriteRefusal(m_enginePath); !refusal.isEmpty()) {
        return ChangeOutcome::failure(refusal);
    }
    const std::string label = rekordboxExportSyncOwner().toStdString();
    try {
        FormatWriteSession &session =
            sharedFormatWriteSession(ctx, "engine", m_enginePath.toStdString(), m_itemCountHint, label);
        const std::string root = session.writeRoot();
        const std::string &real = session.realRoot();
        const int playlists = infrastructure::engine::enginePlaylistCountAtPath(root, m_playlist);
        if (playlists == 0) {
            return ChangeOutcome::failure(
                QStringLiteral("Engine has no playlist %1 to update.").arg(quotedPath(m_playlist)));
        }
        if (playlists > 1) {
            return ChangeOutcome::failure(QStringLiteral("Engine has %1 playlists named %2; not guessing which to update.")
                                              .arg(playlists)
                                              .arg(quotedPath(m_playlist)));
        }
        // The one row of a file, nothing when it has none, a failure
        // message when it has more.
        QString ambiguity;
        const auto onlyRow = [&](const std::string &file) -> std::optional<std::int64_t> {
            const auto ids = infrastructure::engine::engineTrackIdsForFile(root, real, file);
            if (ids.size() > 1) {
                ambiguity = QStringLiteral("Engine has %1 rows for %2; Clean Up can merge them first.")
                                .arg(ids.size())
                                .arg(QString::fromStdString(file));
                return std::nullopt;
            }
            return ids.empty() ? std::nullopt : std::optional(ids.front());
        };

        for (const Remove &r : m_removes) {
            const auto id = engineId(r.engineSourceId);
            if (!id) {
                return ChangeOutcome::failure(QStringLiteral("\"%1\" is not an Engine track id.")
                                                  .arg(QString::fromStdString(r.engineSourceId)));
            }
            const auto rows = infrastructure::engine::engineTrackIdsForFile(root, real, r.filePath);
            if (rows.empty()) {
                ++m_alreadyDone;
                ctx.log().record(label + ": \"" + r.title + "\" has no Engine row any more; nothing to take out of \""
                                 + m_playlist + "\"");
                continue;
            }
            if (std::find(rows.begin(), rows.end(), *id) == rows.end()) {
                return ChangeOutcome::failure(
                    QStringLiteral("Engine track %1 no longer names %2; the library changed since it was read.")
                        .arg(*id)
                        .arg(QString::fromStdString(r.filePath)));
            }
            if (infrastructure::engine::removeFromEnginePlaylist(root, m_playlist, *id)) {
                ++m_removed;
            } else {
                ++m_alreadyDone;
            }
        }

        for (const Add &a : m_adds) {
            const auto id = onlyRow(a.filePath);
            if (!ambiguity.isEmpty()) {
                return ChangeOutcome::failure(ambiguity);
            }
            if (!id) {
                return ChangeOutcome::failure(
                    QStringLiteral("Engine has no row for %1 to put into %2; its track has to be added first.")
                        .arg(QString::fromStdString(a.filePath), quotedPath(m_playlist)));
            }
            bool added = false;
            if (!a.afterFilePath) {
                added = infrastructure::engine::insertAtStartOfEnginePlaylist(root, m_playlist, *id);
            } else {
                // A missing anchor (no row for its file, or a row not in
                // the playlist) appends: insertIntoEnginePlaylist does
                // the second itself.
                const auto anchor = onlyRow(*a.afterFilePath);
                if (!ambiguity.isEmpty()) {
                    return ChangeOutcome::failure(ambiguity);
                }
                added = infrastructure::engine::insertIntoEnginePlaylist(root, m_playlist, *id, anchor);
            }
            if (added) {
                ++m_added;
            } else {
                ++m_alreadyDone;
            }
        }
    } catch (const std::exception &e) {
        return ChangeOutcome::failure(QString::fromStdString(e.what()));
    }
    ctx.log().record(label + ": \"" + m_playlist + "\" in Engine: " + std::to_string(m_added) + " entr(ies) added, "
                     + std::to_string(m_removed) + " taken out, " + std::to_string(m_alreadyDone)
                     + " already as asked");
    if (m_added + m_removed == 0) {
        return ChangeOutcome::skip();
    }
    // Counted once the whole change has landed; a rolled-back one is not.
    if (auto *session = existingFormatWriteSession(ctx, "engine", m_enginePath.toStdString())) {
        session->noteItemApplied();
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
