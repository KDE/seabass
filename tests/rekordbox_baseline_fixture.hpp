// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// What the rekordbox baseline's save tests share, on top of
// engine_change_fixture.hpp: the stick's rekordbox tracks and their
// baseline keys, the baseline file planted and read back, and the
// changes the saves are made of.

#include <QString>

#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/rekordbox_baseline.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "engine_change_fixture.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace seabass::testing
{

// The fixture's export.pdb sequence, and Engine's import counter once
// levelled (levelImportCounter).
inline constexpr std::uint64_t FixtureSequence = 15132;
// Analysed in the fixture (isAnalyzed 1), so its sample rate is known.
inline constexpr std::int64_t EngineTrackId = 6;

inline std::string stickRootOf(const EngineChangeStick &stick)
{
    return infrastructure::backup::stickRootForCatalogPath(pathToUtf8(stick.pioneer));
}

inline std::filesystem::path baselineFileOf(const EngineChangeStick &stick)
{
    return infrastructure::paths::stickRekordboxBaseline(pathFromUtf8(stickRootOf(stick)));
}

// The first `count` rekordbox tracks with a file and a numeric id, as the
// reader gives them.
inline std::vector<domain::Track> rekordboxTracks(const EngineChangeStick &stick, std::size_t count)
{
    infrastructure::rekordbox::KaitaiRekordboxReader reader(pathToUtf8(stick.pioneer));
    std::vector<domain::Track> out;
    for (const auto &track : reader.readAll()) {
        if (!track.filePath.empty() && !track.analysisFile.empty() && !track.sourceId.empty()
            && track.sourceId.find_first_not_of("0123456789") == std::string::npos) {
            out.push_back(track);
            if (out.size() == count) {
                break;
            }
        }
    }
    assert(out.size() == count && "the fixture has the rekordbox tracks the test needs");
    return out;
}

inline std::string keyOf(const EngineChangeStick &stick, const domain::Track &track)
{
    const std::string key = gui::baselinePathKey(gui::baselineStickRelativePath(stickRootOf(stick), track.filePath));
    assert(!key.empty());
    return key;
}

// A baseline row for `track`, keyed as the save keys it, with one rating
// and one hot cue of the given origin.
inline domain::BaselineTrack baselineRow(const EngineChangeStick &stick, const domain::Track &track,
                                         std::optional<int> rating, domain::ValueOrigin origin, double cueAtMs)
{
    domain::BaselineTrack row;
    row.pathKey = keyOf(stick, track);
    row.stickRelativePath = gui::baselineStickRelativePath(stickRootOf(stick), track.filePath);
    row.pdbId = static_cast<std::uint32_t>(std::stoul(track.sourceId));
    row.analysisFile = track.analysisFile;
    row.rating = rating;
    row.ratingOrigin = origin;
    row.cues = {domain::BaselineCue{domain::CuePoint{domain::CuePoint::Kind::Hot, 2, cueAtMs, "", ""}, origin}};
    return row;
}

inline void plantBaseline(const EngineChangeStick &stick, const domain::RekordboxBaseline &baseline)
{
    std::string error;
    const bool written = infrastructure::local::writeRekordboxBaseline(
        pathFromUtf8(stickRootOf(stick)), baseline, [](const std::string &) {}, &error);
    if (!written) {
        std::cerr << "plant: " << error << "\n";
    }
    assert(written);
}

inline std::optional<domain::RekordboxBaseline> readBaseline(const EngineChangeStick &stick)
{
    std::string error;
    auto baseline = infrastructure::local::readRekordboxBaseline(pathFromUtf8(stickRootOf(stick)), &error);
    if (!error.empty()) {
        std::cerr << "read: " << error << "\n";
    }
    assert(error.empty());
    return baseline;
}

inline std::uint64_t pdbSequence(const EngineChangeStick &stick)
{
    const auto state = infrastructure::engine::readRekordboxImportState({}, pathToUtf8(stick.pioneer));
    assert(state.hasRekordboxLibrary);
    return state.librarySequence;
}

// One pad from a stand-in rekordbox track onto Engine track 6: a write on
// the Engine side alone.
inline std::unique_ptr<gui::PendingChange> syncOntoEngine(const EngineChangeStick &stick)
{
    domain::Track rekordbox;
    rekordbox.format = "rekordbox";
    rekordbox.sourceId = "1";
    rekordbox.title = "From rekordbox";
    domain::Track engine;
    engine.format = "engine";
    engine.sourceId = std::to_string(EngineTrackId);
    engine.title = "On Engine";
    domain::SyncPlan plan;
    plan.kind = domain::SyncPlan::Kind::AOnly;
    plan.match.trackA = rekordbox;
    plan.match.trackB = engine;
    plan.direction = domain::SyncPlan::Direction::ToB;
    plan.cuesToApply = {domain::CuePoint{domain::CuePoint::Kind::Hot, 1, 1000.0, "", "onto engine"}};
    return std::make_unique<gui::SyncPlanChange>(stick.pioneerPath(), stick.enginePath(), plan, 2);
}

// Engine's cues onto a rekordbox track: its analysis file and its
// OneLibrary row, the whole set being `cues`.
inline std::unique_ptr<gui::PendingChange> syncOntoRekordbox(const EngineChangeStick &stick,
                                                            const domain::Track &target,
                                                            std::vector<domain::CuePoint> cues)
{
    domain::Track engine;
    engine.format = "engine";
    engine.sourceId = std::to_string(EngineTrackId);
    engine.title = "From Engine";
    domain::SyncPlan plan;
    plan.kind = domain::SyncPlan::Kind::AOnly;
    plan.match.trackA = engine;
    plan.match.trackB = target;
    plan.direction = domain::SyncPlan::Direction::ToB;
    plan.cuesToApply = std::move(cues);
    return std::make_unique<gui::SyncPlanChange>(stick.pioneerPath(), stick.enginePath(), plan, 2);
}

// A change that fails, so the save stops there.
class FailingChange : public gui::PendingChange
{
public:
    explicit FailingChange(QString id) : m_id(std::move(id)) {}
    QString id() const override { return m_id; }
    QString description() const override { return QStringLiteral("fails on purpose"); }
    QString unit() const override { return QStringLiteral("tracks"); }
    QStringList formatsTouched() const override { return {}; }
    gui::ChangeOutcome apply(gui::SaveContext &) override
    {
        return gui::ChangeOutcome::failure(QStringLiteral("failed on purpose"));
    }

private:
    QString m_id;
};

}  // namespace seabass::testing
