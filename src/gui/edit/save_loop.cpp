// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_loop.hpp"
#include "gui/sleep_inhibitor.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace seabass::gui
{

SaveLoopResult runSaveLoop(const std::vector<std::shared_ptr<PendingChange>> &changes, SaveContext &ctx)
{
    SaveLoopResult result;
    // Awake from the first backup to the last finish hook: a suspend halfway
    // through a save leaves the stick half-written. See SleepInhibitor.
    const auto keepAwake = SleepInhibitor::hold(QStringLiteral("Saving changes to a DJ library"));

    // Everything that can say what it will overwrite is backed up before
    // anything is applied, in one pass. Two reasons, and the ordering one
    // matters more than the speed: per-item backups meant the backup of
    // item 201 landed only after items 1 to 200 had already been
    // overwritten, so a crash in the middle left a save half-applied with
    // half a backup. Changes that cannot answer yet keep backing up as
    // they go, and backupOnce() skips whatever this already covered.
    std::vector<BackupTarget> upfront;
    std::vector<std::vector<BackupTarget>> declaredByChange;
    for (const auto &change : changes) {
        declaredByChange.push_back(change->filesToBackup(ctx));
        for (const auto &target : declaredByChange.back()) {
            upfront.push_back(target);
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
    for (size_t index = 0; index < changes.size(); ++index) {
        const auto &change = changes[index];
        if (ctx.cancel().cancelled()) {
            result.cancelled = true;
            break;
        }
        ctx.status(change->description());
        ChangeOutcome outcome;
        try {
            ctx.beginChange(declaredByChange[index]);
            outcome = change->apply(ctx);
        } catch (const std::exception &e) {
            outcome = ChangeOutcome::failure(QString::fromStdString(e.what()));
        }
        if (!outcome.ok) {
            result.failedId = change->id();
            result.error = outcome.error.isEmpty() ? QStringLiteral("failed") : outcome.error;
            // The page shows this once and forgets it; the stick's own log
            // is what is left to read afterwards, next to whatever the
            // change had already written before it failed.
            if (!ctx.rekordboxPath().isEmpty() || !ctx.enginePath().isEmpty()) {
                ctx.log().record("save: stopped at \"" + change->description().toStdString()
                                 + "\": " + result.error.toStdString());
            }
            // Whatever the change had already written, in any catalog, goes
            // back: a file removed from Engine but still listed by rekordbox
            // is the one state a DJ cannot repair from the page.
            if (auto undoError = ctx.rollBackChange()) {
                result.error += QStringLiteral(" -- and putting back what it had already written failed (%1); "
                                               "restore this save's backup")
                                    .arg(*undoError);
            }
            break;
        }
        ctx.endChange();
        result.appliedIds << change->id();
        ctx.progress().tick(++done);
    }

    ctx.status(QStringLiteral("Finishing"));
    // ok means "the whole batch went through"; a cancel or a failure hands
    // the hooks false so a scratch copy commits only what completed.
    const auto finish = ctx.runFinishHooks(result.error.isEmpty() && !result.cancelled);
    if (finish.warning && result.error.isEmpty()) {
        // Everything landed; a tidy-up did not. Said out loud, but the
        // changes stay applied -- clearing them would have the user save
        // the same removals twice.
        result.warning = *finish.warning;
    }
    if (finish.error) {
        // Whatever the hooks were committing did not land: report every
        // change as still pending rather than guess which did.
        result.appliedIds.clear();
        if (result.error.isEmpty()) {
            result.error = *finish.error;
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
