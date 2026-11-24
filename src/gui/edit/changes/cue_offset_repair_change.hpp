// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <optional>

#include "domain/cue_offset_repair.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"

namespace seabass::gui
{

// Moves the cues of one track that are a few milliseconds off (#121) on
// one side, `side`, onto where the other side's are: the cues whose
// evidence names that side, and the unknown ones when the DJ's `pick`
// names it. The write is a Sync Cue Points write of that side's whole cue
// set with only those cues moved, so it goes through the same writers,
// backups and mirror (SyncPlanChange): Engine's row, or rekordbox's
// analysis file with its OneLibrary copy. It writes nothing unless the
// side still holds the cues the analysis read (setCheckTargetAsRead).
// A track whose cues move on both sides is two changes, one a side.
class CueOffsetRepairChange : public SyncPlanChange
{
public:
    CueOffsetRepairChange(QString rekordboxPath, QString enginePath, const domain::CueOffsetFinding &finding,
                          domain::CueOffsetSide side, std::optional<domain::CueOffsetSide> pick, int itemCountHint);

    // The plan the change writes: `side`'s cues, the ones moved there put
    // where the other side's are.
    static domain::SyncPlan planFor(const domain::CueOffsetFinding &finding, domain::CueOffsetSide side,
                                    std::optional<domain::CueOffsetSide> pick);
    // One id a track and side: "cue-offset:rekordbox:103|engine:92:engine".
    static QString idFor(const domain::CueOffsetFinding &finding, domain::CueOffsetSide side);

    QString id() const override;
    // Sync Cue Points': staged on that page beside its syncs, saved by its
    // Save. Owned by the id's own prefix it would be another editor, and
    // the session would refuse a sync and a repair staged together.
    QString owner() const override { return QStringLiteral("sync"); }
    QString description() const override;
    QString verb() const override;

protected:
    std::string logLine() const override;
    // Only the other side's cues it puts its moved ones onto: the rest of
    // that side may have moved since (the other half of the same repair).
    std::optional<std::vector<domain::CuePoint>> sourceCuesCopied() const override { return m_copied; }

private:
    std::vector<domain::CuePoint> m_copied;
    QString m_id;
    domain::CueOffsetSide m_side;
    int m_moved = 0;
    double m_shiftMs;
};

}  // namespace seabass::gui
