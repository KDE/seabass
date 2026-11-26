// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
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
// A missing ledger is an empty one.
const infrastructure::local::AnalysisLedger *saveAnalysisLedger(SaveContext &ctx, const std::string &stickRoot);

// What Analyse again does with rekordbox's analysis of a track before it
// writes a new one (see ReanalyseTrackChange): decided from the grid in
// the track's .DAT now (`gridNow`, its fingerprint, empty for none), what
// the analysis ledger recorded for the track (`ledgerGrid`, empty for no
// entry or a ledger that cannot be believed) and what is kept already.
enum class KeepOriginal {
    // The grid is Seabass's own (the ledger's, or one Seabass wrote since
    // it kept the original) and nothing is kept: nothing to keep.
    Nothing,
    // Nothing kept yet and the analysis is someone else's: kept now.
    KeepNow,
    // Kept already, and the grid is Seabass's or the kept one: the kept
    // original stays the original (the first one wins).
    KeptStays,
    // Kept already, and the grid is neither Seabass's nor the kept one,
    // nor is the file the one kept for: rekordbox analysed the track anew
    // since (an export over the stick). Its newer analysis is kept in
    // place of the older one.
    KeepNewer,
};
KeepOriginal keepOriginalDecision(const std::string &gridNow, const std::string &ledgerGrid,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept,
                                  std::uint64_t audioFileSize);

}  // namespace seabass::gui
