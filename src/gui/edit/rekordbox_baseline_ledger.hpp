// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/rekordbox_baseline.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"

// The rekordbox baseline (domain/rekordbox_baseline.hpp) as the saves keep
// it: the two path functions every writer of it shares, and the origin
// ledger of a save that does not record the baseline itself
// (docs/sync-after-rekordbox-export-plan.md, "When the baseline is
// written", case 2).
namespace seabass::gui
{

// Track::filePath (absolute, on today's mount point) to the path from the
// stick root, '/'-separated; empty when the file is not under the root.
// Lexical only, so it answers for a stick that is gone too. The
// `stickRelativeOf` of baselineFrom, nextBaseline and the planner: the
// page has to pass these two, or its keys and the save's differ.
std::string baselineStickRelativePath(const std::string &stickRoot, const std::string &filePath);
// The stick-relative path to the baseline's pathKey
// (application::normalizedPathKey). The `pathKeyOf` of the same.
std::string baselinePathKey(const std::string &stickRelativePath);

// The writes keyed for the baseline; a write whose file is not under the
// stick root has no key and is left out.
std::vector<domain::SeabassWrite> seabassWritesFor(const std::string &stickRoot,
                                                   const std::vector<RekordboxWrite> &writes);

// What runSaveLoop() reads before anything is written: whether the stick
// has a baseline, its header's pdb sequence, and export.pdb's.
struct BaselineAtSaveStart
{
    // A save with a rekordbox path and a baseline on the stick.
    bool exists = false;
    std::string file;  // UTF-8
    std::optional<std::uint64_t> baselineSequence;
    std::optional<std::uint64_t> pdbSequence;
    std::string error;  // the header could not be read
};
BaselineAtSaveStart readBaselineAtSaveStart(const SaveContext &ctx);

// The origin ledger of a save that wrote the rekordbox side and does not
// record the baseline itself: merges the save's rekordbox writes into the
// baseline as Seabass's own (domain::recordSeabassWrites), and advances
// its pdb sequence to export.pdb's after the save only when the baseline
// was current at save start (its sequence equal to export.pdb's then). Not
// current means rekordbox exported in between, and advancing would hide
// that export's changes; then only the ledger is merged.
//
// Nothing when the stick had no baseline or the save wrote nothing on the
// rekordbox side. Written atomically, `label`'s record backing the file up
// first if the upfront pass did not. Returns a warning, empty when it
// recorded or had nothing to record.
QString recordOriginLedger(SaveContext &ctx, const BaselineAtSaveStart &start, const SaveContext::AfterCommit &after,
                           const std::string &label);

// "Seabass <version>", the baseline header's writer.
std::string baselineWriter();

}  // namespace seabass::gui
