// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/repair_legacy_memory_list_change.hpp"

#include "gui/edit/save_context.hpp"

namespace seabass::gui
{

RepairLegacyMemoryListChange::RepairLegacyMemoryListChange(domain::Track track,
                                                           infrastructure::rekordbox::LegacyMemoryListFinding finding)
    : m_track(std::move(track)), m_finding(std::move(finding))
{
}

QString RepairLegacyMemoryListChange::idFor(const std::string &rekordboxSourceId)
{
    return QStringLiteral("legacy-memory-list:rekordbox:") + QString::fromStdString(rekordboxSourceId);
}

QString RepairLegacyMemoryListChange::id() const
{
    return idFor(m_track.sourceId);
}

QString RepairLegacyMemoryListChange::owner() const
{
    return QStringLiteral("library-health");
}

QString RepairLegacyMemoryListChange::description() const
{
    const QString title = QString::fromStdString(m_track.title);
    if (m_finding.shape.repairable() && !m_finding.debris.empty()) {
        return QStringLiteral("Rebuild the memory cue list of \"%1\" and remove the analysis file a player left beside it")
            .arg(title);
    }
    if (m_finding.shape.repairable()) {
        return QStringLiteral("Rebuild the memory cue list of \"%1\"").arg(title);
    }
    return QStringLiteral("Remove the analysis file a player left beside \"%1\"").arg(title);
}

QString RepairLegacyMemoryListChange::subject() const
{
    const QString title = QString::fromStdString(m_track.title.empty() ? m_track.filename : m_track.title);
    return m_track.artist.empty() ? title : title + QStringLiteral(", ") + QString::fromStdString(m_track.artist);
}

QString RepairLegacyMemoryListChange::unit() const
{
    return QStringLiteral("tracks");
}

QString RepairLegacyMemoryListChange::verb() const
{
    return QStringLiteral("repaired");
}

QStringList RepairLegacyMemoryListChange::formatsTouched() const
{
    return {QStringLiteral("rekordbox")};
}

std::vector<BackupTarget> RepairLegacyMemoryListChange::filesToBackup(SaveContext &) const
{
    // The .DAT the list sits in, and every file the repair removes: Undo
    // puts the debris back too, so nothing a player wrote is gone for
    // good on the strength of this check alone. export.pdb is not
    // touched and not named.
    std::vector<BackupTarget> targets;
    if (m_finding.shape.repairable()) {
        targets.push_back({m_finding.datPath, "legacy-memory-list"});
    }
    for (const auto &debris : m_finding.debris) {
        targets.push_back({debris, "legacy-memory-list"});
    }
    return targets;
}

ChangeOutcome RepairLegacyMemoryListChange::apply(SaveContext &ctx)
{
    const std::string account = infrastructure::rekordbox::repairTrackAnalysis(m_finding);
    ctx.log().record("legacy-memory-list: rekordbox track id=" + m_track.sourceId + " (\"" + m_track.title + "\"): "
                     + account);
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
