// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/kept_analysis_save.hpp"

#include <algorithm>
#include <memory>

#include "application/path_key.hpp"
#include "gui/edit/save_context.hpp"
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

KeepOriginal keepOriginalDecision(const std::string &gridNow, const std::string &ledgerGrid,
                                  const infrastructure::local::KeptRekordboxAnalysis *kept, std::uint64_t audioFileSize)
{
    const bool ledgers = !gridNow.empty() && gridNow == ledgerGrid;
    if (!kept) {
        return ledgers ? KeepOriginal::Nothing : KeepOriginal::KeepNow;
    }
    const bool seabassWroteIt = !gridNow.empty()
                                && std::find(kept->seabassGrids.begin(), kept->seabassGrids.end(), gridNow)
                                       != kept->seabassGrids.end();
    // A grid that cannot be told (none in the file) is no evidence of a
    // newer analysis: the kept one stays.
    const bool known = ledgers || seabassWroteIt || gridNow == kept->originalGrid || gridNow.empty();
    if (known && kept->audioFileSize == audioFileSize) {
        return KeepOriginal::KeptStays;
    }
    return KeepOriginal::KeepNewer;
}

}  // namespace seabass::gui
