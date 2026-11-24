// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/cue_offset_repair_change.hpp"

#include <cstdio>

namespace seabass::gui
{

using domain::CueOffsetFinding;
using domain::CueOffsetSide;
using domain::SyncPlan;

SyncPlan CueOffsetRepairChange::planFor(const CueOffsetFinding &finding, CueOffsetSide side,
                                        std::optional<CueOffsetSide> pick)
{
    SyncPlan plan;
    plan.kind = SyncPlan::Kind::AlreadyConsistent;
    plan.match.trackA = finding.rekordbox;
    plan.match.trackB = finding.engine;
    plan.direction = side == CueOffsetSide::Engine ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
    plan.cuesToApply = finding.cuesMoved(side, pick);
    return plan;
}

QString CueOffsetRepairChange::idFor(const CueOffsetFinding &finding, CueOffsetSide side)
{
    return QString::fromStdString("cue-offset:" + finding.rekordbox.format + ":" + finding.rekordbox.sourceId
                                  + "|engine:" + finding.engine.sourceId + ":"
                                  + (side == CueOffsetSide::Engine ? "engine" : "rekordbox"));
}

CueOffsetRepairChange::CueOffsetRepairChange(QString rekordboxPath, QString enginePath,
                                             const CueOffsetFinding &finding, CueOffsetSide side,
                                             std::optional<CueOffsetSide> pick, int itemCountHint)
    : SyncPlanChange(std::move(rekordboxPath), std::move(enginePath), planFor(finding, side, pick), itemCountHint),
      m_id(idFor(finding, side)),
      m_side(side),
      m_shiftMs(finding.shiftMs)
{
    for (size_t i = 0; i < finding.cues.size(); ++i) {
        if (finding.sideOf(i, pick) != side) {
            continue;
        }
        ++m_moved;
        const auto &offset = finding.cues[i];
        m_copied.push_back(side == CueOffsetSide::Engine ? finding.rekordbox.cues[offset.rekordboxIndex]
                                                         : finding.engine.cues[offset.engineIndex]);
    }
    setCheckTargetAsRead(true);
}

QString CueOffsetRepairChange::id() const
{
    return m_id;
}

QString CueOffsetRepairChange::description() const
{
    const QString side = m_side == CueOffsetSide::Engine
        ? QStringLiteral("Engine")
        : QString::fromStdString(domain::catalogDisplayName(target().format));
    const domain::Track &track = target();
    return QStringLiteral("Move %1 %2 on %3 \"%4\" to where the sound puts them")
        .arg(m_moved)
        .arg(m_moved == 1 ? QStringLiteral("cue") : QStringLiteral("cues"), side,
             QString::fromStdString(track.title.empty() ? track.filename : track.title));
}

QString CueOffsetRepairChange::verb() const
{
    return QStringLiteral("corrected");
}

std::string CueOffsetRepairChange::logLine() const
{
    const domain::Track &tgt = target();
    char shift[32];
    std::snprintf(shift, sizeof shift, "%.3f", m_shiftMs);
    return "cue-offset: moved " + std::to_string(m_moved) + " cue(s) on " + tgt.format + " track \"" + tgt.title
        + "\" (id=" + tgt.sourceId + ") onto the other side's, this file's shift " + shift + " ms";
}

}  // namespace seabass::gui
