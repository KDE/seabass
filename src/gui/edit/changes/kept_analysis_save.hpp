// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "infrastructure/local/analysis_ledger_file.hpp"
#include "infrastructure/local/kept_rekordbox_analysis_file.hpp"

namespace seabass::gui
{

class SaveContext;

// rekordbox's analysis kept on the stick (local/kept_rekordbox_analysis_file.hpp)
// as a save keeps and restores it: Analyse again keeps it before it first
// replaces it (ReanalyseTrackChange), Restore original analysis puts it
// back (RestoreAnalysisChange).

// The kept entry's file of a track on the stick at `stickRoot`; empty
// without a key. A change that may write or remove it declares it in
// filesToBackup(), noted absent where it is not there, so Undo Last Save
// takes a new one away and puts a removed one back.
std::string keptRekordboxAnalysisFileOf(const std::string &stickRoot, const std::string &ledgerKey);

// The save's store on the stick: every file it creates, replaces or
// removes is backed up first (or noted absent) and protected for the
// change, so a change or a save that fails puts it back as it was.
infrastructure::local::StickKeptRekordboxAnalysisStore &saveKeptRekordboxStore(SaveContext &ctx,
                                                                              const std::string &stickRoot);

// The stick's analysis ledger as the save found it, read once per save;
// nullptr when it cannot be believed (it cannot be read, or is damaged).
// A missing ledger is an empty one: it knows nothing, which is not the
// same as one that cannot be believed.
const infrastructure::local::AnalysisLedger *saveAnalysisLedger(SaveContext &ctx, const std::string &stickRoot);

// Whether `grid` (a rekordbox::pqtzGridFingerprint(), empty for none) is
// one Seabass wrote, as the stick records it: the grid of any entry of
// the analysis ledger (any track's, so a track moved or renamed whose
// entry stayed under its old name is still known), one of `own`'s
// seabassGrids, or one of the seabassGrids of any kept entry on the stick
// (read once per save, and only when the first two do not know it). An
// empty grid is never Seabass's. A ledger that cannot be believed adds
// nothing; ledgerBelieved() says whether it could be.
bool isSeabassRekordboxGrid(SaveContext &ctx, const std::string &stickRoot, const std::string &grid,
                            const infrastructure::local::KeptRekordboxAnalysis *own);
bool ledgerBelieved(SaveContext &ctx, const std::string &stickRoot);

// What Analyse again does with rekordbox's analysis of a track before it
// writes a new one (see ReanalyseTrackChange): decided from the grid in
// the track's .DAT now (`gridNow`, its fingerprint, empty for none),
// whether that grid is Seabass's (isSeabassRekordboxGrid()), whether the
// analysis ledger could be believed, and what is kept already. The audio
// file's size decides nothing here: a file of another size with a grid
// Seabass knows keeps its kept entry as it is, and Restore then refuses
// the file of another size, as it should.
enum class KeepOriginal {
    // The grid is Seabass's and nothing is kept: nothing to keep.
    Nothing,
    // Nothing kept yet, the grid is not known for Seabass's, and the
    // analysis ledger is believed (a missing one is an empty one): kept now.
    KeepNow,
    // Kept already, and the grid is Seabass's, the kept one, or none: the
    // kept original stays the original (the first one wins), and its
    // entry is not written again.
    KeptStays,
    // Kept already, and the grid is neither Seabass's nor the kept one:
    // rekordbox analysed the track anew since, or the DJ set its grid (an
    // export over the stick). Its newer analysis is kept in place of the
    // older one, so a grid made by hand is never lost.
    KeepNewer,
    // The grid is neither the kept one nor known for Seabass's, and the
    // analysis ledger cannot be believed (damaged or unreadable: not the
    // same as missing), so Seabass cannot tell whether the grid is its own
    // (keeping it would call Seabass's analysis rekordbox's, or lose the
    // kept original) or rekordbox's (writing over it unkept would lose
    // it): the track is left alone.
    CannotTell,
};
KeepOriginal keepOriginalDecision(const std::string &gridNow, bool gridIsSeabass, bool ledgerBelieved,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept);

// `kept` (the cue moves a kept entry records) with `made` (those an
// Analyse again makes now) added: a cue made moves from where an earlier
// move left it continues that move (its old place stays the first one);
// any other is a move of its own.
std::vector<infrastructure::local::KeptCueMove> withCueMoves(const std::vector<infrastructure::local::KeptCueMove> &kept,
                                                             const std::vector<infrastructure::local::KeptCueMove> &made);

// The bytes free on the stick at `stickRoot` for what a save keeps there
// (infrastructure::backup::availableBytes(), 0 when it cannot be asked).
std::uint64_t stickBytesAvailable(const std::string &stickRoot);
// Test seam: what stickBytesAvailable() answers; unset, the stick's own.
void setStickBytesAvailableForTesting(std::function<std::uint64_t(const std::string &stickRoot)> measure);
// What keeping rekordbox's analysis of a track takes on the stick, at
// most: the analysis files' own sizes (the entry, deflated, is smaller),
// and a megabyte in hand for the files Analyse again writes beside it.
inline constexpr std::uint64_t KeepReserveBytes = 1024u * 1024u;

}  // namespace seabass::gui
