// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_loop.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace seabass::gui
{

SaveLoopResult runSaveLoop(const std::vector<std::shared_ptr<PendingChange>> &changes, SaveContext &ctx)
{
    SaveLoopResult result;

    // Everything that can say what it will overwrite is backed up before
    // anything is applied, in one pass. Two reasons, and the ordering one
    // matters more than the speed: per-item backups meant the backup of
    // item 201 landed only after items 1 to 200 had already been
    // overwritten, so a crash in the middle left a save half-applied with
    // half a backup. Changes that cannot answer yet keep backing up as
    // they go, and backupOnce() skips whatever this already covered.
    std::vector<BackupTarget> upfront;
    for (const auto &change : changes) {
        for (auto &target : change->filesToBackup(ctx)) {
            upfront.push_back(std::move(target));
        }
    }
    if (!upfront.empty()) {
        ctx.status(QStringLiteral("Backing up"));
        try {
            ctx.backupAllNow(upfront);
        } catch (const std::exception &e) {
            // Nothing has been written yet, so refusing here costs the
            // user nothing and protects everything.
            result.error = QStringLiteral("could not back up before saving: %1").arg(QString::fromUtf8(e.what()));
            ctx.progress().finish();
            return result;
        }
    }

    ctx.progress().start("Saving changes", changes.size());
    size_t done = 0;
    for (const auto &change : changes) {
        if (ctx.cancel().cancelled()) {
            result.cancelled = true;
            break;
        }
        ctx.status(change->description());
        ChangeOutcome outcome;
        try {
            outcome = change->apply(ctx);
        } catch (const std::exception &e) {
            outcome = ChangeOutcome::failure(QString::fromStdString(e.what()));
        }
        if (!outcome.ok) {
            result.failedId = change->id();
            result.error = outcome.error.isEmpty() ? QStringLiteral("failed") : outcome.error;
            break;
        }
        result.appliedIds << change->id();
        ctx.progress().tick(++done);
    }

    ctx.status(QStringLiteral("Finishing"));
    // ok means "the whole batch went through"; a cancel or a failure hands
    // the hooks false so a scratch copy commits only what completed.
    if (auto hookError = ctx.runFinishHooks(result.error.isEmpty() && !result.cancelled)) {
        // Whatever the hooks were committing did not land: report every
        // change as still pending rather than guess which did.
        result.appliedIds.clear();
        if (result.error.isEmpty()) {
            result.error = *hookError;
        }
    }
    // Only after the whole batch went through, and only if the stick is
    // actually tight. On a failure or a cancel the backups are the thing
    // that saves you, so nothing is released then; and while there is
    // room, a backup is worth far more than the space it takes.
    //
    // Deliberately not reported as a step or an error: this is Seabass
    // tidying up after itself, and a save that succeeded must not look
    // like it half-failed because a cleanup could not get the lock.
    if (result.error.isEmpty() && !result.cancelled) {
        result.bytesReleased = ctx.releaseAutomaticBackupsIfTight();
    }

    ctx.progress().finish();
    result.backups = ctx.takeBackups();
    return result;
}

}  // namespace seabass::gui
