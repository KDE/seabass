// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/copy_beat_grids.hpp"

#include <exception>
#include <stdexcept>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/plan_engine_update.hpp"

namespace seabass::application
{

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
        BeatGridCopyItem item;
        item.rekordboxTrackId = rekordboxTrack->sourceId;
        item.engineTrackId = engineTrack->sourceId;
        item.title = rekordboxTrack->title;
        item.artist = rekordboxTrack->artist;
        item.stickRelativePath = stickRelativePathOf(rekordboxTrack->filePath, stickRoot);
        item.ledgerKey = item.stickRelativePath.empty() ? std::string() : normalizedPathKey(item.stickRelativePath);
        item.filePath = rekordboxTrack->filePath;
        item.durationMs = rekordboxTrack->durationSeconds > 0.0 ? rekordboxTrack->durationSeconds * 1000.0 : 0.0;
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
            switch (written.outcome) {
            case Outcome::Written:
                if (options.previewOnly) {
                    ++result.wouldWrite;
                    result.wouldWriteItems.push_back(item);
                } else {
                    ++result.written;
                }
                result.barBreaks += written.barBreaks;
                break;
            case Outcome::AlreadySame:
                ++result.alreadySame;
                break;
            case Outcome::NoSourceGrid:
                ++result.noRekordboxGrid;
                break;
            case Outcome::RowNotAnalysed:
                ++result.notAnalysed;
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
            }
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

}  // namespace seabass::application
