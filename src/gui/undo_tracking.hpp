// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <vector>

namespace seabass::gui
{

// One backup made as part of a just-completed write operation, kept just
// long enough to support "Undo Last Operation": restoring every tracked
// backup puts every file that operation touched back exactly as it was
// before it ran. backupDir is the FilesystemBackupStore base directory the
// backup lives under (BackupRecord::path's parent) -- not the record's own
// per-backup directory -- so a fresh store can be pointed at it to call
// restore(id).
struct UndoableBackup
{
    QString backupDir;
    QString id;
};

}  // namespace seabass::gui
