// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/restore_backups_change.hpp"

#include <set>

#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

#include <QStringList>

#include <string>
#include <vector>

#include "gui/edit/save_context.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"

namespace seabass::gui
{

RestoreBackupsChange::RestoreBackupsChange(std::vector<UndoableBackup> backups) : m_backups(std::move(backups)) {}

QString RestoreBackupsChange::description() const
{
    return QStringLiteral("Undo the last save (%1 backup(s))").arg(m_backups.size());
}

ChangeOutcome RestoreBackupsChange::apply(SaveContext &ctx)
{
    QStringList missing;
    for (const UndoableBackup &backup : m_backups) {
        infrastructure::backup::FilesystemBackupStore store(backup.backupDir.toStdString());
        if (!store.isRestorable(backup.id.toStdString())) {
            missing << backup.id;
        }
    }
    if (!missing.isEmpty()) {
        ctx.log().record("undo: refused, " + std::to_string(missing.size()) + " of "
                         + std::to_string(m_backups.size()) + " backup(s) are gone: "
                         + missing.join(QStringLiteral(", ")).toStdString());
        return ChangeOutcome::failure(
            QStringLiteral("Undo Last Save needs %1 of its %2 backup(s), and they are no longer on the stick (%3). "
                           "Nothing was restored.")
                .arg(missing.size())
                .arg(m_backups.size())
                .arg(missing.join(QStringLiteral(", "))));
    }

    // Everything the restores are about to overwrite, so a failure part-way
    // is rolled back rather than left half undone.
    for (const UndoableBackup &backup : m_backups) {
        infrastructure::backup::FilesystemBackupStore store(backup.backupDir.toStdString());
        for (const application::BackupRecord &record : store.list()) {
            if (record.id != backup.id.toStdString()) {
                continue;
            }
            for (const std::string &file : record.filePaths) {
                ctx.protectForThisChange(file);
            }
        }
    }

    int restored = 0;
    for (auto it = m_backups.rbegin(); it != m_backups.rend(); ++it) {
        infrastructure::backup::FilesystemBackupStore store(it->backupDir.toStdString());
        if (!store.restore(it->id.toStdString())) {
            // The pre-restore copies of the records that did go through
            // stay: the save loop rolls those files back next, and if that
            // fails too the copies are the only record of what was there.
            // The store removes a copy itself only when its restore wrote
            // nothing at all.
            // The store says what went wrong -- a damaged archive and a
            // stick with no room for the files read the same "false" and
            // used to be reported alike, as an unreadable backup (#27).
            return ChangeOutcome::failure(
                QStringLiteral("Undo Last Save stopped at backup %1: %2. Nothing it had restored was kept.")
                    .arg(it->id, QString::fromStdString(store.lastRestoreError())));
        }
        restored++;
    }
    ctx.log().record("undo: restored " + std::to_string(restored) + " backup(s) of the last save");

    // A Clean Up save listed the files of the rows it removed in Delete
    // Orphaned Files, each entry naming the backup that holds those rows.
    // The rows are back now, so those files are not orphaned any more;
    // left listed, the page offered to delete files the library uses again
    // (deleting them was refused, but the list said otherwise).
    const QString &catalogPath = ctx.rekordboxPath().isEmpty() ? ctx.enginePath() : ctx.rekordboxPath();
    if (!catalogPath.isEmpty()) {
        std::set<std::string> ids;
        for (const UndoableBackup &backup : m_backups) {
            ids.insert(backup.id.toStdString());
        }
        const std::string stickRoot = infrastructure::paths::stickRootForCatalogPath(catalogPath.toStdString());
        const std::string pendingPath = infrastructure::paths::stickPendingDeletions(stickRoot).string();
        // Rewritten inside this change, so a rollback of it puts the list
        // back along with the catalogs.
        ctx.protectForThisChange(pendingPath);
        // An undo that cannot drop these lines leaves the stick saying
        // two different things: the rows are back, and the files they
        // name are still listed as waiting to be deleted.
        //
        // Said out loud, not refused. By the time this runs the catalogs
        // are already restored, and failing here would have the save
        // loop roll that restore back out: the undo undone over a
        // bookkeeping file, on exactly the stick that is full or failing
        // and where getting the library back mattered most. Nothing is
        // destroyed by the stale lines either -- Delete Orphaned Files
        // re-checks every entry against the catalogs first and leaves a
        // file that a row still names alone.
        if (!infrastructure::cleanup::PendingDeletionManifest(pendingPath).removeForBackups(ids)) {
            ctx.log().record("undo: the list of files waiting to be deleted could not be updated; it still names "
                             "files this undo put back, which a later pass will leave alone");
            ctx.onFinish([](bool ok) {
                if (ok) {
                    throw SaveTidyUpFailed(
                        "Your library is back. The stick's list of files waiting to be deleted could not be "
                        "updated, so it still names files the undo restored; nothing is deleted on that list "
                        "alone, and the next check will drop them.");
                }
            });
        }
    }
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
