// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/analysis_writer.hpp"
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
    // rekordbox's key as it writes it ("Fm", "8A"; empty: none) and its
    // tempo (0: none), for a row the player has not analysed.
    std::string key;
    double bpm = 0.0;
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

// The item for one rekordbox track and the Engine row it goes to, as
// planBeatGridCopy() makes each: keyed for the ledger by the rekordbox
// file's path from `stickRoot`. `engineTrackId` may be empty for a row a
// save has not made yet; the caller fills it in once it has.
BeatGridCopyItem beatGridCopyItemFor(const domain::Track &rekordbox, std::string engineTrackId,
                                     const std::string &stickRoot);

// Every rekordbox track paired with its Engine row the way Sync Cue
// Points pairs them (domain::matchTracks, MatchScope::OneStick, with
// `files` for another spelling of one file). `stickRoot` keys each item
// for the ledger. Every rekordbox track is counted once: in items, in
// rekordboxOnly or in ambiguous.
BeatGridCopyPlan planBeatGridCopy(const std::vector<domain::Track> &rekordbox,
                                  const std::vector<domain::Track> &engine, const std::string &stickRoot,
                                  const domain::StickFiles *files = nullptr);

// Keeps the plan's items whose title, or "artist - title", contains
// `needle`, case ignored (foldedText on both sides), and returns how many
// it took out. An empty needle keeps every item. For trying one named
// track's grid on the player before the whole stick's.
int keepBeatGridItemsMatching(BeatGridCopyPlan &plan, const std::string &needle);

// Keeps the plan's items whose rekordbox track id is in `rekordboxTrackIds`
// and returns how many it took out: the tracks of a playlist, or the ones
// a user picked.
int keepBeatGridItemsWithIds(BeatGridCopyPlan &plan, const std::set<std::string> &rekordboxTrackIds);

struct BeatGridCopyOptions
{
    UnanalysedRowGrid unanalysed = UnanalysedRowGrid::Skip;
    bool overwriteEngineEdits = false;
    // Asks the writer for a preview of every row (BeatGridWriteOptions::
    // previewOnly): the same checks, nothing written. A row the real run
    // would write is counted in wouldWrite, never in written, and listed
    // in wouldWriteItems; ledgerEntries and notRecorded stay empty, since
    // no grid was left anywhere to be recognised by later.
    bool previewOnly = false;
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
    //
    // written is work done: rows whose grid is now rekordbox's. A preview
    // (BeatGridCopyOptions::previewOnly) never counts there; the rows it
    // would write are wouldWrite, a bucket of their own, so nothing that
    // shows a preview's numbers can show them as done.
    int written = 0;
    int wouldWrite = 0;
    int alreadySame = 0;
    int noRekordboxGrid = 0;
    int notAnalysed = 0;
    int lockedOnEngine = 0;
    int editedOnEngine = 0;
    int noSampleRate = 0;
    int unsupportedLibrary = 0;
    // Grids not asked for (copyRekordboxAnalysis() with grids off).
    int notRequested = 0;
    int failed = 0;
    // Not reached: the run was cancelled first.
    int notReached = 0;

    // Bars whose count rekordbox breaks and Engine cannot, over the rows
    // written (or, in a preview, the rows that would be).
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

    // A preview only: the items counted in wouldWrite, in plan order.
    std::vector<BeatGridCopyItem> wouldWriteItems;

    int total() const
    {
        return written + wouldWrite + alreadySame + noRekordboxGrid + notAnalysed + lockedOnEngine + editedOnEngine + noSampleRate
            + unsupportedLibrary + notRequested + failed + notReached;
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
// `progress` sees one phase, "Copying beat grids" ("Checking beat grids"
// for a preview), one tick per item.
BeatGridCopyResult copyBeatGrids(const BeatGridCopyPlan &plan, const BeatGridSource &source, BeatGridWriter &writer,
                                 const std::map<std::string, std::string> &ledger, const BeatGridCopyOptions &options,
                                 const BeatGridFileFactsSource &fileFacts = {},
                                 const CancellationToken &cancel = CancellationToken::none(),
                                 ProgressReporter &progress = NullProgressReporter::instance());

// Copying the whole of rekordbox's analysis (grid, waveform, key, tempo)
// through an AnalysisWriter, which can also complete a row the player has
// not analysed (see AnalysisWriter::importAnalysis()).

struct AnalysisCopyOptions
{
    bool grids = true;
    bool waveforms = true;
    bool overwriteEngineEdits = false;
    // As BeatGridCopyOptions::previewOnly, for both parts: what would be
    // written or completed is counted apart from what was.
    bool previewOnly = false;
    // For testing on hardware only (AnalysisImportOptions).
    bool skipOverviewFile = false;
    bool zeroLoudness = false;
};

// The audio file's rate, for a row the player has not analysed, and whether
// it is an MP3. Its own sample count where the container says it exactly,
// and the rate read with it; for the kinds whose count must come from the
// file (countRequired: FLAC, WAV, AIFF, M4A) a row is not completed without
// it (AnalysisImportOptions::fileSampleCount).
struct AnalysisFileFacts
{
    std::optional<double> sampleRateHz;
    bool isMp3 = false;
    bool countRequired = false;
    std::optional<std::uint64_t> sampleCount;
};

// What a file's name says of it: an MP3; a kind whose container says its
// sample count exactly and whose row is completed only from that (.flac,
// .wav, .aif, .aiff, .m4a, .mp4); or another, completed from an estimate.
bool isMp3File(const std::string &path);
bool requiresOwnSampleCount(const std::string &path);

// One bucket per waveform outcome, every item in exactly one.
struct WaveformCopyCounts
{
    int written = 0;
    int wouldWrite = 0;
    int alreadySame = 0;
    int notRequested = 0;
    int noRekordboxWaveform = 0;
    int notAnalysed = 0;
    int noSampleCount = 0;
    int unsupportedLibrary = 0;
    int failed = 0;
    int notReached = 0;

    int total() const
    {
        return written + wouldWrite + alreadySame + notRequested + noRekordboxWaveform + notAnalysed + noSampleCount
            + unsupportedLibrary + failed + notReached;
    }
};

struct AnalysisCopyResult
{
    // The grid part, bucket for bucket as copyBeatGrids() counts it, with
    // the failures, ledger entries and items that would be written. An item
    // that failed or was not reached is counted in both parts.
    BeatGridCopyResult grids;
    WaveformCopyCounts waveforms;
    // Rows the player had not analysed that were completed, and in a
    // preview the ones that would be. Never both.
    int completedRows = 0;
    int wouldCompleteRows = 0;
    // Rows where either part was (in a preview: would be) written.
    int rowsWritten = 0;
    int rowsWouldWrite = 0;
    // The overview files written, or in a preview to be written, each a
    // path from the Engine Library folder: what a backup before the run
    // must cover besides the database.
    std::vector<std::string> overviewFiles;
    // Rows whose own analysis by the player was kept on the stick before
    // it was replaced (never in a preview).
    int keptEngineAnalyses = 0;
};

// rekordbox's analysis of an item; throwing counts the item as failed.
using AnalysisSource = std::function<RekordboxAnalysis(const BeatGridCopyItem &)>;
// Asked only for a row the writer found no length for (a row the player
// has not analysed). May be empty.
using AnalysisFileFactsSource = std::function<AnalysisFileFacts(const BeatGridCopyItem &)>;

// The writer's options for one item of a run with `options`: the ledger's
// fingerprint for it handed on, so a grid Seabass wrote before is not
// taken for the DJ's edit on the player.
AnalysisImportOptions analysisImportOptionsFor(const BeatGridCopyItem &item,
                                               const std::map<std::string, std::string> &ledger,
                                               const AnalysisCopyOptions &options);

// One item through the writer, as copyRekordboxAnalysis() takes each: with
// the options above, and once more with the file's rate and kind when the
// writer found no length for a row the player has not analysed. What it
// returned, and the options it returned that with, so a later call (a
// save after a preview) can make the same one. Throws what the writer or
// `fileFacts` throws.
struct AnalysisItemOutcome
{
    AnalysisImportResult result;
    AnalysisImportOptions options;
};
AnalysisItemOutcome importAnalysisItem(const BeatGridCopyItem &item, const RekordboxAnalysis &analysis,
                                       AnalysisWriter &writer, const std::map<std::string, std::string> &ledger,
                                       const AnalysisCopyOptions &options, const AnalysisFileFactsSource &fileFacts);

// copyBeatGrids() for the whole analysis: each item through
// AnalysisWriter::importAnalysis(), in order, with the ledger as there and
// the same handling of failures, cancelling and progress ("Copying
// analysis", "Checking analysis" for a preview). Throws std::logic_error
// if either part's buckets do not add up to the plan's items.
AnalysisCopyResult copyRekordboxAnalysis(const BeatGridCopyPlan &plan, const AnalysisSource &source,
                                         AnalysisWriter &writer, const std::map<std::string, std::string> &ledger,
                                         const AnalysisCopyOptions &options,
                                         const AnalysisFileFactsSource &fileFacts = {},
                                         const CancellationToken &cancel = CancellationToken::none(),
                                         ProgressReporter &progress = NullProgressReporter::instance());

// Restore Engine's Analysis: for each item, the player's analysis kept on
// the stick put back (AnalysisWriter::restoreEngineAnalysis()), counted by
// outcome; every item in exactly one bucket. An exception counts the item
// as failed and the run goes on; cancelled between items, the rest are
// notReached. One progress phase, "Restoring Engine's analysis" ("Checking
// Engine's analysis" for a preview).
struct EngineAnalysisRestoreCounts
{
    int restored = 0;
    int wouldRestore = 0;
    int alreadyEngines = 0;
    int nothingKept = 0;
    int keptDamaged = 0;
    int stale = 0;
    int changedSince = 0;
    int unsupportedLibrary = 0;
    int failed = 0;
    int notReached = 0;
    bool cancelled = false;
    // Failures, and refusals with a reason (damaged, stale, changed since).
    std::vector<BeatGridCopyFailure> failures;
    std::vector<BeatGridCopyFailure> refusals;
    // The ledger lines to drop: the rows whose analysis is the player's
    // again (restored, or already so). Never in a preview.
    std::vector<std::string> ledgerKeysToDrop;
    // The overview files put back or taken away (in a preview: would be).
    std::vector<std::string> overviewFiles;

    int total() const
    {
        return restored + wouldRestore + alreadyEngines + nothingKept + keptDamaged + stale + changedSince
            + unsupportedLibrary + failed + notReached;
    }
};
EngineAnalysisRestoreCounts restoreEngineAnalysis(const BeatGridCopyPlan &plan, AnalysisWriter &writer,
                                                  const EngineAnalysisRestoreOptions &options,
                                                  const CancellationToken &cancel = CancellationToken::none(),
                                                  ProgressReporter &progress = NullProgressReporter::instance());

// Let the Player Analyse Again: each item's row marked for the player's
// analysis (AnalysisWriter::letPlayerAnalyseAgain()); the analysis kept on
// the stick stays, so it can still be put back, and the ledger line goes.
// Counted and run as restoreEngineAnalysis() (progress: "Marking tracks
// for analysis", "Checking tracks" for a preview).
struct LetPlayerAnalyseCounts
{
    int marked = 0;
    int wouldMark = 0;
    int notAnalysed = 0;
    int unsupportedLibrary = 0;
    int failed = 0;
    int notReached = 0;
    bool cancelled = false;
    std::vector<BeatGridCopyFailure> failures;
    std::vector<std::string> ledgerKeysToDrop;

    int total() const { return marked + wouldMark + notAnalysed + unsupportedLibrary + failed + notReached; }
};
LetPlayerAnalyseCounts letPlayerAnalyseAgain(const BeatGridCopyPlan &plan, AnalysisWriter &writer, bool previewOnly,
                                             const CancellationToken &cancel = CancellationToken::none(),
                                             ProgressReporter &progress = NullProgressReporter::instance());

}  // namespace seabass::application
