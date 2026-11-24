// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/copy_beat_grids.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"

namespace seabass::application
{

BeatGridCopyItem beatGridCopyItemFor(const domain::Track &rekordbox, std::string engineTrackId,
                                     const std::string &stickRoot)
{
    BeatGridCopyItem item;
    item.rekordboxTrackId = rekordbox.sourceId;
    item.engineTrackId = std::move(engineTrackId);
    item.title = rekordbox.title;
    item.artist = rekordbox.artist;
    item.stickRelativePath = stickRelativePathOf(rekordbox.filePath, stickRoot);
    item.ledgerKey = item.stickRelativePath.empty() ? std::string() : normalizedPathKey(item.stickRelativePath);
    item.filePath = rekordbox.filePath;
    item.durationMs = rekordbox.durationSeconds > 0.0 ? rekordbox.durationSeconds * 1000.0 : 0.0;
    item.key = rekordbox.key;
    item.bpm = rekordbox.bpm > 0.0 ? rekordbox.bpm : 0.0;
    return item;
}

BeatGridCopyPlan planBeatGridCopy(const std::vector<domain::Track> &rekordbox,
                                  const std::vector<domain::Track> &engine, const std::string &stickRoot,
                                  const domain::StickFiles *files)
{
    const auto matches = domain::matchTracks(rekordbox, engine, domain::MatchScope::OneStick, files);

    // An Engine row more than one rekordbox track matched gets no grid:
    // two grids for one row is a choice, and the second write would also
    // read as an edit of the first. Two rows of one file are the same
    // choice seen from the ledger, which keeps one grid per file.
    std::map<const domain::Track *, int> timesMatched;
    std::map<std::string, int> timesKeyed;
    std::vector<BeatGridCopyItem> candidates;
    std::vector<const domain::Track *> engineRows;
    for (const auto &[rekordboxTrack, engineTrack] : matches) {
        ++timesMatched[engineTrack];
        BeatGridCopyItem item = beatGridCopyItemFor(*rekordboxTrack, engineTrack->sourceId, stickRoot);
        if (!item.ledgerKey.empty()) {
            ++timesKeyed[item.ledgerKey];
        }
        candidates.push_back(std::move(item));
        engineRows.push_back(engineTrack);
    }

    BeatGridCopyPlan plan;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (timesMatched[engineRows[i]] > 1 || (!candidates[i].ledgerKey.empty() && timesKeyed[candidates[i].ledgerKey] > 1)) {
            ++plan.ambiguous;
            continue;
        }
        plan.items.push_back(std::move(candidates[i]));
    }
    // matchTracks() pairs each rekordbox track at most once.
    plan.rekordboxOnly = static_cast<int>(rekordbox.size() - matches.size());
    return plan;
}

int keepBeatGridItemsMatching(BeatGridCopyPlan &plan, const std::string &needle)
{
    if (needle.empty()) {
        return 0;
    }
    const std::string folded = foldedText(needle);
    const auto before = plan.items.size();
    // "artist - title" holds the title, so one search covers both.
    std::erase_if(plan.items, [&folded](const BeatGridCopyItem &item) {
        return foldedText(item.artist + " - " + item.title).find(folded) == std::string::npos;
    });
    return static_cast<int>(before - plan.items.size());
}

int keepBeatGridItemsWithIds(BeatGridCopyPlan &plan, const std::set<std::string> &rekordboxTrackIds)
{
    const auto before = plan.items.size();
    std::erase_if(plan.items, [&rekordboxTrackIds](const BeatGridCopyItem &item) {
        return !rekordboxTrackIds.contains(item.rekordboxTrackId);
    });
    return static_cast<int>(before - plan.items.size());
}

namespace
{

// One grid outcome into its bucket, for copyBeatGrids() and
// copyRekordboxAnalysis() alike.
void countGrid(BeatGridCopyResult &result, const BeatGridWriteResult &grid, const BeatGridCopyItem &item, bool preview)
{
    using Outcome = BeatGridWriteResult::Outcome;
    switch (grid.outcome) {
    case Outcome::Written:
        if (preview) {
            ++result.wouldWrite;
            result.wouldWriteItems.push_back(item);
        } else {
            ++result.written;
        }
        result.barBreaks += grid.barBreaks;
        break;
    case Outcome::AlreadySame:
        ++result.alreadySame;
        break;
    case Outcome::NoSourceGrid:
        ++result.noRekordboxGrid;
        break;
    case Outcome::RowNotAnalysed:
        ++(grid.analysedNothingStored ? result.analysedNothingStored : result.notAnalysed);
        break;
    case Outcome::RowLocked:
        ++result.lockedOnEngine;
        break;
    case Outcome::RowEditedOnEngine:
        ++result.editedOnEngine;
        break;
    case Outcome::NoSampleRate:
        ++result.noSampleRate;
        break;
    case Outcome::UnsupportedSchema:
        ++result.unsupportedLibrary;
        break;
    case Outcome::NotRequested:
        ++result.notRequested;
        break;
    case Outcome::CannotKeepEngines:
        ++result.cannotKeep;
        break;
    case Outcome::RowUnreadable:
        ++result.rowUnreadable;
        break;
    case Outcome::DiffersFromEngine:
        ++result.differsFromEngine;
        break;
    }
}

void countWaveform(WaveformCopyCounts &counts, AnalysisImportResult::Waveform waveform, bool preview)
{
    using Waveform = AnalysisImportResult::Waveform;
    switch (waveform) {
    case Waveform::Written:
        ++(preview ? counts.wouldWrite : counts.written);
        break;
    case Waveform::AlreadySame:
        ++counts.alreadySame;
        break;
    case Waveform::NotRequested:
        ++counts.notRequested;
        break;
    case Waveform::NoSourceWaveform:
        ++counts.noRekordboxWaveform;
        break;
    case Waveform::RowNotAnalysed:
        ++counts.notAnalysed;
        break;
    case Waveform::NoSampleCount:
        ++counts.noSampleCount;
        break;
    case Waveform::UnsupportedSchema:
        ++counts.unsupportedLibrary;
        break;
    case Waveform::CannotKeepEngines:
        ++counts.cannotKeep;
        break;
    case Waveform::RowUnreadable:
        ++counts.rowUnreadable;
        break;
    case Waveform::PlayersWaveform:
        ++counts.playersWaveform;
        break;
    case Waveform::DiffersFromEngine:
        ++counts.differsFromEngine;
        break;
    }
}

}  // namespace

BeatGridCopyResult copyBeatGrids(const BeatGridCopyPlan &plan, const BeatGridSource &source, BeatGridWriter &writer,
                                 const std::map<std::string, std::string> &ledger, const BeatGridCopyOptions &options,
                                 const BeatGridFileFactsSource &fileFacts, const CancellationToken &cancel,
                                 ProgressReporter &progress)
{
    using Outcome = BeatGridWriteResult::Outcome;
    BeatGridCopyResult result;
    // A preview copies nothing, and its bar must not say it does.
    progress.start(options.previewOnly ? "Checking beat grids" : "Copying beat grids", plan.items.size());
    std::size_t done = 0;
    for (const BeatGridCopyItem &item : plan.items) {
        // Between rows, never inside one: the row in flight finishes.
        if (cancel.cancelled()) {
            result.notReached = static_cast<int>(plan.items.size() - done);
            result.cancelled = true;
            break;
        }
        try {
            const std::vector<domain::Beat> beats = source(item);
            BeatGridWriteOptions write;
            write.unanalysed = options.unanalysed;
            write.overwriteEngineEdits = options.overwriteEngineEdits;
            write.previewOnly = options.previewOnly;
            write.keepKey = item.ledgerKey;
            if (!item.ledgerKey.empty()) {
                if (const auto own = ledger.find(item.ledgerKey); own != ledger.end()) {
                    write.ownGridFingerprint = own->second;
                }
            }
            BeatGridWriteResult written = writer.writeBeatGrid(item.engineTrackId, beats, write);
            // The file's rate and length only when the row needs them:
            // finding them can mean reading the file.
            if (written.outcome == Outcome::NoSampleRate && fileFacts) {
                const BeatGridFileFacts facts = fileFacts(item);
                if (facts.sampleRateHz && facts.durationMs) {
                    write.fileSampleRateHz = facts.sampleRateHz;
                    write.fileDurationMs = facts.durationMs;
                    written = writer.writeBeatGrid(item.engineTrackId, beats, write);
                }
            }
            if (written.outcome == Outcome::NotRequested) {
                // writeBeatGrid() never answers it; thrown, the item counts
                // as failed rather than in no bucket at all.
                throw std::logic_error("the writer answered \"not requested\" to a grid it was asked for");
            }
            countGrid(result, written, item, options.previewOnly);
            if (!options.previewOnly
                && (written.outcome == Outcome::Written || written.outcome == Outcome::AlreadySame)) {
                // A fingerprint is what the writer promises for these two;
                // without one there is nothing to recognise the grid by.
                if (item.ledgerKey.empty() || written.gridFingerprint.empty()) {
                    ++result.notRecorded;
                } else {
                    result.ledgerEntries[item.ledgerKey] = written.gridFingerprint;
                }
            }
        } catch (const std::exception &e) {
            ++result.failed;
            result.failures.push_back({item, e.what()});
        }
        ++done;
        progress.tick(done);
    }
    progress.finish();

    // Every item counted exactly once.
    if (result.total() != static_cast<int>(plan.items.size())) {
        throw std::logic_error("copy beat grids: " + std::to_string(result.total()) + " outcomes counted for "
                               + std::to_string(plan.items.size()) + " rows");
    }
    return result;
}

AnalysisImportOptions analysisImportOptionsFor(const BeatGridCopyItem &item,
                                               const std::map<std::string, std::string> &ledger,
                                               const AnalysisCopyOptions &options)
{
    AnalysisImportOptions write;
    write.grids = options.grids;
    write.waveforms = options.waveforms;
    write.overwriteEngineEdits = options.overwriteEngineEdits;
    write.overwritePlayerWaveforms = options.overwritePlayerWaveforms;
    write.differingIsConflict = options.differingIsConflict;
    write.respectLock = options.respectLock;
    write.previewOnly = options.previewOnly;
    write.skipOverviewFile = options.skipOverviewFile;
    write.zeroLoudness = options.zeroLoudness;
    write.keepKey = item.ledgerKey;
    if (!item.ledgerKey.empty()) {
        if (const auto own = ledger.find(item.ledgerKey); own != ledger.end()) {
            write.ownGridFingerprint = own->second;
        }
    }
    return write;
}

AnalysisItemOutcome importAnalysisItem(const BeatGridCopyItem &item, const RekordboxAnalysis &analysis,
                                       AnalysisWriter &writer, const std::map<std::string, std::string> &ledger,
                                       const AnalysisCopyOptions &options, const AnalysisFileFactsSource &fileFacts)
{
    AnalysisItemOutcome out;
    out.options = analysisImportOptionsFor(item, ledger, options);
    out.result = writer.importAnalysis(item.engineTrackId, analysis, out.options);
    // The file's rate only when the row needs it: finding it can mean
    // reading the file.
    if (out.result.waveform == AnalysisImportResult::Waveform::NoSampleCount && fileFacts) {
        const AnalysisFileFacts facts = fileFacts(item);
        out.options.fileIsMp3 = facts.isMp3;
        out.options.fileCountRequired = facts.countRequired;
        out.options.fileSampleCount = facts.sampleCount;
        if (facts.sampleRateHz) {
            out.options.fileSampleRateHz = facts.sampleRateHz;
        }
        if (facts.sampleRateHz || facts.countRequired || facts.sampleCount) {
            out.result = writer.importAnalysis(item.engineTrackId, analysis, out.options);
        }
    }
    return out;
}

AnalysisCopyResult copyRekordboxAnalysis(const BeatGridCopyPlan &plan, const AnalysisSource &source,
                                         AnalysisWriter &writer, const std::map<std::string, std::string> &ledger,
                                         const AnalysisCopyOptions &options, const AnalysisFileFactsSource &fileFacts,
                                         const CancellationToken &cancel, ProgressReporter &progress)
{
    using Outcome = BeatGridWriteResult::Outcome;
    using Waveform = AnalysisImportResult::Waveform;
    AnalysisCopyResult result;
    progress.start(options.previewOnly ? "Checking analysis" : "Copying analysis", plan.items.size());
    std::size_t done = 0;
    for (const BeatGridCopyItem &item : plan.items) {
        // Between rows, never inside one: the row in flight finishes.
        if (cancel.cancelled()) {
            const int rest = static_cast<int>(plan.items.size() - done);
            result.grids.notReached = rest;
            result.waveforms.notReached = rest;
            result.grids.cancelled = true;
            break;
        }
        try {
            const RekordboxAnalysis analysis = source(item);
            const AnalysisItemOutcome outcome = importAnalysisItem(item, analysis, writer, ledger, options, fileFacts);
            const AnalysisImportResult &imported = outcome.result;
            countGrid(result.grids, imported.grid, item, options.previewOnly);
            countWaveform(result.waveforms, imported.waveform, options.previewOnly);
            if (imported.grid.outcome == Outcome::Written || imported.waveform == Waveform::Written) {
                ++(options.previewOnly ? result.rowsWouldWrite : result.rowsWritten);
            }
            if (imported.completedRow) {
                ++(options.previewOnly ? result.wouldCompleteRows : result.completedRows);
            }
            if (imported.keptEngineAnalysis) {
                ++result.keptEngineAnalyses;
            }
            if (!imported.overviewFileRelativePath.empty()) {
                result.overviewFiles.push_back(imported.overviewFileRelativePath);
            }
            if (!options.previewOnly
                && (imported.grid.outcome == Outcome::Written || imported.grid.outcome == Outcome::AlreadySame)) {
                if (item.ledgerKey.empty() || imported.grid.gridFingerprint.empty()) {
                    ++result.grids.notRecorded;
                } else {
                    result.grids.ledgerEntries[item.ledgerKey] = imported.grid.gridFingerprint;
                }
            }
        } catch (const std::exception &e) {
            ++result.grids.failed;
            ++result.waveforms.failed;
            result.grids.failures.push_back({item, e.what()});
        }
        ++done;
        progress.tick(done);
    }
    progress.finish();

    // Every item counted exactly once in each part.
    const auto items = static_cast<int>(plan.items.size());
    if (result.grids.total() != items || result.waveforms.total() != items) {
        throw std::logic_error("copy analysis: " + std::to_string(result.grids.total()) + " grid and "
                               + std::to_string(result.waveforms.total()) + " waveform outcomes counted for "
                               + std::to_string(items) + " rows");
    }
    return result;
}

namespace
{

std::string lowerExtension(const std::string &path)
{
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

}  // namespace

bool isMp3File(const std::string &path)
{
    return lowerExtension(path) == ".mp3";
}

bool requiresOwnSampleCount(const std::string &path)
{
    const std::string extension = lowerExtension(path);
    return extension == ".flac" || extension == ".wav" || extension == ".aif" || extension == ".aiff"
        || extension == ".m4a" || extension == ".mp4";
}

EngineAnalysisRestoreCounts restoreEngineAnalysis(const BeatGridCopyPlan &plan, AnalysisWriter &writer,
                                                  const EngineAnalysisRestoreOptions &options,
                                                  const CancellationToken &cancel, ProgressReporter &progress)
{
    using Outcome = EngineAnalysisRestoreResult::Outcome;
    EngineAnalysisRestoreCounts counts;
    progress.start(options.previewOnly ? "Checking Engine's analysis" : "Restoring Engine's analysis", plan.items.size());
    std::size_t done = 0;
    for (const BeatGridCopyItem &item : plan.items) {
        if (cancel.cancelled()) {
            counts.notReached = static_cast<int>(plan.items.size() - done);
            counts.cancelled = true;
            break;
        }
        try {
            const EngineAnalysisRestoreResult r = writer.restoreEngineAnalysis(item.engineTrackId, item.ledgerKey, options);
            switch (r.outcome) {
            case Outcome::Restored:
                ++(options.previewOnly ? counts.wouldRestore : counts.restored);
                break;
            case Outcome::AlreadyEngines:
                ++counts.alreadyEngines;
                break;
            case Outcome::NothingKept:
                ++counts.nothingKept;
                break;
            case Outcome::KeptDamaged:
                ++counts.keptDamaged;
                break;
            case Outcome::Stale:
                ++counts.stale;
                break;
            case Outcome::ChangedSince:
                ++counts.changedSince;
                break;
            case Outcome::UnsupportedSchema:
                ++counts.unsupportedLibrary;
                break;
            }
            if (!r.reason.empty()) {
                counts.refusals.push_back({item, r.reason});
            }
            if (!r.overviewFileRelativePath.empty()) {
                counts.overviewFiles.push_back(r.overviewFileRelativePath);
            }
            if (!options.previewOnly && (r.outcome == Outcome::Restored || r.outcome == Outcome::AlreadyEngines)
                && !item.ledgerKey.empty()) {
                counts.ledgerKeysToDrop.push_back(item.ledgerKey);
            }
        } catch (const std::exception &e) {
            ++counts.failed;
            counts.failures.push_back({item, e.what()});
        }
        ++done;
        progress.tick(done);
    }
    progress.finish();
    if (counts.total() != static_cast<int>(plan.items.size())) {
        throw std::logic_error("restore Engine's analysis: " + std::to_string(counts.total()) + " outcomes counted for "
                               + std::to_string(plan.items.size()) + " rows");
    }
    return counts;
}

LetPlayerAnalyseCounts letPlayerAnalyseAgain(const BeatGridCopyPlan &plan, AnalysisWriter &writer, bool previewOnly,
                                             const CancellationToken &cancel, ProgressReporter &progress)
{
    using Outcome = LetPlayerAnalyseResult::Outcome;
    LetPlayerAnalyseCounts counts;
    progress.start(previewOnly ? "Checking tracks" : "Marking tracks for analysis", plan.items.size());
    std::size_t done = 0;
    for (const BeatGridCopyItem &item : plan.items) {
        if (cancel.cancelled()) {
            counts.notReached = static_cast<int>(plan.items.size() - done);
            counts.cancelled = true;
            break;
        }
        try {
            const LetPlayerAnalyseResult r = writer.letPlayerAnalyseAgain(item.engineTrackId, item.ledgerKey, previewOnly);
            switch (r.outcome) {
            case Outcome::Marked:
                ++(previewOnly ? counts.wouldMark : counts.marked);
                if (!previewOnly && !item.ledgerKey.empty()) {
                    counts.ledgerKeysToDrop.push_back(item.ledgerKey);
                }
                break;
            case Outcome::NotAnalysed:
                ++counts.notAnalysed;
                break;
            case Outcome::UnsupportedSchema:
                ++counts.unsupportedLibrary;
                break;
            }
        } catch (const std::exception &e) {
            ++counts.failed;
            counts.failures.push_back({item, e.what()});
        }
        ++done;
        progress.tick(done);
    }
    progress.finish();
    if (counts.total() != static_cast<int>(plan.items.size())) {
        throw std::logic_error("let the player analyse again: " + std::to_string(counts.total())
                               + " outcomes counted for " + std::to_string(plan.items.size()) + " rows");
    }
    return counts;
}

}  // namespace seabass::application
