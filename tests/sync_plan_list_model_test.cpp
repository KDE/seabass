// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The Sync Cue Points list, on its own. Decisions come before the tracks
// ready to sync; the search narrows both sections without losing a tick;
// Select All and Select None reach only what the search shows; a staged
// track is not counted as waiting to be staged; and removing a track the
// search hides leaves every other row pointing at the right plan.

#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "gui/sync_plan_list_model.hpp"

using namespace seabass::domain;
using seabass::gui::SyncPlanListModel;

namespace
{

CuePoint hot(int pad, double positionMs)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Hot;
    c.hotCueNumber = pad;
    c.positionMs = positionMs;
    return c;
}

Track track(const std::string &format, const std::string &id, const std::string &title, const std::string &artist,
            std::vector<CuePoint> cues)
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.title = title;
    t.artist = artist;
    t.filename = title + ".mp3";
    t.cues = std::move(cues);
    return t;
}

SyncPlan plan(Track source, Track target)
{
    SyncPlan p;
    p.kind = SyncPlan::Kind::AOnly;
    p.direction = SyncPlan::Direction::ToB;
    p.cuesToApply = source.cues;
    p.match.trackA = std::move(source);
    p.match.trackB = std::move(target);
    return p;
}

CrossSourceSyncConflict choice(Track a, Track b)
{
    CrossSourceSyncConflict c;
    c.samePair = true;
    c.target = b;
    c.cuesFromA = a.cues;
    c.cuesFromB = b.cues;
    c.sourceA = std::move(a);
    c.sourceB = std::move(b);
    return c;
}

QVariant at(const SyncPlanListModel &model, int row, int role)
{
    return model.data(model.index(row), role);
}

SyncPlan rej()
{
    return plan(track("engine", "e1", "Rej", "Âme", {hot(1, 5000), hot(2, 20000), hot(3, 40000), hot(4, 60000)}),
                track("rekordbox", "r1", "Rej", "Âme", {hot(1, 5000)}));
}

SyncPlan loopInLoop()
{
    return plan(track("rekordbox", "r2", "Loop In Loop", "Sven Väth", {hot(1, 1000), hot(2, 2000), hot(3, 3000)}),
                track("engine", "e2", "Loop In Loop", "Sven Väth", {}));
}

SyncPlan flaschenpostEdit()
{
    return plan(track("rekordbox", "r3", "Flaschenpost Edit", "Kollektiv Turmstrasse", {hot(1, 1000)}),
                track("engine", "e3", "Flaschenpost Edit", "Kollektiv Turmstrasse", {}));
}

CrossSourceSyncConflict flaschenpost()
{
    return choice(track("rekordbox", "r4", "Flaschenpost", "Kollektiv Turmstrasse", {hot(1, 1000), hot(2, 2000)}),
                  track("engine", "e4", "Flaschenpost", "Kollektiv Turmstrasse", {hot(1, 1500)}));
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    SyncPlanListModel model;
    int countSignals = 0;
    QObject::connect(&model, &SyncPlanListModel::countsChanged, [&countSignals]() { countSignals++; });
    model.setAnalysis({rej(), loopInLoop(), flaschenpostEdit()}, {flaschenpost()});

    // Decisions first, then the ready tracks, each carrying its own index.
    {
        assert(model.rowCount() == 4);
        assert(at(model, 0, SyncPlanListModel::SectionRole).toString() == "decision");
        assert(at(model, 0, SyncPlanListModel::ConflictIndexRole).toInt() == 0);
        assert(at(model, 0, SyncPlanListModel::PlanIndexRole).toInt() == -1);
        assert(at(model, 0, SyncPlanListModel::IncludedRole).toBool() == false && "a decision has no tick");
        for (int row = 1; row < 4; ++row) {
            assert(at(model, row, SyncPlanListModel::SectionRole).toString() == "ready");
            assert(at(model, row, SyncPlanListModel::PlanIndexRole).toInt() == row - 1);
            assert(at(model, row, SyncPlanListModel::IncludedRole).toBool() && "ready tracks start ticked");
        }
        assert(model.selectedCount() == 3);
        assert(model.conflictCount() == 1);
        std::cout << "case 1 (decisions before ready tracks, all ready tracks ticked) OK\n";
    }

    // Each row counts its cues the way the page shows them, and lists its
    // source copy before its target.
    {
        assert(at(model, 1, SyncPlanListModel::CueSummaryRole).toString() == "+3 hot, keeps 1");
        assert(at(model, 2, SyncPlanListModel::CueSummaryRole).toString() == "+3 hot");
        assert(at(model, 0, SyncPlanListModel::CueSummaryRole).toString() == "2 hot vs 1 hot");
        const QVariantList tracks = at(model, 1, SyncPlanListModel::TracksRole).toList();
        assert(tracks.size() == 2);
        assert(tracks[0].toMap()["side"].toString() == "engine" && "source first");
        assert(tracks[1].toMap()["side"].toString() == "rekordbox");
        const QVariantMap change = at(model, 1, SyncPlanListModel::CueChangeRole).toMap();
        assert(change["gainedHot"].toInt() == 3 && change["keptHot"].toInt() == 1);
        std::cout << "case 2 (cue summaries: gained, kept, and a decision's two options) OK\n";
    }

    // The search narrows both sections, and Select None reaches only what
    // it shows. Ticks survive the search coming and going.
    {
        model.setIncluded(1, false);  // Loop In Loop, before searching
        model.setFilter("kollektiv");
        assert(model.rowCount() == 2);
        assert(at(model, 0, SyncPlanListModel::SectionRole).toString() == "decision");
        assert(at(model, 1, SyncPlanListModel::PlanIndexRole).toInt() == 2);
        assert(model.visibleConflictCount() == 1 && model.visiblePlanCount() == 1);
        assert(model.isPlanVisible(2) && !model.isPlanVisible(0));

        model.setAllIncluded(false);
        assert(!model.included(2));
        assert(model.included(0) && "Select None must not reach a track the search hides");
        assert(model.selectedVisibleCount() == 0);
        assert(model.selectedCount() == 1);

        model.setFilter("");
        assert(model.rowCount() == 4);
        assert(!model.included(1) && "a tick made before the search is still there after it");
        std::cout << "case 3 (search narrows both sections; selection stays within it) OK\n";
    }

    // A staged track is not waiting to be staged, however it is ticked --
    // and the count moves even when the search hides the track.
    {
        model.setFilter("loop");
        const int before = countSignals;
        model.setStaged(0, true, "Copy 4 hot");  // Rej, hidden by the search
        assert(countSignals > before && "a hidden staged mark still moves the counts");
        assert(model.selectedCount() == 0);
        model.setIncluded(2, true);
        assert(model.selectedCount() == 1);
        std::cout << "case 4 (staged tracks are not counted as selected) OK\n";
    }

    // Removing a plan the search hides keeps every row on the right plan.
    {
        assert(model.rowCount() == 1);
        model.removePlanAt(0);  // Rej, staged and hidden
        assert(model.rowCount() == 1);
        assert(at(model, 0, SyncPlanListModel::PlanIndexRole).toInt() == 0);
        assert(model.plans()[0].match.trackA.title == "Loop In Loop");
        assert(!model.isStaged(0) && !model.isStaged(1) && "the staged mark leaves with its plan");
        assert(!model.included(0) && model.included(1) && "ticks move with their plans");
        std::cout << "case 5 (removing a hidden plan re-points the remaining rows) OK\n";
    }

    // A decision made: it leaves, and the plan it became joins -- with a
    // row only if the search would show it.
    {
        model.setFilter("");
        assert(model.rowCount() == 3);
        model.removeConflictAt(0);
        assert(model.conflictCount() == 0 && model.rowCount() == 2);
        model.addPlan(flaschenpostEdit());
        assert(model.rowCount() == 3);
        assert(at(model, 2, SyncPlanListModel::PlanIndexRole).toInt() == 2);
        assert(model.included(2));

        model.setFilter("nothing matches this");
        assert(model.rowCount() == 0);
        model.addPlan(rej());
        assert(model.rowCount() == 0 && "a plan the search hides gets no row");
        model.setFilter("");
        assert(model.rowCount() == 4);
        std::cout << "case 6 (resolving a decision moves the track, respecting the search) OK\n";
    }

    // A hot cue moved to another pad is replaced, not kept.
    {
        SyncPlan moved = plan(track("engine", "e9", "Moved", "Pad", {hot(1, 5000)}),
                              track("rekordbox", "r9", "Moved", "Pad", {hot(2, 5000)}));
        assert(SyncPlanListModel::cueSummary(moved) == "+1 hot, replaces 1");
        std::cout << "case 7 (a moved pad reads as replaced) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
