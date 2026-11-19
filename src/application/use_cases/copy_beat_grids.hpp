// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/beat_grid_writer.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/beat_grid.hpp"
#include "domain/track.hpp"
#include "domain/track_matching.hpp"

// Copy Beat Grids: rekordbox's beat grid for every track one stick's
// rekordbox export and Engine library share, into the Engine row's
// adjusted grid. Planned first (which rows are the same track, matched as
// Sync Cue Points matches them), then carried out one row at a time
// through a BeatGridWriter, every outcome counted.
namespace seabass::application
{

struct BeatGridCopyItem
{
    std::string rekordboxTrackId;
    std::string engineTrackId;
    std::string title;
    std::string artist;
    // rekordbox's file, from the stick root, '/'-separated; empty when it
    // is not under the stick root (stickRelativePathOf).
    std::string stickRelativePath;
    // stickRelativePath through normalizedPathKey: the beat grid ledger's
    // key for the track. Empty with it.
    std::string ledgerKey;
    // rekordbox's file and its length in the catalog (0: not known), for
    // the facts the Fill modes need about a row the player has not
    // analysed.
    std::string filePath;
    double durationMs = 0.0;
};

struct BeatGridCopyPlan
{
    std::vector<BeatGridCopyItem> items;
    // rekordbox tracks with no Engine row.
    int rekordboxOnly = 0;
    // rekordbox tracks left out because another rekordbox track matched
    // the same Engine row, or names the same file (one ledger key for two
    // rows): which grid goes where is a guess.
    int ambiguous = 0;
};

// Every rekordbox track paired with its Engine row the way Sync Cue
// Points pairs them (domain::matchTracks, MatchScope::OneStick, with
// `files` for another spelling of one file). `stickRoot` keys each item
// for the ledger. Every rekordbox track is counted once: in items, in
// rekordboxOnly or in ambiguous.
BeatGridCopyPlan planBeatGridCopy(const std::vector<domain::Track> &rekordbox,
                                  const std::vector<domain::Track> &engine, const std::string &stickRoot,
                                  const domain::StickFiles *files = nullptr);

struct BeatGridCopyOptions
{
    UnanalysedRowGrid unanalysed = UnanalysedRowGrid::Skip;
    bool overwriteEngineEdits = false;
};

// The audio file's rate and length, for a row the player has not
// analysed. Either may be absent: the row is then NoSampleRate.
struct BeatGridFileFacts
{
    std::optional<double> sampleRateHz;
    std::optional<double> durationMs;
};

struct BeatGridCopyFailure
{
    BeatGridCopyItem item;
    std::string message;
};

struct BeatGridCopyResult
{
    // One bucket per outcome; every item is in exactly one, so these add
    // up to the plan's items (total()).
    int written = 0;
    int alreadySame = 0;
    int noRekordboxGrid = 0;
    int notAnalysed = 0;
    int lockedOnEngine = 0;
    int editedOnEngine = 0;
    int noSampleRate = 0;
    int unsupportedLibrary = 0;
    int failed = 0;
    // Not reached: the run was cancelled first.
    int notReached = 0;

    // Bars whose count rekordbox breaks and Engine cannot, over the rows
    // written.
    int barBreaks = 0;
    bool cancelled = false;
    std::vector<BeatGridCopyFailure> failures;

    // What the ledger should say from now on, for the rows written or
    // found already the same: ledger key to the grid's fingerprint.
    std::map<std::string, std::string> ledgerEntries;
    // Rows written or already the same that have no ledger key (a file
    // not under the stick root): the next run will take their grid for
    // an edit on the player.
    int notRecorded = 0;

    int total() const
    {
        return written + alreadySame + noRekordboxGrid + notAnalysed + lockedOnEngine + editedOnEngine + noSampleRate
            + unsupportedLibrary + failed + notReached;
    }
};

// rekordbox's beats for an item, every beat on rekordbox's timeline. Empty
// when it has none; throwing counts the item as failed.
using BeatGridSource = std::function<std::vector<domain::Beat>(const BeatGridCopyItem &)>;
// Asked only for a row the writer turned down for want of them (Fill and
// FillAndLock on a row the player has not analysed). May be empty.
using BeatGridFileFactsSource = std::function<BeatGridFileFacts(const BeatGridCopyItem &)>;

// Writes each item's grid, in order. `ledger` is what the stick's ledger
// says (key to fingerprint): an item whose key is in it hands the writer
// that fingerprint, so a grid Seabass wrote before is not taken for the
// DJ's edit on the player. An exception from the source or the writer
// counts that item as failed, with its message, and the run goes on.
// Cancelled between items, never inside one: the rest are notReached.
// `progress` sees one phase, "Copying beat grids", one tick per item.
BeatGridCopyResult copyBeatGrids(const BeatGridCopyPlan &plan, const BeatGridSource &source, BeatGridWriter &writer,
                                 const std::map<std::string, std::string> &ledger, const BeatGridCopyOptions &options,
                                 const BeatGridFileFactsSource &fileFacts = {},
                                 const CancellationToken &cancel = CancellationToken::none(),
                                 ProgressReporter &progress = NullProgressReporter::instance());

}  // namespace seabass::application
