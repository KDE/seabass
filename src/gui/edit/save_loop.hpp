// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "gui/edit/pending_change.hpp"
#include "gui/edit/save_context.hpp"

namespace seabass::gui
{

struct SaveLoopResult
{
    QStringList appliedIds;  // in order; a change here is fully on the stick
    QString failedId;        // the change that failed, if any (it and everything after it stay pending)
    QString error;           // empty unless a change or a finish hook failed
    bool cancelled = false;  // stopped between two changes on request
    std::vector<UndoableBackup> backups;
    // Bytes freed by releasing old automatic backups after this save,
    // which only happens when the stick was below its headroom. Worth
    // surfacing because the user's undo history just got shorter -- but
    // never as "the stick is faster now": neither stick sampled supports
    // TRIM, so freeing space returns nothing to the flash controller.
    std::uint64_t bytesReleased = 0;
};

// The one save loop every session runs (worker thread): applies changes
// in order, checks the token between them, runs the finish hooks, and
// says exactly which changes landed. A finish-hook failure (a scratch
// copy that could not be committed) reports every change as still
// pending -- nothing it covered reached the stick -- so a retry re-applies
// them; the per-item writers are idempotent.
SaveLoopResult runSaveLoop(const std::vector<std::shared_ptr<PendingChange>> &changes, SaveContext &ctx);

}  // namespace seabass::gui
