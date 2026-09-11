// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QDir>
#include <QString>

namespace seabass::gui
{

// The file a stick's full backup lives in: named after the stick label
// with the characters no filesystem accepts replaced, never empty. One
// definition, so the backup page, the restore page and the clone page
// all find the same archive for the same stick.
inline QString archiveFileNameForLabel(const QString &stickLabel)
{
    QString name = stickLabel.trimmed();
    for (QChar &c : name) {
        if (QStringLiteral("/\\:*?\"<>|").contains(c) || c.unicode() < 0x20) {
            c = QLatin1Char('_');
        }
    }
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        name = QStringLiteral("stick");
    }
    return name + QStringLiteral(".zip");
}

inline QString archivePathForLabel(const QString &backupDirectory, const QString &stickLabel)
{
    return QDir(backupDirectory).filePath(archiveFileNameForLabel(stickLabel));
}

}  // namespace seabass::gui
