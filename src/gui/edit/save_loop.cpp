// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_loop.hpp"

#include <QSet>

#include "infrastructure/durable_file_write.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/mark_rekordbox_imported_change.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/rekordbox_baseline_ledger.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "infrastructure/engine/engine_import_state.hpp"

#include <algorithm>
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
// Whatever they were before. This used to run only when the two were
// level already, so an import offer that was pending before Seabass
// touched the stick stayed pending; the thinking was that a rekordbox
// library which really had moved on was something the DJ might want
// imported. Sebastian, 2026-10-02: that prompt is the data loss Seabass
// exists to prevent. Accepting it replaces the Engine side, cues and
// pads included, and once Seabass has handled a stick there is nothing
// it would bring that Seabass did not carry, bar tracks rekordbox
// exported that Engine never saw, and importing those stays a deliberate
// choice from the player's own menu. So every save that has both
// catalogs leaves them level, and the player stays quiet.
//
// Returns a warning when the step could not be made, never an error: by
// now every change the user asked for has landed.
QString keepImportLevel(SaveContext &ctx, bool wanted)
{
    if (!wanted) {
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

// After the finish hooks: the stick itself, not the scratch copies. The
// counter above is written before the hooks commit the rekordbox scratch
// copy, and runFinishHooks() carries on past a hook that fails -- so a
// commit of export.pdb that did not land (stick full, I/O error) would
// leave Engine at the new sequence and the pdb at the old one, which is
// Seabass arming the very prompt this exists to keep quiet. So whenever
// this save meant the two to end level, check what the stick now says
// and, if they are apart, put Engine at the sequence the pdb really has.
// Engine's database was backed up by the step above; this is a single
// row of it.
QString settleImportLevel(SaveContext &ctx)
{
    const auto now = infrastructure::engine::readRekordboxImportState(ctx.enginePath().toStdString(),
                                                                       ctx.rekordboxPath().toStdString());
    if (!now.hasEngineLibrary || !now.hasRekordboxLibrary || !now.error.empty() || importLevel(now)) {
        return {};
    }
    std::string error;
    if (infrastructure::engine::markRekordboxLibraryImported(ctx.enginePath().toStdString(), now.librarySequence,
                                                            &error)) {
        ctx.log().record("engine import counter: put back to " + std::to_string(now.librarySequence)
                         + " to match export.pdb as it landed");
        return {};
    }
    return QStringLiteral("Engine's record of the rekordbox import does not match export.pdb after this save, so "
                          "a Denon player may offer to import the rekordbox library over the Engine one. Library "
                          "Health can mark it imported (%1)")
        .arg(QString::fromStdString(error));
}

}  // namespace

// Linked through a change still to come too, because a playlist's
// membership change that holds a track already added to Engine holds the
// tracks not yet added as well, and without them it would fail and send
// the whole save back.
std::vector<bool> changesACancelLetsLand(const QStringList &landedKeys,
                                         const std::vector<std::shared_ptr<PendingChange>> &changes, size_t from)
{
    std::vector<bool> lands(changes.size(), false);
    std::vector<QStringList> keysOf(changes.size());
    for (size_t index = from; index < changes.size(); ++index) {
        keysOf[index] = changes[index]->unitKeys();
    }
    // Grown until nothing more joins: a change that shares a key with the
    // set lands, and brings its own keys into it.
    QSet<QString> unit(landedKeys.begin(), landedKeys.end());
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t index = from; index < changes.size(); ++index) {
            if (lands[index]) {
                continue;
            }
            const QStringList &keys = keysOf[index];
            if (std::any_of(keys.begin(), keys.end(), [&unit](const QString &key) { return unit.contains(key); })) {
                lands[index] = true;
                unit.unite(QSet<QString>(keys.begin(), keys.end()));
                grew = true;
            }
        }
    }
    return lands;
}

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
    // Both catalogs known: the save leaves the player's import record level
    // with export.pdb, whatever it was before (see keepImportLevel()).
    const bool importLevelWanted = !ctx.rekordboxPath().isEmpty() && !ctx.enginePath().isEmpty();

    std::vector<BackupTarget> upfront;
    std::vector<std::vector<BackupTarget>> declaredByChange;
    for (const auto &change : changes) {
        declaredByChange.push_back(change->filesToBackup(ctx));
        for (const auto &target : declaredByChange.back()) {
            upfront.push_back(target);
        }
    }

    // The stick's rekordbox baseline, read before anything is written: the
    // origin ledger after the commit needs to know whether it was current
    // now. And the file joins the backups, under the first record's label,
    // so Undo puts it back with the catalogs. Only when a change declared
    // something to back up: a save that declares nothing (Undo Last Save
    // itself) writes no rekordbox cue or rating, so its ledger step never
    // writes the baseline, and an extra record would be a copy nothing
    // restores. A save without a rekordbox path keeps out of it entirely.
    const BaselineAtSaveStart baselineAtStart = readBaselineAtSaveStart(ctx);
    const std::string baselineLabel = upfront.empty() ? std::string("rekordbox-baseline") : upfront.front().label;
    if (baselineAtStart.exists && !upfront.empty()) {
        upfront.push_back({baselineAtStart.file, baselineLabel});
    }

    // One save, one bar (#58): announced once, with everything it will
    // do in its total, and ticked through to the end. The three stretches
    // used to be three announcements, and a bar that fills, empties and
    // fills again reads as the save starting over, or as the first bar
    // having lied. The stretches are the undo copy (one unit per file,
    // known before the first copy), the items (one each), and the commit
    // at the end, which has no count of its own and is one unit the bar
    // sits just short of while it runs. The label says which stretch,
    // through ctx.status().
    const size_t backupUnits = ctx.countBackupFiles(upfront);
    const size_t total = backupUnits + changes.size() + 1;
    ctx.progress().start("Saving changes", total);
    if (!upfront.empty()) {
        // Not "Backing up": on Restore Metadata that read as the page doing
        // the opposite of what was asked. It is the copy Undo Last Save
        // restores from, and says so.
        ctx.status(QStringLiteral("Saving an undo copy"));
        try {
            ctx.backupAllNow(upfront, /*countFiles=*/true);
        } catch (const std::exception &e) {
            // Nothing has been written yet, so refusing here costs the
            // user nothing and protects everything.
            result.error = QStringLiteral("could not back up before saving: %1").arg(QString::fromUtf8(e.what()));
            ctx.progress().finish();
            return result;
        }
    }

    // Before the first change, for every change: a record of what the save
    // did registers here, so a save stopped before it still records what
    // landed (PendingChange::beforeSave).
    for (const auto &change : changes) {
        change->beforeSave(ctx);
    }

    size_t done = 0;
    // The unit keys of every change that landed, and once a cancel has
    // arrived, which of the changes still to come belong to a unit one of
    // them started (changesACancelLetsLand()).
    QStringList landedKeys;
    std::vector<bool> finishesAUnit;
    // Said only if the save's own rollback (below) does not put it right:
    // that one starts from the copies taken before the save, so a stick it
    // put back has nothing left of the change whose put-back failed.
    std::optional<QString> changeUndoError;
    for (size_t index = 0; index < changes.size(); ++index) {
        const auto &change = changes[index];
        // Between units only: the rest of a unit a cancel arrived inside
        // still lands, wherever it is staged (PendingChange::unitKeys()),
        // and nothing else does.
        if (ctx.cancel().cancelled()) {
            if (!result.cancelled) {
                result.cancelled = true;
                finishesAUnit = changesACancelLetsLand(landedKeys, changes, index);
            }
            if (!finishesAUnit[index]) {
                continue;
            }
        }
        ctx.status(change->subject());
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
            changeUndoError = ctx.rollBackChange();
            // And the changes before it go back too: the save lands whole
            // or not at all (SaveContext::rollBackSave(), below, once the
            // finish hooks have closed what they hold).
            ctx.markSaveFailed();
            break;
        }
        ctx.endChange();
        landedKeys << change->unitKeys();
        result.appliedIds << change->id();
        if (outcome.skipped) {
            result.skippedIds << change->id();
        }
        ctx.progress().tick(backupUnits + ++done);
    }

    // After a cancel: the changes that did land are committed by the
    // finish hooks below, and any of them may have moved the sequence. Not
    // after a failure, which goes back whole and so moved nothing.
    // "Mark as imported" in the same save carries the sequence from when
    // it was staged, and a repair later in the save may move the pdb past
    // it; the step below reads the sequence as the save left it.
    // Said before the steps that follow, not after: levelling the import
    // record backs Engine's database up again, and the finish hooks copy
    // scratch databases back onto the stick. With every item ticked and
    // the last track's name still showing, those seconds read as a hang.
    // No count of its own: it is the last unit of the one bar, which
    // stays one short of full until the stick is committed.
    ctx.status(QStringLiteral("Committing changes to the stick"));
    ctx.progress().tick(backupUnits + changes.size());
    QString importWarning = ctx.saveFailed() ? QString() : keepImportLevel(ctx, importLevelWanted);

    // ok means "the whole batch went through". A cancel hands the hooks
    // false, so a scratch copy commits what completed; a failed save
    // (ctx.saveFailed()) has it commit nothing.
    const auto finish = ctx.runFinishHooks(result.error.isEmpty() && !result.cancelled);
    if (finish.error) {
        // A commit that did not land: whatever the others committed, and
        // whatever the changes wrote straight onto the stick, goes back
        // with it, or the formats would disagree by exactly that commit.
        ctx.markSaveFailed();
    }
    std::optional<QString> rollbackError;
    if (ctx.saveFailed()) {
        rollbackError = ctx.rollBackSave();
        result.rolledBack = !rollbackError;
        // A change a cancel let land that then failed: nothing the cancel
        // would have kept was kept, so the save reads as the failure it is
        // and not as a cancel (the pages decide on `cancelled` first).
        result.cancelled = false;
        if (changeUndoError && rollbackError) {
            result.error += QStringLiteral(", and putting back what it had already written failed (%1)")
                                .arg(*changeUndoError);
        }
        result.appliedIds.clear();
        result.skippedIds.clear();
    }
    // Only when nothing actually failed: a warning saying "the changes are
    // applied, do not save again" beside an error that clears appliedIds
    // would contradict itself.
    if (finish.warning && !ctx.saveFailed() && result.error.isEmpty()) {
        // Everything landed; a tidy-up did not. Said out loud, but the
        // changes stay applied -- clearing them would have the user save
        // the same removals twice.
        result.warning = *finish.warning;
    }
    if (importLevelWanted && importWarning.isEmpty() && !ctx.saveFailed()) {
        importWarning = settleImportLevel(ctx);
    }

    // The records of what the save did, read from the stick as the save
    // left it: the after-commit hooks (the page's baseline), then, unless a
    // change of this save recorded the baseline itself, the origin ledger.
    // Warnings, never failures: everything they report on has landed.
    QStringList afterCommitWarnings;
    {
        SaveContext::AfterCommit after;
        if (!ctx.saveFailed()) {
            after.appliedIds = result.appliedIds;
            after.skippedIds = result.skippedIds;
        }
        after.succeeded = result.error.isEmpty() && !result.cancelled && !ctx.saveFailed();
        for (const auto &change : changes) {
            if (after.appliedIds.contains(change->id()) && !after.skippedIds.contains(change->id())) {
                for (auto &write : change->rekordboxWrites()) {
                    after.rekordboxWrites.push_back(std::move(write));
                }
            }
        }
        for (const QString &warning : ctx.runAfterCommitHooks(after)) {
            afterCommitWarnings << warning;
        }
        if (!ctx.rekordboxBaselineRecordedBySave()) {
            const QString ledger = recordOriginLedger(ctx, baselineAtStart, after, baselineLabel);
            if (!ledger.isEmpty()) {
                ctx.log().record("save: " + ledger.toStdString());
                afterCommitWarnings << ledger;
            }
        }
    }
    // Everything this save left for the kernel to write later (the backup
    // archive, the log) is written now, while the dialog still says the
    // stick is being committed, and not at eject. See flushFilesystemOf().
    if (const std::string stick = ctx.stickRoot(); !stick.empty()) {
        infrastructure::flushFilesystemOf(stick);
    }
    if (!importWarning.isEmpty() && !finish.error && result.error.isEmpty()) {
        result.warning = result.warning.isEmpty() ? importWarning : result.warning + QStringLiteral("; ") + importWarning;
    }
    for (const QString &warning : afterCommitWarnings) {
        // Beside an error it joins the error, as the import warning does.
        if (!finish.error && result.error.isEmpty()) {
            result.warning = result.warning.isEmpty() ? warning : result.warning + QStringLiteral("; ") + warning;
        }
    }
    if (finish.error) {
        // Whatever the hooks were committing did not land: report every
        // change as still pending rather than guess which did.
        result.appliedIds.clear();
        result.skippedIds.clear();
        if (result.error.isEmpty()) {
            result.error = *finish.error;
        }
    }
    // Beside an error it is part of the error: a warning next to one is
    // dropped above, and this one is worth reading.
    if (!importWarning.isEmpty() && !result.error.isEmpty()) {
        result.error += QStringLiteral("; ") + importWarning;
    }
    if (!result.error.isEmpty()) {
        for (const QString &warning : afterCommitWarnings) {
            result.error += QStringLiteral("; ") + warning;
        }
    }
    if (ctx.saveFailed()) {
        if (rollbackError) {
            // The one case a failed save leaves something written: the
            // backup records are the way back, so they stay, and the note
            // on the stick keeps Undo Last Save on offer.
            result.error += QStringLiteral("; putting the stick back as it was before this save failed (%1), so "
                                           "Undo Last Save restores this save's backup")
                                .arg(*rollbackError);
        } else {
            // Everything went back, so the records this save took are
            // copies of a stick that never changed. Round 5 found one left
            // on a stick too full to save to: a complete record, taking
            // the space the save had just been refused for.
            ctx.discardBackupsTakenThisSave();
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

    ctx.progress().tick(total);
    ctx.progress().finish();
    result.backups = ctx.takeBackups();
    return result;
}

}  // namespace seabass::gui
