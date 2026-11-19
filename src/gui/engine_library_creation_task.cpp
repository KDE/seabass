// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/engine_library_creation_task.hpp"

#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "application/phased_progress.hpp"
#include "gui/edit/changes/copy_analysis_change.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/local/analysis_file_facts.hpp"
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/rekordbox_analysis_source.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::gui
{

namespace
{

using infrastructure::engine::EngineLibraryCreator;
using infrastructure::engine::EngineSchemaGeneration;

EngineSchemaGeneration schemaFromInt(int value)
{
    switch (value) {
    case 0: return EngineSchemaGeneration::V1;
    case 1: return EngineSchemaGeneration::V2;
    case 2: return EngineSchemaGeneration::V3;
    default: return EngineSchemaGeneration::V3;  // the page's default, see EngineLibraryCreatorPage.qml
    }
}

// What the copy of rekordbox's analysis did to the new library: the note's
// counts, and the ledger lines for the grids it wrote.
struct NewLibraryAnalysis
{
    AnalysisNoteCounts counts;
    infrastructure::local::BeatGridLedger ledgerEntries;
};

// rekordbox's analysis onto every row of the scratch library, as
// EngineLibraryCreator::ScratchLibraryStep. Each row is new and unanalysed:
// importAnalysisItem() completes it all or nothing (both parts asked for),
// from the file's own length. Reading rekordbox's analysis or the audio
// file writes nothing, so a failure there leaves that row for the player
// and is counted; a failure while writing throws, and the creation fails
// with the stick untouched.
void copyAnalysisIntoNewLibrary(const std::string &scratchLibrary, const std::string &engineLibraryPath,
                                const std::string &pioneer, const std::string &stickRoot,
                                const std::vector<domain::Track> &tracks,
                                const std::vector<EngineLibraryCreator::CreatedRow> &rows,
                                const application::AnalysisCopyOptions &options,
                                infrastructure::local::CachedSampleRates &rates, application::ProgressReporter &progress,
                                const application::CancellationToken &cancel, NewLibraryAnalysis &out)
{
    std::unique_ptr<infrastructure::rekordbox::AnlzPathIndex> index;
    try {
        index = std::make_unique<infrastructure::rekordbox::AnlzPathIndex>(pioneer);
    } catch (const std::exception &) {
        // Each track then finds its analysis file the slow way.
    }
    infrastructure::engine::LibdjinteropEngineCueWriter writer(scratchLibrary);
    // The rows name their files from the library's place on the stick, not
    // the scratch folder's: the MP3 lead-in that moves the grid onto
    // Engine's timeline is read there, as the creator read it for the cues.
    writer.setAudioRoot(engineLibraryPath);
    writer.setOverviewFileRoot(scratchLibrary);

    // A file two created rows name: one ledger key, two grids. Neither is
    // recorded, so a later copy leaves both alone (the safe side).
    std::map<std::string, int> rowsPerKey;
    for (const auto &row : rows) {
        const auto key = application::beatGridCopyItemFor(tracks[row.trackIndex], {}, stickRoot).ledgerKey;
        if (!key.empty()) {
            ++rowsPerKey[key];
        }
    }

    const application::AnalysisFileFactsSource facts = [&rates](const application::BeatGridCopyItem &of) {
        try {
            return infrastructure::local::analysisFileFactsOf(of, rates);
        } catch (const application::OperationCancelled &) {
            throw;
        } catch (const std::exception &) {
            // A file that cannot be read gives no length: left for the player.
            return application::AnalysisFileFacts{};
        }
    };

    progress.start("Copying rekordbox's analysis", rows.size());
    std::size_t done = 0;
    for (const auto &row : rows) {
        if (cancel.cancelled()) {
            throw application::OperationCancelled();
        }
        const application::BeatGridCopyItem item =
            application::beatGridCopyItemFor(tracks[row.trackIndex], std::to_string(row.engineTrackId), stickRoot);
        application::RekordboxAnalysis analysis;
        bool readable = true;
        try {
            analysis = infrastructure::local::rekordboxAnalysisOf(pioneer, item, index.get());
        } catch (const std::exception &e) {
            AnalysisSaveTally::Row left;
            left.otherReason = QStringLiteral("rekordbox's analysis could not be read (%1)").arg(QString::fromUtf8(e.what()));
            out.counts.add(left);
            readable = false;
        }
        if (readable) {
            // A new row: no ledger line of its own to hand on.
            const application::AnalysisItemOutcome outcome =
                application::importAnalysisItem(item, analysis, writer, {}, options, facts);
            out.counts.add(analysisTallyRowOf(outcome.result));
            const auto &grid = outcome.result.grid;
            if (grid.outcome == application::BeatGridWriteResult::Outcome::Written && !item.ledgerKey.empty()
                && !grid.gridFingerprint.empty() && rowsPerKey[item.ledgerKey] == 1) {
                out.ledgerEntries[item.ledgerKey] = grid.gridFingerprint;
            }
        }
        progress.tick(++done);
    }
    progress.finish();
}

}  // namespace

EngineLibraryCreationTaskResult runEngineLibraryCreation(const QString &rekordboxPath, int schemaGeneration,
                                                         const application::AnalysisCopyOptions &analysisOptions,
                                                         application::ProgressReporter &reporter,
                                                         application::CancellationToken cancel)
{
    EngineLibraryCreationTaskResult result;
    const bool copyAnalysis = analysisOptions.grids && analysisOptions.waveforms;
    // One bar (#58): the rekordbox read the cache still has to make, one
    // unit per track created, one per track given rekordbox's analysis
    // when that is asked for, and the copy to the stick. The playlists
    // stretch is a few dozen units nobody can count before the read and
    // is left out of the plan; it runs the bar a moment early.
    // The plan counts rekordbox once (the cache remembers the count for
    // the countTracks() after plannedUnits()) and looks at the token
    // first, so a stop before the read ends here as cancelled.
    auto &cache = LibraryCatalogCache::instance();
    try {
        const auto readUnits =
            cache.plannedUnits("rekordbox", rekordboxPath.toStdString(), LibraryCatalogCache::Detail::Full, cancel);
        const auto rows = cache.countTracks("rekordbox", rekordboxPath.toStdString(), cancel);
        application::PhasedProgress progress(
            reporter, "Creating the Engine library",
            readUnits && rows ? *readUnits + *rows + (copyAnalysis ? *rows : 0) + 1 : 0);
        auto tracks = cache.tracksFor("rekordbox", rekordboxPath.toStdString(), progress, cancel);

        std::string engineLibraryPath =
            pathToUtf8(pathFromQString(rekordboxPath).parent_path() / "Engine Library");
        // Invalidated before create() runs, not just on success: a
        // partially-failed create() may still have written real files at
        // this exact path, and a stale "engine" entry from a prior
        // library that once lived here (and was since deleted) must not
        // keep being served either way.
        LibraryCatalogCache::instance().invalidate("engine", engineLibraryPath);
        // The sequence of the export the tracks came from, recorded in
        // the new library so a player does not offer to import it all
        // over again on first insert (issue #42).
        const auto rekordbox = infrastructure::engine::readRekordboxImportState({}, rekordboxPath.toStdString());
        // Engine counts cues in the file's own samples. The creator reads
        // an MP3's rate from its header; any other file's comes from the
        // stick's metadata cache, probed once and cached when it is not
        // there, as the catalog cache's Cues stage does.
#ifdef SEABASS_HAVE_TAGLIB
        infrastructure::audio::TagLibMetadataProbe probe;
#else
        application::NullTrackMetadataProbe probe;
#endif
        infrastructure::local::CachedSampleRates rates(
            infrastructure::paths::stickRootForCatalogPath(rekordboxPath.toStdString()), probe);

        // rekordbox's analysis, keyed for the ledger and the kept analyses
        // as Beat Grids & Waveforms keys them.
        const std::string pioneer = rekordboxPath.toStdString();
        const std::string analysisStickRoot = infrastructure::paths::normalStickRootForCatalogPath(pioneer);
        NewLibraryAnalysis analysis;
        EngineLibraryCreator::ScratchLibraryStep step;
        if (copyAnalysis) {
            step = [&](const std::string &scratchLibrary, const std::vector<EngineLibraryCreator::CreatedRow> &made) {
                copyAnalysisIntoNewLibrary(scratchLibrary, engineLibraryPath, pioneer, analysisStickRoot, tracks, made,
                                           analysisOptions, rates, progress, cancel, analysis);
            };
        }

        infrastructure::engine::EngineLibraryCreationResult creation;
        try {
            creation = EngineLibraryCreator::create(
                engineLibraryPath, tracks, schemaFromInt(schemaGeneration), progress, cancel,
                rekordbox.hasRekordboxLibrary ? std::optional<std::uint64_t>(rekordbox.librarySequence)
                                              : std::nullopt,
                [&rates](const std::string &file) { return rates.rateOf(file); }, step);
        } catch (...) {
            // What was probed before it ended is still true.
            rates.save();
            throw;
        }
        rates.save();

        result.tracksCreated = creation.tracksCreated;
        result.tracksSkipped = creation.tracksSkipped;
        result.cuesCopied = creation.cuesCopied;
        result.tracksTotal = creation.tracksTotal;
        result.cancelled = creation.cancelled;
        result.importNotRecorded = rekordbox.hasRekordboxLibrary && !creation.rekordboxImportRecorded;
        result.memoryCuesDroppedByLeadInShift = creation.memoryCuesDroppedByLeadInShift;
        result.loopsDroppedByLeadInShift = creation.loopsDroppedByLeadInShift;
        if (!creation.errorMessage.empty()) {
            result.errorMessage = QString::fromStdString(creation.errorMessage);
        }
        const bool created = result.errorMessage.isEmpty() && !result.cancelled;

        // One switch alone: a new row has no analysis of the player's to
        // give the one part to, so every row is left for the player, and
        // the summary says why.
        if (created && (analysisOptions.grids || analysisOptions.waveforms) && !copyAnalysis
            && creation.tracksCreated > 0) {
            analysis.counts.forPlayer = creation.tracksCreated;
            analysis.counts.forPlayerBecause[QStringLiteral("a new track is completed only with both beat grids and "
                                                            "waveforms switched on")] = creation.tracksCreated;
        }
        if (created) {
            const AnalysisNoteCounts &c = analysis.counts;
            result.analysisNote = analysisSummaryNote(c);
            result.analysisGrids = c.grids;
            result.analysisWaveforms = c.waveforms;
            result.analysisCompleted = c.completed;
            result.analysisLeftForPlayer = c.forPlayer;
            result.analysisLeftOther = c.noAnalysis + c.same + c.setOnPlayer;
            for (const auto &[why, n] : c.other) {
                result.analysisLeftOther += n;
            }
        }
        // The grids written, in the stick's ledger, now that the library
        // they are in is on the stick. A ledger that cannot be written
        // costs a later copy's recognising them, nothing of the library.
        if (created && !analysis.ledgerEntries.empty()) {
            std::string error;
            const auto root = pathFromUtf8(analysisStickRoot);
            infrastructure::local::BeatGridLedger ledger = infrastructure::local::readBeatGridLedger(root, &error);
            for (const auto &[key, fingerprint] : analysis.ledgerEntries) {
                ledger[key] = fingerprint;
            }
            std::string writeError;
            if (!infrastructure::local::writeBeatGridLedger(root, ledger, &writeError)) {
                result.analysisWarning =
                    QStringLiteral("The beat grids were copied, but Seabass could not record which ones it wrote (%1), "
                                   "so a later copy takes them for grids set on the player and leaves them alone.")
                        .arg(QString::fromStdString(writeError));
            }
        }
    } catch (const application::OperationCancelled &) {
        result.cancelled = true;  // during the scan; nothing was written
    } catch (const std::exception &e) {
        result.errorMessage = QString::fromStdString(e.what());
    }
    return result;
}

}  // namespace seabass::gui
