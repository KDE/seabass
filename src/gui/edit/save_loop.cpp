// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_loop.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "infrastructure/engine/engine_import_state.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace seabass::gui
{

namespace
{

// Whether an Engine player on this stick would stay quiet about the
// rekordbox library right now: both libraries there, and Engine's record
// of the last import level with the sequence export.pdb carries. See
// infrastructure/engine/engine_import_state.hpp for the mechanism, which
// was measured on a Prime 4 and a Prime Go+.
bool importLevel(const infrastructure::engine::RekordboxImportState &state)
{
    return state.hasEngineLibrary && state.hasRekordboxLibrary && state.error.empty()
        && state.engineCounter == state.librarySequence;
}

// Issue #42. Rewriting export.pdb moves its sequence -- Clean Up and
// Repair do, measured against both committed fixtures -- and a player
// that sees the sequence move offers to import the rekordbox library
// over the Engine one on the next insert: "Existing playlist and track
// metadata will be overwritten." Seabass's own edit is not a new
// rekordbox export, and must not look like one.
//
// So when the two were level before this save and are not after it,
// the move was this save's, and the new sequence goes into Engine's
// Information row as one more change of the same save: backed up with
// it, undone with it, through the same Engine write session. Read from
// where the save's writes are, which until the finish hooks commit is a
// scratch copy, not the stick.
//
// Never when they were apart to begin with. A stick with an import offer
// already pending has a rekordbox library that really did move on before
// Seabass touched it, and quietly swallowing that hides something the
// DJ may want.
//
// Returns a warning when the step could not be made, never an error: by
// now every change the user asked for has landed.
QString keepImportLevel(SaveContext &ctx, bool levelBefore)
{
    if (!levelBefore) {
        return {};
    }
    const std::string rekordboxPath = ctx.rekordboxPath().toStdString();
    const std::string enginePath = ctx.enginePath().toStdString();
    const FormatWriteSession *rekordbox = existingFormatWriteSession(ctx, "rekordbox", rekordboxPath);
    const FormatWriteSession *engine = existingFormatWriteSession(ctx, "engine", enginePath);
    const auto after = infrastructure::engine::readRekordboxImportState(engine ? engine->writeRoot() : enginePath,
                                                                         rekordbox ? rekordbox->writeRoot()
                                                                                   : rekordboxPath);
    if (!after.hasEngineLibrary || !after.hasRekordboxLibrary || !after.error.empty() || importLevel(after)) {
        return {};
    }
    MarkRekordboxImportedChange keep(ctx.enginePath(), after.librarySequence);
    const QString failed = QStringLiteral(
        "export.pdb changed, and Engine could not be told this save made the change, so a Denon player may offer "
        "to import the rekordbox library over the Engine one. Library Health can mark it imported (%1)");
    try {
        const std::vector<BackupTarget> targets = keep.filesToBackup(ctx);
        ctx.backupAllNow(targets);
        ctx.beginChange(targets);
        const ChangeOutcome outcome = keep.apply(ctx);
        if (!outcome.ok) {
            const auto undoError = ctx.rollBackChange();
            return failed.arg(outcome.error + (undoError ? QStringLiteral("; ") + *undoError : QString()));
        }
        ctx.endChange();
    } catch (const std::exception &e) {
        const auto undoError = ctx.rollBackChange();
        return failed.arg(QString::fromUtf8(e.what()) + (undoError ? QStringLiteral("; ") + *undoError : QString()));
    }
    return {};
}

}  // namespace

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
    const bool importLevelBefore =
        !ctx.rekordboxPath().isEmpty() && !ctx.enginePath().isEmpty()
        && importLevel(infrastructure::engine::readRekordboxImportState(ctx.enginePath().toStdString(),
                                                                        ctx.rekordboxPath().toStdString()));

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
            } else if (result.appliedIds.isEmpty()) {
                // Nothing was applied and everything went back, so the
                // backup this save took is a copy of a stick that never
                // changed. Round 5 found one left on a stick too full to
                // save to: a complete record, taking the space the save
                // had just been refused for. Only in this one case --
                // a change that DID apply needs its backup to undo from,
                // and a rollback that left anything behind needs it
                // more than that.
                ctx.discardBackupsTakenThisSave();
            }
            break;
        }
        ctx.endChange();
        result.appliedIds << change->id();
        ctx.progress().tick(++done);
    }

    // After a cancel or a failure too: the changes that did land are
    // committed by the finish hooks below, and any of them may have moved
    // the sequence.
    QString importWarning = keepImportLevel(ctx, importLevelBefore);

    ctx.status(QStringLiteral("Finishing"));
    // ok means "the whole batch went through"; a cancel or a failure hands
    // the hooks false so a scratch copy commits only what completed.
    const auto finish = ctx.runFinishHooks(result.error.isEmpty() && !result.cancelled);
    // Only when nothing actually failed: a warning saying "the changes are
    // applied, do not save again" beside an error that clears appliedIds
    // would contradict itself.
    if (finish.warning && !finish.error && result.error.isEmpty()) {
        // Everything landed; a tidy-up did not. Said out loud, but the
        // changes stay applied -- clearing them would have the user save
        // the same removals twice.
        result.warning = *finish.warning;
    }
    if (!importWarning.isEmpty() && !finish.error && result.error.isEmpty()) {
        result.warning = result.warning.isEmpty() ? importWarning : result.warning + QStringLiteral("; ") + importWarning;
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
