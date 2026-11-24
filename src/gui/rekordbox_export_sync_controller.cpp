// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/rekordbox_export_sync_controller.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "application/phased_progress.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "application/use_cases/copy_beat_grids.hpp"
#include "application/use_cases/plan_engine_update.hpp"
#include "domain/engine_update_planning.hpp"
#include "domain/metadata_restore.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "domain/track_matching.hpp"
#include "gui/edit/changes/add_engine_track_change.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/engine_membership_change.hpp"
#include "gui/edit/changes/engine_playlist_changes.hpp"
#include "gui/edit/changes/owned_change.hpp"
#include "gui/edit/changes/record_rekordbox_baseline_change.hpp"
#include "gui/edit/changes/remove_engine_track_change.hpp"
#include "gui/edit/changes/restore_metadata_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/engine_update_stick_facts.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

// What one analysis read and decided, built on the worker with no access
// to the controller, and kept for stageSelected(): the baseline a save
// records is computed from the same reads as the proposal it answers.
struct RekordboxExportSyncAnalysis
{
    domain::EngineUpdateProposal proposal;
    // As read, for domain::nextBaseline.
    std::vector<domain::Track> rekordbox;
    std::vector<domain::PlaylistInfo> rekordboxPlaylists;
    std::optional<domain::RekordboxBaseline> baseline;
    std::uint64_t currentSequence = 0;
    std::string stickRoot;
    // A track by its baseline pathKey, to the file Engine finds it by: the
    // file of the Engine row the planner paired with it, when there is
    // one, else rekordbox's. A membership names its track and its "after"
    // anchor by key and by rekordbox's row, and EngineMembershipChange
    // finds the Engine row by file; a pair matched by name (a row whose
    // file is missing, OneStick) has two different files, and only
    // Engine's names its row there. A track added in the same save gets
    // rekordbox's file, which is the one AddEngineTrackChange writes.
    std::map<std::string, std::string> engineFileByKey;
    // The grids and waveforms the save's rider leaves to an answer (#136),
    // by rekordbox track id: the ones the record says were answered with
    // Engine's side, unchanged since (not asked), and the ones asked again
    // over a write the rider would otherwise make.
    std::map<std::string, std::set<domain::AnalysisEdit::Part>> rememberedAnalysisAnswers;
    // Tracks whose analysis could not be compared, rekordbox track id to
    // title: said on the page, and left alone by the save.
    std::map<std::string, std::string> uncomparedAnalysis;
    // Tracks whose analysis differs but whose rekordbox answer could not be
    // carried out (Engine's own analysis cannot be kept first), rekordbox
    // track id to title: not asked, said on the page, left alone.
    std::map<std::string, std::string> unkeepableAnalysis;
    QString errorMessage;
    bool cancelled = false;
};

namespace
{

using domain::CueEdit;
using domain::EngineUpdateEdit;
using domain::MembershipEdit;
using domain::MetadataEdit;
using domain::PlaylistCreate;
using domain::PlaylistDelete;
using domain::PlaylistRename;
using domain::SyncPlan;
using domain::TrackToAdd;
using domain::TrackToRemove;
using Row = RekordboxExportSyncListModel::Row;
using Section = RekordboxExportSyncListModel::Section;

// A change this page stages with something of its own said about it: an
// id of its own (the chosen cues of a conflict, staged after the track's
// other cue write, which has the same id), a description (a rating sent
// to Engine is not "put back"), the unit and verb the save's summary
// is told in, or the units a cancel does not split
// (RekordboxExportSyncController::unitKeysOf()). Everything else
// is the wrapped change's, as OwnedChange has it. A change this page
// already owns keeps its id unless one is given:
// wrapping it a second time must not rename it.
class PageChange final : public OwnedChange
{
public:
    struct Said
    {
        QString id;
        QString description;
        QString unit;
        QString verb;
        QStringList unitKeys;
    };

    PageChange(std::unique_ptr<PendingChange> inner, Said said)
        : OwnedChange(rekordboxExportSyncOwner(), std::move(inner)), m_said(std::move(said))
    {
    }

    QString id() const override
    {
        if (!m_said.id.isEmpty()) {
            return m_said.id;
        }
        return inner().owner() == rekordboxExportSyncOwner() ? inner().id() : OwnedChange::id();
    }
    QString description() const override
    {
        return m_said.description.isEmpty() ? OwnedChange::description() : m_said.description;
    }
    QString unit() const override { return m_said.unit.isEmpty() ? OwnedChange::unit() : m_said.unit; }
    QString verb() const override { return m_said.verb.isEmpty() ? OwnedChange::verb() : m_said.verb; }
    QStringList unitKeys() const override
    {
        if (!m_said.unitKeys.isEmpty()) {
            return m_said.unitKeys;
        }
        return inner().owner() == rekordboxExportSyncOwner() ? inner().unitKeys() : OwnedChange::unitKeys();
    }

private:
    Said m_said;
};


QString q(const std::string &text)
{
    return QString::fromStdString(text);
}

// Whether a write onto rekordbox puts back what Seabass wrote there (the
// export dropped it) rather than Engine's value over rekordbox's. The
// planner says which.
bool restoresSeabassWrite(const domain::MetadataEdit &edit)
{
    return edit.restoresSeabassWrite;
}

QString titleOf(const domain::Track &track)
{
    return q(track.title.empty() ? track.filename : track.title);
}

int depthOf(const std::string &path)
{
    return static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

std::shared_ptr<application::TrackMetadataProbe> metadataProbe()
{
#ifdef SEABASS_HAVE_TAGLIB
    return std::make_shared<infrastructure::audio::TagLibMetadataProbe>();
#else
    return std::make_shared<application::NullTrackMetadataProbe>();
#endif
}

// #136, with "Copy beat grids/waveforms from rekordbox to Engine when
// syncing" on: the tracks whose analysis a save of this proposal would copy
// (an Engine row the plan writes a rating, a comment or cues to, a
// conflict's choice included; a track the save adds has no analysis of
// Engine's own) are previewed through the writer with nothing written, as
// SyncAnalysisChange will write them, under differingIsConflict. A part
// both sides hold, and that differs, is a conflict of its own: rekordbox's
// side puts rekordbox's part over Engine's, Engine's side writes nothing.
// Until it is answered, the save's rider leaves that part alone. An empty
// side is never one: rekordbox without a grid or a waveform leaves Engine's
// as it is, and Engine without one (never analysed, or marked analysed with
// nothing stored) gets rekordbox's, or is left alone, as the rider decides.
// A track whose preview fails raises nothing: the rider then reads it again
// and leaves it alone if it still cannot. Runs on the analysis' worker.
//
// Answered with Engine's side (both left as they are), a conflict is
// recorded in the record's declined items under its key, with both sides'
// part as they were then (header.rekordboxState,
// infrastructure::local::analysisConflictState): rekordbox's grid or
// waveform, and Engine's as the player reads it. It is not raised again
// while that is the same; a change on either side raises it again. The
// parts so remembered come back in `remembered` (rekordbox track id to
// parts), for the stage: the save's rider leaves them out, as it leaves
// out a part answered now.
//
// A row whose flag is down over the player's own waveform takes
// rekordbox's only as a completion (the writer raises the difference only
// where one is possible): the answer then writes rekordbox's whole
// analysis (AnalysisEdit::completesRow).
using RememberedAnalysisAnswers = std::map<std::string, std::set<domain::AnalysisEdit::Part>>;

void raiseAnalysisConflicts(domain::EngineUpdateProposal &proposal, const QString &rekordboxPath,
                            const QString &enginePath, const SyncAnalysisStaging &staging,
                            const std::optional<domain::RekordboxBaseline> &baseline,
                            RememberedAnalysisAnswers &remembered, std::map<std::string, std::string> &uncompared,
                            std::map<std::string, std::string> &unkeepable,
                            const application::CancellationToken &cancel)
{
    if (!staging.any()) {
        return;
    }
    struct Pair
    {
        domain::Track rekordbox;
        domain::Track engine;
        std::string pathKey;
    };
    std::vector<Pair> pairs;
    std::set<std::string> seen;
    std::map<std::string, int> perEngineRow;
    const auto add = [&](const domain::Track &rekordbox, const domain::Track &engine, const std::string &pathKey) {
        if (rekordbox.sourceId.empty() || engine.sourceId.empty() || rekordbox.filePath.empty() || pathKey.empty()
            || !seen.insert(rekordbox.sourceId).second) {
            return;
        }
        ++perEngineRow[engine.sourceId];
        pairs.push_back({rekordbox, engine, pathKey});
    };
    const auto addEdit = [&](const EngineUpdateEdit &edit) {
        if (const auto *m = std::get_if<MetadataEdit>(&edit); m && m->direction == MetadataEdit::Direction::ToEngine) {
            add(m->rekordbox, m->engine, m->pathKey);
        } else if (const auto *c = std::get_if<CueEdit>(&edit); c && c->plan.direction == SyncPlan::Direction::ToB) {
            add(c->plan.match.trackA, c->plan.match.trackB, c->pathKey);
        }
    };
    for (const auto &e : proposal.metadataToEngine) {
        addEdit(e);
    }
    for (const auto &e : proposal.cuesToEngine) {
        addEdit(e);
    }
    for (const auto &conflict : proposal.conflicts) {
        for (const auto &e : conflict.rekordboxChoice) {
            addEdit(e);
        }
    }
    // Two rekordbox tracks for one Engine row: the rider leaves the row
    // alone (tracksForAnalysis), so there is nothing to ask.
    std::erase_if(pairs, [&](const Pair &p) { return perEngineRow[p.engine.sourceId] > 1; });
    if (pairs.empty()) {
        return;
    }

    const std::string pioneer = rekordboxPath.toStdString();
    const std::string &stickRoot = staging.stickRoot;
    std::string ledgerError;
    const auto ledger = infrastructure::local::readBeatGridLedger(pathFromUtf8(stickRoot), &ledgerError);
    std::unique_ptr<infrastructure::rekordbox::AnlzPathIndex> index;
    try {
        index = std::make_unique<infrastructure::rekordbox::AnlzPathIndex>(pioneer);
    } catch (const std::exception &) {
        // Each track then finds its analysis file the slow way.
    }
    infrastructure::engine::LibdjinteropEngineCueWriter writer(enginePath.toStdString());
    // Read only: a preview finds what is kept and never writes it.
    infrastructure::local::StickKeptEngineAnalysisStore kept(pathFromUtf8(stickRoot));
    writer.setKeptAnalysisStore(&kept);
    const auto probe = SyncAnalysisStaging::metadataProbe();
    infrastructure::local::CachedSampleRates rates(stickRoot, *probe);
    writer.setLeadInSource([&rates](const std::string &file) { return rates.leadInOf(file); });
    const application::AnalysisFileFactsSource facts = [&rates](const application::BeatGridCopyItem &item) {
        return infrastructure::local::analysisFileFactsOf(item, rates);
    };
    application::AnalysisCopyOptions options = staging.options;
    options.previewOnly = true;
    options.overwriteEngineEdits = false;
    options.overwritePlayerWaveforms = false;
    options.differingIsConflict = true;
    options.skipOverviewFile = false;
    options.zeroLoudness = false;

    using Outcome = application::BeatGridWriteResult::Outcome;
    using Waveform = application::AnalysisImportResult::Waveform;
    using Part = domain::AnalysisEdit::Part;
    for (const Pair &pair : pairs) {
        cancel.throwIfCancelled();
        const application::BeatGridCopyItem item =
            application::beatGridCopyItemFor(pair.rekordbox, pair.engine.sourceId, stickRoot);
        application::RekordboxAnalysis analysis;
        application::AnalysisItemOutcome outcome;
        std::optional<domain::TrackAnalysis> engineNow;
        // An answer the record holds for a part of this track: a write the
        // rider would now make there without a difference (a grid of the
        // ledger's unlocked since) is no longer what was answered, and is
        // asked again rather than written.
        const auto answeredBefore = [&](Part part) {
            return baseline && baseline->declined.count(domain::analysisItemKey(part, pair.pathKey)) > 0;
        };
        const auto asked = [&](Part part) {
            const bool grid = part == Part::Grid;
            if (grid ? outcome.result.grid.outcome == Outcome::DiffersFromEngine
                     : outcome.result.waveform == Waveform::DiffersFromEngine) {
                return true;
            }
            return answeredBefore(part)
                && (grid ? outcome.result.grid.outcome == Outcome::Written
                         : outcome.result.waveform == Waveform::Written);
        };
        try {
            analysis = infrastructure::local::rekordboxAnalysisOf(pioneer, item, index.get(), options.grids,
                                                                   options.waveforms);
            outcome = application::importAnalysisItem(item, analysis, writer, ledger, options, facts);
            if (asked(Part::Grid) || asked(Part::Waveform)) {
                engineNow = infrastructure::engine::readTrackAnalysis(enginePath.toStdString(), pair.engine.sourceId);
            }
        } catch (const std::exception &e) {
            // This track alone, named on the page; the save leaves its
            // analysis alone (the rider skips it), so nothing is said to
            // differ that was not compared.
            qWarning("Sync after Rekordbox Export: the analysis of \"%s\" (Engine track %s) was not compared: %s",
                     pair.rekordbox.title.c_str(), item.engineTrackId.c_str(), e.what());
            uncompared[pair.rekordbox.sourceId] =
                pair.rekordbox.title.empty() ? pair.rekordbox.filename : pair.rekordbox.title;
            continue;
        }
        for (const Part part : {Part::Grid, Part::Waveform}) {
            const bool grid = part == Part::Grid;
            if (!asked(part)) {
                continue;
            }
            // Asked again over a write the rider would make: the rider
            // leaves the part to the conflict.
            if (grid ? outcome.result.grid.outcome == Outcome::Written
                     : outcome.result.waveform == Waveform::Written) {
                remembered[pair.rekordbox.sourceId].insert(part);
            }
            // A flag down over the player's waveform: rekordbox's goes in
            // only with the rest of its analysis, a completion.
            const bool completesRow =
                !grid && (outcome.result.grid.outcome == Outcome::RowNotAnalysed || outcome.result.completedRow);
            // The digest of what the answer reads, as SyncAnalysisChange
            // reads it: this part alone, or both for a completion.
            application::RekordboxAnalysis shown = analysis;
            if (!grid && !completesRow) {
                shown.beats.clear();
            } else if (grid) {
                shown.threeBandPreview.reset();
                shown.colourPreview.reset();
                shown.scrollEntries.reset();
            }
            domain::EngineUpdateConflict conflict;
            conflict.header.key = domain::analysisItemKey(part, pair.pathKey);
            conflict.header.rekordboxState =
                infrastructure::local::analysisConflictState(grid, analysis, *engineNow, outcome.result.rowLocked);
            if (baseline) {
                // Answered with Engine's side, and neither side changed since.
                const auto answered = baseline->declined.find(conflict.header.key);
                if (answered != baseline->declined.end() && answered->second == conflict.header.rekordboxState) {
                    remembered[pair.rekordbox.sourceId].insert(part);
                    continue;
                }
            }
            // The answer as it would be staged, previewed: one that cannot
            // be carried out (Engine's own analysis cannot be kept first,
            // what was kept of it before being unreadable) is not offered.
            // The track is named on the page instead, and left alone.
            {
                application::AnalysisCopyOptions answer = options;
                answer.differingIsConflict = false;
                answer.grids = grid || completesRow;
                answer.waveforms = !grid;
                answer.overwriteEngineEdits = answer.grids;
                answer.overwritePlayerWaveforms = answer.waveforms;
                bool keepable = true;
                try {
                    const auto answered = application::importAnalysisItem(item, analysis, writer, ledger, answer, facts);
                    keepable = answered.result.grid.outcome != Outcome::CannotKeepEngines
                        && answered.result.waveform != Waveform::CannotKeepEngines;
                } catch (const std::exception &) {
                    keepable = false;
                }
                if (!keepable) {
                    unkeepable[pair.rekordbox.sourceId] =
                        pair.rekordbox.title.empty() ? pair.rekordbox.filename : pair.rekordbox.title;
                    continue;
                }
            }
            conflict.header.conflict = true;
            conflict.header.reason = domain::EngineUpdateReason::AnalysisDiffers;
            conflict.header.reasonText = grid
                ? "rekordbox and Engine each have a beat grid for this track, and they differ. Seabass never "
                  "writes a beat grid into rekordbox, so keeping Engine's leaves both as they are."
                : "rekordbox and Engine each have a waveform for this track, and they differ. Seabass never "
                  "writes a waveform into rekordbox, so keeping Engine's leaves both as they are.";
            if (completesRow) {
                conflict.header.reasonText += " The player has not finished analysing the track, so rekordbox's "
                                              "waveform goes in only with the rest of rekordbox's analysis: its "
                                              "beat grid, tempo and key too. The player's waveform is kept on the "
                                              "stick first.";
            }
            conflict.pathKey = pair.pathKey;
            conflict.rekordboxSide = grid ? "Use rekordbox's beat grid on Engine"
                : completesRow            ? "Use rekordbox's analysis (beat grid, waveform, tempo and key) on Engine"
                                          : "Use rekordbox's waveform on Engine";
            conflict.engineSide = grid ? "Keep Engine's beat grid, and leave both as they are"
                                       : "Keep Engine's waveform, and leave both as they are";
            domain::AnalysisEdit edit;
            edit.header = conflict.header;
            edit.header.conflict = false;
            edit.header.checkedByDefault = true;
            edit.part = part;
            edit.completesRow = completesRow;
            edit.engineLocked = outcome.result.rowLocked;
            edit.rekordbox = pair.rekordbox;
            edit.engine = pair.engine;
            edit.pathKey = pair.pathKey;
            edit.analysisDigest = infrastructure::local::rekordboxAnalysisDigest(shown);
            conflict.rekordboxChoice.push_back(std::move(edit));
            proposal.conflicts.push_back(std::move(conflict));
        }
    }
    rates.save();
}


RekordboxExportSyncAnalysis runAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                        const SyncAnalysisStaging &analysisStaging,
                                        const std::shared_ptr<QtProgressReporter> &reporter,
                                        application::CancellationToken cancel)
{
    RekordboxExportSyncAnalysis result;
    try {
        if (rekordboxPath.isEmpty() || enginePath.isEmpty()) {
            throw std::runtime_error("Sync after Rekordbox Export needs a stick with both a rekordbox export and an "
                                     "Engine library.");
        }
        // The cache is keyed by the path as the session names it, so a
        // save's invalidation reaches these entries.
        const std::string pioneer = rekordboxPath.toStdString();
        const std::string engine = enginePath.toStdString();

        // First: a damaged record fails here, before the long reads, and
        // the page falls back only for a stick that has none.
        application::EngineUpdateStickFacts facts = infrastructure::local::readEngineUpdateStickFacts(
            pathToUtf8(pathFromQString(rekordboxPath)), pathToUtf8(pathFromQString(enginePath)));
        cancel.throwIfCancelled();

        auto &cache = LibraryCatalogCache::instance();
        std::optional<size_t> planned = 0;
        const auto addUnits = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        addUnits(cache.plannedUnits("rekordbox", pioneer, LibraryCatalogCache::Detail::Full, cancel));
        addUnits(cache.plannedUnits("engine", engine, LibraryCatalogCache::Detail::Full, cancel));
        application::PhasedProgress progress(*reporter, "Comparing the libraries", planned.value_or(0));

        // Full: the file checks and lengths matching needs, and Engine's
        // cues at each file's own rate (the Cues stage's sample-rate
        // source), so a 48 kHz file's cues are not read 9 percent late.
        std::vector<domain::Track> rekordboxTracks =
            cache.tracksFor("rekordbox", pioneer, LibraryCatalogCache::Detail::Full, progress, cancel);
        std::vector<domain::Track> engineTracks =
            cache.tracksFor("engine", engine, LibraryCatalogCache::Detail::Full, progress, cancel);
        cancel.throwIfCancelled();

        result.rekordbox = rekordboxTracks;
        result.rekordboxPlaylists = facts.rekordboxPlaylists;
        result.baseline = facts.baseline;
        result.currentSequence = facts.currentSequence;
        result.stickRoot = facts.stickRoot;
        const auto keyOf = [&result](const domain::Track &track) {
            return baselinePathKey(baselineStickRelativePath(result.stickRoot, track.filePath));
        };
        for (const auto &track : result.rekordbox) {
            if (const std::string key = keyOf(track); !key.empty()) {
                result.engineFileByKey.emplace(key, track.filePath);
            }
        }
        {
            // The planner's own pairing (EngineUpdatePlanner::plan):
            // streaming rows left out, then matchTracks within one stick.
            std::vector<domain::Track> engineRows;
            for (const auto &track : engineTracks) {
                if (track.streamingSource.empty()) {
                    engineRows.push_back(track);
                }
            }
            for (const auto &[r, e] : domain::matchTracks(result.rekordbox, engineRows, domain::MatchScope::OneStick)) {
                if (const std::string key = keyOf(*r); !key.empty() && !e->filePath.empty()) {
                    result.engineFileByKey[key] = e->filePath;
                }
            }
        }

        domain::EngineUpdateInput input =
            application::buildEngineUpdateInput(std::move(rekordboxTracks), std::move(engineTracks), std::move(facts));
        // The save's own path functions (rekordbox_baseline_ledger.hpp),
        // so the keys of this proposal are the keys the record is made
        // with. buildEngineUpdateInput's are the same rule spelled in the
        // application layer; these are the ones the save uses.
        input.stickRelativeOf = [root = result.stickRoot](const std::string &filePath) {
            return baselineStickRelativePath(root, filePath);
        };
        input.pathKeyOf = [](const std::string &relative) { return baselinePathKey(relative); };
        result.proposal = domain::EngineUpdatePlanner::plan(input);
        raiseAnalysisConflicts(result.proposal, rekordboxPath, enginePath, analysisStaging, result.baseline,
                               result.rememberedAnalysisAnswers, result.uncomparedAnalysis, result.unkeepableAnalysis,
                               cancel);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromUtf8(e.what());
    }
    return result;
}

// The rows one change is made of, and how to make it once the save's
// Engine change count is known.
struct Planned
{
    std::vector<std::size_t> rows;
    bool engine = true;
    std::function<std::unique_ptr<PendingChange>(int engineChanges)> make;
};

// An item's key and its cue items' keys.
std::vector<std::string> withCueItems(const domain::EngineUpdateItemHeader &header)
{
    std::vector<std::string> keys{header.key};
    for (const auto &item : header.cueItems) {
        keys.push_back(item.key);
    }
    return keys;
}

// What a stage records beside its changes (domain::nextBaseline): every
// item the page offered, the items it applies (the ticked rows', and the
// conflicts decided without a write), and the items left unticked, which
// are declined. anyTicked: some ticked row makes a change of its own.
// stageSelected() records this; canStage() asks whether it holds anything.
struct RecordPlan
{
    std::set<std::string> offered;
    std::set<std::string> applied;
    std::map<std::string, std::string> declined;
    bool anyTicked = false;
    // A conflict on a stick with a record answered with a side that writes
    // nothing: decided all the same, and only the record says so.
    bool answeredWithoutAWrite = false;
};

RecordPlan recordPlanOf(const std::vector<Row> &rows, const RekordboxExportSyncAnalysis &analysis)
{
    RecordPlan record;
    std::set<std::string> &offered = record.offered;
    std::set<std::string> &applied = record.applied;
    std::map<std::string, std::string> &declined = record.declined;
    for (const Row &row : rows) {
        for (const auto &key : withCueItems(row.header)) {
            offered.insert(key);
        }
    }
    for (const auto &key : analysis.proposal.declinedSuppressed) {
        offered.insert(key);
    }
    std::set<int> answeredByARow;
    for (const Row &row : rows) {
        if (RekordboxExportSyncListModel::writable(row.section) && row.included && row.fromConflictUid >= 0) {
            answeredByARow.insert(row.fromConflictUid);
        }
    }
    // A row is declined only when the user unticked it itself: one whose
    // dependency is unticked went with it (a playlist's create takes its
    // members) and is neither applied nor declined, so it is offered again
    // with the record before. Declined on its own key, a member would stay
    // out after rekordbox renamed its playlist and the create came back.
    std::set<std::string> listedKeys;
    std::set<std::string> tickedKeys;
    for (const Row &row : rows) {
        if (row.section == Section::Conflicts || RekordboxExportSyncListModel::writable(row.section)) {
            listedKeys.insert(row.header.key);
        }
        for (const auto *conflict : RekordboxExportSyncListModel::conflictsOf(row)) {
            listedKeys.insert(conflict->header.key);
        }
        if (RekordboxExportSyncListModel::writable(row.section) && row.included) {
            tickedKeys.insert(row.header.key);
        }
    }
    const auto untickedWithADependency = [&](const Row &row) {
        return std::any_of(row.header.dependsOn.begin(), row.header.dependsOn.end(), [&](const std::string &key) {
            return listedKeys.count(key) && !tickedKeys.count(key);
        });
    };
    for (const Row &row : rows) {
        if (row.section == Section::Conflicts) {
            // An answer whose side writes nothing is decided all the same:
            // the item advances, and Engine keeps what it has.
            // Each conflict the row answers: its own, or a group's.
            for (const auto *conflict : RekordboxExportSyncListModel::conflictsOf(row)) {
                const bool emptyAnswer = !row.resolvedSide.isEmpty()
                    && (row.resolvedSide == QLatin1String("rekordbox") ? conflict->rekordboxChoice.empty()
                                                                       : conflict->engineChoice.empty());
                // Left open on a run without a record (unanswered, or its
                // answer unticked): recorded as the rekordbox side stands,
                // so it reads as Engine's own from now on and is not asked
                // again (the plan's step 10 review decision). Keeping "the
                // record before" would take the item out of a record there
                // was none of, and a member both sides hold would then come
                // back as a change of both. A refusal answers nothing and
                // comes back.
                const bool refusal = conflict->rekordboxChoice.empty() && conflict->engineChoice.empty();
                // #136: a grid or a waveform answered with Engine's side is
                // recorded with both sides' state, and asked again only
                // once either changes (raiseAnalysisConflicts). Left open,
                // it is asked again, with or without a record.
                if (conflict->header.reason == domain::EngineUpdateReason::AnalysisDiffers) {
                    if (emptyAnswer && !answeredByARow.count(row.uid)) {
                        declined[conflict->header.key] = conflict->header.rekordboxState;
                        record.answeredWithoutAWrite = true;
                    }
                    continue;
                }
                const bool openWithoutRecord = !analysis.baseline && !refusal;
                if ((emptyAnswer || openWithoutRecord) && !answeredByARow.count(row.uid)) {
                    for (const auto &key : withCueItems(conflict->header)) {
                        applied.insert(key);
                    }
                    // Not for rekordbox playlists sharing one path: the
                    // planner raises that conflict from the libraries as
                    // they are, whatever the record says, so a save of the
                    // answer alone would change nothing.
                    record.answeredWithoutAWrite = record.answeredWithoutAWrite
                        || (emptyAnswer
                            && conflict->header.reason != domain::EngineUpdateReason::RekordboxPathShared);
                }
            }
            continue;  // unanswered: comes back, never declined
        }
        if (!RekordboxExportSyncListModel::writable(row.section)) {
            continue;  // Engine's own and the refused adds are never written
        }
        if (row.included) {
            continue;  // applied below, through the change it rides in
        }
        if (row.fromConflictUid >= 0) {
            continue;  // an answer left unticked leaves its conflict open
        }
        if (untickedWithADependency(row)) {
            continue;  // unticked with what it depends on: offered, not declined
        }
        declined[row.header.key] = row.header.rekordboxState;
        for (const auto &item : row.header.cueItems) {
            declined[item.key] = item.rekordboxState;
        }
    }
    // What the ticked rows apply: each rides in one change of the save.
    for (const Row &row : rows) {
        if (!RekordboxExportSyncListModel::writable(row.section) || !row.included || !row.edit) {
            continue;
        }
        record.anyTicked = true;
        for (const auto &key : withCueItems(row.header)) {
            applied.insert(key);
        }
    }
    for (const auto &key : applied) {
        declined.erase(key);
    }
    return record;
}

// The tracks a save writes to or creates on the Engine side, for
// "Copy beat grids/waveforms from rekordbox to Engine when syncing": the
// ticked adds (rows the save creates, found by rekordbox's file, which is
// the one AddEngineTrackChange writes), ratings and comments to Engine,
// and cues to Engine (a conflict's answer included), each track once, in
// that order, with the keys of every row that writes it. Not memberships,
// renames or deletes, which write playlists and not a track's row. Two
// tracks for one Engine row or one file are left alone (and counted), as
// Beat Grids & Waveforms leaves them: which analysis goes where is a guess.
std::vector<SyncAnalysisChange::Track> tracksForAnalysis(const std::vector<Row> &rows,
                                                         const std::vector<std::size_t> &adds,
                                                         const std::vector<std::size_t> &metadataToEngine,
                                                         const std::vector<std::size_t> &cuesToEngine,
                                                         const std::string &stickRoot)
{
    std::vector<SyncAnalysisChange::Track> tracks;
    std::map<std::string, std::size_t> byRekordboxId;
    const auto add = [&](std::size_t rowIndex, const domain::Track &rekordbox, const domain::Track *engine) {
        if (rekordbox.sourceId.empty() || rekordbox.filePath.empty()) {
            return;
        }
        const QString key = QString::fromStdString(rows[rowIndex].header.key);
        if (const auto known = byRekordboxId.find(rekordbox.sourceId); known != byRekordboxId.end()) {
            QStringList &keys = tracks[known->second].unitKeys;
            if (!keys.contains(key)) {
                keys << key;
            }
            return;
        }
        SyncAnalysisChange::Track track;
        track.rekordbox = rekordbox;
        if (engine) {
            track.engineTrackId = engine->sourceId;
            track.engineFilePath = engine->filePath;
        } else {
            track.engineFilePath = rekordbox.filePath;
        }
        track.unitKeys << key;
        byRekordboxId[rekordbox.sourceId] = tracks.size();
        tracks.push_back(std::move(track));
    };
    for (std::size_t i : adds) {
        add(i, std::get<TrackToAdd>(*rows[i].edit).rekordbox, nullptr);
    }
    for (std::size_t i : metadataToEngine) {
        const auto &e = std::get<MetadataEdit>(*rows[i].edit);
        add(i, e.rekordbox, &e.engine);
    }
    for (std::size_t i : cuesToEngine) {
        const SyncPlan &plan = std::get<CueEdit>(*rows[i].edit).plan;
        add(i, plan.match.trackA, &plan.match.trackB);
    }
    std::map<std::string, int> perEngineRow;
    std::map<std::string, int> perFile;
    // Each track's ledger key worked out once.
    std::vector<std::string> ledgerKeys;
    ledgerKeys.reserve(tracks.size());
    for (const auto &track : tracks) {
        if (!track.engineTrackId.empty()) {
            ++perEngineRow[track.engineTrackId];
        }
        ledgerKeys.push_back(application::beatGridCopyItemFor(track.rekordbox, {}, stickRoot).ledgerKey);
        if (!ledgerKeys.back().empty()) {
            ++perFile[ledgerKeys.back()];
        }
    }
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        auto &track = tracks[i];
        const std::string &ledgerKey = ledgerKeys[i];
        if ((!track.engineTrackId.empty() && perEngineRow[track.engineTrackId] > 1)
            || (!ledgerKey.empty() && perFile[ledgerKey] > 1)) {
            track.leaveAloneBecause = QStringLiteral("two rekordbox tracks for one Engine row");
        }
    }
    return tracks;
}

}  // namespace

RekordboxExportSyncController::RekordboxExportSyncController(QObject *parent) : StagedCueEditController(parent)
{
    // Every count the page shows comes from the rows, and the rows change
    // from many places: an analysis, a tick, an answer, a row leaving once
    // its change landed. The model says so for all of them.
    connect(&m_model, &RekordboxExportSyncListModel::countsChanged, this, &RekordboxExportSyncController::listChanged);
    // canStage reads the rows and the analysis, and the rows change before
    // the analysis they came from is in place.
    connect(this, &RekordboxExportSyncController::listChanged, this, &RekordboxExportSyncController::canStageChanged);
    connect(this, &RekordboxExportSyncController::analysisChanged, this,
            &RekordboxExportSyncController::canStageChanged);
}

RekordboxExportSyncController::~RekordboxExportSyncController() = default;

bool RekordboxExportSyncController::hasBaseline() const
{
    return m_analysis && m_analysis->proposal.hasBaseline;
}

qint64 RekordboxExportSyncController::baselineSequence() const
{
    return m_analysis ? static_cast<qint64>(m_analysis->proposal.baselineSequence) : 0;
}

qint64 RekordboxExportSyncController::currentSequence() const
{
    return m_analysis ? static_cast<qint64>(m_analysis->proposal.currentSequence) : 0;
}

QString RekordboxExportSyncController::introText() const
{
    if (!m_analysis) {
        return {};
    }
    QString text = QString::fromStdString(application::engineUpdateIntroText(m_analysis->proposal));
    // #136: a track whose beat grid and waveform could not be compared is
    // said here, and the save leaves its analysis alone.
    const auto &uncompared = m_analysis->uncomparedAnalysis;
    if (!uncompared.empty()) {
        QStringList titles;
        for (const auto &[id, title] : uncompared) {
            titles << QStringLiteral("\"%1\"").arg(QString::fromStdString(title));
        }
        text += (text.isEmpty() ? QString() : QStringLiteral(" "))
            + (uncompared.size() == 1
                   ? QStringLiteral("The beat grid and waveform of %1 could not be compared with rekordbox's, so "
                                    "this sync leaves them as they are.")
                   : QStringLiteral("The beat grids and waveforms of %1 could not be compared with rekordbox's, so "
                                    "this sync leaves them as they are."))
                  .arg(titles.join(QStringLiteral(", ")));
    }
    // A difference whose rekordbox answer could not be carried out: not
    // asked, said here, and left alone.
    const auto &unkeepable = m_analysis->unkeepableAnalysis;
    if (!unkeepable.empty()) {
        QStringList titles;
        for (const auto &[id, title] : unkeepable) {
            titles << QStringLiteral("\"%1\"").arg(QString::fromStdString(title));
        }
        text += (text.isEmpty() ? QString() : QStringLiteral(" "))
            + QStringLiteral("Engine's own analysis of %1 differs from rekordbox's, but cannot be kept on the stick "
                             "first (what was kept of it before cannot be read), so this sync leaves it as it is. "
                             "Let the Player Analyse Again in Beat Grids & Waveforms clears what was kept.")
                  .arg(titles.join(QStringLiteral(", ")));
    }
    return text;
}

bool RekordboxExportSyncController::proposalEmpty() const
{
    return m_analysis && m_analysis->proposal.empty();
}

// The same test stageSelected() answers "nothing to stage" with: no
// change, nothing declined, and a record already on the stick.
bool RekordboxExportSyncController::canStage() const
{
    if (!m_analysis) {
        return false;
    }
    if (!m_analysis->proposal.hasBaseline) {
        return true;  // the first record is worth staging on its own
    }
    const RecordPlan record = recordPlanOf(m_model.rows(), *m_analysis);
    return record.anyTicked || !record.declined.empty() || record.answeredWithoutAWrite;
}

void RekordboxExportSyncController::rememberExport()
{
    if (busy() || !m_analysis || !m_analysis->proposal.empty() || m_analysis->proposal.hasBaseline) {
        return;  // only for matching libraries on a stick without a record
    }
    attachSession();
    LibraryEditSession *s = session();
    if (!s) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (s->writing()) {
        setErrorMessage("A save is running. Try again once it has finished.");
        return;
    }
    // The save writes everything staged; the record is saved alone.
    if (s->dirty()) {
        setErrorMessage("Save or discard the staged changes first; nothing was changed.");
        return;
    }
    setErrorMessage({});
    setStatusMessage({});

    const RekordboxExportSyncAnalysis &analysis = *m_analysis;
    const RecordPlan record = recordPlanOf(m_model.rows(), analysis);
    const auto stickRelativeOf = [root = analysis.stickRoot](const std::string &filePath) {
        return baselineStickRelativePath(root, filePath);
    };
    const auto pathKeyOf = [](const std::string &relative) { return baselinePathKey(relative); };
    domain::RekordboxBaseline next =
        domain::nextBaseline(nullptr, analysis.rekordbox, analysis.rekordboxPlaylists, analysis.currentSequence,
                             record.offered, record.applied, record.declined, stickRelativeOf, pathKeyOf);
    // Said as what the user knows of the export, not as "1 record".
    const auto tracks = static_cast<int>(next.tracks.size());
    const auto playlists = static_cast<int>(
        std::count_if(next.playlists.begin(), next.playlists.end(), [](const auto &p) { return !p.folder; }));
    const QString text = QStringLiteral("Remembered this export: %1 %2 and %3 %4.")
                             .arg(tracks)
                             .arg(tracks == 1 ? QStringLiteral("track") : QStringLiteral("tracks"))
                             .arg(playlists)
                             .arg(playlists == 1 ? QStringLiteral("playlist") : QStringLiteral("playlists"));

    std::vector<std::unique_ptr<PendingChange>> changes;
    changes.push_back(std::make_unique<RecordRekordboxBaselineChange>(analysis.stickRoot, std::move(next),
                                                                      RecordRekordboxBaselineChange::AppliedBy{}));
    if (!s->stageAll(std::move(changes))) {
        return;  // the session said why (a lock, a read-only backup, another page)
    }
    m_stagedIds = {RecordRekordboxBaselineChange::idString()};
    m_rememberedText = text;
    // Saved at once, as Undo Last Save is: there is nothing to choose, so
    // nothing is left staged for a Save button to offer.
    s->save();
}

bool RekordboxExportSyncController::onlyCues() const
{
    return m_analysis && m_analysis->proposal.onlyCues();
}

QString RekordboxExportSyncController::analysisKey() const
{
    return m_rekordboxPath + QLatin1Char('\n') + m_enginePath;
}

void RekordboxExportSyncController::analyze(const QString &stickLabel, const QString &rekordboxPath,
                                            const QString &enginePath)
{
    if (rekordboxPath + QLatin1Char('\n') + enginePath == analysisKey() && scanServes(analysisKey())) {
        return;  // the analysis this asks for is the one running
    }
    m_stickLabel = stickLabel;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    startAnalysis(false);
}

void RekordboxExportSyncController::startAnalysis(bool restart)
{
    attachSession();
    setErrorMessage({});
    setScanProgress(0, 0);
    auto reporter = makeReporter();
    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    // Preferences read here, on the GUI thread, as a stage reads them.
    const SyncAnalysisStaging analysisStaging = SyncAnalysisStaging::fromPreferences(rekordboxPath);
    startScan<RekordboxExportSyncAnalysis>(
        analysisKey(), stickRootOf(enginePath.isEmpty() ? rekordboxPath : enginePath), restart,
        [rekordboxPath, enginePath, analysisStaging, reporter](application::CancellationToken cancel) {
            return runAnalysis(rekordboxPath, enginePath, analysisStaging, reporter, std::move(cancel));
        },
        [this](RekordboxExportSyncAnalysis &&result) { onAnalyzeFinished(std::move(result)); });
}

void RekordboxExportSyncController::onAnalyzeFinished(RekordboxExportSyncAnalysis &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        // Refuse, never fall back: nothing of an earlier analysis stays
        // listed to be staged against a stick it no longer describes.
        m_analysis.reset();
        m_model.clear();
        setErrorMessage(result.errorMessage);
        emit analysisChanged();
        return;
    }
    auto analysis = std::make_shared<RekordboxExportSyncAnalysis>(std::move(result));
    m_model.setProposal(analysis->proposal, analysis->stickRoot);
    m_analysis = std::move(analysis);
    // Rows staged before this analysis keep their mark while they are
    // still listed; the changes themselves live in the session.
    for (const auto &[key, info] : m_stagedByKey) {
        const int index = indexOfStagedKey(key);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    emit analysisChanged();
}

void RekordboxExportSyncController::reanalyzeAfterUndo()
{
    if (!m_rekordboxPath.isEmpty() && !m_enginePath.isEmpty()) {
        startAnalysis(true);
    }
}

void RekordboxExportSyncController::onStagedCleared()
{
    m_stagedIds.clear();
}

void RekordboxExportSyncController::attachSession()
{
    attachSessionForPath(m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath);
    LibraryEditSession *s = session();
    if (!s) {
        return;
    }
    s->setLibraryPaths(m_rekordboxPath, m_enginePath);
    if (!m_stickLabel.isEmpty()) {
        s->setStickLabel(m_stickLabel);
    }
    // The base disconnects everything from a session it leaves, this
    // included, so it is made again for a new one and once only.
    if (s != m_connectedSession) {
        m_connectedSession = s;
        connect(s, &LibraryEditSession::saveFinished, this, &RekordboxExportSyncController::onSessionSaveFinished);
    }
}

// A save of this stick ended (this page's, or the undo of one): what is
// staged of ours is whatever the session still holds, and the page is
// analysed again, so it shows what is left.
void RekordboxExportSyncController::onSessionSaveFinished(const QVariantMap &summary)
{
    LibraryEditSession *s = session();
    // A save of the record and nothing else counts no unit, so the
    // session's summary has nothing to say about it ("0 records
    // recorded"): the page says it in the words given here.
    const QString record = RecordRekordboxBaselineChange::idString();
    const bool recordLanded = m_stagedIds.contains(record) && !(s && s->hasChange(record));
    const QString remembered = std::exchange(m_rememberedText, QString());
    const bool whole = summary.value(QStringLiteral("error")).toString().isEmpty()
        && summary.value(QStringLiteral("warning")).toString().isEmpty()
        && !summary.value(QStringLiteral("cancelled")).toBool();
    if (recordLanded && whole && summary.value(QStringLiteral("total")).toInt() == 0) {
        emit recordSaved(remembered.isEmpty() ? QStringLiteral("Recorded your choices for the next sync.") : remembered);
    }
    // "Remember this export" stages the record only to save it at once. A
    // save that ended without it (refused, failed and rolled back,
    // cancelled) takes it back out, so nothing is left staged for a Save
    // to offer and the button can be pressed again; the page says why.
    QString notRemembered;
    if (!remembered.isEmpty() && s && s->hasChange(record)) {
        unstageAll();
        const QString error = summary.value(QStringLiteral("error")).toString();
        notRemembered = error.isEmpty() ? QStringLiteral("the save was cancelled.")
            : error + (error.endsWith(QLatin1Char('.')) ? QString() : QStringLiteral("."));
        notRemembered.prepend(QStringLiteral("This export was not remembered: "));
    }
    QStringList still;
    for (const QString &id : std::as_const(m_stagedIds)) {
        if (s && s->hasChange(id)) {
            still << id;
        }
    }
    m_stagedIds = still;
    // The base drops one row per landed change; a membership change is
    // many rows, so the rest of them go here.
    bool dropped = false;
    for (auto it = m_stagedByKey.begin(); it != m_stagedByKey.end();) {
        if (!s || !s->hasChange(it->second.changeId)) {
            it = m_stagedByKey.erase(it);
            dropped = true;
        } else {
            ++it;
        }
    }
    if (dropped) {
        clearStagedStatusIfNothingStaged();
        emit stagedChanged();
    }
    // Not when an undo already asked for it (reanalyzeAfterUndo).
    if (!m_rekordboxPath.isEmpty() && !m_enginePath.isEmpty() && !scanServes(analysisKey())) {
        startAnalysis(true);
    }
    // After the analysis has started, which clears the page's error.
    if (!notRemembered.isEmpty()) {
        setErrorMessage(notRemembered);
    }
}

void RekordboxExportSyncController::setIncluded(int row, bool included)
{
    m_model.setIncluded(row, included);
}

void RekordboxExportSyncController::setSectionIncluded(const QString &section, bool included)
{
    if (const auto s = RekordboxExportSyncListModel::sectionFromName(section)) {
        m_model.setSectionIncluded(*s, included);
    }
}

void RekordboxExportSyncController::resolveConflict(int row, bool rekordboxSide)
{
    m_model.resolveConflict(row, rekordboxSide);
}

void RekordboxExportSyncController::clearConflictResolution(int row)
{
    m_model.clearResolution(row);
}

void RekordboxExportSyncController::resolveAllConflicts(bool rekordboxSide)
{
    m_model.resolveAllConflicts(rekordboxSide);
}

void RekordboxExportSyncController::clearAllConflictResolutions()
{
    m_model.clearAllResolutions();
}

void RekordboxExportSyncController::unstageAll()
{
    if (m_stagedIds.isEmpty() && m_stagedByKey.empty()) {
        return;
    }
    if (LibraryEditSession *s = session(); s && !m_stagedIds.isEmpty()) {
        s->unstageAll(m_stagedIds);
    }
    m_stagedIds.clear();
    m_stagedByKey.clear();
    m_model.clearStaged();
    clearStagedStatusIfNothingStaged();
    emit stagedChanged();
}

QStringList RekordboxExportSyncController::unitKeysOf(const std::vector<Row> &rows,
                                                      const std::vector<std::size_t> &rowIndexes)
{
    // The planner's own graph: a row's key and every key it depends on,
    // the same links the page ticks rows together by. A membership depends
    // on its playlist's create or rename and on those of the folders above
    // it, and on the add of a track not yet in Engine; whatever a cancel
    // lets land, it lets what that needs land with it.
    QStringList keys;
    const auto add = [&keys](const std::string &key) {
        const QString text = QString::fromStdString(key);
        if (!text.isEmpty() && !keys.contains(text)) {
            keys << text;
        }
    };
    for (std::size_t i : rowIndexes) {
        add(rows[i].header.key);
        for (const auto &key : rows[i].header.dependsOn) {
            add(key);
        }
        // A conflict's chosen cues are a change of their own, staged after
        // the track's other cue write and linked to nothing else in the
        // graph: the track is the unit both write.
        if (rows[i].edit) {
            if (const auto *cues = std::get_if<domain::CueEdit>(&*rows[i].edit)) {
                add("cues-of|" + cues->pathKey);
            }
        }
    }
    return keys;
}

QString RekordboxExportSyncController::describeMetadataWrite(const std::vector<domain::MetadataEdit> &edits,
                                                            bool toEngine)
{
    using domain::MetadataEdit;
    if (edits.empty()) {
        return {};
    }
    QStringList fields;
    QString ratingText;
    bool restore = true;
    for (const MetadataEdit &e : edits) {
        if (e.field == MetadataEdit::Field::Rating) {
            fields << QStringLiteral("rating");
            ratingText = e.rating && *e.rating > 0 ? QStringLiteral("%1 star(s)").arg(*e.rating)
                                                   : QStringLiteral("unrated");
        } else {
            fields << QStringLiteral("comment");
        }
        restore = restore && restoresSeabassWrite(e);
    }
    const bool ratingAlone = fields.size() == 1 && fields.front() == QLatin1String("rating");
    const QString what = fields.join(QStringLiteral(" and "));
    if (toEngine) {
        const QString title = titleOf(edits.front().engine);
        return ratingAlone ? QStringLiteral("Set the rating of \"%1\" in Engine to %2").arg(title, ratingText)
                           : QStringLiteral("Set the %1 of \"%2\" in Engine as rekordbox has it").arg(what, title);
    }
    const QString title = titleOf(edits.front().rekordbox);
    if (restore) {
        // The export dropped what Seabass wrote there: it goes back.
        return QStringLiteral("Put the %1 Seabass wrote back on \"%2\" in rekordbox").arg(what, title);
    }
    // Nothing recorded who wrote rekordbox's value (an answer toward
    // Engine of an OriginUnknown conflict): Engine's goes over it, and is
    // not called Seabass's.
    return ratingAlone ? QStringLiteral("Set the rating of \"%1\" in rekordbox to %2, Engine's").arg(title, ratingText)
                       : QStringLiteral("Set the %1 of \"%2\" in rekordbox to Engine's").arg(what, title);
}

void RekordboxExportSyncController::stageSelected()
{
    if (busy() || !m_analysis) {
        return;
    }
    attachSession();
    LibraryEditSession *s = session();
    if (!s) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (s->writing()) {
        setErrorMessage("A save is running. Stage more once it has finished.");
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    // The record of a save is computed from the whole selection, so a
    // second stage replaces the first rather than adding to it.
    unstageAll();

    const RekordboxExportSyncAnalysis &analysis = *m_analysis;
    const std::vector<Row> &rows = m_model.rows();

    // The ticked rows by what they are, in the model's order.
    std::vector<std::size_t> creates, adds, renames, members, metadataToEngine, cuesToEngine, deletes, removals,
        restoresMetadata, restoresCues, analysisAnswers;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row &row = rows[i];
        if (!RekordboxExportSyncListModel::writable(row.section) || !row.included || !row.edit) {
            continue;
        }
        const EngineUpdateEdit &edit = *row.edit;
        if (std::holds_alternative<PlaylistCreate>(edit)) {
            creates.push_back(i);
        } else if (std::holds_alternative<TrackToAdd>(edit)) {
            adds.push_back(i);
        } else if (std::holds_alternative<PlaylistRename>(edit)) {
            renames.push_back(i);
        } else if (std::holds_alternative<MembershipEdit>(edit)) {
            members.push_back(i);
        } else if (const auto *m = std::get_if<MetadataEdit>(&edit)) {
            (m->direction == MetadataEdit::Direction::ToEngine ? metadataToEngine : restoresMetadata).push_back(i);
        } else if (const auto *c = std::get_if<CueEdit>(&edit)) {
            (c->plan.direction == SyncPlan::Direction::ToB ? cuesToEngine : restoresCues).push_back(i);
        } else if (std::holds_alternative<PlaylistDelete>(edit)) {
            deletes.push_back(i);
        } else if (std::holds_alternative<TrackToRemove>(edit)) {
            removals.push_back(i);
        } else if (std::holds_alternative<domain::AnalysisEdit>(edit)) {
            analysisAnswers.push_back(i);
        }
    }
    // Parents first for creates, deepest first for deletes, whatever order
    // an answer's rows joined in; and a conflict's chosen cues after the
    // track's other cue write, so the complete set is written last.
    std::stable_sort(creates.begin(), creates.end(), [&](std::size_t a, std::size_t b) {
        return depthOf(std::get<PlaylistCreate>(*rows[a].edit).path)
            < depthOf(std::get<PlaylistCreate>(*rows[b].edit).path);
    });
    std::stable_sort(deletes.begin(), deletes.end(), [&](std::size_t a, std::size_t b) {
        return depthOf(std::get<PlaylistDelete>(*rows[a].edit).path)
            > depthOf(std::get<PlaylistDelete>(*rows[b].edit).path);
    });
    const auto answersLast = [&](std::vector<std::size_t> &list) {
        std::stable_partition(list.begin(), list.end(), [&](std::size_t i) { return rows[i].fromConflictUid < 0; });
    };
    answersLast(cuesToEngine);
    answersLast(restoresCues);

    const QString rekordboxPath = m_rekordboxPath;
    const QString enginePath = m_enginePath;
    std::vector<Planned> planned;

    // The plan's staging order: create playlists, add tracks, renames,
    // memberships, metadata, cues to Engine, playlist deletes, track
    // removals, restores to rekordbox, record baseline. But for an id
    // rekordbox reused (#99): the old playlist's delete directly before
    // the new one's create, when both are ticked. A save that stops
    // between them then leaves what the record can say: the old playlist
    // gone and the new one not yet made (nothing under the id), never the
    // new one made and the old one left (which the record cannot hold
    // under the id together with the new one). The reuse rule made sure
    // nothing else needs the old order: the paths differ, rekordbox has
    // nothing at the old one, Engine has exactly one item there and, for
    // a folder, nothing inside it. Staged before this save's renames, the
    // delete names the old playlist where Engine has it now: the path the
    // reused key records, not the planner's path after the renames.
    std::map<std::uint32_t, std::size_t> reusedDeleteOf;
    for (std::size_t i : deletes) {
        if (const auto reused = domain::parseReusedPlaylistItemKey(rows[i].header.key)) {
            reusedDeleteOf.emplace(reused->id, i);
        }
    }
    std::set<std::size_t> deletedEarly;
    for (std::size_t i : creates) {
        const auto e = std::get<PlaylistCreate>(*rows[i].edit);
        if (const auto d = reusedDeleteOf.find(e.pdbId); d != reusedDeleteOf.end()) {
            const std::string oldPath = domain::parseReusedPlaylistItemKey(rows[d->second].header.key)->recordedPath;
            planned.push_back({{d->second}, true, [enginePath, oldPath](int hint) -> std::unique_ptr<PendingChange> {
                                   return deleteEnginePlaylistChange(enginePath, oldPath, hint);
                               }});
            deletedEarly.insert(d->second);
        }
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<CreateEnginePlaylistChange>(enginePath, e.path, hint);
                           }});
    }
    const auto probe = metadataProbe();
    for (std::size_t i : adds) {
        const auto e = std::get<TrackToAdd>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e, probe](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<AddEngineTrackChange>(enginePath, e.rekordbox, probe, hint);
                           }});
    }
    for (std::size_t i : renames) {
        const auto e = std::get<PlaylistRename>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<RenameEnginePlaylistChange>(enginePath, e.fromPath, e.toPath,
                                                                                   hint);
                           }});
    }
    {
        // One change per playlist with every ticked edit for it: removes
        // first, adds in rekordbox's order.
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::size_t>> byPlaylist;
        for (std::size_t i : members) {
            const auto &e = std::get<MembershipEdit>(*rows[i].edit);
            if (!byPlaylist.count(e.playlistPath)) {
                order.push_back(e.playlistPath);
            }
            byPlaylist[e.playlistPath].push_back(i);
        }
        for (const std::string &playlist : order) {
            std::vector<std::size_t> group = byPlaylist[playlist];
            std::stable_partition(group.begin(), group.end(), [&](std::size_t i) {
                return std::get<MembershipEdit>(*rows[i].edit).kind == MembershipEdit::Kind::Remove;
            });
            std::vector<EngineMembershipChange::Add> addList;
            std::vector<EngineMembershipChange::Remove> removeList;
            for (std::size_t i : group) {
                const auto &e = std::get<MembershipEdit>(*rows[i].edit);
                if (e.kind == MembershipEdit::Kind::Remove) {
                    removeList.push_back({e.track.sourceId, e.track.filePath, e.track.title});
                    continue;
                }
                EngineMembershipChange::Add add;
                const auto own = analysis.engineFileByKey.find(e.pathKey);
                add.filePath = own != analysis.engineFileByKey.end() ? own->second : e.track.filePath;
                add.title = e.track.title;
                if (!e.afterPathKey.empty()) {
                    // The anchor by its file. A key no file is known for
                    // still goes as a path (a non-empty one that names
                    // nothing), so the writer appends rather than putting
                    // the track first.
                    const auto file = analysis.engineFileByKey.find(e.afterPathKey);
                    add.afterFilePath = file != analysis.engineFileByKey.end() ? file->second : e.afterPathKey;
                }
                addList.push_back(std::move(add));
            }
            planned.push_back({group, true,
                               [enginePath, playlist, addList, removeList](int hint) -> std::unique_ptr<PendingChange> {
                                   return std::make_unique<EngineMembershipChange>(enginePath, playlist, addList,
                                                                                   removeList, hint);
                               }});
        }
    }
    // A track's rating and comment are one change: one id per track and
    // catalog, and a second would replace the first.
    const auto metadataChanges = [&](const std::vector<std::size_t> &list, bool toEngine) {
        std::vector<std::string> order;
        std::map<std::string, std::vector<std::size_t>> byTrack;
        for (std::size_t i : list) {
            const auto &e = std::get<MetadataEdit>(*rows[i].edit);
            const std::string id = toEngine ? e.engine.sourceId : e.rekordbox.sourceId;
            if (!byTrack.count(id)) {
                order.push_back(id);
            }
            byTrack[id].push_back(i);
        }
        for (const std::string &id : order) {
            const std::vector<std::size_t> group = byTrack[id];
            const auto &first = std::get<MetadataEdit>(*rows[group.front()].edit);
            domain::MetadataRestoreProposal proposal;
            proposal.stickTrack = toEngine ? first.engine : first.rekordbox;
            std::vector<MetadataEdit> edits;
            for (std::size_t i : group) {
                const auto &e = std::get<MetadataEdit>(*rows[i].edit);
                edits.push_back(e);
                if (e.field == MetadataEdit::Field::Rating) {
                    proposal.ratingOffered = true;
                    // 0 stars is unrated to every writer: a cleared rating
                    // is written as one.
                    proposal.rating = e.rating.value_or(0);
                } else {
                    proposal.commentOffered = true;
                    proposal.comment = e.comment;
                }
            }
            const QString description = describeMetadataWrite(edits, toEngine);
            const QString format = toEngine ? QStringLiteral("engine") : QStringLiteral("rekordbox");
            const QString path = toEngine ? enginePath : rekordboxPath;
            planned.push_back({group, toEngine,
                               [format, path, id, proposal, description](int hint) -> std::unique_ptr<PendingChange> {
                                   auto inner = std::make_unique<RestoreMetadataChange>(format, path, q(id), proposal,
                                                                                        hint);
                                   return std::make_unique<PageChange>(std::move(inner),
                                                                       PageChange::Said{{}, description, {}, {}});
                               }});
        }
    };
    const auto cueChanges = [&](const std::vector<std::size_t> &list, bool toEngine) {
        for (std::size_t i : list) {
            const SyncPlan plan = std::get<CueEdit>(*rows[i].edit).plan;
            const bool answer = rows[i].fromConflictUid >= 0;
            planned.push_back({{i}, toEngine,
                               [rekordboxPath, enginePath, plan, answer](int hint) -> std::unique_ptr<PendingChange> {
                                   auto inner = std::make_unique<SyncPlanChange>(rekordboxPath, enginePath, plan, hint);
                                   if (!answer) {
                                       return ownedByRekordboxExportSync(std::move(inner));
                                   }
                                   // The track's other cue write has this
                                   // id; both are staged, this one last.
                                   const QString id =
                                       rekordboxExportSyncOwner() + QStringLiteral(":answer:") + inner->id();
                                   return std::make_unique<PageChange>(std::move(inner),
                                                                       PageChange::Said{id, {}, {}, {}});
                               }});
        }
    };
    metadataChanges(metadataToEngine, true);
    cueChanges(cuesToEngine, true);
    // rekordbox's analysis of the tracks written to Engine goes here, after
    // every Engine write of a track and before the deletes, when
    // Preferences asks for it (application::SyncAnalysisPolicy). With both
    // off nothing is staged, and the save is the one it always was.
    const std::size_t analysisAt = planned.size();
    const SyncAnalysisStaging analysisStaging = SyncAnalysisStaging::fromPreferences(rekordboxPath);
    const std::vector<SyncAnalysisChange::Track> analysisTracks = analysisStaging.any()
        ? tracksForAnalysis(rows, adds, metadataToEngine, cuesToEngine, analysisStaging.stickRoot)
        : std::vector<SyncAnalysisChange::Track>{};
    // The conflicts over a grid or a waveform answered with rekordbox's
    // (#136): each part a change of its own, after the track's rider, so
    // the part the rider left alone is written last.
    for (std::size_t i : analysisAnswers) {
        const auto e = std::get<domain::AnalysisEdit>(*rows[i].edit);
        SyncAnalysisChange::Track track;
        track.rekordbox = e.rekordbox;
        track.engineTrackId = e.engine.sourceId;
        track.engineFilePath = e.engine.filePath;
        track.analysisDigest = e.analysisDigest;
        application::AnalysisCopyOptions options;
        // A completion takes both parts: rekordbox's waveform goes into a
        // row the player has not finished only with rekordbox's grid.
        options.grids = e.part == domain::AnalysisEdit::Part::Grid || e.completesRow;
        options.waveforms = e.part == domain::AnalysisEdit::Part::Waveform;
        const std::string stickRoot = analysisStaging.stickRoot;
        const auto answerProbe = analysisStaging.probe;
        const bool lockedWhenAsked = e.engineLocked;
        planned.push_back({{i}, true,
                           [rekordboxPath, enginePath, stickRoot, track, options, answerProbe,
                            lockedWhenAsked](int hint) -> std::unique_ptr<PendingChange> {
                               auto change = std::make_unique<SyncAnalysisChange>(
                                   rekordboxExportSyncOwner(), rekordboxPath, enginePath, stickRoot, track, options,
                                   answerProbe, hint, true);
                               change->answerWithRekordboxs(lockedWhenAsked);
                               return change;
                           }});
    }
    for (std::size_t i : deletes) {
        if (deletedEarly.count(i)) {
            continue;
        }
        const auto e = std::get<PlaylistDelete>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return deleteEnginePlaylistChange(enginePath, e.path, hint);
                           }});
    }
    for (std::size_t i : removals) {
        const auto e = std::get<TrackToRemove>(*rows[i].edit);
        planned.push_back({{i}, true, [enginePath, e](int hint) -> std::unique_ptr<PendingChange> {
                               return std::make_unique<RemoveEngineTrackChange>(enginePath, e.engine, hint);
                           }});
    }
    metadataChanges(restoresMetadata, false);
    cueChanges(restoresCues, false);

    // What the save may write into m.db: every Engine change, which is what
    // decides the scratch copy.
    const int engineChanges =
        static_cast<int>(std::count_if(planned.begin(), planned.end(), [](const Planned &p) { return p.engine; }))
        + static_cast<int>(analysisTracks.size());

    // The record: what the page offered, what it stages, what was left.
    RecordPlan record = recordPlanOf(rows, analysis);
    std::set<std::string> &offered = record.offered;
    std::set<std::string> &applied = record.applied;
    std::map<std::string, std::string> &declined = record.declined;

    std::vector<std::unique_ptr<PendingChange>> changes;
    RecordRekordboxBaselineChange::AppliedBy appliedBy;
    // Each change's rows with the change's own id and words, taken here:
    // the analysis riders are inserted among the changes below, so a
    // position in `changes` is not a position in `planned`.
    struct RowsOfChange
    {
        std::vector<std::size_t> rows;
        QString id;
        QString description;
    };
    std::vector<RowsOfChange> rowsOfChange;
    std::set<std::pair<QString, QString>> unitsAndVerbs;
    for (Planned &p : planned) {
        std::unique_ptr<PendingChange> change = std::make_unique<PageChange>(
            p.make(engineChanges), PageChange::Said{{}, {}, {}, {}, unitKeysOf(rows, p.rows)});
        std::vector<std::string> keys;
        for (std::size_t i : p.rows) {
            for (const auto &key : withCueItems(rows[i].header)) {
                applied.insert(key);
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                    keys.push_back(key);
                }
            }
        }
        appliedBy.emplace_back(change->id().toStdString(), std::move(keys));
        rowsOfChange.push_back({p.rows, change->id(), change->description()});
        unitsAndVerbs.insert({change->unit(), change->verb()});
        changes.push_back(std::move(change));
    }
    for (const auto &key : applied) {
        declined.erase(key);
    }

    if (changes.empty() && declined.empty() && !record.answeredWithoutAWrite && analysis.proposal.hasBaseline) {
        setStatusMessage(QStringLiteral("Nothing is selected, so there is nothing to stage."));
        return;
    }

    // The save's summary sums every change's units under the first
    // change's noun and verb: "12 of 12 playlists created" for a save that
    // also added tracks would not be true. A save of more than one kind of
    // change is told in changes.
    if (unitsAndVerbs.size() > 1) {
        changes.front() = std::make_unique<PageChange>(
            std::move(changes.front()), PageChange::Said{{}, {}, QStringLiteral("changes"), QStringLiteral("made")});
    }

    // The analysis rides with the tracks: it carries none of the save's
    // count (SyncAnalysisChange::unitsWritten) and says what it did in the
    // save's summary notes, so the count above is the sync's as it was.
    // A part whose conflict is answered, either way, now or remembered from
    // an earlier save, is the answer's alone: the rider leaves it out, so
    // the note never asks what was answered (Engine's side writes nothing;
    // rekordbox's is its own change, both parts for a completion).
    std::map<std::string, std::set<domain::AnalysisEdit::Part>> answeredParts = analysis.rememberedAnalysisAnswers;
    for (std::size_t i : analysisAnswers) {
        const auto &e = std::get<domain::AnalysisEdit>(*rows[i].edit);
        answeredParts[e.rekordbox.sourceId].insert(e.part);
        if (e.completesRow) {
            answeredParts[e.rekordbox.sourceId].insert(domain::AnalysisEdit::Part::Grid);
        }
    }
    for (const Row &row : rows) {
        if (row.section != Section::Conflicts || row.resolvedSide != QLatin1String("engine") || !row.conflict) {
            continue;
        }
        for (const auto &edit : row.conflict->rekordboxChoice) {
            if (const auto *e = std::get_if<domain::AnalysisEdit>(&edit)) {
                answeredParts[e->rekordbox.sourceId].insert(e->part);
            }
        }
    }
    std::size_t riderCount = 0;
    if (!analysisTracks.empty()) {
        std::vector<std::unique_ptr<PendingChange>> riders;
        for (const auto &track : analysisTracks) {
            // Not compared when the page was read: left as it is, as the
            // page says, rather than written or said to differ unasked.
            if (analysis.uncomparedAnalysis.count(track.rekordbox.sourceId)
                || analysis.unkeepableAnalysis.count(track.rekordbox.sourceId)) {
                continue;
            }
            application::AnalysisCopyOptions options = analysisStaging.options;
            // #136: a part both sides hold, differently, is asked, never
            // written over Engine's own or left without saying so.
            options.differingIsConflict = true;
            if (const auto answered = answeredParts.find(track.rekordbox.sourceId); answered != answeredParts.end()) {
                options.grids = options.grids && !answered->second.count(domain::AnalysisEdit::Part::Grid);
                options.waveforms = options.waveforms && !answered->second.count(domain::AnalysisEdit::Part::Waveform);
            }
            if (!options.grids && !options.waveforms) {
                continue;
            }
            riders.push_back(std::make_unique<SyncAnalysisChange>(rekordboxExportSyncOwner(), rekordboxPath,
                                                                  enginePath, analysisStaging.stickRoot, track,
                                                                  options, analysisStaging.probe, engineChanges,
                                                                  riders.empty()));
        }
        riderCount = riders.size();
        changes.insert(changes.begin() + static_cast<std::ptrdiff_t>(analysisAt),
                       std::make_move_iterator(riders.begin()), std::make_move_iterator(riders.end()));
    }

    const auto stickRelativeOf = [root = analysis.stickRoot](const std::string &filePath) {
        return baselineStickRelativePath(root, filePath);
    };
    const auto pathKeyOf = [](const std::string &relative) { return baselinePathKey(relative); };
    // The analysis answers the record holds (#136) stay in it, whether or
    // not this save offered their track: nextBaseline keeps only the
    // declines of items offered. A key this save answered anew, either
    // way, takes the new answer (rekordbox's side: no entry, the two agree
    // once it is written).
    if (analysis.baseline) {
        for (const auto &[key, state] : analysis.baseline->declined) {
            if (key.rfind("analysis:", 0) == 0 && !declined.count(key) && !applied.count(key)) {
                declined.emplace(key, state);
            }
        }
    }
    domain::RekordboxBaseline next =
        domain::nextBaseline(analysis.baseline ? &*analysis.baseline : nullptr, analysis.rekordbox,
                             analysis.rekordboxPlaylists, analysis.currentSequence, offered, applied, declined,
                             stickRelativeOf, pathKeyOf);
    const int changeCount = static_cast<int>(changes.size() - riderCount);
    std::set<std::string> reusedIds;
    for (const auto &key : offered) {
        if (domain::parseReusedPlaylistItemKey(key)) {
            reusedIds.insert(key);
        }
    }
    changes.push_back(std::make_unique<RecordRekordboxBaselineChange>(analysis.stickRoot, std::move(next),
                                                                      std::move(appliedBy), std::move(reusedIds)));

    QStringList ids;
    for (const auto &change : changes) {
        ids << change->id();
    }
    if (!s->stageAll(std::move(changes))) {
        return;  // the session said why (a lock, a read-only backup, another page)
    }
    m_stagedIds = ids;
    for (const RowsOfChange &staged : rowsOfChange) {
        for (std::size_t i : staged.rows) {
            m_stagedByKey[rows[i].planKey] = {staged.id, staged.description};
            m_model.setStaged(static_cast<int>(i), true, staged.description);
        }
    }
    emit stagedChanged();
    setStagedStatusMessage(changeCount > 0
                               ? QStringLiteral("Staged %1 change(s). Press Sync Engine to write them to the stick.")
                                     .arg(changeCount)
                                   + (analysisTracks.empty()
                                          ? QString()
                                          : QStringLiteral(" The tracks it writes also get rekordbox's %1, as set in "
                                                           "Preferences.")
                                                .arg(analysisStaging.partsInWords()))
                               : QStringLiteral("Staged the record of your choices. Press Sync Engine to write it "
                                                "to the stick."));
}

}  // namespace seabass::gui
