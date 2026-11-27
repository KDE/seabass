// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/kept_engine_analysis.hpp"
#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"

namespace seabass::gui
{

class SaveContext;

// rekordbox's analysis kept on the stick (local/kept_rekordbox_analysis_file.hpp)
// as a save keeps and restores it: Analyse again keeps it before it first
// replaces it (ReanalyseTrackChange), Restore original analysis puts it
// back (RestoreAnalysisChange).

// The kept entry's file of a track on the stick at `stickRoot`, and its
// record's; empty without a key. A change that may write or remove them
// declares both in filesToBackup(), noted absent where they are not there,
// so Undo Last Save takes a new one away and puts a removed one back.
std::string keptRekordboxAnalysisFileOf(const std::string &stickRoot, const std::string &ledgerKey);
std::string keptRekordboxRecordFileOf(const std::string &stickRoot, const std::string &ledgerKey);

// The save's store on the stick: every file it creates, replaces or
// removes is backed up first (or noted absent) and protected for the
// change, so a change or a save that fails puts it back as it was. After
// the save, the names Browse's playlist menu counts by
// (keptAnalysisNamesOf()) are brought up to date for each file it touched.
infrastructure::local::StickKeptRekordboxAnalysisStore &saveKeptRekordboxStore(SaveContext &ctx,
                                                                              const std::string &stickRoot);

// The stick's analysis ledger as the save found it, read once per save;
// nullptr when it cannot be believed (it cannot be read, or is damaged).
// A missing ledger is an empty one: it knows nothing, which is not the
// same as one that cannot be believed. ledgerIncomplete(): a believed
// ledger that lists only what was recorded since an earlier one was
// moved aside (incomplete-since, or none beside one moved aside): a track
// it has no entry for may hold Seabass's analysis all the same.
const infrastructure::local::AnalysisLedger *saveAnalysisLedger(SaveContext &ctx, const std::string &stickRoot);
bool ledgerIncomplete(SaveContext &ctx, const std::string &stickRoot);

// Whether `grid` (a rekordbox::pqtzGridFingerprint(), empty for none) is
// one Seabass wrote, as the stick records it: one of `own`'s seabassGrids
// (the track's own record), the grid of any entry of the analysis ledger
// (any track's, so a track moved or renamed whose entry stayed under its
// old name is still known), or one of the seabassGrids of any record on
// the stick (read once per save, and only when the first two do not know
// it). An empty grid is never Seabass's. A ledger that cannot be believed
// adds nothing; ledgerBelieved() says whether it could be.
bool isSeabassRekordboxGrid(SaveContext &ctx, const std::string &stickRoot, const std::string &grid,
                            const infrastructure::local::KeptRekordboxRecord *own);
bool ledgerBelieved(SaveContext &ctx, const std::string &stickRoot);

// What Analyse again does with rekordbox's analysis of a track before it
// writes a new one (see ReanalyseTrackChange): decided from the grid in
// the track's .DAT now (`gridNow`, its fingerprint, empty for none),
// whether that grid is Seabass's (isSeabassRekordboxGrid()), whether the
// analysis ledger could be believed, what is kept already and its record
// (nullptr where there is none or it cannot be read). A kept original is
// never replaced: the first one kept stays until Restore puts it back or
// the person removes it in Free Up Space. The audio file's size decides
// nothing here: a file of another size with a grid Seabass knows keeps
// its kept entry as it is, and Restore then refuses the file of another
// size, as it should.
enum class KeepOriginal {
    // The grid is Seabass's and nothing is kept: nothing to keep.
    Nothing,
    // Nothing kept yet, the grid is not known for Seabass's, and the
    // analysis ledger is believed (a missing one is an empty one): kept
    // now, its origin unknown where the ledger is incomplete.
    KeepNow,
    // Kept already, and the grid is the kept one, one Seabass wrote (the
    // record's, or any record's), or none: the kept original stays, and
    // the grid this writes is added to its record.
    KeptStays,
    // Kept already, and the grid is neither the kept one nor Seabass's:
    // the track's analysis was changed outside Seabass since (rekordbox
    // analysed it anew, or the DJ set its grid). Keeping it would replace
    // the kept original and writing over it would lose it, so the track is
    // left alone, and Restore leaves it alone too. Removing the kept
    // original in Free Up Space lets the next Analyse again keep this one,
    // and discards the original Seabass kept.
    //
    // Analyse again asks this of every analysis file it writes, each with
    // that file's own kept original: one ChangedOutside or CannotTell
    // decides for the track.
    ChangedOutside,
    // Seabass cannot tell whether the grid is its own: an original is kept
    // and its record is missing or cannot be read, or nothing is kept, the
    // grid is not known for Seabass's and the analysis ledger cannot be
    // believed (damaged or unreadable: not the same as missing). Keeping
    // it would call Seabass's analysis the original, and writing over it
    // unkept could lose rekordbox's: the track is left alone.
    CannotTell,
};
KeepOriginal keepOriginalDecision(const std::string &gridNow, bool gridIsSeabass, bool ledgerBelieved,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept,
                                  const infrastructure::local::KeptRekordboxRecord *record);

// The fingerprint a record holds of the player's analysis Analyse again
// kept (KeptRekordboxRecord::engineKept): of the kept entry's original
// columns and overview file, and the row it is of.
std::string keptEngineOriginalFingerprint(const application::KeptEngineAnalysis &entry);

// `kept` (the cue moves a kept entry records) with `made` (those an
// Analyse again makes now) added: a cue made moves from where an earlier
// move left it continues that move (its old place stays the first one);
// any other is a move of its own.
std::vector<infrastructure::local::KeptCueMove> withCueMoves(const std::vector<infrastructure::local::KeptCueMove> &kept,
                                                             const std::vector<infrastructure::local::KeptCueMove> &made);

// The bytes free on the stick at `stickRoot` for what a save keeps there;
// nullopt when the volume cannot be asked, which is not "no room": the
// keep goes ahead, and its write fails cleanly if it really does not fit.
std::optional<std::uint64_t> stickBytesAvailable(const std::string &stickRoot);
// Test seam: what stickBytesAvailable() answers; unset, the stick's own.
void setStickBytesAvailableForTesting(
    std::function<std::optional<std::uint64_t>(const std::string &stickRoot)> measure);
// What keeping rekordbox's analysis of a track takes on the stick, at
// most: the analysis files' own sizes (the entry, deflated, is smaller),
// and a megabyte in hand for the files Analyse again writes beside it.
inline constexpr std::uint64_t KeepReserveBytes = 1024u * 1024u;

// The file names in a stick's two kept-analysis folders, listed once per
// stick (`stickId`, empty: by its root) and kept: the menu asks each time
// it opens, and a folder of thousands of entries is not listed each time.
// Safe from any thread.
struct KeptAnalysisNames
{
    std::set<std::string> rekordbox;
    std::set<std::string> engine;
};
std::shared_ptr<const KeptAnalysisNames> keptAnalysisNamesOf(const std::string &stickId,
                                                             const std::filesystem::path &stickRoot);
// The names listed afresh, kept unless a change was noted meanwhile.
std::shared_ptr<const KeptAnalysisNames> relistKeptAnalysisNames(const std::string &stickId,
                                                                 const std::filesystem::path &stickRoot);
// The kept-entry file names of a playlist's tracks, for
// keptCountFromCache().
void noteKeptAnalysisPlaylist(const std::string &stickId, const std::filesystem::path &stickRoot,
                              const std::string &playlist, std::vector<std::string> names);
// The count for the playlist from what is kept in memory, at once: 0 when
// the stick's rekordbox kept folder is known to hold nothing, else -1 when
// the stick's names or the playlist's tracks are not known yet.
int keptCountFromCache(const std::string &stickId, const std::string &playlist);
// Each of `files` (absolute, UTF-8) in the kept folders of the stick at
// `stickRoot` as it is on the stick now, there or not: a save's store
// tells each file it touched once the save is over, and Free Up Space
// each file it removed. The names stay right without a new listing.
void noteKeptAnalysisFilesChanged(const std::filesystem::path &stickRoot, const std::set<std::string> &files);
// One stick's names and playlists (it went away), or every stick's.
void forgetKeptAnalysisNames(const std::string &stickId);
void forgetKeptAnalysisNames();

}  // namespace seabass::gui
