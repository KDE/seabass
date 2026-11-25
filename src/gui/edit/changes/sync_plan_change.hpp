// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <string>

#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One sync plan: the source track's cues copied onto the matched track in
// the other catalog. The plan's direction decides which of the matched
// pair is the source and which the target.
//
// The analysis rider. With "Copy beat grids/waveforms from rekordbox to
// Engine" on in Preferences, a plan whose cues go from rekordbox (either
// format) to Engine carries a SyncAnalysisChange for its Engine row
// (setAnalysisRider()), inside this change rather than beside it: the page
// stages, unstages and replaces one change per row, and the session lists
// one, so nothing can take the cues and leave the analysis or the other
// way round, and a cancel between changes can never split them. The
// rider answers to this change's id (SyncAnalysisChange::rideIn()), runs
// after the cues in the same apply() and the same protection, declares its
// files with this change's, and says what it did in the save's summary
// note; this change's own count, description and verb stay what they
// were. A failure while writing the analysis fails this change, and so the
// save, as any write does.
class SyncPlanChange : public PendingChange
{
public:
    SyncPlanChange(QString rekordboxPath, QString enginePath, domain::SyncPlan plan, int itemCountHint);
    ~SyncPlanChange() override;

    // With this on, apply() first reads the target's cues as the catalog
    // holds them now and writes nothing (a skip, said in the summary)
    // unless they are still the cues the plan read
    // (domain::sameCuesAsRead): another save since the read, a sync or a
    // move of cues a few milliseconds off, would otherwise be written over
    // with what this plan read before it. Sync Cue Points' own plans and
    // its cue offset repairs set it.
    void setCheckTargetAsRead(bool check) { m_checkTargetAsRead = check; }

    // Carries `rider` (see above). Null takes it away.
    void setAnalysisRider(std::unique_ptr<PendingChange> rider);
    bool hasAnalysisRider() const { return m_rider != nullptr; }

    const domain::Track &target() const;
    const domain::Track &source() const;

    QString id() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_rekordboxPath, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    // The plan's cues, when the target is rekordbox or OneLibrary.
    std::vector<RekordboxWrite> rekordboxWrites() const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

protected:
    const domain::SyncPlan &plan() const { return m_plan; }
    // The stick log's line for the write apply() made.
    virtual std::string logLine() const;
    // The source's cues this change copies, as read, when it copies only
    // some: then only they must still be there (setCheckTargetAsRead()).
    // Nothing: it copies the source's whole set, which must be unchanged.
    virtual std::optional<std::vector<domain::CuePoint>> sourceCuesCopied() const { return std::nullopt; }

private:
    std::unique_ptr<PendingChange> m_rider;
    QString m_rekordboxPath;
    QString m_enginePath;
    domain::SyncPlan m_plan;
    int m_itemCountHint;
    bool m_checkTargetAsRead = false;
};

}  // namespace seabass::gui
