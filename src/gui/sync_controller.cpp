// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "sync_controller.hpp"

#include "gui/stick_path.hpp"
#include "gui/future_result.hpp"

#include <QDebug>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "application/use_cases/onelibrary_sync_rows.hpp"
#include "application/path_key.hpp"
#include "application/ports/backup_store.hpp"
#include "application/use_cases/scan_library.hpp"
#include "application/use_cases/sync_libraries.hpp"
#include "domain/cross_source_sync_conflict.hpp"
#include "domain/track_scope.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/library_catalog_cache.hpp"
#include "application/phased_progress.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/file_clock.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#include "gui/edit/changes/sync_plan_change.hpp"
#include "gui/edit/changes/cue_offset_repair_change.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "gui/qt_path.hpp"
#include "gui/seabass_settings.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using domain::SyncPlan;

namespace
{

std::chrono::system_clock::time_point fileMtime(const std::string &path)
{
    return infrastructure::toSystemClock(std::filesystem::last_write_time(pathFromUtf8(path)));
}

// Builds SyncTaskResult::playlistNames/playlistTrackCounts from the union
// of every catalog's own (unfiltered) tracks -- called before any
// TrackScope filtering below, so picking a playlist never shrinks the
// picker's own list of choices. A given playlist name's count is the max
// across whichever catalogs have it, not a sum: the same playlist
// typically exists independently in each catalog present on a stick with
// near-identical membership, and summing would roughly double-count it
// whenever two catalogs are present, without meaning "distinct real
// tracks" (that would need real cross-catalog matching, not just a
// display count).
void collectPlaylistSummary(const std::vector<domain::Track> &rekordboxTracks,
                             const std::vector<domain::Track> &engineTracks,
                             const std::vector<domain::Track> &oneLibraryTracks, SyncTaskResult &result)
{
    // std::map (ordered), not unordered_map -- iterating it directly below
    // gives sorted names for free, no separate std::set pass just to get
    // an ordering.
    std::map<std::string, int> maxCountByName;
    auto tally = [&](const std::vector<domain::Track> &tracks) {
        std::unordered_map<std::string, int> countThisCatalog;
        for (const auto &track : tracks) {
            for (const auto &playlist : track.playlists) {
                countThisCatalog[playlist.name]++;
            }
        }
        for (const auto &[name, count] : countThisCatalog) {
            int &best = maxCountByName[name];
            best = std::max(best, count);
        }
    };
    tally(rekordboxTracks);
    tally(engineTracks);
    tally(oneLibraryTracks);

    for (const auto &[name, count] : maxCountByName) {
        QString qName = QString::fromStdString(name);
        result.playlistNames << qName;
        result.playlistTrackCounts[qName] = count;
    }
}

// What copying rekordbox's analysis would do, previewed for the tracks in
// scope rekordbox and Engine share, paired as Beat Grids & Waveforms pairs
// them, each through the writer with nothing written (previewOnly), as that
// page's preview takes it. A part counts only when the preview would write
// it: a grid locked or moved on the player and a waveform the player drew
// are never replaced here (overwriteEngineEdits and
// overwritePlayerWaveforms stay off), and a row the player has not analysed
// is completed with both parts or not at all. A track whose preview failed
// is left alone and counted (`unchecked`): the page says it was not
// checked rather than showing it in step. Runs on the analysis' worker,
// inside its bar.
//
// Only the parts asked for are read of rekordbox's analysis, and each
// track's digest is of the parts it would get, as its save reads them
// (SyncAnalysisChange). Every MP3's lead-in comes from the stick's
// metadata cache, read once and kept there (saved at the end, also when
// the pass ends early), so a page that analyses again after every save
// does not read every file's head over USB again.
//
// One row per track, and never both a rider and a change of its own:
// - a plan whose cues go from rekordbox (either format) onto Engine
//   carries the analysis as a rider; what it carries is `riders`, by
//   Engine sourceId (none for a OneLibrary row naming no analysis file,
//   which the rider leaves alone);
// - every other track a plan or a decision touches is left out, a
//   decision because it may go either way, a plan the other way because
//   it is that track's row; its analysis is offered once its row is gone;
// - the rest whose analysis would change are `only`, rows of their own.
struct AnalysisPreview
{
    std::vector<SyncAnalysisOnlyTrack> only;
    std::map<std::string, SyncRiderAnalysis> riders;
    int unchecked = 0;
};

AnalysisPreview previewAnalysis(const std::vector<domain::Track> &rekordboxTracks,
                                const std::vector<domain::Track> &engineTracks, const std::vector<SyncPlan> &plans,
                                const std::vector<domain::CrossSourceSyncConflict> &conflicts,
                                const QString &rekordboxPath, const QString &enginePath, const std::string &stickRoot,
                                application::AnalysisCopyOptions options, application::ProgressReporter &progress,
                                const application::CancellationToken &cancel)
{
    std::set<std::string> riderEngineIds;
    std::set<std::string> riderRekordboxIds;
    std::set<std::string> leftEngineIds;
    std::set<std::string> leftRekordboxIds;
    const auto leave = [&](const domain::Track &track) {
        if (track.format == "engine") {
            leftEngineIds.insert(track.sourceId);
        } else if (track.format == "rekordbox") {
            leftRekordboxIds.insert(track.sourceId);
        }
    };
    for (const SyncPlan &plan : plans) {
        const bool toB = plan.direction == SyncPlan::Direction::ToB;
        const domain::Track &source = toB ? plan.match.trackA : plan.match.trackB;
        const domain::Track &target = toB ? plan.match.trackB : plan.match.trackA;
        const bool rides = target.format == "engine"
            && (source.format == "rekordbox" || (source.format == "onelibrary" && !source.analysisFile.empty()));
        if (rides) {
            riderEngineIds.insert(target.sourceId);
            if (source.format == "rekordbox") {
                riderRekordboxIds.insert(source.sourceId);
            }
            continue;
        }
        leave(plan.match.trackA);
        leave(plan.match.trackB);
    }
    for (const auto &conflict : conflicts) {
        for (const domain::Track *track : {&conflict.sourceA, &conflict.sourceB, &conflict.target}) {
            leave(*track);
        }
    }

    application::BeatGridCopyPlan plan =
        application::planBeatGridCopy(rekordboxTracks, engineTracks, stickRoot, &application::SyncLibraries::stickFiles());
    std::erase_if(plan.items, [&](const application::BeatGridCopyItem &item) {
        return leftEngineIds.count(item.engineTrackId) > 0 || leftRekordboxIds.count(item.rekordboxTrackId) > 0
            || (riderRekordboxIds.count(item.rekordboxTrackId) > 0 && riderEngineIds.count(item.engineTrackId) == 0);
    });
    std::map<std::string, const domain::Track *> rekordboxById;
    for (const domain::Track &track : rekordboxTracks) {
        rekordboxById[track.sourceId] = &track;
    }
    std::map<std::string, const domain::Track *> engineById;
    for (const domain::Track &track : engineTracks) {
        engineById[track.sourceId] = &track;
    }

    AnalysisPreview out;
    progress.start("Checking beat grids and waveforms", plan.items.size());
    if (plan.items.empty()) {
        progress.finish();
        return out;
    }
    const std::string pioneer = rekordboxPath.toStdString();
    std::string ledgerError;
    // A ledger that cannot be read is no ledger: a grid Seabass wrote
    // before reads as one set on the player and is left alone.
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
    const application::AnalysisFileFactsSource facts = [&rates](const application::BeatGridCopyItem &item) {
        return infrastructure::local::analysisFileFactsOf(item, rates);
    };
    writer.setLeadInSource([&rates](const std::string &file) { return rates.leadInOf(file); });
    options.previewOnly = true;
    options.overwriteEngineEdits = false;
    options.overwritePlayerWaveforms = false;
    options.skipOverviewFile = false;
    options.zeroLoudness = false;
    using Outcome = application::BeatGridWriteResult::Outcome;
    using Waveform = application::AnalysisImportResult::Waveform;
    // What the preview found of the metadata kept: a read-only or full
    // stick costs a read next time, never the pass.
    struct SaveRates
    {
        infrastructure::local::CachedSampleRates &rates;
        ~SaveRates() { rates.save(); }
    } saveRates{rates};
    for (size_t i = 0; i < plan.items.size(); ++i) {
        if (cancel.cancelled()) {
            throw application::OperationCancelled();
        }
        const application::BeatGridCopyItem &item = plan.items[i];
        const auto rekordbox = rekordboxById.find(item.rekordboxTrackId);
        const auto engine = engineById.find(item.engineTrackId);
        if (rekordbox == rekordboxById.end() || engine == engineById.end()) {
            ++out.unchecked;
        } else {
            try {
                const application::RekordboxAnalysis analysis = infrastructure::local::rekordboxAnalysisOf(
                    pioneer, item, index.get(), options.grids, options.waveforms);
                const application::AnalysisItemOutcome outcome =
                    application::importAnalysisItem(item, analysis, writer, ledger, options, facts);
                const bool grid = outcome.result.grid.outcome == Outcome::Written;
                const bool waveform = outcome.result.waveform == Waveform::Written;
                const SyncRowAnalysis parts{grid, waveform, outcome.result.completedRow};
                // The digest of what the save reads: the parts it gets.
                application::RekordboxAnalysis staged = analysis;
                if (!grid) {
                    staged.beats.clear();
                }
                if (!waveform) {
                    staged.threeBandPreview.reset();
                    staged.colourPreview.reset();
                    staged.scrollEntries.reset();
                }
                const std::string digest = infrastructure::local::rekordboxAnalysisDigest(staged);
                if (!parts.any()) {
                    // Nothing it would write: no row, nothing said.
                } else if (riderEngineIds.count(item.engineTrackId)) {
                    out.riders[item.engineTrackId] = SyncRiderAnalysis{parts, digest};
                } else {
                    SyncAnalysisOnlyTrack track;
                    track.rekordbox = *rekordbox->second;
                    track.engine = *engine->second;
                    track.analysisDigest = digest;
                    track.analysis = parts;
                    out.only.push_back(std::move(track));
                }
            } catch (const std::exception &e) {
                // Unsure: left alone, and said on the page as not checked.
                ++out.unchecked;
                qWarning("Sync Cue Points: rekordbox's analysis of Engine track %s was not checked: %s",
                         item.engineTrackId.c_str(), e.what());
            }
        }
        progress.tick(i + 1);
    }
    progress.finish();
    return out;
}

// Runs entirely on a background thread (see SyncController::analyze()) -
// no access to the controller itself. Scans whichever of the three
// catalogs are present (via the shared LibraryCatalogCache -- a repeat
// analyze()/Re-Analyze on an unchanged stick pays no disk-read cost at
// all), scopes them to playlistName when it's non-empty, then runs the
// exact same real diff+direction logic (domain::SyncLibraries) once per
// pair actually available on this stick, combining every pair's
// actionable plans into one list.
// The cue copies the stick's log records, oldest first (#121).
std::vector<domain::LoggedCueCopy> readLoggedCueCopies(const QString &rekordboxPath)
{
    std::vector<domain::LoggedCueCopy> copies;
    const fs::path log = infrastructure::paths::stickOperationLog(pathFromQString(stickRootOf(rekordboxPath)));
    std::ifstream in(log, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (auto copy = domain::parseLoggedCueCopy(line)) {
            copies.push_back(std::move(*copy));
        }
    }
    return copies;
}

// An Engine MP3 row and a rekordbox row, both with cues, the Engine row
// no longer import-marked: the only pairs domain::findCueOffsets can find
// anything in. Asked before a pair is kept, so a big library keeps few.
bool mayHoldCueOffsets(const domain::SyncMatch &pair)
{
    const bool engineIsB = pair.trackB.format == "engine";
    const domain::Track &engine = engineIsB ? pair.trackB : pair.trackA;
    const domain::Track &other = engineIsB ? pair.trackA : pair.trackB;
    return engine.format == "engine" && (other.format == "rekordbox" || other.format == "onelibrary")
        && !engine.cues.empty() && !other.cues.empty() && !engine.engineCuesOnRekordboxTimeline
        && engine.streamingSource.empty() && !engine.filePath.empty() && domain::hasMp3Extension(engine.filePath);
}

// Cues a few milliseconds off (#121) among the matched pairs. Only MP3s
// are read, and only rows with cues on both sides. The lead-ins come from
// the stick's metadata cache, where the Engine reader's pass left them
// (CachedSampleRates::leadInOf): an analysis after every save must not read
// the start of every MP3 on the stick again.
std::vector<domain::CueOffsetFinding> findCueOffsetsIn(const std::vector<domain::SyncMatch> &pairs,
                                                      const QString &rekordboxPath, const std::string &stickRoot,
                                                      const application::CancellationToken &cancel)
{
    std::vector<domain::CueOffsetFinding> findings;
    std::optional<std::vector<domain::LoggedCueCopy>> log;
    const auto probe = SyncAnalysisStaging::metadataProbe();
    std::optional<infrastructure::local::CachedSampleRates> rates;
    if (!stickRoot.empty() && probe) {
        rates.emplace(stickRoot, *probe);
    }
    for (const domain::SyncMatch &pair : pairs) {
        cancel.throwIfCancelled();
        const bool engineIsB = pair.trackB.format == "engine";
        const domain::Track &engine = engineIsB ? pair.trackB : pair.trackA;
        const domain::Track &other = engineIsB ? pair.trackA : pair.trackB;
        if (!mayHoldCueOffsets(pair)) {
            continue;
        }
        if (!log) {
            log = readLoggedCueCopies(rekordboxPath);
        }
        const auto leadIn = rates ? rates->leadInOf(engine.filePath)
                                  : infrastructure::audio::readMp3LeadIn(engine.filePath);
        if (auto finding = domain::findCueOffsets(other, engine, leadIn, *log)) {
            findings.push_back(std::move(*finding));
        }
    }
    if (rates) {
        rates->save();  // a read-only or full stick costs a read next time, never the pass
    }
    return findings;
}

SyncTaskResult runAnalyzeTask(QString rekordboxPath, QString enginePath, QString playlistName,
                               application::AnalysisCopyOptions analysisOptions, std::string stickRoot,
                               std::shared_ptr<QtProgressReporter> reporter, application::CancellationToken cancel)
{
    SyncTaskResult result;
    try {
        bool hasRekordbox = !rekordboxPath.isEmpty();
        bool hasEngine = !enginePath.isEmpty();
        bool hasOneLibrary = hasRekordbox
            && infrastructure::onelibrary::OneLibraryCueWriter::existsFor(rekordboxPath.toStdString());

        std::vector<domain::Track> rekordboxTracks, engineTracks, oneLibraryTracks;
        std::chrono::system_clock::time_point rekordboxMtime, engineMtime, oneLibraryMtime;

        auto &catalogCache = LibraryCatalogCache::instance();

        // One bar for the whole analysis (#58): the reads the cache still
        // has to make, then "Comparing cues" per pair, which is at most
        // one tick per track of the smaller catalog. Counted from the
        // catalogs' row counts before the first read; a catalog that
        // cannot be counted leaves the bar sweeping. Each catalog is
        // counted once: the cache remembers its count for plannedUnits()
        // and the countTracks() after it, and looks at the token before
        // each count it makes (a OneLibrary count is a key derivation no
        // token interrupts).
        std::optional<size_t> planned = 0;
        std::optional<size_t> rekordboxRows, engineRows, oneLibraryRows;
        const auto addUnits = [&planned](std::optional<size_t> units) {
            if (planned && units) {
                *planned += *units;
            } else {
                planned = std::nullopt;
            }
        };
        if (hasRekordbox) {
            addUnits(catalogCache.plannedUnits("rekordbox", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full,
                                               cancel));
            rekordboxRows = catalogCache.countTracks("rekordbox", rekordboxPath.toStdString(), cancel);
        }
        if (hasEngine) {
            addUnits(
                catalogCache.plannedUnits("engine", enginePath.toStdString(), LibraryCatalogCache::Detail::Full, cancel));
            engineRows = catalogCache.countTracks("engine", enginePath.toStdString(), cancel);
        }
        if (hasOneLibrary) {
            addUnits(catalogCache.plannedUnits("onelibrary", rekordboxPath.toStdString(),
                                               LibraryCatalogCache::Detail::Full, cancel));
            oneLibraryRows = catalogCache.countTracks("onelibrary", rekordboxPath.toStdString(), cancel);
        }
        if (hasRekordbox && hasEngine) {
            addUnits(rekordboxRows && engineRows ? std::optional<size_t>(std::min(*rekordboxRows, *engineRows))
                                                 : std::nullopt);
        }
        if (hasEngine && hasOneLibrary) {
            addUnits(engineRows && oneLibraryRows ? std::optional<size_t>(std::min(*engineRows, *oneLibraryRows))
                                                  : std::nullopt);
        }
        // With an analysis switch on, "Checking beat grids and waveforms":
        // at most one tick per track rekordbox and Engine share. With both
        // off nothing of it is planned, read or run.
        result.analysisApplies = hasRekordbox && hasEngine && !stickRoot.empty();
        const bool withAnalysis = (analysisOptions.grids || analysisOptions.waveforms) && result.analysisApplies;
        if (result.analysisApplies) {
            result.analysedGrids = analysisOptions.grids;
            result.analysedWaveforms = analysisOptions.waveforms;
        }
        if (withAnalysis) {
            addUnits(rekordboxRows && engineRows ? std::optional<size_t>(std::min(*rekordboxRows, *engineRows))
                                                 : std::nullopt);
        }
        application::PhasedProgress progress(*reporter, "Comparing the libraries", planned.value_or(0));

        if (hasRekordbox) {
            // Full, not Cues: matching refuses to pair two tracks of one
            // name while either length is unknown, and the lengths a
            // catalog leaves out are probed in the Full stage.
            rekordboxTracks = catalogCache.tracksFor("rekordbox", rekordboxPath.toStdString(),
                                                     LibraryCatalogCache::Detail::Full, progress, cancel);
            rekordboxMtime = fileMtime(pathToUtf8(pathFromQString(rekordboxPath) / "rekordbox" / "export.pdb"));
        }
        if (hasEngine) {
            engineTracks = catalogCache.tracksFor("engine", enginePath.toStdString(), LibraryCatalogCache::Detail::Full, progress, cancel);
            // Streaming tracks (TIDAL) have no real local file. Never
            // sync cues onto/from one. See domain::Track::streamingSource's
            // own doc comment.
            engineTracks.erase(std::remove_if(engineTracks.begin(), engineTracks.end(),
                                               [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                                engineTracks.end());
            engineMtime = fileMtime(pathToUtf8(pathFromQString(enginePath) / "Database2" / "m.db"));
        }
        if (hasOneLibrary) {
            oneLibraryTracks = catalogCache.tracksFor("onelibrary", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full, progress, cancel);
            oneLibraryMtime =
                fileMtime(infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(rekordboxPath.toStdString()));
        }

        collectPlaylistSummary(rekordboxTracks, engineTracks, oneLibraryTracks, result);
        if (hasRekordbox && hasEngine) {
            result.importState = infrastructure::engine::readRekordboxImportState(enginePath.toStdString(),
                                                                                   rekordboxPath.toStdString());
        }

        if (!playlistName.isEmpty()) {
            domain::TrackScope scope = domain::TrackScope::playlist(playlistName.toStdString());
            rekordboxTracks = domain::filterByScope(rekordboxTracks, scope);
            engineTracks = domain::filterByScope(engineTracks, scope);
            oneLibraryTracks = domain::filterByScope(oneLibraryTracks, scope);
        }

        result.rekordboxTrackCount = static_cast<int>(rekordboxTracks.size());
        result.engineTrackCount = static_cast<int>(engineTracks.size());
        result.oneLibraryTrackCount = static_cast<int>(oneLibraryTracks.size());

        std::vector<SyncPlan> actionable;
        // The matched pairs that may hold cues a few milliseconds off
        // (#121), whatever else Sync plans for them: a cue on both sides
        // within Sync's tolerance (half a beat) is the same cue to Sync,
        // however far from the sound one side has it.
        std::vector<domain::SyncMatch> offsetCandidates;
        auto addPairPlans = [&](const std::vector<domain::Track> &tracksA, const std::vector<domain::Track> &tracksB,
                                 std::chrono::system_clock::time_point mtimeA,
                                 std::chrono::system_clock::time_point mtimeB) {
            for (auto &plan : application::SyncLibraries().execute(tracksA, tracksB, mtimeA, mtimeB, progress)) {
                if (mayHoldCueOffsets(plan.match)) {
                    offsetCandidates.push_back(plan.match);
                }
                if (plan.direction != SyncPlan::Direction::None || plan.needsChoice) {
                    actionable.push_back(std::move(plan));
                }
            }
        };
        if (hasRekordbox && hasEngine) {
            addPairPlans(rekordboxTracks, engineTracks, rekordboxMtime, engineMtime);
        }
        if (hasEngine && hasOneLibrary) {
            // Only for the OneLibrary rows no DeviceLibrary row speaks for: a
            // row naming the same analysis file has the same cues (that
            // file is where a OneLibrary player takes them from, #59), and
            // a row at a path DeviceLibrary lists gets every DeviceLibrary
            // write mirrored into it. Planning those against Engine too
            // would decide one file twice, and whichever write landed last
            // would win. Matched against every row first, then narrowed:
            // see planEngineWithOneLibrary().
            for (auto &plan : application::planEngineWithOneLibrary(engineTracks, oneLibraryTracks, rekordboxTracks,
                                                                    engineMtime, oneLibraryMtime, progress)) {
                if (mayHoldCueOffsets(plan.match)) {
                    offsetCandidates.push_back(plan.match);
                }
                if (plan.direction != SyncPlan::Direction::None || plan.needsChoice) {
                    actionable.push_back(std::move(plan));
                }
            }
        }
        if (hasEngine && hasRekordbox) {
            result.cueOffsets = findCueOffsetsIn(offsetCandidates, rekordboxPath, stickRoot, cancel);
        }
        // rekordbox <-> OneLibrary is deliberately NOT planned as a pair.
        // For cues they are one source, not two: a OneLibrary row's cues
        // are read from, and written to, the analysis file its row names
        // (#59), which is the file the DeviceLibrary row for that track
        // names too. Comparing the two would compare a file with itself.
        //
        // Where they name different files (a re-analysed copy), or one
        // lists a track the other does not, that is a library that arrived
        // crooked: Library Health's to report and level, not a sync's.

        // Two different pairs can independently target the same third
        // catalog's track (e.g. both rekordbox and Engine have cues
        // OneLibrary lacks) -- neither pairwise SyncPlanner can see the
        // other pair, so it can't know this is happening. Split those
        // out into unresolved conflicts (requires a manual pick, see
        // SyncController::resolveConflict()) before anything below
        // treats `actionable` as safe to apply directly.
        //
        // First, though, every plan the planner could not settle. Those are
        // not plans at all but choices, each with its reason (see
        // SyncPlan::needsChoice), listed for a pick and never staged until
        // one is made.
        auto hotCueChoices = domain::CrossSourceConflictDetector::takeHotCueChoices(actionable,
                                                                                    application::normalizedPathKey);
        auto conflictSplit = domain::CrossSourceConflictDetector::detect(actionable);
        actionable = std::move(conflictSplit.nonConflicting);
        result.conflicts = std::move(hotCueChoices);
        for (auto &conflict : conflictSplit.conflicts) {
            result.conflicts.push_back(std::move(conflict));
        }

        // Waveforms are deliberately NOT loaded here -- see
        // sync_controller.hpp's own comment on SyncPlanList Model::
        // setPlans() for why eagerly decoding one per actionable track
        // (thousands, on a real library where most of it is actionable)
        // was the actual cause of a real "scanning takes forever" report,
        // confirmed at over 5 minutes on real removable media for a
        // ~1400-track library, against ~4 seconds for everything else in
        // this function combined. QML fetches a waveform on demand
        // instead, only for whichever rows are actually rendered.
        if (withAnalysis) {
            // A library the preview cannot read (an Engine library too old
            // for the writer) offers nothing: unsure is left alone, and the
            // cues are still compared as they always were.
            try {
                AnalysisPreview preview = previewAnalysis(rekordboxTracks, engineTracks, actionable, result.conflicts,
                                                          rekordboxPath, enginePath, stickRoot, analysisOptions,
                                                          progress, cancel);
                result.analysisOnly = std::move(preview.only);
                result.riders = std::move(preview.riders);
                result.analysisUnchecked = preview.unchecked;
            } catch (const application::OperationCancelled &) {
                throw;
            } catch (const std::exception &e) {
                qWarning("Sync Cue Points: rekordbox's analysis was not checked: %s", e.what());
                result.analysisCheckFailed = true;
            }
        }
        result.plans = std::move(actionable);
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace

SyncController::SyncController(QObject *parent) : StagedCueEditController(parent)
{
    // Every count the page shows is derived from the rows, and the rows
    // change from many places: a scan, the search, a tick, a staged mark,
    // a row leaving once its change is saved. The model announces all of
    // them, so none of those paths can forget to.
    connect(&m_model, &SyncPlanListModel::countsChanged, this, &SyncController::listChanged);
}

void SyncController::analyze(const QString &rekordboxPath, const QString &enginePath, const QString &playlistName)
{
    startAnalysis(rekordboxPath, enginePath, playlistName, false);
}

void SyncController::startAnalysis(const QString &rekordboxPath, const QString &enginePath,
                                   const QString &playlistName, bool restart)
{
    // Every request is answered now (docs/async-requests.md): the same
    // scope again by the analysis already running, another scope by a new
    // one that supersedes it. The scope is recorded here, for the request
    // that is actually read -- it used to be recorded before a busy check
    // that then dropped the request, so the page named one playlist over
    // another's plans.
    // Preferences' analysis switches, read here on the GUI thread: with one
    // on, the analysis also works out the tracks that need only rekordbox's
    // analysis. Part of the key, so a switch turned meanwhile is a new
    // request.
    const SyncAnalysisStaging staging =
        rekordboxPath.isEmpty() ? SyncAnalysisStaging{} : SyncAnalysisStaging::fromPreferences(rekordboxPath);
    application::AnalysisCopyOptions analysisOptions = staging.options;
    if (rekordboxPath.isEmpty()) {
        // No rekordbox to copy from (and a default staging's options say
        // both parts).
        analysisOptions.grids = false;
        analysisOptions.waveforms = false;
    }
    const QString key = rekordboxPath + QLatin1Char('\n') + enginePath + QLatin1Char('\n') + playlistName
        + QStringLiteral("\nanalysis:%1%2").arg(analysisOptions.grids ? 1 : 0).arg(analysisOptions.waveforms ? 1 : 0);
    if (!restart && scanServes(key)) {
        return;
    }
    m_currentPlaylistName = playlistName;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    attachSession();
    setErrorMessage({});
    setStatusMessage({});
    setScanProgress(0, 0);
    const QString catalog = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
    auto reporter = makeReporter();
    startScan<SyncTaskResult>(
        key,
        stickRootOf(catalog), restart,
        [rekordboxPath, enginePath, playlistName, analysisOptions, stickRoot = staging.stickRoot,
         reporter](application::CancellationToken cancel) {
            return runAnalyzeTask(rekordboxPath, enginePath, playlistName, analysisOptions, stickRoot, reporter,
                                  cancel);
        },
        [this](SyncTaskResult &&result) { onAnalyzeFinished(std::move(result)); });
}

void SyncController::onAnalyzeFinished(SyncTaskResult &&result)
{
    if (result.cancelled) {
        emit scanCancelled();
        return;
    }
    if (!result.errorMessage.isEmpty()) {
        setErrorMessage(result.errorMessage);
        return;
    }

    m_rekordboxTrackCount = result.rekordboxTrackCount;
    m_engineTrackCount = result.engineTrackCount;
    m_oneLibraryTrackCount = result.oneLibraryTrackCount;
    m_playlistNames = std::move(result.playlistNames);
    m_playlistTrackCounts = std::move(result.playlistTrackCounts);
    m_importState = result.importState;
    emit importStateChanged();
    // A rescan is a fresh snapshot -- any decision made against the
    // previous one no longer means anything (the plan it produced is
    // already gone too, replaced by whatever this scan found), so this
    // never tries to carry old resolutions forward.
    m_analysisApplies = result.analysisApplies;
    m_analysedGrids = result.analysedGrids;
    m_analysedWaveforms = result.analysedWaveforms;
    m_analysisUnchecked = result.analysisUnchecked;
    m_analysisCheckFailed = result.analysisCheckFailed;
    m_model.setAnalysis(std::move(result.plans), std::move(result.conflicts), result.analysisOnly, result.riders);
    m_cueOffsets.setFindings(std::move(result.cueOffsets));
    // A repair staged before this re-analysis keeps its mark if its row is
    // still listed; one whose row is gone is still in the session.
    for (const auto &[key, side] : m_cueOffsetStagedSides) {
        const int row = m_cueOffsets.indexOfKey(key);
        if (row >= 0) {
            m_cueOffsets.setStagedSide(row, side);
        }
    }
    emit cueOffsetsChanged();
    // Plans staged before this re-analyze keep their mark if they are
    // still listed (the change itself lives in the session either way).
    for (const auto &[targetKey, info] : m_stagedByKey) {
        int index = indexOfStagedKey(targetKey);
        if (index >= 0) {
            m_model.setStaged(index, true, info.description);
        }
    }
    emit analysisChanged();
}

void SyncController::search(const QString &query)
{
    m_model.setFilter(query);
}

void SyncController::setIncluded(int planIndex, bool included)
{
    m_model.setIncluded(planIndex, included);
}

void SyncController::setAllIncluded(bool included)
{
    m_model.setAllIncluded(included);
}

bool SyncController::wouldStage(int planIndex, bool matchingSearchOnly) const
{
    if (!m_model.included(planIndex) || m_stagedByKey.count(m_model.planKeyAt(planIndex))) {
        return false;
    }
    if (m_model.plans()[static_cast<size_t>(planIndex)].direction == SyncPlan::Direction::None) {
        return false;
    }
    if (cueOffsetStagedFor(m_model.plans()[static_cast<size_t>(planIndex)].match)) {
        return false;
    }
    return !matchingSearchOnly || m_model.isPlanVisible(planIndex);
}

void SyncController::stageSelected(bool matchingSearchOnly)
{
    if (busy()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    std::vector<int> staged;
    int withAnalysis = 0;
    int analysisOnly = 0;
    const int count = m_model.planCount();
    // Preferences read once for the batch, not once a track.
    const SyncAnalysisStaging staging = SyncAnalysisStaging::fromPreferences(m_rekordboxPath);
    // The rows say what the analysis previewed with the switches as they
    // were then: staged with others, a row would write what it does not
    // say, or leave out what it says. Nothing is staged until the list is
    // read again.
    if (analysisSwitchesMoved(staging)) {
        setErrorMessage(analysisSwitchesMovedMessage());
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (!wouldStage(i, matchingSearchOnly)) {
            continue;
        }
        if (m_model.isAnalysisOnly(i)) {
            if (!stageAnalysisOnly(i, staging)) {
                // Refused (no session, a save running, the lock, or the
                // switches turned off since): it said so, and the rest
                // would be refused alike.
                break;
            }
            ++analysisOnly;
        } else {
            withAnalysis += stagePlan(i, &staging) ? 1 : 0;
            staged.push_back(i);
        }
        if (session() && !session()->lockHeld()) {
            return;  // refused at the first one; no point trying the rest
        }
    }
    const int cueRows = static_cast<int>(staged.size());
    if (cueRows > 0) {
        // With the analysis switches on, said as Sync after Rekordbox Export
        // says it; with them off, the line it always was.
        QString analysis;
        if (withAnalysis > 0) {
            analysis = QStringLiteral(" The tracks whose cues go to Engine also get rekordbox's %1, as set in "
                                      "Preferences.")
                           .arg(staging.partsInWords());
        }
        if (analysisOnly > 0) {
            setStagedStatusMessage(QStringLiteral("Staged %1 track(s). Press Save to write them to the stick.")
                                       .arg(cueRows + analysisOnly)
                                   + analysis
                                   + QStringLiteral(" %1 of them, their cues already in step, get only rekordbox's %2.")
                                         .arg(analysisOnly)
                                         .arg(staging.partsInWords()));
        } else {
            setStagedStatusMessage(
                QStringLiteral("Staged %1 track(s). Press Save to write the cues to the stick.").arg(cueRows)
                + analysis);
        }
        noticeCuesLeftOut(staged);
    } else if (analysisOnly > 0) {
        setStagedStatusMessage(QStringLiteral("Staged rekordbox's %1 for %2 track(s) whose cues are already in step. "
                                              "Press Save to write them to the stick.")
                                   .arg(staging.partsInWords())
                                   .arg(analysisOnly));
    }
}

bool SyncController::stageAnalysisOnly(int index, const SyncAnalysisStaging &staging)
{
    if (!m_model.isAnalysisOnly(index)) {
        return false;
    }
    // The analysis found it with the switches as they were, and
    // Preferences may have changed since: then nothing of it is staged.
    const SyncRowAnalysis parts = m_model.analysis(index);
    if (!parts.any() || analysisSwitchesMoved(staging) || m_rekordboxPath.isEmpty() || m_enginePath.isEmpty()) {
        setErrorMessage(analysisSwitchesMovedMessage());
        return false;
    }
    if (!session()) {
        attachSession();
    }
    // As stagePlan() hints a rider's: the most Engine writes this save may
    // make, for the scratch-copy decision.
    int itemCountHint = 0;
    for (int i = 0; i < m_model.planCount(); ++i) {
        if (m_model.planKeyAt(i).section(':', 0, 0) == QLatin1String("engine")) {
            ++itemCountHint;
        }
    }
    const SyncPlan &plan = m_model.plans()[static_cast<size_t>(index)];
    SyncAnalysisChange::Track track;
    track.rekordbox = plan.match.trackA;
    track.engineTrackId = plan.match.trackB.sourceId;
    track.engineFilePath = plan.match.trackB.filePath;
    track.analysisDigest = m_model.analysisDigest(index);
    // Exactly the parts the row said: a switch on since that it did not
    // preview, or a part already the same, is not written.
    auto change = std::make_unique<SyncAnalysisChange>(QStringLiteral("sync"), m_rekordboxPath, m_enginePath,
                                                       staging.stickRoot, std::move(track),
                                                       previewedParts(staging, parts), staging.probe, itemCountHint,
                                                       true);
    // One track of the save, as a row whose cues sync is.
    change->countAsUnit();
    startNewSyncedCount();
    return stageChange(index, m_model.planKeyAt(index), std::move(change));
}

bool SyncController::analysisSwitchesMoved(const SyncAnalysisStaging &staging) const
{
    // Nothing was previewed where there is nothing to copy between (no
    // rekordbox, no Engine): the switches say nothing about those rows.
    if (!m_analysisApplies || m_rekordboxPath.isEmpty()) {
        return false;
    }
    return staging.options.grids != m_analysedGrids || staging.options.waveforms != m_analysedWaveforms;
}

QString SyncController::analysisSwitchesMovedMessage()
{
    return QStringLiteral("Copying beat grids and waveforms from rekordbox was changed in Preferences since the list "
                          "was read. Re-Analyze to update the list.");
}

application::AnalysisCopyOptions SyncController::previewedParts(const SyncAnalysisStaging &staging,
                                                                const SyncRowAnalysis &parts)
{
    application::AnalysisCopyOptions options = staging.options;
    options.grids = parts.grid;
    options.waveforms = parts.waveform;
    return options;
}

QString SyncController::analysisUncheckedText() const
{
    if (m_analysisCheckFailed) {
        return QStringLiteral("Beat grids and waveforms could not be checked.");
    }
    if (m_analysisUnchecked > 0) {
        return m_analysisUnchecked == 1
            ? QStringLiteral("Beat grids and waveforms could not be checked for 1 track.")
            : QStringLiteral("Beat grids and waveforms could not be checked for %1 tracks.").arg(m_analysisUnchecked);
    }
    return {};
}

void SyncController::setAnalysisUncheckedForTesting(int tracks, bool wholeCheckFailed)
{
    m_analysisUnchecked = tracks;
    m_analysisCheckFailed = wholeCheckFailed;
    emit analysisChanged();
}

void SyncController::startNewSyncedCount()
{
    if (stagedCount() == 0 && m_syncedCueCount != 0) {
        m_syncedCueCount = 0;
        m_gainedByChangeId.clear();
        emit syncedChanged();
    }
}

void SyncController::setAnalysisOnlyForTesting(int tracks, int grids, int waveforms, int completed)
{
    std::vector<domain::SyncPlan> plans;
    for (int i = 0; i < m_model.planCount(); ++i) {
        if (!m_model.isAnalysisOnly(i)) {
            plans.push_back(m_model.plans()[static_cast<size_t>(i)]);
        }
    }
    std::vector<SyncAnalysisOnlyTrack> only;
    for (int i = 0; i < tracks; ++i) {
        SyncAnalysisOnlyTrack track;
        const std::string n = std::to_string(i + 1);
        track.rekordbox.format = "rekordbox";
        track.rekordbox.sourceId = "test-" + n;
        track.rekordbox.title = "Analysis Only " + n;
        track.rekordbox.artist = "Seabass Test";
        track.rekordbox.filename = "Analysis Only " + n + ".mp3";
        track.engine = track.rekordbox;
        track.engine.format = "engine";
        track.engine.sourceId = "test-engine-" + n;
        track.analysis.grid = i < grids;
        track.analysis.waveform = i < waveforms;
        track.analysis.completes = i < completed;
        only.push_back(std::move(track));
    }
    m_model.setAnalysis(std::move(plans), m_model.conflicts(), only);
}

namespace
{
const QString LeftOutNoticeSuppressedKey = QStringLiteral("sync/cuesLeftOutNotice/suppressed");
const QString LeftOutNoticeSuppressedTracksKey = QStringLiteral("sync/cuesLeftOutNotice/suppressedTracks");
}  // namespace

// The file name, not a catalog row id: the same track on another stick
// or after a re-export is the same track to the DJ, and the notice is
// about the track.
QString SyncController::leftOutKeyFor(const SyncPlan &plan)
{
    const domain::Track &nonEngine = plan.match.trackA.format == "engine" ? plan.match.trackB : plan.match.trackA;
    return QString::fromStdString(nonEngine.filename).toLower();
}

void SyncController::noticeCuesLeftOut(const std::vector<int> &stagedIndices)
{
    QSettings settings = openSeabassSettings();
    if (settings.value(LeftOutNoticeSuppressedKey, false).toBool()) {
        return;
    }
    const QStringList suppressedTracks = settings.value(LeftOutNoticeSuppressedTracksKey).toStringList();
    QVariantList tracks;
    QStringList keys;
    for (int index : stagedIndices) {
        const SyncPlan &plan = m_model.plans()[static_cast<size_t>(index)];
        const domain::Track &target = plan.direction == SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
        if (plan.cuesLeftOut.empty() || target.format != "engine") {
            continue;
        }
        const QString key = leftOutKeyFor(plan);
        if (suppressedTracks.contains(key)) {
            continue;
        }
        const domain::Track &source = plan.direction == SyncPlan::Direction::ToB ? plan.match.trackA : plan.match.trackB;
        QVariantMap entry;
        entry["title"] = QString::fromStdString(source.title);
        entry["artist"] = QString::fromStdString(source.artist);
        entry["count"] = static_cast<int>(plan.cuesLeftOut.size());
        tracks << entry;
        keys << key;
    }
    if (tracks.isEmpty()) {
        return;
    }
    m_lastLeftOutKeys = keys;
    emit cuesLeftOutNoticed(tracks);
}

void SyncController::suppressCuesLeftOutNotice(bool forTheseTracks, bool ever)
{
    QSettings settings = openSeabassSettings();
    if (ever) {
        settings.setValue(LeftOutNoticeSuppressedKey, true);
    }
    if (forTheseTracks) {
        QStringList tracks = settings.value(LeftOutNoticeSuppressedTracksKey).toStringList();
        for (const QString &key : m_lastLeftOutKeys) {
            if (!tracks.contains(key)) {
                tracks << key;
            }
        }
        settings.setValue(LeftOutNoticeSuppressedTracksKey, tracks);
    }
}

void SyncController::resetCuesLeftOutNotice()
{
    QSettings settings = openSeabassSettings();
    settings.remove(LeftOutNoticeSuppressedKey);
    settings.remove(LeftOutNoticeSuppressedTracksKey);
}

void SyncController::markRekordboxImported()
{
    if (busy() || m_importMarkStaged || !m_importState.playerWillOfferImport()) {
        return;
    }
    setErrorMessage({});
    setStatusMessage({});
    if (!session()) {
        attachSession();
    }
    if (!session()) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return;
    }
    if (!session()->stage(std::make_unique<MarkRekordboxImportedChange>(m_enginePath, m_importState.librarySequence))) {
        return;  // the session reported the refusal; the page shows it
    }
    m_importMarkStaged = true;
    emit importStateChanged();
    setStagedStatusMessage(
        QStringLiteral("Staged marking this stick's rekordbox library as imported. Press Save to write it."));
}

void SyncController::unstageRekordboxImportMark()
{
    if (!m_importMarkStaged) {
        return;
    }
    if (session()) {
        session()->unstage(MarkRekordboxImportedChange::idFor());
    }
    m_importMarkStaged = false;
    emit importStateChanged();
}

void SyncController::setImportStateForTesting(bool playerWillOfferImport)
{
    m_importState = {};
    m_importState.hasEngineLibrary = true;
    m_importState.hasRekordboxLibrary = true;
    m_importState.librarySequence = playerWillOfferImport ? 2 : 1;
    m_importState.engineCounter = 1;
    emit importStateChanged();
}

// A landed mark, or any landed save: what the stick says now. One 24-byte
// read and one row; fine on the main thread.
void SyncController::rereadImportState()
{
    if (m_rekordboxPath.isEmpty() || m_enginePath.isEmpty()) {
        return;
    }
    m_importState = infrastructure::engine::readRekordboxImportState(m_enginePath.toStdString(),
                                                                      m_rekordboxPath.toStdString());
    emit importStateChanged();
}

void SyncController::onImportSessionChangeApplied(const QString &changeId)
{
    if (changeId == MarkRekordboxImportedChange::idFor()) {
        m_importMarkStaged = false;
    }
    if (auto gained = m_gainedByChangeId.find(changeId); gained != m_gainedByChangeId.end()) {
        // A change the save reached and found nothing to do for (its cues
        // changed since they were read) synced nothing: it is off the
        // pending list, and not counted.
        const bool skipped = session() && session()->lastSkippedIds().contains(changeId);
        if (!skipped) {
            m_syncedCueCount += gained->second;
        }
        m_gainedByChangeId.erase(gained);
        emit syncedChanged();
    }
    onCueOffsetChangeApplied(changeId);
    rereadImportState();
}

void SyncController::onImportSessionChangesDiscarded()
{
    m_importMarkStaged = false;
    emit importStateChanged();
    if (!m_cueOffsetChangeIds.empty()) {
        m_cueOffsetChangeIds.clear();
        m_cueOffsetStagedSides.clear();
        clearStagedStatusIfNothingStaged();
        m_cueOffsets.clearStaged();
        emit cueOffsetsChanged();
    }
}

namespace
{

QString trackKeyOf(const domain::Track &track)
{
    return QString::fromStdString(track.format + ":" + track.sourceId);
}

}  // namespace

// A change that reached the stick: a repair's own row goes once all of
// its changes have landed, and whatever else is listed for the tracks it
// wrote goes with it. A repair's row and a sync's plan of one pair were
// read before either wrote; neither is true once one has (the save is
// followed by a fresh read, onSaveFinished()).
void SyncController::onCueOffsetChangeApplied(const QString &changeId)
{
    QStringList written;
    for (auto it = m_cueOffsetChangeIds.begin(); it != m_cueOffsetChangeIds.end(); ++it) {
        auto &ids = it->second;
        const auto found = std::find(ids.begin(), ids.end(), changeId);
        if (found == ids.end()) {
            continue;
        }
        ids.erase(found);
        written << it->first.section(QLatin1Char('|'), 0, 0) << it->first.section(QLatin1Char('|'), 1, 1);
        if (ids.empty()) {
            m_cueOffsets.removeAt(m_cueOffsets.indexOfKey(it->first));
            m_cueOffsetStagedSides.erase(it->first);
            m_cueOffsetChangeIds.erase(it);
            clearStagedStatusIfNothingStaged();
        }
        break;
    }
    if (written.isEmpty() && changeId.startsWith(QStringLiteral("sync:"))) {
        written << changeId.mid(5);
    }
    if (written.isEmpty()) {
        return;
    }
    // The other list's rows for those tracks, unless something is staged
    // for them (then the save that staged it is the next to write).
    for (int row = m_cueOffsets.count() - 1; row >= 0; --row) {
        const QString key = CueOffsetListModel::keyOf(m_cueOffsets.findings()[static_cast<size_t>(row)]);
        if (m_cueOffsetChangeIds.count(key) == 0
            && (written.contains(key.section(QLatin1Char('|'), 0, 0))
                || written.contains(key.section(QLatin1Char('|'), 1, 1)))) {
            m_cueOffsets.removeAt(row);
        }
    }
    for (int index = m_model.planCount() - 1; index >= 0; --index) {
        const auto &match = m_model.plans()[static_cast<size_t>(index)].match;
        if ((written.contains(trackKeyOf(match.trackA)) || written.contains(trackKeyOf(match.trackB)))
            && m_stagedByKey.count(m_model.planKeyAt(index)) == 0) {
            m_model.removePlanAt(index);
        }
    }
    for (int index = m_model.conflictCount() - 1; index >= 0; --index) {
        const auto &conflict = m_model.conflicts()[static_cast<size_t>(index)];
        if (written.contains(trackKeyOf(conflict.sourceA)) || written.contains(trackKeyOf(conflict.sourceB))
            || written.contains(trackKeyOf(conflict.target))) {
            m_model.removeConflictAt(index);
        }
    }
    emit cueOffsetsChanged();
    emit listChanged();
}

// Every save that wrote or skipped anything is followed by a fresh read:
// both lists were read before it, and a row of one may be out of date
// once a change of the other has landed.
void SyncController::onSaveFinished(const QVariantMap &summary)
{
    // Undo Last Save is read again by reanalyzeAfterUndo(), once.
    if (summary.value(QStringLiteral("undo")).toBool()) {
        return;
    }
    if ((summary.value(QStringLiteral("written")).toInt() > 0 || summary.value(QStringLiteral("skipped")).toInt() > 0)
        && (!m_rekordboxPath.isEmpty() || !m_enginePath.isEmpty())) {
        startAnalysis(m_rekordboxPath, m_enginePath, m_currentPlaylistName, true);
    }
}

bool SyncController::stageCueOffset(int row, const QString &side)
{
    if (row < 0 || row >= m_cueOffsets.count() || busy()) {
        return false;
    }
    const domain::CueOffsetFinding &finding = m_cueOffsets.findings()[static_cast<size_t>(row)];
    // No side: the cues whose evidence names one, each on its own. A side:
    // the DJ's pick for the cues nothing explains; the known ones keep
    // theirs.
    std::optional<domain::CueOffsetSide> pick;
    if (side == QStringLiteral("engine")) {
        pick = domain::CueOffsetSide::Engine;
    } else if (side == QStringLiteral("rekordbox")) {
        pick = domain::CueOffsetSide::Rekordbox;
    } else if (!side.isEmpty()) {
        return false;
    }
    const bool onEngine = finding.moves(domain::CueOffsetSide::Engine, pick);
    const bool onRekordbox = finding.moves(domain::CueOffsetSide::Rekordbox, pick);
    if (!onEngine && !onRekordbox) {
        return false;  // unsure: only the DJ's pick of a side moves it
    }
    if (!session()) {
        attachSession();
    }
    LibraryEditSession *s = session();
    if (!s) {
        setErrorMessage("This stick's library could not be identified; nothing was changed.");
        return false;
    }
    if (s->writing()) {
        setErrorMessage("A save is running. Stage more once it has finished.");
        return false;
    }
    const QString key = CueOffsetListModel::keyOf(finding);
    if (syncStagedFor(finding)) {
        // A sync staged for this pair writes its cues from what it read:
        // one of the two would undo the other.
        setErrorMessage(QStringLiteral("\"%1\" has a sync staged. Save that first, then move its cues.")
                            .arg(QString::fromStdString(finding.rekordbox.title)));
        return false;
    }
    unstageCueOffset(row);
    std::vector<std::unique_ptr<PendingChange>> changes;
    std::vector<QString> ids;
    for (const auto moved : {domain::CueOffsetSide::Engine, domain::CueOffsetSide::Rekordbox}) {
        if (finding.moves(moved, pick)) {
            changes.push_back(std::make_unique<CueOffsetRepairChange>(m_rekordboxPath, m_enginePath, finding, moved,
                                                                      pick, m_cueOffsets.count()));
            ids.push_back(changes.back()->id());
        }
    }
    if (!s->stageAll(std::move(changes))) {
        return false;  // the session said why
    }
    m_cueOffsetChangeIds[key] = ids;
    m_cueOffsetStagedSides[key] = pick ? sideName(*pick) : QStringLiteral("known");
    m_cueOffsets.setStagedSide(row, pick ? sideName(*pick) : QStringLiteral("known"));
    setStagedStatusMessage(QStringLiteral("Staged. Press Save to move the cues on the stick."));
    emit cueOffsetsChanged();
    return true;
}

void SyncController::unstageCueOffset(int row)
{
    if (row < 0 || row >= m_cueOffsets.count()) {
        return;
    }
    const auto it = m_cueOffsetChangeIds.find(CueOffsetListModel::keyOf(m_cueOffsets.findings()[static_cast<size_t>(row)]));
    if (it == m_cueOffsetChangeIds.end()) {
        return;
    }
    if (LibraryEditSession *s = session()) {
        QStringList ids;
        for (const QString &id : it->second) {
            ids << id;
        }
        s->unstageAll(ids);
    }
    m_cueOffsetStagedSides.erase(it->first);
    m_cueOffsetChangeIds.erase(it);
    m_cueOffsets.setStagedSide(row, QString());
    clearStagedStatusIfNothingStaged();
    emit cueOffsetsChanged();
}

// A sync staged that reads or writes either track of the finding's pair:
// its target, or its source (whose cues it copies as they were read).
bool SyncController::syncStagedFor(const domain::CueOffsetFinding &finding)
{
    const QString rekordboxKey = trackKeyOf(finding.rekordbox);
    const QString engineKey = trackKeyOf(finding.engine);
    for (const auto &[targetKey, info] : m_stagedByKey) {
        (void)info;
        if (targetKey == rekordboxKey || targetKey == engineKey) {
            return true;
        }
        const int index = indexOfStagedKey(targetKey);
        if (index < 0) {
            continue;
        }
        const auto &match = m_model.plans()[static_cast<size_t>(index)].match;
        for (const QString &key : {trackKeyOf(match.trackA), trackKeyOf(match.trackB)}) {
            if (key == rekordboxKey || key == engineKey) {
                return true;
            }
        }
    }
    return false;
}

bool SyncController::cueOffsetStagedFor(const domain::SyncMatch &pair) const
{
    for (const auto &[key, ids] : m_cueOffsetChangeIds) {
        (void)ids;
        for (const domain::Track *track : {&pair.trackA, &pair.trackB}) {
            const QString trackKey = trackKeyOf(*track);
            if (key.section(QLatin1Char('|'), 0, 0) == trackKey || key.section(QLatin1Char('|'), 1, 1) == trackKey) {
                return true;
            }
        }
    }
    return false;
}

int SyncController::stageKnownCueOffsets()
{
    int staged = 0;
    QStringList refused;
    setErrorMessage({});
    for (int row = 0; row < m_cueOffsets.count(); ++row) {
        const auto &finding = m_cueOffsets.findings()[static_cast<size_t>(row)];
        if (!finding.hasKnown() || !m_cueOffsets.stagedSide(row).isEmpty()) {
            continue;
        }
        if (stageCueOffset(row)) {
            ++staged;
        } else {
            refused << QString::fromStdString(finding.rekordbox.title.empty() ? finding.rekordbox.filename
                                                                                : finding.rekordbox.title);
        }
    }
    if (staged > 0) {
        setStagedStatusMessage(QStringLiteral("Staged %1 track(s). Press Save to move their cues on the stick.").arg(staged));
    }
    if (!refused.isEmpty()) {
        // Every refusal named, not only the last one's reason.
        setErrorMessage(QStringLiteral("Not staged: %1. A sync staged for a track, or a save running, keeps its "
                                       "cues where they are until it is saved.")
                            .arg(refused.join(QStringLiteral(", "))));
    }
    return staged;
}

void SyncController::resolveConflict(int conflictIndex, bool useSourceA)
{
    if (conflictIndex < 0 || conflictIndex >= m_model.conflictCount()) {
        return;
    }
    // A copy: the decision leaves the model below, before this is done.
    const domain::CrossSourceSyncConflict conflict = m_model.conflicts()[static_cast<size_t>(conflictIndex)];

    domain::SyncPlan plan;
    if (conflict.samePair) {
        // One pair's own two sides: the chosen side's hot cues go onto the
        // other side of that same pair, with the memory cues the planner
        // prepared for that direction.
        plan.kind = domain::SyncPlan::Kind::Conflict;
        plan.match.trackA = conflict.sourceA;
        plan.match.trackB = conflict.sourceB;
        plan.direction = useSourceA ? domain::SyncPlan::Direction::ToB : domain::SyncPlan::Direction::ToA;
        plan.cuesToApply = useSourceA ? conflict.cuesFromA : conflict.cuesFromB;
    } else {
        plan.kind = domain::SyncPlan::Kind::AOnly;
        plan.match.trackA = useSourceA ? conflict.sourceA : conflict.sourceB;
        plan.match.trackB = conflict.target;
        plan.direction = domain::SyncPlan::Direction::ToB;
        plan.cuesToApply = useSourceA ? conflict.cuesFromA : conflict.cuesFromB;
    }
    plan.cuesLeftOut = conflict.cuesLeftOut;
    plan.positionToleranceMs = conflict.positionToleranceMs;
    m_model.addPlan(std::move(plan));
    // The decision is the edit: staged right away, Save writes it. The
    // decision leaves the list only once it is staged; refused, the plan
    // goes again and the choice stays to be made.
    const int index = m_model.planCount() - 1;
    bool staged = false;
    stagePlan(index, nullptr, &staged);
    if (!staged) {
        m_model.removePlanAt(index);
        return;
    }
    m_model.removeConflictAt(conflictIndex);
    noticeCuesLeftOut({m_model.planCount() - 1});
}

// The base wires the session's state and staged-change signals; the only
// part specific to this page is that a sync spans two catalogs, so the
// session is told about both paths.
//
// A row whose change lands is dropped rather than re-derived (see
// StagedCueEditController::onSessionChangeApplied()), and so are the
// rows of the other list for the same tracks (onCueOffsetChangeApplied()):
// a sync and a repair of one pair were both read before either wrote.
// After the save the whole page is read again (onSaveFinished()).
void SyncController::attachSession()
{
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    attachSessionForPath(any);
    if (LibraryEditSession *s = session()) {
        s->setLibraryPaths(m_rekordboxPath, m_enginePath);
        // The base disconnects everything from a session it leaves, these
        // included, so they are made again for a new one and once only.
        if (s != m_importSession) {
            m_importSession = s;
            connect(s, &LibraryEditSession::changeApplied, this, &SyncController::onImportSessionChangeApplied);
            connect(s, &LibraryEditSession::changesDiscarded, this, &SyncController::onImportSessionChangesDiscarded);
            connect(s, &LibraryEditSession::saveFinished, this, &SyncController::onSaveFinished);
        }
    }
}

bool SyncController::stagePlan(int index, const SyncAnalysisStaging *staging, bool *staged)
{
    if (staged) {
        *staged = false;
    }
    const auto &plans = m_model.plans();
    if (index < 0 || static_cast<size_t>(index) >= plans.size()) {
        return false;
    }
    const SyncPlan &plan = plans[static_cast<size_t>(index)];
    if (plan.direction == SyncPlan::Direction::None) {
        return false;
    }
    if (cueOffsetStagedFor(plan.match)) {
        // Both would write this pair's cues, each from what it read: the
        // second would undo the first.
        setErrorMessage(QStringLiteral("\"%1\" has cues staged to move a few milliseconds. Save that first, then "
                                       "sync it.")
                            .arg(QString::fromStdString(plan.match.trackA.title)));
        return false;
    }
    if (!session()) {
        attachSession();
    }
    // How many writes this save may make against the target's database
    // (drives the scratch-copy decision): every listed plan for that
    // format, the most that could be staged.
    QString targetKey = m_model.planKeyAt(index);
    QString targetFormat = targetKey.section(':', 0, 0);
    int itemCountHint = 0;
    for (int i = 0; i < m_model.planCount(); ++i) {
        if (m_model.planKeyAt(i).section(':', 0, 0) == targetFormat) {
            itemCountHint++;
        }
    }
    startNewSyncedCount();
    auto change = std::make_unique<SyncPlanChange>(m_rekordboxPath, m_enginePath, plan, itemCountHint);
    // Cues from rekordbox (either format) onto Engine: the row also gets
    // rekordbox's analysis when Preferences asks for it, inside the same
    // change, so the row stays one staged change. Every other direction,
    // and both switches off, stage the change they always did.
    const domain::Track &source = change->source();
    const domain::Track &target = change->target();
    // Only for a row the analysis' preview gave analysis to (its words on
    // the row), with that preview's parts and digest: a track the preview
    // could not check, or left out (an Engine row matched twice), gets
    // none, and a row says all it writes.
    const SyncRowAnalysis parts = m_model.analysis(index);
    if (target.format == "engine" && (source.format == "rekordbox" || source.format == "onelibrary")
        && !m_rekordboxPath.isEmpty() && parts.any()) {
        std::optional<SyncAnalysisStaging> own;
        if (!staging) {
            own = SyncAnalysisStaging::fromPreferences(m_rekordboxPath);
            staging = &*own;
        }
        // stageSelected() refuses the batch first; a lone stage that
        // finds the switches moved stages the cues alone.
        if (!analysisSwitchesMoved(*staging)) {
            SyncAnalysisChange::Track track;
            track.rekordbox = source;
            track.engineTrackId = target.sourceId;
            track.engineFilePath = target.filePath;
            track.analysisDigest = m_model.analysisDigest(index);
            if (source.format == "onelibrary") {
                // Its id is not export.pdb's: its analysis is read from the
                // file its row names, and a row naming none is left alone.
                track.analysisFile = source.analysisFile;
                if (track.analysisFile.empty()) {
                    track.leaveAloneBecause = QStringLiteral("its OneLibrary row names no analysis file");
                }
            }
            auto rider = std::make_unique<SyncAnalysisChange>(QStringLiteral("sync"), m_rekordboxPath, m_enginePath,
                                                              staging->stickRoot, std::move(track),
                                                              previewedParts(*staging, parts), staging->probe,
                                                              itemCountHint, true);
            rider->rideIn(change->id());
            change->setAnalysisRider(std::move(rider));
        }
    }
    const bool withAnalysis = change->hasAnalysisRider();
    const domain::CueChange gained = SyncPlanListModel::cueChangeOf(plan);
    m_gainedByChangeId[change->id()] = gained.gainedHot + gained.gainedMemory;
    // Written only onto the cues it read (H1 of #121's review): a save
    // since the read, of this page or another, is not written over.
    change->setCheckTargetAsRead(true);
    const bool done = stageChange(index, targetKey, std::move(change));
    if (staged) {
        *staged = done;
    }
    return done && withAnalysis;
}

}  // namespace seabass::gui
