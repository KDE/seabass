// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>

#include <cstddef>
#include <vector>

#include "domain/cross_source_sync_conflict.hpp"
#include "domain/sync_planning.hpp"
#include "gui/staged_plan_model.hpp"

namespace seabass::gui
{

// The Sync Cue Points list: every track SyncController's last analysis
// found out of step across the stick's catalogs, in one list with two
// sections.
//
// First the tracks waiting for a decision (domain::CrossSourceSyncConflict:
// two sides with different hot cues, which no clock can settle), then the
// tracks ready to sync (SyncPlans with a direction). They used to be two
// designs on the page -- a Repeater of amber cards in the ListView's header
// above a list of plan rows -- which is how resolving a conflict came to
// move a track from one look to another. One model with a `section` role
// lets the page draw both with one delegate, and keeps the decisions
// virtualized like everything else.
//
// Selection and search work the way Clean Up's do (CleanupPlanListModel):
// every ready track starts ticked; the search is a filter over the rows
// already loaded, not a rescan, and never touches a tick; Select All and
// Select None reach only the rows the search shows. Decisions carry no
// tick at all -- they cannot be staged until someone picks a side.
//
// Two index spaces, kept apart on purpose. A *row* is what the ListView
// shows and moves with the search. A *plan index* or *conflict index* is
// a position in plans()/conflicts() and does not. Everything the page
// sends back (tick, stage, unstage, pick a side) uses the plan or conflict
// index its row carries in planIndex/conflictIndex, and StagedPlanModel's
// interface is plan-indexed too, so a search never changes what an action
// lands on.
class SyncPlanListModel : public QAbstractListModel, public StagedPlanModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Populated by SyncController; not constructible from QML")

public:
    enum Roles {
        // "decision" or "ready" -- ListView's section.property.
        SectionRole = Qt::UserRole + 1,
        NeedsDecisionRole,
        // Position in plans()/conflicts(); -1 on the other kind of row.
        PlanIndexRole,
        ConflictIndexRole,
        // A decision between one pair's own two sides (see
        // CrossSourceSyncConflict::samePair), rather than between two
        // catalogs each proposing cues for a third.
        SamePairRole,
        // A ready track: where its cues come from and go to. A decision:
        // empty source, and the catalog both options would be written to
        // when that is a third catalog (empty for a same-pair decision,
        // where the pick decides which side is written).
        SourceFormatRole,
        TargetFormatRole,
        FilenameRole,
        // Two track maps. A ready track: its source copy, then its target.
        // A decision: the two options, each carrying the cues picking it
        // would write.
        TracksRole,
        // One line for the row: "+3 hot, keeps 1" or "5 hot vs 4 hot".
        CueSummaryRole,
        // domain::CueChange as a map, for the sentence under a ready
        // track's waveforms. Empty on a decision.
        CueChangeRole,
        // Per option of a decision: whether it carries a memory cue at 0:00
        // that is usually an accident. Two falses on a ready track.
        JunkCuesRole,
        IncludedRole,
        // This plan is staged in the edit session: what Save will write.
        StagedRole,
        StagedDescriptionRole,
    };

    explicit SyncPlanListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // A fresh analysis: every ready track ticked, nothing staged. The
    // current search stays applied -- it is the page's, not the scan's.
    //
    // Waveforms are NOT carried here. They used to be precomputed for
    // every actionable track during the scan, which on a stick where most
    // of the library needs syncing meant thousands of reads against
    // removable media before anyone looked at a row -- the confirmed cause
    // of a real "scanning takes forever" report. QML fetches a waveform on
    // demand via PlaybackController::waveformFor(), so only rows actually
    // rendered pay for one.
    void setAnalysis(std::vector<domain::SyncPlan> plans, std::vector<domain::CrossSourceSyncConflict> conflicts);
    const std::vector<domain::SyncPlan> &plans() const { return m_plans; }
    const std::vector<domain::CrossSourceSyncConflict> &conflicts() const { return m_conflicts; }

    // A decision made: the pick becomes an ordinary plan, appended ticked,
    // and the decision leaves. See SyncController::resolveConflict().
    void addPlan(domain::SyncPlan plan);
    void removeConflictAt(int conflictIndex);

    // Case-insensitive substring match against title and artist of every
    // track a row involves. Empty shows everything.
    void setFilter(const QString &query);
    QString filter() const { return m_filter; }
    bool isPlanVisible(int planIndex) const;

    void setIncluded(int planIndex, bool included);
    // Only the ready tracks the search shows.
    void setAllIncluded(bool included);
    bool included(int planIndex) const;
    bool isStaged(int planIndex) const;

    int conflictCount() const { return static_cast<int>(m_conflicts.size()); }
    int visiblePlanCount() const;
    int visibleConflictCount() const;
    // Ticked and not staged yet -- exactly what Stage Selected would
    // stage, which is why a staged track does not count however it is
    // ticked.
    int selectedCount() const;
    int selectedVisibleCount() const;

    // StagedPlanModel, all plan-indexed.
    int planCount() const override { return static_cast<int>(m_plans.size()); }
    // A plan's identity across re-analyses: the track it would write to,
    // as "<format>:<sourceId>". At most one plan per target track ever
    // exists (SyncPlanner classifies each matched pair once), so this is
    // also the staged change's key.
    QString planKeyAt(int index) const override;
    void setStaged(int index, bool staged, const QString &description) override;
    void removePlanAt(int index) override;
    void clearStaged() override;

    static QString cueSummary(const domain::SyncPlan &plan);
    static QString choiceSummary(const domain::CrossSourceSyncConflict &conflict);

signals:
    // Any count above may have moved. Emitted even when no row did: a
    // staged mark or a tick on a track the search hides changes
    // selectedCount without a visible row to announce it through.
    void countsChanged();

private:
    struct Row
    {
        bool decision = false;
        std::size_t index = 0;
    };

    bool planMatches(const domain::SyncPlan &plan) const;
    bool conflictMatches(const domain::CrossSourceSyncConflict &conflict) const;
    void rebuildRows();
    int rowOfPlan(std::size_t planIndex) const;
    // Tells the view which rows now carry a lower index; see the .cpp.
    void announceShiftedIndexes(bool decisions, std::size_t removed);
    int rowOfConflict(std::size_t conflictIndex) const;

    std::vector<domain::SyncPlan> m_plans;
    std::vector<QString> m_stagedDescriptions;  // empty = not staged; parallel to m_plans
    std::vector<bool> m_included;               // parallel to m_plans
    std::vector<domain::CrossSourceSyncConflict> m_conflicts;
    QString m_filter;
    // What rowCount()/data() walk: decisions first, then ready tracks,
    // each only while it matches m_filter.
    std::vector<Row> m_rows;
};

}  // namespace seabass::gui
