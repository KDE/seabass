// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/kept_analysis_save.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

#include "application/path_key.hpp"
#include "gui/edit/save_context.hpp"
#include "infrastructure/backup/stick_space.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace
{

constexpr const char *Label = "track-reanalysis";

// A missing ledger is an empty one; one that cannot be believed is none.
struct SaveLedger
{
    infrastructure::local::AnalysisLedger ledger;
    bool believed = false;
};

}  // namespace

std::string keptRekordboxAnalysisFileOf(const std::string &stickRoot, const std::string &ledgerKey)
{
    return ledgerKey.empty() || stickRoot.empty()
               ? std::string()
               : pathToUtf8(infrastructure::local::keptRekordboxAnalysisFile(pathFromUtf8(stickRoot), ledgerKey));
}

infrastructure::local::StickKeptRekordboxAnalysisStore &saveKeptRekordboxStore(SaveContext &ctx,
                                                                              const std::string &stickRoot)
{
    return ctx.shared<infrastructure::local::StickKeptRekordboxAnalysisStore>(
        "kept-rekordbox-analysis:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<infrastructure::local::StickKeptRekordboxAnalysisStore>(pathFromUtf8(stickRoot));
            made->setBeforeChange([&ctx](const std::string &file) {
                BackupTarget target;
                target.file = file;
                target.label = Label;
                target.removeOnRestoreIfAbsent = true;
                ctx.backupAllNow({target});
                ctx.protectForThisChange(file);
            });
            return made;
        });
}

const infrastructure::local::AnalysisLedger *saveAnalysisLedger(SaveContext &ctx, const std::string &stickRoot)
{
    const auto &read = ctx.sharedForWholeSave<SaveLedger>(
        "kept-rekordbox-analysis:ledger:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<SaveLedger>();
            std::string error;
            infrastructure::local::AnalysisLedgerFault fault = infrastructure::local::AnalysisLedgerFault::None;
            made->ledger = infrastructure::local::readAnalysisLedger(pathFromUtf8(stickRoot), &error, &fault);
            made->believed = fault == infrastructure::local::AnalysisLedgerFault::None;
            return made;
        });
    return read.believed ? &read.ledger : nullptr;
}

namespace
{

// Seabass's rekordbox grids as the stick records them, read once per save:
// the ledger's at once, the kept entries' heads only when asked.
struct SeabassGrids
{
    std::set<std::string> ledger;
    std::optional<std::set<std::string>> kept;
};

SeabassGrids &saveSeabassGrids(SaveContext &ctx, const std::string &stickRoot)
{
    return ctx.sharedForWholeSave<SeabassGrids>(
        "kept-rekordbox-analysis:seabass-grids:" + application::normalizedPathKey(stickRoot), [&]() {
            auto made = std::make_unique<SeabassGrids>();
            if (const auto *ledger = saveAnalysisLedger(ctx, stickRoot)) {
                for (const auto &[key, entry] : *ledger) {
                    if (!entry.rekordboxGrid.empty()) {
                        made->ledger.insert(entry.rekordboxGrid);
                    }
                }
            }
            return made;
        });
}

std::function<std::uint64_t(const std::string &)> &bytesAvailableHook()
{
    static std::function<std::uint64_t(const std::string &)> hook;
    return hook;
}

}  // namespace

bool ledgerBelieved(SaveContext &ctx, const std::string &stickRoot)
{
    return saveAnalysisLedger(ctx, stickRoot) != nullptr;
}

bool isSeabassRekordboxGrid(SaveContext &ctx, const std::string &stickRoot, const std::string &grid,
                            const infrastructure::local::KeptRekordboxAnalysis *own)
{
    if (grid.empty()) {
        return false;
    }
    if (own && std::find(own->seabassGrids.begin(), own->seabassGrids.end(), grid) != own->seabassGrids.end()) {
        return true;
    }
    SeabassGrids &grids = saveSeabassGrids(ctx, stickRoot);
    if (grids.ledger.count(grid) > 0) {
        return true;
    }
    if (!grids.kept) {
        std::string error;
        const auto heads = infrastructure::local::keptRekordboxSeabassGrids(pathFromUtf8(stickRoot), &error);
        grids.kept = std::set<std::string>(heads.begin(), heads.end());
    }
    return grids.kept->count(grid) > 0;
}

KeepOriginal keepOriginalDecision(const std::string &gridNow, bool gridIsSeabass, bool ledgerBelieved,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept)
{
    // A ledger that cannot be believed (damaged, unreadable) is not one
    // that knows nothing: the grid may be Seabass's own, and keeping it
    // as rekordbox's original would be a lie Restore later tells.
    if (!kept) {
        return gridIsSeabass ? KeepOriginal::Nothing : ledgerBelieved ? KeepOriginal::KeepNow : KeepOriginal::CannotTell;
    }
    // A grid that cannot be told (none in the file) is no evidence of a
    // newer analysis: the kept one stays.
    if (gridNow.empty() || gridNow == kept->originalGrid || gridIsSeabass) {
        return KeepOriginal::KeptStays;
    }
    return ledgerBelieved ? KeepOriginal::KeepNewer : KeepOriginal::CannotTell;
}

std::vector<infrastructure::local::KeptCueMove> withCueMoves(const std::vector<infrastructure::local::KeptCueMove> &kept,
                                                             const std::vector<infrastructure::local::KeptCueMove> &made)
{
    std::vector<infrastructure::local::KeptCueMove> merged = kept;
    for (const auto &move : made) {
        const auto earlier = std::find_if(merged.begin(), merged.end(), [&](const auto &e) {
            return e.library == move.library && e.hot == move.hot && e.pad == move.pad && e.loop == move.loop
                   && std::abs(e.newMs - move.oldMs) < 1.0 && (!e.loop || std::abs(e.newEndMs - move.oldEndMs) < 1.0);
        });
        if (earlier != merged.end()) {
            earlier->newMs = move.newMs;
            earlier->newEndMs = move.newEndMs;
        } else {
            merged.push_back(move);
        }
    }
    return merged;
}

std::uint64_t stickBytesAvailable(const std::string &stickRoot)
{
    if (bytesAvailableHook()) {
        return bytesAvailableHook()(stickRoot);
    }
    return infrastructure::backup::availableBytes(pathFromUtf8(stickRoot));
}

void setStickBytesAvailableForTesting(std::function<std::uint64_t(const std::string &stickRoot)> measure)
{
    bytesAvailableHook() = std::move(measure);
}

}  // namespace seabass::gui
