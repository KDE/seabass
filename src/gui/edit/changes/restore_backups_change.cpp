// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/changes/restore_backups_change.hpp"

#include <QStringList>

#include <string>

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
            return ChangeOutcome::failure(
                QStringLiteral("Undo Last Save could not read backup %1, so it stopped; nothing it had restored "
                               "was kept.")
                    .arg(it->id));
        }
        restored++;
    }
    ctx.log().record("undo: restored " + std::to_string(restored) + " backup(s) of the last save");
    return ChangeOutcome::success();
}

}  // namespace seabass::gui
