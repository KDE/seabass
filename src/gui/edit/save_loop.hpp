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
    // In order; a change here is settled: fully on the stick, or skipped.
    // Either way it leaves the pending list, since there is nothing left of
    // it to retry.
    QStringList appliedIds;
    // The part of appliedIds that did not do what it was staged for
    // (ChangeOutcome::skip); it may still have left a file behind. Kept
    // apart so the summary does not count it as done: a skip reported as a
    // repair let "1174 of 1174 tracks repaired" stand over a save whose
    // images had all become unreadable since the scan.
    QStringList skippedIds;
    QString failedId;        // the change that failed, if any (it and everything after it stay pending)
    QString error;           // empty unless a change or a finish hook failed
    // Everything landed, but a tidy-up after it did not -- a write-ahead
    // log that would not fold. Not an error: the changes ARE applied, and
    // reporting them as pending would have the user save again and apply
    // the same removals twice.
    QString warning;
    bool cancelled = false;  // stopped between two changes on request
    // The save failed and everything it had written went back: the stick
    // is as it was before it, and every change is still pending. A save
    // lands whole or not at all; see SaveContext::rollBackSave().
    bool rolledBack = false;
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
// says exactly which changes landed. It lands whole or not at all: a
// change that fails, or a finish hook that cannot commit its scratch copy,
// sends every change of the save back (rolledBack) and reports all of them
// as still pending, so a retry re-applies them; the per-item writers are
// idempotent. A cancel is not a failure: the changes before it stand,
// and so do the rest of the units they belong to (changesACancelLetsLand()).
SaveLoopResult runSaveLoop(const std::vector<std::shared_ptr<PendingChange>> &changes, SaveContext &ctx);

// Which of changes[from..] a cancel arriving before changes[from] still
// lets land, given the unit keys of the changes that landed: those linked
// to one of them through PendingChange::unitKeys(), directly or through
// other changes still to come. Indexed like `changes`; false before `from`.
std::vector<bool> changesACancelLetsLand(const QStringList &landedKeys,
                                         const std::vector<std::shared_ptr<PendingChange>> &changes, size_t from);

}  // namespace seabass::gui
